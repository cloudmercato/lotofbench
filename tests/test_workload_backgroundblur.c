/* Unit tests — workload #10: Background Blur. */

#include "tests.h"

#include "imgops.h"
#include "sha1.h"
#include "workload_backgroundblur.h"

#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.14.0, seed 1). After a
 * DELIBERATE change to the corpus, the models or the pipeline, re-run
 * the tool and port the printed values here in the same commit. */
#define BB_GOLDEN_MODELS_SHA1 "f9a45ff819791a76360984ef8ae96eb2f63f8b28"
#define BB_GOLDEN_FRAME0_SHA1 "f4493af27dfe7b911c4d5846648fdc656f2e422c"
static const uint8_t BB_GOLDEN_MASK[20] = {
    0x92, 0x8c, 0x22, 0xb4, 0x8c, 0xfe, 0x19, 0x38, 0x22, 0xc2,
    0xca, 0xd2, 0xf6, 0xac, 0x3c, 0x8a, 0xce, 0xdf, 0xc2, 0x98,
};
static const uint8_t BB_GOLDEN_OUT[20] = {
    0xab, 0xaf, 0x01, 0xd4, 0x9e, 0x91, 0xd1, 0x1f, 0x17, 0xf1,
    0x9d, 0x59, 0xd8, 0xcf, 0x8e, 0x19, 0xd7, 0xca, 0x28, 0x03,
};
#define BB_GOLDEN_MASK_ONES     185264u
#define BB_GOLDEN_CHANGED       1887497u
#define BB_GOLDEN_FRAMES        12ull
#define BB_GOLDEN_FRAMES_MULTI  48ull

/* One shared prepare for the read-only tests (the corpus is immutable;
 * the process exits right after the suite anyway). */
static bb_ctx *shared_bb(void)
{
    static bb_ctx *ctx;

    if (!ctx)
        ctx = bb_prepare(1);
    return ctx;
}

/* Rec.601 luma of one RGB pixel (integer, the imgops convention). */
static unsigned luma(const uint8_t *px)
{
    return (77 * px[0] + 150 * px[1] + 29 * px[2] + 128) >> 8;
}

/* Is one cell's 4x4 source block fully inside the subject rect? */
static int block_inside(bb_bbox b, unsigned cx, unsigned cy)
{
    return 4 * cx >= (unsigned)b.x0 && 4 * cx + 4 <= (unsigned)b.x1 &&
           4 * cy >= (unsigned)b.y0 && 4 * cy + 4 <= (unsigned)b.y1;
}

/* Is one cell's 4x4 source block fully outside the subject rect? */
static int block_outside(bb_bbox b, unsigned cx, unsigned cy)
{
    return 4 * cx + 4 <= (unsigned)b.x0 || 4 * cx >= (unsigned)b.x1 ||
           4 * cy + 4 <= (unsigned)b.y0 || 4 * cy >= (unsigned)b.y1;
}

/* --- determinism and corpus -------------------------------------------------- */

void test_bb_frames_deterministic(void)
{
    bb_ctx *a = bb_prepare(1), *b = bb_prepare(1);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        bb_free(a);
        bb_free(b);
        return;
    }
    for (unsigned f = 0; f < BB_FRAMES; f++) {
        CHECK_U64(bb_frame_len(a, f), bb_frame_len(b, f));
        sha1_hex(bb_frame(a, f), bb_frame_len(a, f), ha);
        sha1_hex(bb_frame(b, f), bb_frame_len(b, f), hb);
        CHECK_STR(ha, hb);
        CHECK(memcmp(bb_reference(a, f), bb_reference(b, f),
                     sizeof(bb_result)) == 0);
    }
    bb_free(a);
    bb_free(b);
}

void test_bb_input_invariants(void)
{
    bb_ctx *ctx = shared_bb();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned f = 0; f < BB_FRAMES; f++) {
        bb_bbox b = bb_subject_bbox(ctx, f);
        int32_t tone = bb_subject_tone(ctx, f);
        int32_t cy = (b.y0 + b.y1) / 2;
        const uint8_t *outp, *inn;
        int32_t step;

        CHECK_U64(bb_frame_len(ctx, f),
                  (size_t)BB_FRAME_W * BB_FRAME_H * 3);
        CHECK_U64(bb_archetype(ctx, f), f % 4); /* archetypes cycle */
        /* the subject is a 180-360 x 300-600 px rectangle, at least
         * 16 px inside the frame, darkened by 50-90 per channel */
        CHECK(b.x0 >= 16 && b.y0 >= 16);
        CHECK(b.x1 <= (int32_t)BB_FRAME_W - 16);
        CHECK(b.y1 <= (int32_t)BB_FRAME_H - 16);
        CHECK(b.x1 - b.x0 >= 180 && b.x1 - b.x0 <= 360);
        CHECK(b.y1 - b.y0 >= 300 && b.y1 - b.y0 <= 600);
        CHECK(tone <= -50 && tone >= -90);
        /* the planted step is real across the left edge: the luma
         * jumps by |tone| +- noise (sampled 2 px off the edge) */
        outp = bb_frame(ctx, f) +
               ((size_t)cy * BB_FRAME_W + (size_t)(b.x0 - 2)) * 3;
        inn = bb_frame(ctx, f) +
              ((size_t)cy * BB_FRAME_W + (size_t)(b.x0 + 2)) * 3;
        step = (int32_t)luma(inn) - (int32_t)luma(outp);
        if (step < 0)
            step = -step;
        CHECK(step >= 30);
        CHECK(bb_reference(ctx, f) != NULL);
    }
    /* the background models derive from the palette, not the seed */
    {
        bb_ctx *other = bb_prepare(2);
        uint8_t da[20], db[20];

        CHECK(other != NULL);
        if (!other) {
            bb_free(other);
            return;
        }
        memcpy(da, bb_model_digest(ctx), 20);
        memcpy(db, bb_model_digest(other), 20);
        CHECK(memcmp(da, db, 20) == 0); /* models do not move with seeds */
        for (unsigned a = 0; a < 4; a++)
            CHECK(bb_model(other, a) != NULL);
        bb_free(other);
    }
}

void test_bb_seed_variation(void)
{
    bb_ctx *a = bb_prepare(1), *b = bb_prepare(2);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        bb_free(a);
        bb_free(b);
        return;
    }
    sha1_hex(bb_frame(a, 0), bb_frame_len(a, 0), ha);
    sha1_hex(bb_frame(b, 0), bb_frame_len(b, 0), hb);
    CHECK(strcmp(ha, hb) != 0); /* different seed => different clip */
    CHECK(memcmp(bb_reference(a, 0), bb_reference(b, 0),
                 sizeof(bb_result)) != 0);
    bb_free(a);
    bb_free(b);
}

/* --- integer kernels ------------------------------------------------------------ */

void test_bb_segment_units(void)
{
    /* 10 cells; model flat 100; the tap moves one channel at a time
     * around the threshold (max per-channel absolute difference) */
    static const uint8_t TAP[30] = {
        100, 100, 100, /* d = 0  -> 0   */
        123, 100, 100, /* d = 23 -> 0   */
        124, 100, 100, /* d = 24 -> 255 */
        76,  100, 100, /* d = 24 below -> 255 */
        100, 130, 100, /* d = 30 green -> 255 */
        100, 100, 69,  /* d = 31 blue -> 255 */
        101, 99,  123, /* d = 23 (mixed signs) -> 0 */
        155, 45,  100, /* d = 55 -> 255 */
        100, 100, 100, /* d = 0 -> 0 */
        125, 0,   255, /* d = 100 -> 255 */
    };
    static const uint8_t WANT[10] = { 0, 0, 255, 255, 255,
                                      255, 0, 255, 0, 255 };
    uint8_t model[30], coarse[10];

    memset(model, 100, sizeof model);
    bb_segment(TAP, model, 10, coarse);
    for (unsigned i = 0; i < 10; i++)
        CHECK_U64(coarse[i], WANT[i]);
}

void test_bb_majority_units(void)
{
    /* all-zero plane is stable; an all-one plane keeps its interior
     * and its edges (6 ones) but the zero padding erodes the four
     * corners (4 ones) */
    {
        uint8_t in[25], out[25];

        memset(in, 0, sizeof in);
        bb_majority3(in, 5, 5, out);
        for (unsigned i = 0; i < 25; i++)
            CHECK_U64(out[i], 0);
        memset(in, 255, sizeof in);
        bb_majority3(in, 5, 5, out);
        for (unsigned y = 0; y < 5; y++)
            for (unsigned x = 0; x < 5; x++) {
                unsigned ones = 0;

                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if (x + dx < 5 && y + dy < 5)
                            ones++; /* every in-bounds cell is 255 */
                CHECK_U64(out[y * 5 + x], ones >= 5 ? 255 : 0);
            }
    }
    /* an isolated 1 dies, an isolated 0 fills (8 neighbours) */
    {
        uint8_t in[25], out[25];

        memset(in, 0, sizeof in);
        in[2 * 5 + 2] = 255;
        bb_majority3(in, 5, 5, out);
        CHECK_U64(out[2 * 5 + 2], 0);
        memset(in, 255, sizeof in);
        in[2 * 5 + 2] = 0;
        bb_majority3(in, 5, 5, out);
        CHECK_U64(out[2 * 5 + 2], 255);
    }
    /* a straight vertical edge stays put away from the frame border:
     * a cell with 3 subject neighbours keeps 0, its inside neighbour
     * keeps 255; the zero padding erodes the two end rows (4 ones) */
    {
        uint8_t in[25], out[25];

        memset(in, 0, sizeof in);
        for (unsigned y = 0; y < 5; y++) {
            in[y * 5 + 0] = 255;
            in[y * 5 + 1] = 255;
        }
        bb_majority3(in, 5, 5, out);
        for (unsigned y = 0; y < 5; y++) {
            CHECK_U64(out[y * 5 + 2], 0); /* 3 ones at worst */
            CHECK_U64(out[y * 5 + 1],
                      (y >= 1 && y <= 3) ? 255 : 0); /* 6 / 4 ones */
        }
    }
    /* the 4/5 boundary: a plus-shaped centre (4 ones) dies, adding one
     * diagonal (5 ones) survives */
    {
        uint8_t in[25], out[25];

        memset(in, 0, sizeof in);
        in[1 * 5 + 2] = 255;
        in[3 * 5 + 2] = 255;
        in[2 * 5 + 1] = 255;
        in[2 * 5 + 3] = 255;
        bb_majority3(in, 5, 5, out);
        CHECK_U64(out[2 * 5 + 2], 0); /* 4 ones */
        in[1 * 5 + 1] = 255;
        bb_majority3(in, 5, 5, out);
        CHECK_U64(out[2 * 5 + 2], 255); /* 5 ones */
    }
    /* zero padding erodes a corner: a 3x3 block touching the frame
     * corner loses its corner cell (4 in-bounds ones) but keeps the
     * edge cells (5-6 ones) */
    {
        uint8_t in[25], out[25];

        memset(in, 0, sizeof in);
        for (unsigned y = 0; y < 3; y++)
            for (unsigned x = 0; x < 3; x++)
                in[y * 5 + x] = 255;
        bb_majority3(in, 5, 5, out);
        CHECK_U64(out[0], 0);                   /* 4 ones */
        CHECK_U64(out[1 * 5 + 1], 255);         /* 9 ones */
        CHECK_U64(out[2 * 5 + 1], 255);         /* 6 ones */
        CHECK_U64(out[1 * 5 + 2], 255);         /* 6 ones */
        CHECK_U64(out[2 * 5 + 2], 0);           /* 4 ones */
    }
    /* degenerate 1x1 plane: a lone cell is never a majority (1 one,
     * 8 padding zeros) */
    {
        uint8_t in[1] = { 255 }, out[1];

        bb_majority3(in, 1, 1, out);
        CHECK_U64(out[0], 0);
        in[0] = 0;
        bb_majority3(in, 1, 1, out);
        CHECK_U64(out[0], 0);
    }
}

void test_bb_mask_upscale_units(void)
{
    /* constant plane stays constant */
    {
        uint8_t src[16], dst[64];

        memset(src, 77, sizeof src);
        bb_mask_upscale(dst, 16, 4, src, 4, 1);
        for (unsigned i = 0; i < 64; i++)
            CHECK_U64(dst[i], 77);
    }
    /* 1x1 source: every destination pixel is the source value */
    {
        uint8_t src[1] = { 42 }, dst[16];

        bb_mask_upscale(dst, 4, 4, src, 1, 1);
        for (unsigned i = 0; i < 16; i++)
            CHECK_U64(dst[i], 42);
    }
    /* x4 upscale of a 2x2 plane, hand-computed fixed-point values
     * (weights in eighths, one half-up rounding: (S + 32) >> 6) */
    {
        static const uint8_t SRC[4] = { 100, 200, 0, 50 };
        uint8_t dst[64];

        bb_mask_upscale(dst, 8, 8, SRC, 2, 2);
        /* px (0,0): weights (3,5)e x (3,5)e over {100,200;0,50}:
         * S = 100*9 + 200*15 + 0*15 + 50*25 = 5150 */
        CHECK_U64(dst[0], (5150u + 32u) >> 6);
        /* px (2,2): weights (7,1)e x (7,1)e:
         * S = 100*49 + 200*7 + 0*7 + 50*1 = 6350 */
        CHECK_U64(dst[2 * 8 + 2], (6350u + 32u) >> 6);
        /* px (4,4): same weights as (0,0) (period 4) */
        CHECK_U64(dst[4 * 8 + 4], (5150u + 32u) >> 6);
        /* px (7,7): both axes clamp onto source row/col 1 -> exactly
         * the source value */
        CHECK_U64(dst[7 * 8 + 7], 50);
    }
    /* an increasing ramp stays non-decreasing from the fourth sample
     * on; the left-edge clamp (the first three samples blend the
     * clamped border cells) breaks monotonicity for a steep ramp —
     * documented kernel behaviour, shared with img_scale_bilinear */
    {
        uint8_t src[8], dst[32];

        for (unsigned i = 0; i < 8; i++)
            src[i] = (uint8_t)(i * 30);
        bb_mask_upscale(dst, 32, 1, src, 8, 1);
        for (unsigned x = 3; x < 32; x++)
            CHECK(dst[x] >= dst[x - 1]);
    }
}

void test_bb_composite_units(void)
{
    /* mask 255 -> exactly the sharp pixel; mask 0 -> exactly the
     * blurred pixel */
    {
        static const uint8_t FRAME[6] = { 100, 100, 100, 255, 0, 128 };
        static const uint8_t BLUR[6] = { 200, 200, 200, 0, 255, 127 };
        static const uint8_t MASK[2] = { 255, 0 };
        uint8_t out[6];

        bb_composite(out, FRAME, BLUR, MASK, 2);
        CHECK_U64(out[0], 100);
        CHECK_U64(out[1], 100);
        CHECK_U64(out[2], 100);
        CHECK_U64(out[3], 0);
        CHECK_U64(out[4], 255);
        CHECK_U64(out[5], 127);
    }
    /* hand-computed midpoints: (m*f + (255-m)*b + 127) / 255 */
    {
        static const uint8_t FRAME[3] = { 100, 0, 255 };
        static const uint8_t BLUR[3] = { 200, 255, 0 };
        static const uint8_t MASK[1] = { 128 };
        uint8_t out[3];

        bb_composite(out, FRAME, BLUR, MASK, 1);
        CHECK_U64(out[0], (128u * 100 + 127u * 200 + 127u) / 255u);
        CHECK_U64(out[1], (128u * 0 + 127u * 255 + 127u) / 255u);
        CHECK_U64(out[2], (128u * 255 + 127u * 0 + 127u) / 255u);
    }
    /* one level off the ends: m = 254 and m = 1 */
    {
        static const uint8_t FRAME[6] = { 0, 255, 64, 0, 255, 64 };
        static const uint8_t BLUR[6] = { 255, 0, 200, 255, 0, 200 };
        static const uint8_t M254[1] = { 254 }, M1[1] = { 1 };
        uint8_t out[6];

        bb_composite(out, FRAME, BLUR, M254, 1);
        CHECK_U64(out[0], (254u * 0 + 1u * 255 + 127u) / 255u);
        CHECK_U64(out[1], (254u * 255 + 1u * 0 + 127u) / 255u);
        bb_composite(out + 3, FRAME + 3, BLUR + 3, M1, 1);
        CHECK_U64(out[5], (1u * 64 + 254u * 200 + 127u) / 255u);
    }
}

/* --- the differential test --------------------------------------------------- */

/* An independent naive pipeline, re-derived from the documented
 * contract with its own memory layout and its own arithmetic: block
 * means accumulated per channel straight from the frame (no downscale
 * buffer), an x4-specialized bilinear in eighths with a single
 * (S + 32) >> 6 rounding, and direct-window box blur passes instead
 * of the shared running-sum kernel. If any workload kernel drifts
 * (layout mixup, rounding change, wrong edge rule), the planes stop
 * matching; each plane is compared element by element with explicitly
 * derived indices, and only the mismatch count is CHECKed (one
 * diagnostic per plane instead of millions of lines). */

static uint32_t nv_diff_count(const uint8_t *a, const uint8_t *b,
                              size_t n, size_t *first)
{
    uint32_t bad = 0;

    *first = (size_t)-1;
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) {
            if (*first == (size_t)-1)
                *first = i;
            bad++;
        }
    return bad;
}

static uint8_t nv_seg_cell(const uint8_t *frame, const uint8_t *model,
                           unsigned cx, unsigned cy)
{
    size_t cell = (size_t)cy * BB_SEG_W + cx;

    for (int c = 0; c < 3; c++) {
        uint32_t sum = 0;
        int32_t d;

        for (unsigned j = 0; j < 4; j++)
            for (unsigned i = 0; i < 4; i++)
                sum += frame[((size_t)(4 * cy + j) * BB_FRAME_W +
                              4 * cx + i) * 3 + c];
        d = (int32_t)((sum + 8u) >> 4) - (int32_t)model[cell * 3 + c];
        if (d < 0)
            d = -d;
        if (d >= (int32_t)BB_SEG_THRESH)
            return 255;
    }
    return 0;
}

static void nv_majority(const uint8_t *in, size_t w, size_t h,
                        uint8_t *out)
{
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
            int ones = 0;

            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int64_t iy = (int64_t)y + dy;
                    int64_t ix = (int64_t)x + dx;

                    if (iy < 0 || ix < 0 ||
                        iy >= (int64_t)h || ix >= (int64_t)w)
                        continue;
                    ones += in[(size_t)iy * w + (size_t)ix] ? 1 : 0;
                }
            out[y * w + x] = ones >= 5 ? 255 : 0;
        }
}

/* weights (in eighths) of the two sampled source cells per k = x & 3 */
static const unsigned NV_W[4][2] = { { 3, 5 }, { 1, 7 }, { 7, 1 },
                                     { 5, 3 } };

static void nv_upscale4(uint8_t *dst, size_t dw, size_t dh,
                        const uint8_t *src, size_t sw, size_t sh)
{
    for (size_t y = 0; y < dh; y++) {
        size_t ry = y >> 2, ky = y & 3;
        int64_t ya0 = (int64_t)ry + (ky < 2 ? -1 : 0);
        size_t ya = ya0 < 0 ? 0 : (size_t)ya0;
        size_t yb = ya + 1 < sh ? ya + 1 : sh - 1;

        for (size_t x = 0; x < dw; x++) {
            size_t rx = x >> 2, kx = x & 3;
            int64_t xa0 = (int64_t)rx + (kx < 2 ? -1 : 0);
            size_t xa = xa0 < 0 ? 0 : (size_t)xa0;
            size_t xb = xa + 1 < sw ? xa + 1 : sw - 1;
            unsigned wxa = NV_W[kx][0], wxb = NV_W[kx][1];
            unsigned wya = NV_W[ky][0], wyb = NV_W[ky][1];
            uint32_t s =
                (uint32_t)src[ya * sw + xa] * wxa * wya +
                (uint32_t)src[ya * sw + xb] * wxb * wya +
                (uint32_t)src[yb * sw + xa] * wxa * wyb +
                (uint32_t)src[yb * sw + xb] * wxb * wyb;

            dst[y * dw + x] = (uint8_t)((s + 32u) >> 6);
        }
    }
}

/* one pass of a radius-r box blur over one compact plane with the
 * documented edge-clamped fixed window and half-up rounding; along
 * selects the window axis (0 = horizontal, 1 = vertical) */
static void nv_blur_pass(const uint8_t *in, uint8_t *out, size_t w,
                         size_t h, unsigned r, int along)
{
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
            uint32_t sum = 0;

            for (unsigned k = 0; k <= 2 * r; k++) {
                int64_t d = (int64_t)k - (int64_t)r;
                int64_t iy = along ? (int64_t)y + d : (int64_t)y;
                int64_t ix = along ? (int64_t)x : (int64_t)x + d;
                size_t jc = iy < 0 ? 0 : (size_t)iy;
                size_t ic = ix < 0 ? 0 : (size_t)ix;

                if (jc >= h)
                    jc = h - 1;
                if (ic >= w)
                    ic = w - 1;
                sum += in[jc * w + ic];
            }
            out[y * w + x] =
                (uint8_t)((sum + (2u * r + 1u) / 2u) / (2u * r + 1u));
        }
}

static void nv_blur_rgb(uint8_t *img, size_t w, size_t h, unsigned r,
                        uint8_t *chan, uint8_t *tmp)
{
    for (int c = 0; c < 3; c++) {
        /* de-interleave one channel into a compact plane */
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++)
                chan[y * w + x] = img[(y * w + x) * 3 + c];
        nv_blur_pass(chan, tmp, w, h, r, 0);
        nv_blur_pass(tmp, chan, w, h, r, 1);
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++)
                img[(y * w + x) * 3 + c] = chan[y * w + x];
    }
}

void test_bb_differential_naive(void)
{
    bb_ctx *ctx = shared_bb();
    bb_scratch sc;
    uint8_t *coarse, *refined, *mask, *blurred, *chan, *tmp;
    size_t first;

    CHECK(ctx);
    if (!ctx)
        return;
    memset(&sc, 0, sizeof sc);
    coarse = malloc(BB_SEG_W * BB_SEG_H);
    refined = malloc(BB_SEG_W * BB_SEG_H);
    mask = malloc((size_t)BB_FRAME_W * BB_FRAME_H);
    blurred = malloc(BB_FRAME_W * BB_FRAME_H * 3);
    chan = malloc((size_t)BB_FRAME_W * BB_FRAME_H);
    tmp = malloc((size_t)BB_FRAME_W * BB_FRAME_H);
    CHECK(coarse && refined && mask && blurred && chan && tmp);
    if (!coarse || !refined || !mask || !blurred || !chan || !tmp)
        goto out;

    CHECK(bb_scratch_init(&sc) == 0);

    /* three frames = three archetypes, three subject placements */
    for (unsigned f = 0; f < 3; f++) {
        const uint8_t *model = bb_model(ctx, bb_archetype(ctx, f));
        const uint8_t *frame = bb_frame(ctx, f);
        bb_result r;
        const bb_result *ref = bb_reference(ctx, f);
        uint32_t bad;
        size_t ones = 0, changed = 0;

        CHECK(model != NULL && frame != NULL);
        CHECK(bb_process(frame, model, &sc, &r) == 0);

        /* naive segmentation straight from the frame */
        for (unsigned cy = 0; cy < BB_SEG_H; cy++)
            for (unsigned cx = 0; cx < BB_SEG_W; cx++)
                coarse[(size_t)cy * BB_SEG_W + cx] =
                    nv_seg_cell(frame, model, cx, cy);
        bad = nv_diff_count(coarse, sc.coarse, BB_SEG_W * BB_SEG_H,
                            &first);
        CHECK_U64(bad, 0);
        if (bad)
            test_fail(__FILE__, __LINE__,
                      "coarse[%zu] diverges on frame %u", first, f);
        nv_majority(coarse, BB_SEG_W, BB_SEG_H, refined);
        bad = nv_diff_count(refined, sc.refined, BB_SEG_W * BB_SEG_H,
                            &first);
        CHECK_U64(bad, 0);
        if (bad)
            test_fail(__FILE__, __LINE__,
                      "refined[%zu] diverges on frame %u", first, f);
        nv_upscale4(mask, BB_FRAME_W, BB_FRAME_H, refined, BB_SEG_W,
                    BB_SEG_H);
        bad = nv_diff_count(mask, sc.mask,
                            (size_t)BB_FRAME_W * BB_FRAME_H, &first);
        CHECK_U64(bad, 0);
        if (bad)
            test_fail(__FILE__, __LINE__,
                      "mask[%zu] diverges on frame %u", first, f);

        /* naive blur + composite */
        memcpy(blurred, frame, BB_FRAME_W * BB_FRAME_H * 3);
        nv_blur_rgb(blurred, BB_FRAME_W, BB_FRAME_H, BB_BLUR_RADIUS,
                    chan, tmp);
        for (size_t p = 0; p < (size_t)BB_FRAME_W * BB_FRAME_H; p++)
            for (int c = 0; c < 3; c++)
                blurred[p * 3 + c] =
                    (uint8_t)(((uint32_t)mask[p] * frame[p * 3 + c] +
                               (255u - mask[p]) * blurred[p * 3 + c] +
                               127u) / 255u);
        bad = nv_diff_count(blurred, sc.blurred,
                            (size_t)BB_FRAME_W * BB_FRAME_H * 3,
                            &first);
        CHECK_U64(bad, 0);
        if (bad)
            test_fail(__FILE__, __LINE__,
                      "out[%zu] diverges on frame %u", first, f);

        /* degeneracy guard: the mask has a foreground plateau and the
         * composite changed a real share of the pixels but preserved
         * the subject (changed < every pixel) */
        for (size_t p = 0; p < (size_t)BB_FRAME_W * BB_FRAME_H; p++) {
            if (sc.mask[p] == 255)
                ones++;
            if (sc.blurred[p * 3] != frame[p * 3] ||
                sc.blurred[p * 3 + 1] != frame[p * 3 + 1] ||
                sc.blurred[p * 3 + 2] != frame[p * 3 + 2])
                changed++;
        }
        CHECK_U64(ones, r.mask_ones);
        CHECK_U64(changed, r.changed);
        CHECK(ones > 0);
        CHECK(changed > 0);
        CHECK(changed < (size_t)BB_FRAME_W * BB_FRAME_H);
        CHECK(changed * 4 > (size_t)BB_FRAME_W * BB_FRAME_H);
        CHECK_U64(r.mask_ones, ref->mask_ones);
        CHECK_U64(r.changed, ref->changed);
    }

out:
    bb_scratch_free(&sc);
    free(coarse);
    free(refined);
    free(mask);
    free(blurred);
    free(chan);
    free(tmp);
}

/* --- ground truth ------------------------------------------------------------- */

void test_bb_mask_ground_truth(void)
{
    bb_ctx *ctx = shared_bb();
    bb_scratch sc;
    uint8_t *blur_ref;
    uint32_t in_bad = 0, out_bad = 0, zero_cells = 0, one_cells = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    memset(&sc, 0, sizeof sc);
    blur_ref = malloc(BB_FRAME_W * BB_FRAME_H * 3);
    CHECK(blur_ref != NULL);
    if (!blur_ref)
        return;
    CHECK(bb_scratch_init(&sc) == 0);

    for (unsigned f = 0; f < BB_FRAMES; f++) {
        const uint8_t *model = bb_model(ctx, bb_archetype(ctx, f));
        const uint8_t *frame = bb_frame(ctx, f);
        bb_bbox b = bb_subject_bbox(ctx, f);
        bb_result r;

        CHECK(model != NULL && frame != NULL);
        CHECK(bb_process(frame, model, &sc, &r) == 0);

        /* every pure-subject tap cell segments to 255 (the documented
         * noise/threshold margins) and every pure-background cell to
         * 0; cells whose whole 3x3 neighbourhood is pure subject keep
         * 255 through the majority filter, cells whose whole
         * neighbourhood is pure background keep 0 — the plateau's
         * outermost corner cells may erode, that is the filter's
         * documented behaviour */
        for (unsigned cy = 0; cy < BB_SEG_H; cy++)
            for (unsigned cx = 0; cx < BB_SEG_W; cx++) {
                size_t i = (size_t)cy * BB_SEG_W + cx;
                int all_in = 1, all_out = 1;

                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int64_t nx = (int64_t)cx + dx;
                        int64_t ny = (int64_t)cy + dy;

                        if (nx < 0 || ny < 0 ||
                            nx >= (int64_t)BB_SEG_W ||
                            ny >= (int64_t)BB_SEG_H)
                            continue;
                        if (!block_inside(b, (unsigned)nx,
                                          (unsigned)ny))
                            all_in = 0;
                        if (!block_outside(b, (unsigned)nx,
                                           (unsigned)ny))
                            all_out = 0;
                    }
                if (block_inside(b, cx, cy) && sc.coarse[i] != 255)
                    in_bad++;
                if (all_in && sc.refined[i] != 255)
                    in_bad++;
                if (block_outside(b, cx, cy) && sc.coarse[i] != 0)
                    in_bad++;
                if (all_out && sc.refined[i] != 0)
                    in_bad++;
            }

        /* the upsampled mask: 255 across the subject eroded by 24 px,
         * 0 at least 25 px away from it; the composite: the subject
         * interior is bit-identical to the input (sharp), the far
         * background exactly the blurred frame */
        memcpy(blur_ref, frame, BB_FRAME_W * BB_FRAME_H * 3);
        img_box_blur(blur_ref, BB_FRAME_W, BB_FRAME_H, BB_BLUR_RADIUS,
                     sc.blur_sc);
        for (int32_t y = 0; y < (int32_t)BB_FRAME_H; y++)
            for (int32_t x = 0; x < (int32_t)BB_FRAME_W; x++) {
                size_t p = (size_t)y * BB_FRAME_W + (size_t)x;
                uint8_t m = sc.mask[p];
                int deep_in = x >= b.x0 + 24 && x < b.x1 - 24 &&
                              y >= b.y0 + 24 && y < b.y1 - 24;
                int far_out = x <= b.x0 - 25 || x >= b.x1 + 25 ||
                              y <= b.y0 - 25 || y >= b.y1 + 25;

                if (deep_in) {
                    one_cells++;
                    if (m != 255 ||
                        sc.blurred[p * 3] != frame[p * 3] ||
                        sc.blurred[p * 3 + 1] != frame[p * 3 + 1] ||
                        sc.blurred[p * 3 + 2] != frame[p * 3 + 2])
                        out_bad++;
                } else if (far_out) {
                    zero_cells++;
                    if (m != 0 ||
                        sc.blurred[p * 3] != blur_ref[p * 3] ||
                        sc.blurred[p * 3 + 1] != blur_ref[p * 3 + 1] ||
                        sc.blurred[p * 3 + 2] != blur_ref[p * 3 + 2])
                        out_bad++;
                }
            }

        /* structural guards: the mask has a foreground plateau and the
         * blur changed a real share of the pixels */
        CHECK_U64(r.mask_ones, bb_reference(ctx, f)->mask_ones);
        CHECK_U64(r.changed, bb_reference(ctx, f)->changed);
        CHECK(r.mask_ones > 0);
        CHECK(r.changed > 0);
        CHECK(r.changed < (uint32_t)BB_FRAME_W * BB_FRAME_H);
        CHECK(r.changed * 4u > (uint32_t)BB_FRAME_W * BB_FRAME_H);
    }

    /* the asserted regions were actually exercised (lower bounds far
     * below the geometric minimum: the eroded subject band is at
     * least ~400K px across the clip, the far background ~17M) */
    CHECK(one_cells > 100000);
    CHECK(zero_cells > 10000000);
    CHECK_U64(in_bad, 0);
    CHECK_U64(out_bad, 0);

    bb_scratch_free(&sc);
    free(blur_ref);
}

/* --- golden pin, run sanity, clamping, accessors ------------------------------ */

void test_bb_corpus_golden(void)
{
    bb_ctx *ctx = shared_bb();
    char hex[41];
    const bb_result *r;
    uint8_t md[20];

    CHECK(ctx);
    if (!ctx)
        return;
    memcpy(md, bb_model_digest(ctx), 20);
    sha1_hex(md, 20, hex);
    CHECK_STR(hex, BB_GOLDEN_MODELS_SHA1);
    sha1_hex(bb_frame(ctx, 0), bb_frame_len(ctx, 0), hex);
    CHECK_STR(hex, BB_GOLDEN_FRAME0_SHA1);
    r = bb_reference(ctx, 0);
    CHECK(r != NULL);
    if (!r)
        return;
    CHECK(memcmp(r->mask_digest, BB_GOLDEN_MASK, 20) == 0);
    CHECK(memcmp(r->out_digest, BB_GOLDEN_OUT, 20) == 0);
    CHECK_U64(r->mask_ones, BB_GOLDEN_MASK_ONES);
    CHECK_U64(r->changed, BB_GOLDEN_CHANGED);
    CHECK_U64(bb_task_frames(0), BB_GOLDEN_FRAMES);
    CHECK_U64(bb_task_frames(1), BB_GOLDEN_FRAMES_MULTI);
}

void test_bb_run_sane(void)
{
    bb_ctx *ctx = shared_bb();
    unsigned threads = 1;
    double fps = 0.0;
    uint64_t frames = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(bb_run(ctx, 1, &threads, &fps, &frames) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(frames, BB_FRAMES);
    CHECK(fps > 0.0);
}

void test_bb_thread_clamp(void)
{
    bb_ctx *ctx = shared_bb();
    unsigned threads = 999;
    double fps = 0.0;
    uint64_t frames = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* more threads than frames: clamped to the 48-frame multi set */
    CHECK(bb_run(ctx, 1, &threads, &fps, &frames) == 0);
    CHECK_U64(threads, BB_FRAMES_MULTI);
    CHECK_U64(frames, BB_FRAMES_MULTI);
}

void test_bb_accessors_null_safe(void)
{
    bb_ctx *ready = shared_bb(); /* build the models before the checks */
    bb_bbox zero = { 0, 0, 0, 0 };

    CHECK(ready != NULL);
    CHECK(bb_frame(NULL, 0) == NULL);
    CHECK_U64(bb_frame_len(NULL, 0), 0);
    CHECK(bb_reference(NULL, 0) == NULL);
    CHECK_U64(bb_archetype(NULL, 0), 0);
    CHECK(bb_subject_bbox(NULL, 0).x1 == zero.x1);
    CHECK_U64(bb_subject_tone(NULL, 0), 0);
    CHECK(bb_model(NULL, 0) == NULL);
    CHECK(bb_model_digest(NULL) != NULL); /* models exist without a ctx */

    CHECK(bb_frame(ready, BB_FRAMES) == NULL);
    CHECK_U64(bb_frame_len(ready, BB_FRAMES), 0);
    CHECK(bb_reference(ready, BB_FRAMES) == NULL);
    CHECK_U64(bb_subject_tone(ready, BB_FRAMES), 0);
    CHECK(bb_subject_bbox(ready, BB_FRAMES).x1 == 0);
    CHECK(bb_model(ready, 4) == NULL);
}
