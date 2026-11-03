/* LotofBench — Photo Filter workload (workload #13).
 *
 * Mirrors the thirteenth Geekbench 6 CPU workload: a stack of color and
 * blur filters, levels, crop and scale over a set of photos. LotofBench
 * reproduces the shape dependency-free, with the same honesty rules as
 * the other workloads:
 *
 *   - deterministic synthetic input — a seeded set of 10 photos
 *     (1600x1200 RGB) in six archetype families (sunset landscape,
 *     studio portrait, night street, product shot, food close-up,
 *     scanned document) that cycle the way a real import session does.
 *     Each photo is a vertical gradient, a banding texture, seeded
 *     shapes and per-pixel noise, all integer arithmetic — fixed
 *     --seed means byte-identical corpora on every machine. The
 *     photos are generated off the clock — the stand-in for the
 *     camera import;
 *   - a fixed filter pipeline per photo — saturation (Rec.601 luma,
 *     8.8 fixed point), a black/white-point levels stretch, a two-pass
 *     box blur (Gaussian approximation, radius 2), a central 7/8 crop
 *     and a bilinear rescale back to the full frame — every stage from
 *     src/imgops.c, the shared deterministic image kernels. No
 *     floating point anywhere, so digests are machine-independent;
 *   - correctness inside the timed cycle — a pass only counts if every
 *     photo's filtered output reproduces its prepare-time reference
 *     SHA-1 digest; any divergence aborts the run instead of scoring;
 *   - shared-task multi-core through the common harness — the
 *     multi-core pass filters 40 photos (4x the set) split into equal
 *     contiguous slices, one per thread.
 *
 * The throughput metric is output pixels: every photo contributes its
 * full 1600x1200 output frame, exact and machine-independent, the
 * photo-workload analogue of the corpus size.
 *
 * Deliberate divergences from Geekbench 6 (all documented in
 * docs/benchmarks.md): synthetic photos instead of real image files,
 * box-blur Gaussian approximation instead of the exact kernel, and a
 * documented fixed pipeline instead of GB6's per-photo filter choices.
 */

#include "workload_photofilter.h"

#include "harness.h"
#include "imgops.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <stdlib.h>
#include <string.h>

#define PHOTO_BYTES ((size_t)PF_PHOTO_W * PF_PHOTO_H * 3)

/* --- exact integer square root (bit-by-bit, machine-independent) ------- */

static uint32_t isqrt64(uint64_t v)
{
    uint64_t res = 0;
    uint64_t bit = 1ull << 62;

    while (bit > v)
        bit >>= 2;
    while (bit) {
        uint64_t cand = res + bit;

        if (v >= cand) {
            v -= cand;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

/* --- corpus generation --------------------------------------------------*/

/* Archetype palettes: {gradient top, gradient bottom, accent}, each
 * RGB, plus the noise amplitude (0..255 scale). Six families that
 * cycle the way a real import session does. */
typedef struct {
    uint8_t top[3], bottom[3], accent[3];
    unsigned noise_amp;
    const char *name;
} pf_scene;

static const pf_scene ARCHETYPES[6] = {
    { { 252, 176, 64 }, { 84, 42, 24 }, { 250, 240, 220 }, 10, "sunset" },
    { { 235, 225, 215 }, { 120, 90, 70 }, { 190, 120, 90 }, 6, "portrait" },
    { { 20, 24, 40 }, { 4, 6, 12 }, { 250, 210, 120 }, 14, "night" },
    { { 244, 244, 248 }, { 200, 205, 215 }, { 60, 120, 200 }, 4, "product" },
    { { 255, 244, 214 }, { 190, 120, 60 }, { 200, 60, 40 }, 8, "food" },
    { { 250, 250, 245 }, { 225, 222, 210 }, { 70, 70, 80 }, 5, "document" },
};

struct pf_ctx {
    uint8_t *photo[PF_PHOTOS];
    uint8_t  ref_digest[PF_PHOTOS][20];
    int32_t  sat[PF_PHOTOS];    /* 8.8 fixed point, 256 = identity */
    unsigned black[PF_PHOTOS], white[PF_PHOTOS];
    unsigned arch[PF_PHOTOS];
    size_t   crop_x, crop_y, crop_w, crop_h;
};

static inline uint8_t clamp_u8(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static void gen_photo(uint8_t *img, unsigned arch, rng_t *rng)
{
    const pf_scene *a = &ARCHETYPES[arch % 6];
    unsigned n_shapes = 3 + rng_below(rng, 5);

    /* gradient + banding texture */
    for (size_t y = 0; y < PF_PHOTO_H; y++) {
        uint32_t t = (uint32_t)(y * 255u / (PF_PHOTO_H - 1));
        uint8_t *row = img + y * PF_PHOTO_W * 3;

        for (size_t x = 0; x < PF_PHOTO_W; x++) {
            int32_t band = (int32_t)((x * x / 97u + y * y / 61u) % 17u) - 8;

            for (int c = 0; c < 3; c++) {
                int32_t base = (int32_t)a->top[c] +
                    (((int32_t)a->bottom[c] - (int32_t)a->top[c]) *
                     (int32_t)t + 127) / 255;

                row[x * 3 + c] = clamp_u8(base + band);
            }
        }
    }

    /* seeded shapes: alternating disks and rectangles in the accent
     * color (lightness-varied), the photo's "subjects" */
    for (unsigned s = 0; s < n_shapes; s++) {
        uint32_t cx = rng_below(rng, PF_PHOTO_W);
        uint32_t cy = rng_below(rng, PF_PHOTO_H);
        uint32_t rad = 40 + rng_below(rng, 240);
        int32_t tone = (int32_t)rng_below(rng, 81) - 40; /* -40..40 */
        uint8_t col[3];
        int rect = (s & 1u) != 0;

        for (int c = 0; c < 3; c++)
            col[c] = clamp_u8((int32_t)a->accent[c] + tone);
        if (rect) {
            uint32_t x0 = cx > rad ? cx - rad : 0;
            uint32_t x1 = cx + rad < PF_PHOTO_W ? cx + rad : PF_PHOTO_W;
            uint32_t y0 = cy > rad ? cy - rad : 0;
            uint32_t y1 = cy + rad < PF_PHOTO_H ? cy + rad : PF_PHOTO_H;

            for (uint32_t y = y0; y < y1; y++)
                for (uint32_t x = x0; x < x1; x++)
                    for (int c = 0; c < 3; c++)
                        img[(y * PF_PHOTO_W + x) * 3 + c] = col[c];
        } else {
            uint32_t y0 = cy > rad ? cy - rad : 0;
            uint32_t y1 = cy + rad < PF_PHOTO_H ? cy + rad : PF_PHOTO_H;

            for (uint32_t y = y0; y < y1; y++) {
                int64_t dy = (int64_t)y - cy;
                uint32_t half = isqrt64((uint64_t)rad * rad -
                                        (uint64_t)(dy * dy));
                uint32_t x0 = cx > half ? cx - half : 0;
                uint32_t x1 = cx + half < PF_PHOTO_W ? cx + half
                                                     : PF_PHOTO_W;

                for (uint32_t x = x0; x < x1; x++)
                    for (int c = 0; c < 3; c++)
                        img[(y * PF_PHOTO_W + x) * 3 + c] = col[c];
            }
        }
    }

    /* per-pixel noise, amplitude per archetype — one rng draw per
     * pixel, low three bytes become the channel deltas */
    for (size_t y = 0; y < PF_PHOTO_H; y++) {
        uint8_t *row = img + y * PF_PHOTO_W * 3;

        for (size_t x = 0; x < PF_PHOTO_W; x++) {
            uint64_t n = rng_next(rng);
            int32_t amp = (int32_t)a->noise_amp;

            for (int c = 0; c < 3; c++) {
                int32_t d = (int32_t)((n >> (c * 8)) & 0xFFu);

                d = (d * (2 * amp + 1) >> 8) - amp;
                row[x * 3 + c] = clamp_u8((int32_t)row[x * 3 + c] + d);
            }
        }
    }
}

/* The fixed filter pipeline, shared by the reference pass (prepare)
 * and the timed cycle. Operates in place on work, using blur_tmp and
 * crop_buf as scratch. */
static void filter_pipeline(pf_ctx *ctx, uint8_t *work, uint8_t *blur_tmp,
                            uint8_t *crop_buf, unsigned p)
{
    img_saturate(work, PF_PHOTO_W, PF_PHOTO_H, ctx->sat[p]);
    img_levels(work, PF_PHOTO_W, PF_PHOTO_H, ctx->black[p], ctx->white[p]);
    img_box_blur(work, PF_PHOTO_W, PF_PHOTO_H, PF_BLUR_RADIUS, blur_tmp);
    img_crop(crop_buf, ctx->crop_w, work, PF_PHOTO_W, PF_PHOTO_H,
             ctx->crop_x, ctx->crop_y, ctx->crop_w, ctx->crop_h);
    img_scale_bilinear(work, PF_PHOTO_W, PF_PHOTO_H, crop_buf,
                       ctx->crop_w, ctx->crop_h);
}

pf_ctx *pf_prepare(unsigned seed)
{
    pf_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t rng;

    if (!ctx)
        return NULL;

    /* central 7/8 crop: offset 1/16 of each dimension on each side */
    ctx->crop_w = (size_t)PF_PHOTO_W * PF_CROP_NUM / PF_CROP_DEN;
    ctx->crop_h = (size_t)PF_PHOTO_H * PF_CROP_NUM / PF_CROP_DEN;
    ctx->crop_x = ((size_t)PF_PHOTO_W - ctx->crop_w) / 2;
    ctx->crop_y = ((size_t)PF_PHOTO_H - ctx->crop_h) / 2;

    rng_init(&rng, seed);
    for (unsigned p = 0; p < PF_PHOTOS; p++) {
        unsigned arch = p % 6;

        ctx->photo[p] = malloc(PHOTO_BYTES);
        if (!ctx->photo[p]) {
            pf_free(ctx);
            return NULL;
        }
        ctx->arch[p] = arch;
        gen_photo(ctx->photo[p], arch, &rng);

        /* per-photo filter parameters, drawn from the same stream so
         * the whole corpus stays a pure function of the seed */
        ctx->sat[p] = 180 + (int32_t)rng_below(&rng, 151); /* 180..330 */
        ctx->black[p] = rng_below(&rng, 41);               /* 0..40 */
        ctx->white[p] = 255 - rng_below(&rng, 41);         /* 215..255 */

        /* reference: one full pipeline pass, digest the output */
        {
            uint8_t *work = malloc(PHOTO_BYTES);
            uint8_t *blur_tmp = malloc(PHOTO_BYTES);
            uint8_t *crop_buf =
                malloc(ctx->crop_w * ctx->crop_h * 3);

            if (!work || !blur_tmp || !crop_buf) {
                free(work);
                free(blur_tmp);
                free(crop_buf);
                pf_free(ctx);
                return NULL;
            }
            memcpy(work, ctx->photo[p], PHOTO_BYTES);
            filter_pipeline(ctx, work, blur_tmp, crop_buf, p);
            sha1(work, PHOTO_BYTES, ctx->ref_digest[p]);
            free(work);
            free(blur_tmp);
            free(crop_buf);
        }
    }
    return ctx;
}

void pf_free(pf_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned p = 0; p < PF_PHOTOS; p++)
        free(ctx->photo[p]);
    free(ctx);
}

/* --- run -----------------------------------------------------------------*/

/* One thread's slice: contiguous photo instances, own scratch —
 * threads never share writable state (shared-task harness contract). */
typedef struct {
    pf_ctx *ctx;
    uint8_t *work, *blur_tmp, *crop_buf;
    size_t begin, end; /* instance range [begin, end) of the pass */
} pf_slice;

static int pf_cycle(void *arg)
{
    pf_slice *sl = arg;
    pf_ctx *ctx = sl->ctx;
    uint8_t dig[20];

    for (size_t i = sl->begin; i < sl->end; i++) {
        unsigned p = (unsigned)(i % PF_PHOTOS);

        memcpy(sl->work, ctx->photo[p], PHOTO_BYTES);
        filter_pipeline(ctx, sl->work, sl->blur_tmp, sl->crop_buf, p);
        sha1(sl->work, PHOTO_BYTES, dig);
        if (memcmp(dig, ctx->ref_digest[p], 20) != 0)
            return -1;
    }
    return 0;
}

uint64_t pf_task_pixels(unsigned multi)
{
    unsigned set = multi ? PF_PHOTOS_MULTI : PF_PHOTOS;

    return (uint64_t)set * PF_PHOTO_W * PF_PHOTO_H;
}

int pf_run(pf_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mpixels, uint64_t *pixels)
{
    unsigned t_count = *threads;
    unsigned photo_set = t_count > 1 ? PF_PHOTOS_MULTI : PF_PHOTOS;
    pf_slice *slices;
    void **ctxs;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    int rc = -1;

    if (t_count > photo_set) /* one photo instance per thread at least */
        t_count = photo_set;
    if (t_count < 1)
        t_count = 1;

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;

    {
        size_t per = photo_set / t_count;
        unsigned extra = photo_set % t_count;
        size_t begin = 0;

        for (unsigned t = 0; t < t_count; t++) {
            size_t count = per + (t < extra ? 1u : 0u);

            slices[t].ctx = ctx;
            slices[t].begin = begin;
            slices[t].end = begin + count;
            begin += count;
            slices[t].work = malloc(PHOTO_BYTES);
            slices[t].blur_tmp = malloc(PHOTO_BYTES);
            slices[t].crop_buf =
                malloc(ctx->crop_w * ctx->crop_h * 3);
            if (!slices[t].work || !slices[t].blur_tmp ||
                !slices[t].crop_buf)
                goto out;
            ctxs[t] = &slices[t];
        }
    }

    rc = harness_run(t_count, runs, pf_cycle, ctxs, elapsed);
    if (rc == 0) {
        uint64_t px = (uint64_t)photo_set * PF_PHOTO_W * PF_PHOTO_H;

        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0
                           ? (double)px / elapsed[r] / 1e6
                           : -1.0;
        *throughput_mpixels = lb_median(rates, runs);
        *threads = t_count;
        *pixels = px;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++) {
            free(slices[t].work);
            free(slices[t].blur_tmp);
            free(slices[t].crop_buf);
        }
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors -----------------------------------------------------------*/

const uint8_t *pf_photo(const pf_ctx *ctx, unsigned photo)
{
    return ctx && photo < PF_PHOTOS ? ctx->photo[photo] : NULL;
}

size_t pf_photo_len(const pf_ctx *ctx, unsigned photo)
{
    return ctx && photo < PF_PHOTOS ? PHOTO_BYTES : 0;
}

const uint8_t *pf_reference_digest(const pf_ctx *ctx, unsigned photo)
{
    return ctx && photo < PF_PHOTOS ? ctx->ref_digest[photo] : NULL;
}

int32_t pf_photo_saturation(const pf_ctx *ctx, unsigned photo)
{
    return ctx && photo < PF_PHOTOS ? ctx->sat[photo] : 0;
}

unsigned pf_photo_black(const pf_ctx *ctx, unsigned photo)
{
    return ctx && photo < PF_PHOTOS ? ctx->black[photo] : 0;
}

unsigned pf_photo_white(const pf_ctx *ctx, unsigned photo)
{
    return ctx && photo < PF_PHOTOS ? ctx->white[photo] : 0;
}

unsigned pf_archetype(const pf_ctx *ctx, unsigned photo)
{
    return ctx && photo < PF_PHOTOS ? ctx->arch[photo] : 0;
}
