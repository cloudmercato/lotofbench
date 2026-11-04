/* LotofBench — Background Blur workload (workload #10).
 *
 * Mirrors the tenth Geekbench 6 CPU workload: segment the subject out
 * of a video clip and blur the background behind it, the portrait-mode
 * video-call effect. LotofBench reproduces the shape dependency-free,
 * with the same honesty rules as the other workloads:
 *
 *   - deterministic synthetic input — a seeded clip of 12 frames
 *     (1920x1080 RGB, Full HD, the stand-in for the capture size) in
 *     four scene archetype families (video call, living room, backyard,
 *     office) that cycle the way a clip reel does. Every frame is a
 *     two-stop vertical background gradient plus per-pixel noise, with
 *     one planted subject: a 180-360 x 300-600 px rectangle darkened
 *     by 50-90 per channel, kept at least 16 px inside the frame. The
 *     subject box and tone are the ground truth, exposed through
 *     accessors. The background is the same in every frame of an
 *     archetype (a static camera), so a prepare-time background model
 *     per archetype is exact. Fixed --seed means byte-identical frames
 *     on every machine; generation is off the clock — the stand-in for
 *     the clip loader;
 *   - a deterministic segmentation pass — the trained-DeepLabV3+
 *     stand-in (docs/benchmarks.md, "Workload 10"): the frame is
 *     downscaled 1/4 with the shared exact box kernel (the DeepLab-
 *     style low-res inference input), each 480x270 tap cell scores
 *     the max per-channel absolute difference against its archetype's
 *     background model, and the score is thresholded at 24. The
 *     bounds are documented and guaranteed, not statistical: a 4x4
 *     block mean of the +-6 noise never exceeds 6, and the planted
 *     step (>= 50 applied, >= 44 after the block mean) clears the
 *     threshold with margin — every pure-background cell scores 0 and
 *     every pure-subject cell 255. A 3x3 majority filter (the
 *     morphological cleanup stand-in) smooths the binary mask, and a
 *     bilinear x4 upsample to the frame resolution feathers it;
 *   - the per-pixel-blur-strength composite — a two-pass box blur
 *     (radius 9, the shared img_box_blur kernel) produces the blurred
 *     frame, and the composite
 *         out = (mask*sharp + (255-mask)*blurred + 127) / 255
 *     blends per channel with half-up rounding: a pixel with mask 255
 *     is bit-identical to the input (the subject stays sharp), a pixel
 *     with mask 0 is exactly the blurred value, and the feathered edge
 *     interpolates the blur strength per pixel. All integer, so mask
 *     and output are bit-identical across machines and compilers;
 *   - correctness inside the timed cycle — a pass only counts if every
 *     frame reproduces its prepare-time reference pipeline: SHA-1 of
 *     the upsampled mask and of the composited output frame, plus the
 *     exact mask_ones and changed counters, all bit-identical. Any
 *     drift aborts the run instead of scoring;
 *   - shared-task multi-core through the common harness — the
 *     multi-core pass blurs 48 frames (4x the set) split into equal
 *     contiguous slices, one per thread.
 *
 * The throughput metric is frames: every frame contributes exactly one
 * full pipeline (downscale, segment, filter, upsample, blur,
 * composite), exact and machine-independent, the image-workload
 * analogue of the corpus size.
 *
 * Deliberate divergences from Geekbench 6 (all documented in
 * docs/benchmarks.md): exact background subtraction instead of the
 * trained DeepLabV3+ (no model file, no licence, no runtime dependency
 * — and the mask provably recovers the planted subject, so the
 * workload can verify real segmentation quality, which an untrained
 * net could not), a static-camera clip per archetype instead of a
 * panning camera, a single blur radius interpolated by the mask
 * instead of a true variable-radius kernel, and synthetic seeded
 * frames instead of a real video — the pipeline structure (low-res
 * segmentation, feathered mask upsample, mask-modulated composite)
 * and the metric (frames per second) are the workload's.
 */

#include "workload_backgroundblur.h"

#include "harness.h"
#include "imgops.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <stdlib.h>
#include <string.h>

#define BB_FRAME_BYTES ((size_t)BB_FRAME_W * BB_FRAME_H * 3)
#define BB_SEG_CELLS   ((size_t)BB_SEG_W * BB_SEG_H)
#define BB_ARCHETYPES  4u

/* --- corpus generation ------------------------------------------------------*/

typedef struct {
    const char *name;
    uint8_t bg_a[3], bg_b[3]; /* the two-stop vertical background */
} bb_scene;

/* Every palette channel stays within [76, 210]: the bottom stops are
 * high enough that darkening a pixel by up to 90 keeps the applied
 * step >= 50 even at the darkest corner (the darkest background byte
 * is 76 - 6 = 70, so the worst clamp leaves a 70-level step), and the
 * top stops are low enough that no darkening ever pins to white. */
static const bb_scene ARCHETYPES[BB_ARCHETYPES] = {
    { "video-call",  { 196, 190, 182 }, { 142, 138, 132 } },
    { "living-room", { 182, 158, 128 }, { 122, 102, 84 } },
    { "backyard",    { 152, 188, 140 }, { 86, 120, 76 } },
    { "office",      { 210, 208, 202 }, { 150, 152, 154 } },
};

/* The background's channel value at row y: a two-stop vertical
 * gradient in half-up rounding (fixed-point, no float). */
static uint8_t bg_stop(int32_t a, int32_t b, uint32_t y)
{
    int32_t v = a + (((b - a) * (int32_t)y * 2 + (int32_t)BB_FRAME_H) /
                     ((int32_t)BB_FRAME_H * 2));

    return (uint8_t)v;
}

/* Paint the noise-free archetype background (the static-camera scene):
 * the same code paints the model taps and every frame's background. */
static void paint_bg(uint8_t *img, unsigned arch)
{
    const bb_scene *sc = &ARCHETYPES[arch % BB_ARCHETYPES];

    for (uint32_t y = 0; y < BB_FRAME_H; y++)
        for (uint32_t x = 0; x < BB_FRAME_W; x++) {
            uint8_t *px = img + ((size_t)y * BB_FRAME_W + x) * 3;

            for (int c = 0; c < 3; c++)
                px[c] = bg_stop(sc->bg_a[c], sc->bg_b[c], y);
        }
}

/* Symmetric uint8 saturate helper for the corpus painter. */
static uint8_t clamp_u8(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

struct bb_ctx {
    const uint8_t *model[BB_ARCHETYPES]; /* shared, read-only */
    uint8_t  *frame[BB_FRAMES];
    bb_result ref[BB_FRAMES];
    unsigned  arch[BB_FRAMES];
    bb_bbox   subj_box[BB_FRAMES];
    int32_t   subj_tone[BB_FRAMES];
};

/* The background models, built once per process on the first
 * bb_prepare(): the exact 1/4 box downscale of every archetype's
 * noise-free background — derived from the archetype palette alone,
 * so they are seed-independent, the static-camera median stand-in.
 * The pointer is published before any harness thread starts, so the
 * pipeline threads only ever read them (never freed: like the fixed
 * net, they live for the whole process). */
static struct {
    uint8_t *plane[BB_ARCHETYPES];
    uint8_t  digest[20];
} MODELS;

static void models_init_once(void)
{
    uint8_t *bg;

    if (MODELS.plane[0])
        return;
    bg = malloc(BB_FRAME_BYTES);
    if (!bg)
        abort(); /* a benchmark must not run with half-built models */
    for (unsigned a = 0; a < BB_ARCHETYPES; a++) {
        MODELS.plane[a] = malloc(BB_SEG_CELLS * 3);
        if (!MODELS.plane[a])
            abort();
        paint_bg(bg, a);
        img_box_downscale(MODELS.plane[a], BB_SEG_W, BB_SEG_H, bg,
                          BB_FRAME_W, BB_FRAME_H);
    }
    free(bg);
    {
        uint8_t *blob = malloc(BB_ARCHETYPES * BB_SEG_CELLS * 3);

        if (!blob)
            abort();
        for (unsigned a = 0; a < BB_ARCHETYPES; a++)
            memcpy(blob + (size_t)a * BB_SEG_CELLS * 3,
                   MODELS.plane[a], BB_SEG_CELLS * 3);
        sha1(blob, BB_ARCHETYPES * BB_SEG_CELLS * 3, MODELS.digest);
        free(blob);
    }
}

static void gen_frame(uint8_t *img, unsigned arch, rng_t *rng,
                      bb_bbox *box, int32_t *tone)
{
    /* one planted subject per frame: 180-360 x 300-600 px, at least
     * 16 px inside the frame, darkened by 50-90 per channel (the
     * palette bounds above keep the applied step >= 50 everywhere,
     * two orders above the +-6 noise) */
    uint32_t w = 180 + rng_below(rng, 181);
    uint32_t h = 300 + rng_below(rng, 301);
    int32_t x0 = 16 + (int32_t)rng_below(rng, BB_FRAME_W - 32 - w + 1);
    int32_t y0 = 16 + (int32_t)rng_below(rng, BB_FRAME_H - 32 - h + 1);

    paint_bg(img, arch);
    box->x0 = x0;
    box->y0 = y0;
    box->x1 = x0 + (int32_t)w;
    box->y1 = y0 + (int32_t)h;
    *tone = -(int32_t)(50 + rng_below(rng, 41)); /* -90..-50 */

    /* paint the subject: the rectangle darkened by its tone */
    for (int32_t y = box->y0; y < box->y1; y++)
        for (int32_t x = box->x0; x < box->x1; x++) {
            uint8_t *px = img + ((size_t)y * BB_FRAME_W +
                                 (size_t)x) * 3;

            for (int c = 0; c < 3; c++)
                px[c] = clamp_u8((int32_t)px[c] + *tone);
        }

    /* per-pixel noise: +-6, one draw per channel byte (the stand-in
     * for sensor noise; two orders below the planted edge step) */
    for (size_t i = 0; i < BB_FRAME_BYTES; i++)
        img[i] = (uint8_t)((int32_t)img[i] +
                           (int32_t)rng_below(rng, 13) - 6);
}

/* --- the integer kernels ---------------------------------------------------- */

void bb_segment(const uint8_t *tap, const uint8_t *model, size_t n,
                uint8_t *coarse)
{
    for (size_t i = 0; i < n; i++) {
        int32_t d0 = (int32_t)tap[i * 3 + 0] - model[i * 3 + 0];
        int32_t d1 = (int32_t)tap[i * 3 + 1] - model[i * 3 + 1];
        int32_t d2 = (int32_t)tap[i * 3 + 2] - model[i * 3 + 2];
        int32_t d = d0 < 0 ? -d0 : d0;

        if (d1 < 0)
            d1 = -d1;
        if (d1 > d)
            d = d1;
        if (d2 < 0)
            d2 = -d2;
        if (d2 > d)
            d = d2;
        coarse[i] = d >= (int32_t)BB_SEG_THRESH ? 255 : 0;
    }
}

void bb_majority3(const uint8_t *in, size_t w, size_t h, uint8_t *out)
{
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
            unsigned ones = 0;

            for (size_t dy = 0; dy < 3; dy++) {
                size_t iy = y + dy - 1; /* size_t wraps: -1 becomes
                                           huge, caught below */

                if (iy >= h)
                    continue;
                for (size_t dx = 0; dx < 3; dx++) {
                    size_t ix = x + dx - 1;

                    if (ix >= w)
                        continue;
                    ones += in[iy * w + ix] ? 1u : 0u;
                }
            }
            out[y * w + x] = ones >= 5 ? 255 : 0;
        }
}

void bb_mask_upscale(uint8_t *dst, size_t dw, size_t dh,
                     const uint8_t *src, size_t sw, size_t sh)
{
    /* Same convention as img_scale_bilinear, one channel: source
     * coordinate (x + 0.5) * sw / dw - 0.5 stepped in 16.16 fixed
     * point, edge-clamped neighbours, half-up rounding. The four
     * products multiply two 16.16 weights, so the accumulator lives
     * at 32.32 and the final shift is >> 32 (255 * 65536^2 fits
     * uint64). */
    uint64_t dx = ((uint64_t)sw << 16) / dw;
    uint64_t dy = ((uint64_t)sh << 16) / dh;
    int64_t sx0 = (int64_t)(((uint64_t)sw * 32768u) / dw) - 32768;
    int64_t sy0 = (int64_t)(((uint64_t)sh * 32768u) / dh) - 32768;

    for (size_t y = 0; y < dh; y++) {
        int64_t sy = sy0 + (int64_t)(dy * y);
        int64_t y0 = sy >> 16; /* floor, handles the negative start */
        int64_t fy = sy - (y0 << 16);
        size_t ya = (size_t)(y0 < 0 ? 0 : y0);
        size_t yb = (ya + 1 < sh) ? ya + 1 : sh - 1;

        for (size_t x = 0; x < dw; x++) {
            int64_t sx = sx0 + (int64_t)(dx * x);
            int64_t x0 = sx >> 16;
            int64_t fx = sx - (x0 << 16);
            size_t xa = (size_t)(x0 < 0 ? 0 : x0);
            size_t xb = (xa + 1 < sw) ? xa + 1 : sw - 1;
            uint32_t wx0 = 65536u - (uint32_t)fx;
            uint32_t wx1 = (uint32_t)fx;
            uint32_t wy0 = 65536u - (uint32_t)fy;
            uint32_t wy1 = (uint32_t)fy;
            uint64_t top = (uint64_t)src[ya * sw + xa] * wx0 +
                           (uint64_t)src[ya * sw + xb] * wx1;
            uint64_t bot = (uint64_t)src[yb * sw + xa] * wx0 +
                           (uint64_t)src[yb * sw + xb] * wx1;
            uint64_t v = (top * wy0 + bot * wy1 + (1ull << 31)) >> 32;

            dst[y * dw + x] = (uint8_t)v;
        }
    }
}

void bb_composite(uint8_t *out, const uint8_t *frame,
                  const uint8_t *blurred, const uint8_t *mask,
                  size_t n_px)
{
    for (size_t p = 0; p < n_px; p++) {
        uint32_t m = mask[p];
        uint32_t im = 255u - m;

        for (int c = 0; c < 3; c++) {
            /* mask 255 -> exactly the sharp pixel, mask 0 -> exactly
             * the blurred pixel, everything between half-up; the sum
             * is < 2^17, uint32 is exact */
            uint32_t v = (m * frame[p * 3 + c] + im * blurred[p * 3 + c] +
                          127u) / 255u;

            out[p * 3 + c] = (uint8_t)v;
        }
    }
}

/* The full per-frame pipeline. Buffer roles: frame (read-only input),
 * model (read-only, the frame's archetype model); tap <- downscale,
 * coarse <- segment, refined <- majority, mask <- upscale, blurred <-
 * frame copy then img_box_blur, then the composite writes the output
 * back into blurred. The model must belong to the frame's archetype. */

int bb_process(const uint8_t *frame, const uint8_t *model, bb_scratch *s,
               bb_result *out)
{
    if (!frame || !model || !s || !out || !s->tap || !s->coarse ||
        !s->refined || !s->mask || !s->blurred || !s->blur_sc)
        return -1;

    img_box_downscale(s->tap, BB_SEG_W, BB_SEG_H, frame, BB_FRAME_W,
                      BB_FRAME_H);
    bb_segment(s->tap, model, BB_SEG_CELLS, s->coarse);
    bb_majority3(s->coarse, BB_SEG_W, BB_SEG_H, s->refined);
    bb_mask_upscale(s->mask, BB_FRAME_W, BB_FRAME_H, s->refined,
                    BB_SEG_W, BB_SEG_H);

    memcpy(s->blurred, frame, BB_FRAME_BYTES);
    img_box_blur(s->blurred, BB_FRAME_W, BB_FRAME_H, BB_BLUR_RADIUS,
                 s->blur_sc);
    bb_composite(s->blurred, frame, s->blurred, s->mask,
                 (size_t)BB_FRAME_W * BB_FRAME_H);

    sha1(s->mask, (size_t)BB_FRAME_W * BB_FRAME_H, out->mask_digest);
    sha1(s->blurred, BB_FRAME_BYTES, out->out_digest);

    /* exact counters: the fully-foreground plateau and the pixels the
     * blur actually changed (a degeneracy guard — a collapsed mask or
     * an identity composite shows up here as well as in the digests) */
    out->mask_ones = 0;
    out->changed = 0;
    for (size_t p = 0; p < (size_t)BB_FRAME_W * BB_FRAME_H; p++) {
        if (s->mask[p] == 255)
            out->mask_ones++;
        if (s->blurred[p * 3] != frame[p * 3] ||
            s->blurred[p * 3 + 1] != frame[p * 3 + 1] ||
            s->blurred[p * 3 + 2] != frame[p * 3 + 2])
            out->changed++;
    }
    s->res = *out; /* the scratch keeps the result of the last pipeline */
    return 0;
}

/* --- prepare / run ----------------------------------------------------------*/

bb_ctx *bb_prepare(unsigned seed)
{
    bb_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t prng;

    if (!ctx)
        return NULL;
    /* the background models: hard-coded archetype palettes,
     * independent of the workload seed — they are part of the
     * workload's identity */
    models_init_once();
    for (unsigned a = 0; a < BB_ARCHETYPES; a++)
        ctx->model[a] = MODELS.plane[a];

    rng_init(&prng, seed);
    for (unsigned f = 0; f < BB_FRAMES; f++) {
        unsigned arch = f % BB_ARCHETYPES;
        bb_scratch sc;

        ctx->frame[f] = malloc(BB_FRAME_BYTES);
        if (!ctx->frame[f]) {
            bb_free(ctx);
            return NULL;
        }
        ctx->arch[f] = arch;
        gen_frame(ctx->frame[f], arch, &prng, &ctx->subj_box[f],
                  &ctx->subj_tone[f]);

        /* reference pipeline, off the clock */
        if (bb_scratch_init(&sc) != 0) {
            bb_free(ctx);
            return NULL;
        }
        if (bb_process(ctx->frame[f], ctx->model[arch], &sc,
                       &ctx->ref[f]) != 0) {
            bb_scratch_free(&sc);
            bb_free(ctx);
            return NULL;
        }
        bb_scratch_free(&sc);
    }
    return ctx;
}

void bb_free(bb_ctx *ctx)
{
    if (!ctx)
        return;
    /* the MODELS planes are shared, process-lifetime (like the fixed
     * net's weights) — only the per-ctx frames are freed here */
    for (unsigned f = 0; f < BB_FRAMES; f++)
        free(ctx->frame[f]);
    free(ctx);
}

int bb_scratch_init(bb_scratch *s)
{
    memset(s, 0, sizeof(*s));
    s->tap = malloc(BB_SEG_CELLS * 3);
    s->coarse = malloc(BB_SEG_CELLS);
    s->refined = malloc(BB_SEG_CELLS);
    s->mask = malloc((size_t)BB_FRAME_W * BB_FRAME_H);
    s->blurred = malloc(BB_FRAME_BYTES);
    s->blur_sc = malloc(BB_FRAME_BYTES);
    if (!s->tap || !s->coarse || !s->refined || !s->mask ||
        !s->blurred || !s->blur_sc) {
        bb_scratch_free(s);
        return -1;
    }
    return 0;
}

void bb_scratch_free(bb_scratch *s)
{
    if (!s)
        return;
    free(s->tap);
    free(s->coarse);
    free(s->refined);
    free(s->mask);
    free(s->blurred);
    free(s->blur_sc);
    memset(s, 0, sizeof(*s));
}

/* One thread's slice: contiguous frame instances, own scratch —
 * threads never share writable state (shared-task harness contract). */
typedef struct {
    bb_ctx *ctx;
    bb_scratch sc;
    size_t begin, end; /* instance range [begin, end) of the pass */
} bb_slice;

static int bb_cycle(void *arg)
{
    bb_slice *sl = arg;
    bb_ctx *ctx = sl->ctx;

    for (size_t i = sl->begin; i < sl->end; i++) {
        unsigned f = (unsigned)(i % BB_FRAMES);
        bb_result r;
        const bb_result *ref = &ctx->ref[f];

        if (bb_process(ctx->frame[f], ctx->model[ctx->arch[f]], &sl->sc,
                       &r) != 0)
            return -1;
        if (memcmp(r.mask_digest, ref->mask_digest, 20) != 0 ||
            memcmp(r.out_digest, ref->out_digest, 20) != 0 ||
            r.mask_ones != ref->mask_ones ||
            r.mask_ones == 0 || /* the subject must appear */
            r.changed != ref->changed ||
            r.changed == 0) /* the blur must do work */
            return -1;
    }
    return 0;
}

uint64_t bb_task_frames(unsigned multi)
{
    return multi ? BB_FRAMES_MULTI : BB_FRAMES;
}

int bb_run(bb_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_fps, uint64_t *frames)
{
    unsigned t_count = *threads;
    unsigned frame_set = t_count > 1 ? BB_FRAMES_MULTI : BB_FRAMES;
    bb_slice *slices;
    void **ctxs;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    int rc = -1;

    if (t_count > frame_set) /* one frame instance per thread at least */
        t_count = frame_set;
    if (t_count < 1)
        t_count = 1;

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;

    {
        size_t per = frame_set / t_count;
        unsigned extra = frame_set % t_count;
        size_t begin = 0;

        for (unsigned t = 0; t < t_count; t++) {
            size_t count = per + (t < extra ? 1u : 0u);

            slices[t].ctx = ctx;
            slices[t].begin = begin;
            slices[t].end = begin + count;
            begin += count;
            if (bb_scratch_init(&slices[t].sc) != 0)
                goto out;
            ctxs[t] = &slices[t];
        }
    }

    rc = harness_run(t_count, runs, bb_cycle, ctxs, elapsed);
    if (rc == 0) {
        uint64_t nframes = bb_task_frames(t_count > 1 ? 1u : 0u);

        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0
                           ? (double)nframes / elapsed[r]
                           : -1.0;
        *throughput_fps = lb_median(rates, runs);
        *threads = t_count;
        *frames = nframes;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++)
            bb_scratch_free(&slices[t].sc);
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors ---------------------------------------------------------------*/

const uint8_t *bb_frame(const bb_ctx *ctx, unsigned frame)
{
    return ctx && frame < BB_FRAMES ? ctx->frame[frame] : NULL;
}

size_t bb_frame_len(const bb_ctx *ctx, unsigned frame)
{
    return ctx && frame < BB_FRAMES ? BB_FRAME_BYTES : 0;
}

const bb_result *bb_reference(const bb_ctx *ctx, unsigned frame)
{
    return ctx && frame < BB_FRAMES ? &ctx->ref[frame] : NULL;
}

unsigned bb_archetype(const bb_ctx *ctx, unsigned frame)
{
    return ctx && frame < BB_FRAMES ? ctx->arch[frame] : 0;
}

bb_bbox bb_subject_bbox(const bb_ctx *ctx, unsigned frame)
{
    bb_bbox zero = { 0, 0, 0, 0 };

    if (!ctx || frame >= BB_FRAMES)
        return zero;
    return ctx->subj_box[frame];
}

int32_t bb_subject_tone(const bb_ctx *ctx, unsigned frame)
{
    if (!ctx || frame >= BB_FRAMES)
        return 0;
    return ctx->subj_tone[frame];
}

const uint8_t *bb_model(const bb_ctx *ctx, unsigned arch)
{
    return ctx && arch < BB_ARCHETYPES ? ctx->model[arch] : NULL;
}

const uint8_t *bb_model_digest(const bb_ctx *ctx)
{
    (void)ctx;
    return MODELS.digest; /* the models exist without a ctx */
}
