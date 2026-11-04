 /* Unit tests — workload #14: HDR (exposure fusion). */

#include "tests.h"

#include "rng.h"
#include "workload_hdr.h"

#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.15.0, seed 1). After a
 * DELIBERATE change to the corpus, the exposure mapping or the fusion
 * pipeline, re-run the tool and port the printed values here in the
 * same commit. */
static const uint8_t HDR_GOLDEN_WEIGHTS[4][20] = {
{
    0x6c, 0x00, 0xab, 0xcf, 0x49,
    0x51, 0x1c, 0x28, 0xc5, 0x6a,
    0xfb, 0xb7, 0xe2, 0x42, 0xe7,
    0xa1, 0x3b, 0x0a, 0x97, 0x83
},
{
    0x0c, 0xb9, 0x2b, 0xe7, 0x3c,
    0xa9, 0x1a, 0x81, 0xd8, 0xe6,
    0x60, 0x02, 0xf6, 0xbf, 0x4e,
    0x1d, 0x1c, 0x78, 0x99, 0xaf
},
{
    0x0d, 0xbc, 0xc4, 0x74, 0x60,
    0x39, 0xb0, 0xfb, 0x7a, 0x63,
    0x60, 0x70, 0x05, 0xfc, 0xaf,
    0x90, 0x64, 0x3d, 0xe1, 0x62
},
{
    0x12, 0xc9, 0x36, 0x4f, 0x99,
    0x94, 0xbd, 0x65, 0x03, 0xd5,
    0xb6, 0x50, 0x39, 0x36, 0x6d,
    0xa3, 0x70, 0xe8, 0xa0, 0x21
}
};

static const uint8_t HDR_GOLDEN_FUSED[4][20] = {
{
    0x5d, 0x95, 0x38, 0xce, 0xb7,
    0xd0, 0xf7, 0xfa, 0x27, 0xfa,
    0xe9, 0xd7, 0xc9, 0xef, 0x1d,
    0x4e, 0x7a, 0xf0, 0xe9, 0x98
},
{
    0x57, 0x4e, 0x67, 0x62, 0xc0,
    0x6e, 0xf3, 0x0e, 0xdf, 0xb9,
    0x36, 0x7d, 0xc9, 0x8d, 0xe2,
    0x2e, 0x97, 0x21, 0x4c, 0xe2
},
{
    0x7b, 0x8f, 0xe7, 0x64, 0xe6,
    0x3d, 0x08, 0xa2, 0x9a, 0xc1,
    0x40, 0x5c, 0x31, 0xa6, 0xeb,
    0x9b, 0x75, 0x66, 0xca, 0x8e
},
{
    0x79, 0x35, 0xed, 0xb5, 0xa3,
    0xbd, 0xc1, 0x20, 0x1c, 0xc1,
    0x18, 0x44, 0x9f, 0xad, 0x9f,
    0xfc, 0x38, 0xfa, 0x01, 0x0b
}
};


#define HDR_GOLDEN_HI_CLIP_0  23891u
#define HDR_GOLDEN_HI_CLIP_1      0u
#define HDR_GOLDEN_HI_CLIP_2   6417u
#define HDR_GOLDEN_HI_CLIP_3   7533u
#define HDR_GOLDEN_TASK_PIXELS       7680000ull
#define HDR_GOLDEN_TASK_PIXELS_MULTI 30720000ull

/* One shared prepare for the read-only tests (the corpus is immutable;
 * the process exits right after the suite anyway). */
static hdr_ctx *shared_hdr(void)
{
    static hdr_ctx *ctx;

    if (!ctx)
        ctx = hdr_prepare(1);
    return ctx;
}

/* Rec.601 luma of one RGB pixel (integer, the imgops convention). */
static unsigned luma(const uint8_t *px)
{
    return (77 * px[0] + 150 * px[1] + 29 * px[2] + 128) >> 8;
}

/* --- the documented weight formulas, re-derived ------------------------ */

/* Well-exposedness: 255 * (1 - sum_c (v - 128)^2 / (3 * 128^2)),
 * half-up — the linear decay from the midtone peak. */
static unsigned nv_well(unsigned r, unsigned g, unsigned b)
{
    const unsigned v[3] = { r, g, b };
    unsigned sq = 0;

    for (int c = 0; c < 3; c++) {
        unsigned d = v[c] > 128u ? v[c] - 128u : 128u - v[c];

        sq += d * d;
    }
    return 255u - (sq * 255u + 24576u) / 49152u;
}

/* One pixel's raw weight, re-derived from the documented contract:
 * contrast = 16 + quarter-half-up |4*gray - sum(neighbours)| capped at
 * 256 (neighbours replication-clamped), saturation = 16 + spread
 * capped at 256, well-exposedness as above; W16 = (c * s * w) >> 8. */
static unsigned nv_weight_at(const uint8_t *rgb, size_t w, size_t h,
                             size_t x, size_t y)
{
    size_t yu = y ? y - 1 : 0, yd = y + 1 < h ? y + 1 : h - 1;
    size_t xl = x ? x - 1 : 0, xr = x + 1 < w ? x + 1 : w - 1;
    const uint8_t *pc = rgb + (y * w + x) * 3;
    unsigned gc = luma(pc);
    unsigned gu = luma(rgb + (yu * w + x) * 3);
    unsigned gd = luma(rgb + (yd * w + x) * 3);
    unsigned gl = luma(rgb + (y * w + xl) * 3);
    unsigned gr = luma(rgb + (y * w + xr) * 3);
    int32_t lap = 4 * (int32_t)gc - (int32_t)gu - (int32_t)gd -
                  (int32_t)gl - (int32_t)gr;
    int32_t al = lap < 0 ? -lap : lap;
    unsigned wc = 16u + (unsigned)(al + 2) / 4u;
    unsigned mn = pc[0], mx = pc[0], ws, we;

    for (int c = 1; c < 3; c++) {
        if (pc[c] < mn)
            mn = pc[c];
        if (pc[c] > mx)
            mx = pc[c];
    }
    ws = 16u + (mx - mn);
    if (wc > 256u)
        wc = 256u;
    if (ws > 256u)
        ws = 256u;
    we = nv_well(pc[0], pc[1], pc[2]);
    return (wc * ws * we) >> 8;
}

/* The per-exposure weight of a CONSTANT plane: the Laplacian is zero
 * everywhere (even on the replication-clamped border), so contrast
 * sits at its floor and W16 = (16 * ws * we) >> 8. */
static unsigned flat_w16(unsigned r, unsigned g, unsigned b)
{
    const unsigned v[3] = { r, g, b };
    unsigned mn = v[0], mx = v[0], ws;

    for (int c = 1; c < 3; c++) {
        if (v[c] < mn)
            mn = v[c];
        if (v[c] > mx)
            mx = v[c];
    }
    ws = 16u + (mx - mn);
    if (ws > 256u)
        ws = 256u;
    return (16u * ws * nv_well(r, g, b)) >> 8;
}

/* The closed-form fused channel value of six constant planes: every
 * pyramid level of a constant plane is that constant (the half-up box
 * downsample and the convex bilinear expand are exact on constants),
 * every Laplacian is zero, so the whole fusion reduces to the level-6
 * weighted mean with the documented normalize (floor split, the last
 * exposure carries the remainder, uniform fallback on all-zero). */
static unsigned flat_fused(const uint8_t tint[HDR_EXPOSURES][3], int c)
{
    uint32_t w16[HDR_EXPOSURES], sum = 0, acc = 0;
    unsigned wn[HDR_EXPOSURES];

    for (unsigned k = 0; k < HDR_EXPOSURES; k++) {
        w16[k] = flat_w16(tint[k][0], tint[k][1], tint[k][2]);
        sum += w16[k];
    }
    if (sum == 0) {
        for (unsigned k = 0; k < 5; k++)
            wn[k] = 42u;
        wn[5] = 45u;
    } else {
        for (unsigned k = 0; k < 5; k++) {
            wn[k] = w16[k] * 255u / sum;
            acc += wn[k];
        }
        wn[5] = 255u - acc;
    }
    {
        uint64_t v = 0;

        for (unsigned k = 0; k < HDR_EXPOSURES; k++)
            v += (uint64_t)wn[k] * tint[k][c];
        v = (v + 127u) / 255u; /* symmetric half-up, v >= 0 here */
        return v > 255u ? 255u : (unsigned)v;
    }
}

static void fill_flat(uint8_t *plane, unsigned r, unsigned g, unsigned b)
{
    for (size_t i = 0; i < (size_t)HDR_SCENE_W * HDR_SCENE_H; i++) {
        plane[i * 3] = (uint8_t)r;
        plane[i * 3 + 1] = (uint8_t)g;
        plane[i * 3 + 2] = (uint8_t)b;
    }
}

/* --- determinism and corpus --------------------------------------------- */

void test_hdr_scenes_deterministic(void)
{
    hdr_ctx *a = hdr_prepare(1);
    hdr_ctx *b = hdr_prepare(1);

    CHECK(a && b);
    if (!a || !b) {
        hdr_free(a);
        hdr_free(b);
        return;
    }
    for (unsigned s = 0; s < HDR_SCENES; s++) {
        for (unsigned k = 0; k < HDR_EXPOSURES; k++)
            CHECK(memcmp(hdr_exposure(a, s, k), hdr_exposure(b, s, k),
                         hdr_exposure_len(a, s)) == 0);
        {
            const hdr_result *ra = hdr_reference(a, s);
            const hdr_result *rb = hdr_reference(b, s);

            CHECK(ra && rb);
            if (ra && rb) {
                CHECK(memcmp(ra->weights_digest, rb->weights_digest, 20) == 0);
                CHECK(memcmp(ra->fused_digest, rb->fused_digest, 20) == 0);
                CHECK_U64(ra->uniform, rb->uniform);
                CHECK_U64(ra->hi_clip, rb->hi_clip);
                CHECK_U64(ra->lo_black, rb->lo_black);
            }
        }
        CHECK_U64(hdr_archetype(a, s), hdr_archetype(b, s));
        CHECK(hdr_hilite_luma(a, s) == hdr_hilite_luma(b, s));
        CHECK(hdr_shadow_luma(a, s) == hdr_shadow_luma(b, s));
    }
    hdr_free(a);
    hdr_free(b);
}

void test_hdr_input_invariants(void)
{
    hdr_ctx *ctx = shared_hdr();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned s = 0; s < HDR_SCENES; s++) {
        hdr_box hi = hdr_hilite_box(ctx, s);
        hdr_box lo = hdr_shadow_box(ctx, s);
        int32_t lhi = hdr_hilite_luma(ctx, s);
        int32_t llo = hdr_shadow_luma(ctx, s);
        const uint8_t *e5 = hdr_exposure(ctx, s, HDR_EXPOSURES - 1);
        const uint8_t *e0 = hdr_exposure(ctx, s, 0);
        const uint8_t *e4 = hdr_exposure(ctx, s, 4);
        size_t p;

        CHECK_U64(hdr_archetype(ctx, s), s);
        /* the planted patches: 192x192, inside the frame, disjoint,
         * luminances inside the documented bands */
        CHECK(hi.x1 - hi.x0 == (int32_t)HDR_PATCH);
        CHECK(hi.y1 - hi.y0 == (int32_t)HDR_PATCH);
        CHECK(lo.x1 - lo.x0 == (int32_t)HDR_PATCH);
        CHECK(lo.y1 - lo.y0 == (int32_t)HDR_PATCH);
        CHECK(hi.x0 >= 0 && hi.y0 >= 0 && hi.x1 <= (int32_t)HDR_SCENE_W &&
              hi.y1 <= (int32_t)HDR_SCENE_H);
        CHECK(lo.x0 >= 0 && lo.y0 >= 0 && lo.x1 <= (int32_t)HDR_SCENE_W &&
              lo.y1 <= (int32_t)HDR_SCENE_H);
        CHECK(hi.x1 <= lo.x0 || lo.x1 <= hi.x0 ||
              hi.y1 <= lo.y0 || lo.y1 <= hi.y0);
        CHECK(lhi >= (int32_t)HDR_HILITE_MIN && lhi <= (int32_t)HDR_HILITE_MAX);
        CHECK(llo >= (int32_t)HDR_SHADOW_MIN && llo <= (int32_t)HDR_SHADOW_MAX);
        /* the burst does its job at the patches: the longest exposure
         * clips the highlight patch flat at 255, the shortest leaves
         * the shadow patch near black, the fourth stop reads it well */
        for (size_t dy = 0; dy < HDR_PATCH; dy++)
            for (size_t dx = 0; dx < HDR_PATCH; dx++) {
                p = ((size_t)hi.y0 + dy) * HDR_SCENE_W +
                    (size_t)hi.x0 + dx;
                CHECK(e5[p * 3] == 255 && e5[p * 3 + 1] == 255 &&
                      e5[p * 3 + 2] == 255);
            }
        {
            int max_lo0 = 0, min_lo4 = 255;

            for (size_t dy = 0; dy < HDR_PATCH; dy++)
                for (size_t dx = 0; dx < HDR_PATCH; dx++) {
                    const uint8_t *px;

                    p = ((size_t)lo.y0 + dy) * HDR_SCENE_W +
                        (size_t)lo.x0 + dx;
                    px = e0 + p * 3;
                    for (int c = 0; c < 3; c++)
                        if (px[c] > max_lo0)
                            max_lo0 = px[c];
                    px = e4 + p * 3;
                    for (int c = 0; c < 3; c++)
                        if (px[c] < min_lo4)
                            min_lo4 = px[c];
                }
            CHECK(max_lo0 <= 24);  /* the shortest exposure drowns it */
            /* the fourth stop reads it back (the patch bytes carry the
             * +-3 exposure noise: the measured floor is 162 - 3) */
            CHECK(min_lo4 >= 150);
        }
        /* the four scenes are four distinct corpora */
        if (s) {
            const uint8_t *p0 = hdr_exposure(ctx, 0, 0);

            CHECK(memcmp(hdr_exposure(ctx, s, 0), p0,
                         hdr_exposure_len(ctx, s)) != 0);
        }
    }
}

/* --- kernel units -------------------------------------------------------- */

void test_hdr_weight_units(void)
{
    const size_t W = 24, H = 16;
    uint8_t *rgb = malloc(W * H * 3);
    uint8_t *gray = malloc(W * H);
    uint16_t *w16 = malloc(W * H * sizeof(uint16_t));
    rng_t r;

    CHECK(rgb && gray && w16);
    if (!rgb || !gray || !w16) {
        free(rgb);
        free(gray);
        free(w16);
        return;
    }
    /* a seeded random plane: every pixel's weight matches the
     * re-derived formula, borders included */
    rng_init(&r, 3);
    for (size_t i = 0; i < W * H * 3; i++)
        rgb[i] = (uint8_t)rng_below(&r, 256);
    hdr_weights(rgb, W, H, gray, w16);
    for (size_t y = 0; y < H; y++)
        for (size_t x = 0; x < W; x++) {
            size_t i = y * W + x;

            CHECK_U64(gray[i], luma(rgb + i * 3));
            CHECK_U64(w16[i], nv_weight_at(rgb, W, H, x, y));
        }
    /* a constant plane: weight = (16 * 16 * well) >> 8 everywhere, the
     * Laplacian is zero even on the replication-clamped border */
    for (size_t i = 0; i < W * H * 3; i += 3) {
        rgb[i] = 90;
        rgb[i + 1] = 90;
        rgb[i + 2] = 90;
    }
    hdr_weights(rgb, W, H, gray, w16);
    for (size_t i = 0; i < W * H; i++) {
        CHECK_U64(gray[i], 90); /* (77+150+29)*90 = 256*90, half-up */
        CHECK_U64(w16[i], (16u * 16u * nv_well(90, 90, 90)) >> 8);
    }
    /* saturation drives the weight: same hue shape, wider spread,
     * strictly heavier plane */
    {
        uint16_t w16b[16];
        uint8_t grayb[16];

        for (size_t i = 0; i < 16; i++) {
            rgb[i * 3] = 178;
            rgb[i * 3 + 1] = 128;
            rgb[i * 3 + 2] = 78;
        }
        hdr_weights(rgb, 4, 4, grayb, w16b);
        for (size_t i = 0; i < 16; i++)
            CHECK(w16b[i] > (16u * 16u * nv_well(90, 90, 90)) >> 8);
    }
    free(rgb);
    free(gray);
    free(w16);
}

void test_hdr_normalize_units(void)
{
    const size_t N = 8 * 6;
    uint16_t w16[HDR_EXPOSURES][48];
    uint8_t out[HDR_EXPOSURES][48];
    const uint16_t *wp[HDR_EXPOSURES];
    uint8_t *op[HDR_EXPOSURES];
    /* hdr_normalize ACCUMULATES into *uniform so callers can sum the
     * count across threads and scenes */
    uint32_t uniform = 0;

    for (unsigned k = 0; k < HDR_EXPOSURES; k++) {
        wp[k] = w16[k];
        op[k] = out[k];
        for (size_t p = 0; p < N; p++)
            w16[k][p] = (uint16_t)(((k + 1) * 37u + (unsigned)p * 11u) % 800u);
    }
    for (unsigned k = 0; k < HDR_EXPOSURES; k++)
        for (size_t p = 3; p < N; p += 8)
            w16[k][p] = 0; /* one all-starved column (x = 3, six pixels) */
    hdr_normalize(wp, N, op, &uniform);
    CHECK_U64(uniform, 6);
    for (size_t p = 0; p < N; p++) {
        uint32_t sum = 0, acc = 0;

        for (unsigned k = 0; k < HDR_EXPOSURES; k++)
            sum += w16[k][p];
        if (sum == 0) {
            for (unsigned k = 0; k < 5; k++)
                CHECK_U64(op[k][p], 42u);
            CHECK_U64(op[5][p], 45u);
            continue;
        }
        for (unsigned k = 0; k < 5; k++) {
            CHECK_U64(op[k][p], w16[k][p] * 255u / sum);
            acc += op[k][p];
        }
        CHECK_U64(op[5][p], 255u - acc);
        {
            uint32_t total = acc + op[5][p];

            CHECK_U64(total, 255u);
        }
    }
    /* the exact floor split of 255 over five 1s: the last plane drops
     * to zero (the remainder can be empty) */
    for (unsigned k = 0; k < HDR_EXPOSURES; k++)
        for (size_t p = 0; p < N; p++)
            w16[k][p] = k < 5 ? 1u : 0u;
    uniform = 0;
    hdr_normalize(wp, N, op, &uniform);
    for (unsigned k = 0; k < 5; k++)
        CHECK_U64(op[k][0], 51u);
    CHECK_U64(op[5][0], 0u);
    CHECK_U64(uniform, 0);
    /* a NULL uniform counter is accepted */
    hdr_normalize(wp, N, op, NULL);
    CHECK_U64(op[0][0], 51u);
}

void test_hdr_down_units(void)
{
    /* half-up 2x2 box downsample of a 5x5 ramp, including the
     * edge-clamped second tap on the odd border */
    static const uint8_t SRC[25] = {
         0,  1,  2,  3,  4,
        10, 11, 12, 13, 14,
        20, 21, 22, 23, 24,
        30, 31, 32, 33, 34,
        40, 41, 42, 43, 44,
    };
    static const uint8_t DST[9] = { 6, 8, 9, 26, 28, 29, 41, 43, 44 };
    uint8_t dst[9];

    hdr_down_gray(dst, 3, 3, SRC, 5, 5);
    for (int i = 0; i < 9; i++)
        CHECK_U64(dst[i], DST[i]);
    /* the RGB variant downsamples the three channels independently */
    {
        uint8_t rs[4 * 2 * 3] = {
            10, 200, 30,  40, 210, 60,
            70, 220, 90, 100, 230, 120,
        };
        uint8_t rd[2 * 3];

        hdr_down_rgb(rd, 2, 1, rs, 2, 2);
        CHECK_U64(rd[0], (10 + 40 + 70 + 100 + 2) >> 2);
        CHECK_U64(rd[1], (200 + 210 + 220 + 230 + 2) >> 2);
        CHECK_U64(rd[2], (30 + 60 + 90 + 120 + 2) >> 2);
    }
}

void test_hdr_expand_units(void)
{
    /* the documented convention ((x + 0.5) * sw / dw - 0.5 in 16.16,
     * edge-clamped sample indices, half-up): a 2x2 signed RGB plane
     * into 4x4 lands exactly on these bilinear mixes. Every channel
     * of a pixel carries the same value, so the anchors hold for all
     * three channels. */
    static const int16_t VALS[4] = { 100, -200, 300, -400 };
    int16_t src[3 * 2 * 3]; /* the bigger convexity plane reuses it */
    int16_t dst[9 * 7 * 3];
    int max_abs = 0;

    for (int p = 0; p < 4; p++)
        for (int c = 0; c < 3; c++)
            src[p * 3 + c] = VALS[p];
    hdr_expand16(dst, 4, 4, src, 2, 2);
    CHECK(dst[0] == -200);  /* x=0,y=0: 0.25/0.75 clamped-edge mix */
    CHECK(dst[(1 * 4 + 1) * 3] == 50);  /* x=1,y=1: 0.75/0.25 both axes */
    CHECK(dst[(3 * 4 + 3) * 3] == -400); /* x=3,y=3: clamped to s[1][1] */
    /* convexity: every dst sample is bounded by max |src|, on a bigger
     * seeded signed plane (3x2 -> 9x7, RGB interleaved) */
    {
        rng_t r;

        rng_init(&r, 7);
        for (size_t i = 0; i < 3 * 2 * 3; i++)
            src[i] = (int16_t)((int)rng_below(&r, 2001) - 1000);
        hdr_expand16(dst, 9, 7, src, 3, 2);
        for (size_t i = 0; i < 9 * 7 * 3; i++) {
            int a = dst[i] < 0 ? -dst[i] : dst[i];

            if (a > max_abs)
                max_abs = a;
        }
        CHECK(max_abs <= 1000);
    }
}

/* --- the flat closed-form fusion ------------------------------------------ */

static void check_flat_constellation(hdr_scratch *s,
                                     const uint8_t tint[HDR_EXPOSURES][3],
                                     unsigned want_uniform,
                                     unsigned want_hi_clip,
                                     unsigned want_lo_black)
{
    const uint8_t *planes[HDR_EXPOSURES];
    uint8_t *buf[HDR_EXPOSURES] = { 0 };
    hdr_result r;
    size_t n = (size_t)HDR_SCENE_W * HDR_SCENE_H;
    unsigned k;

    for (k = 0; k < HDR_EXPOSURES; k++) {
        buf[k] = malloc(n * 3);
        CHECK(buf[k] != NULL);
        if (!buf[k])
            goto out;
        fill_flat(buf[k], tint[k][0], tint[k][1], tint[k][2]);
        planes[k] = buf[k];
    }
    CHECK(hdr_process(planes, s, &r) == 0);
    for (int c = 0; c < 3; c++) {
        unsigned want = flat_fused(tint, c);

        for (size_t p = 0; p < n; p++)
            CHECK_U64(s->fused[p * 3 + c], want);
    }
    CHECK_U64(r.uniform, want_uniform);
    CHECK_U64(r.hi_clip, want_hi_clip);
    CHECK_U64(r.lo_black, want_lo_black);
    CHECK(memcmp(r.weights_digest, s->res.weights_digest, 20) == 0);
    CHECK(memcmp(r.fused_digest, s->res.fused_digest, 20) == 0);
    /* re-run on a second scratch: byte-identical digests (the whole
     * pipeline is deterministic, the scratch holds no state) */
    {
        hdr_scratch s2;
        hdr_result r2;

        CHECK(hdr_scratch_init(&s2) == 0);
        CHECK(hdr_process(planes, &s2, &r2) == 0);
        CHECK(memcmp(r2.weights_digest, r.weights_digest, 20) == 0);
        CHECK(memcmp(r2.fused_digest, r.fused_digest, 20) == 0);
        hdr_scratch_free(&s2);
    }
out:
    for (k = 0; k < HDR_EXPOSURES; k++)
        free(buf[k]);
}

void test_hdr_flat_fusion_exact(void)
{
    hdr_scratch s;
    static const uint8_t GREYS[HDR_EXPOSURES][3] = {
        { 40, 40, 40 }, { 80, 80, 80 }, { 120, 120, 120 },
        { 160, 160, 160 }, { 200, 200, 200 }, { 240, 240, 240 },
    };
    static const uint8_t TINTS[HDR_EXPOSURES][3] = {
        { 200, 100, 50 }, { 210, 110, 60 }, { 220, 120, 70 },
        { 230, 130, 80 }, { 240, 140, 90 }, { 250, 150, 100 },
    };

    CHECK(hdr_scratch_init(&s) == 0);
    /* six identical midtone planes: the floor split of 255 lands on
     * the documented uniform 42x5+45, the fused value is exactly the
     * input (no clipping anywhere) */
    {
        const uint8_t mid[HDR_EXPOSURES][3] = {
            { 128, 128, 128 }, { 128, 128, 128 }, { 128, 128, 128 },
            { 128, 128, 128 }, { 128, 128, 128 }, { 128, 128, 128 },
        };

        check_flat_constellation(&s, mid, 0, 0, 0);
        for (size_t p = 0; p < (size_t)HDR_SCENE_W * HDR_SCENE_H; p++)
            CHECK_U64(s.fused[p * 3], 128);
    }
    /* six black planes: every raw weight is zero -> the uniform
     * fallback fires on every pixel, the fused frame is black */
    {
        const uint8_t blk[HDR_EXPOSURES][3] = {
            { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
            { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
        };

        check_flat_constellation(
            &s, blk,
            (unsigned)HDR_SCENE_W * HDR_SCENE_H, 0,
            (unsigned)HDR_SCENE_W * HDR_SCENE_H);
        for (size_t p = 0; p < (size_t)HDR_SCENE_W * HDR_SCENE_H; p++)
            CHECK_U64(s.fused[p * 3 + 2], 0);
    }
    /* six white planes: weights floor to a nonzero uniform split and
     * the whole frame clamps at 255 */
    {
        const uint8_t wht[HDR_EXPOSURES][3] = {
            { 255, 255, 255 }, { 255, 255, 255 }, { 255, 255, 255 },
            { 255, 255, 255 }, { 255, 255, 255 }, { 255, 255, 255 },
        };

        check_flat_constellation(
            &s, wht, 0,
            (unsigned)HDR_SCENE_W * HDR_SCENE_H, 0);
        for (size_t p = 0; p < (size_t)HDR_SCENE_W * HDR_SCENE_H; p++)
            CHECK_U64(s.fused[p * 3 + 1], 255);
    }
    /* six distinct grays: the closed form pins the fused byte at 132
     * (the helper's own value is asserted first — a wrong restatement
     * of the formulas must fail here before it fails the pipeline) */
    CHECK_U64(flat_fused(GREYS, 0), 132u);
    check_flat_constellation(&s, GREYS, 0, 0, 0);
    for (size_t p = 0; p < (size_t)HDR_SCENE_W * HDR_SCENE_H; p++)
        CHECK_U64(s.fused[p * 3], 132u);
    /* tinted flats: the saturation factor (spread 150) is live in the
     * closed form, checked per channel */
    check_flat_constellation(&s, TINTS, 0, 0, 0);
    hdr_scratch_free(&s);
}

/* --- corpus goldens and reproducibility ------------------------------------ */

void test_hdr_process_reproducible(void)
{
    hdr_ctx *ctx = shared_hdr();
    hdr_scratch sc;
    int ok;

    CHECK(ctx);
    if (!ctx)
        return;
    ok = hdr_scratch_init(&sc) == 0;
    CHECK(ok);
    if (!ok)
        return;
    for (unsigned s = 0; s < HDR_SCENES; s++) {
        hdr_result r;
        const hdr_result *ref = hdr_reference(ctx, s);

        CHECK(ref != NULL);
        if (!ref)
            break;
        const uint8_t *planes[HDR_EXPOSURES];

        for (unsigned k = 0; k < HDR_EXPOSURES; k++)
            planes[k] = hdr_exposure(ctx, s, k);
        CHECK(hdr_process(planes, &sc, &r) == 0);
        CHECK(memcmp(r.weights_digest, ref->weights_digest, 20) == 0);
        CHECK(memcmp(r.fused_digest, ref->fused_digest, 20) == 0);
        CHECK_U64(r.uniform, ref->uniform);
        CHECK_U64(r.hi_clip, ref->hi_clip);
        CHECK_U64(r.lo_black, ref->lo_black);
    }
    hdr_scratch_free(&sc);
}

void test_hdr_seed_variation(void)
{
    hdr_ctx *ctx = shared_hdr();
    hdr_ctx *alt = hdr_prepare(2);

    CHECK(ctx && alt);
    if (!ctx || !alt) {
        hdr_free(alt);
        return;
    }
    for (unsigned s = 0; s < HDR_SCENES; s++) {
        const hdr_result *r1 = hdr_reference(ctx, s);
        const hdr_result *r2 = hdr_reference(alt, s);
        hdr_box hi = hdr_hilite_box(alt, s);
        hdr_box lo = hdr_shadow_box(alt, s);

        CHECK(r1 && r2);
        if (!r1 || !r2)
            continue;
        /* same invariants under a new seed... */
        CHECK_U64(hdr_archetype(alt, s), s);
        CHECK(hdr_hilite_luma(alt, s) >= (int32_t)HDR_HILITE_MIN);
        CHECK(hdr_hilite_luma(alt, s) <= (int32_t)HDR_HILITE_MAX);
        CHECK(hdr_shadow_luma(alt, s) >= (int32_t)HDR_SHADOW_MIN);
        CHECK(hdr_shadow_luma(alt, s) <= (int32_t)HDR_SHADOW_MAX);
        CHECK_U64(r2->uniform, 0);
        CHECK_U64(r2->lo_black, 0);
        CHECK(hi.x1 - hi.x0 == (int32_t)HDR_PATCH);
        CHECK(lo.y1 - lo.y0 == (int32_t)HDR_PATCH);
        /* ...but a different corpus: every digest moves */
        CHECK(memcmp(r1->weights_digest, r2->weights_digest, 20) != 0);
        CHECK(memcmp(r1->fused_digest, r2->fused_digest, 20) != 0);
        CHECK(memcmp(hdr_exposure(ctx, s, 0), hdr_exposure(alt, s, 0),
                     hdr_exposure_len(ctx, s)) != 0);
    }
    hdr_free(alt);
}

/* --- run cycles ------------------------------------------------------------ */

void test_hdr_run_sane(void)
{
    hdr_ctx *ctx = shared_hdr();
    unsigned threads = 1;
    double mpx = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(hdr_run(ctx, 1, &threads, &mpx, &pixels) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(pixels, HDR_GOLDEN_TASK_PIXELS);
    CHECK(mpx > 0.0);
    threads = 2;
    CHECK(hdr_run(ctx, 1, &threads, &mpx, &pixels) == 0);
    CHECK_U64(threads, 2);
    CHECK_U64(pixels, HDR_GOLDEN_TASK_PIXELS_MULTI);
    CHECK(mpx > 0.0);
}

void test_hdr_thread_clamp(void)
{
    hdr_ctx *ctx = shared_hdr();
    unsigned threads = 0;
    double mpx = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* zero threads: clamped to one, the single-scene-set pass */
    CHECK(hdr_run(ctx, 1, &threads, &mpx, &pixels) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(pixels, HDR_GOLDEN_TASK_PIXELS);
    /* more threads than scene instances: clamped to the 16-scene
     * multi set */
    threads = 999;
    CHECK(hdr_run(ctx, 1, &threads, &mpx, &pixels) == 0);
    CHECK_U64(threads, HDR_SCENES_MULTI);
    CHECK_U64(pixels, HDR_GOLDEN_TASK_PIXELS_MULTI);
}

void test_hdr_accessors_null_safe(void)
{
    hdr_ctx *ctx = shared_hdr();
    hdr_box b;
    hdr_scratch sc;
    hdr_result r;
    const uint8_t *ex[HDR_EXPOSURES] = { 0 };

    CHECK(ctx);
    CHECK(hdr_reference(NULL, 0) == NULL);
    CHECK(hdr_reference(ctx, HDR_SCENES) == NULL);
    CHECK(hdr_exposure(NULL, 0, 0) == NULL);
    CHECK(hdr_exposure(ctx, 0, HDR_EXPOSURES) == NULL);
    CHECK(hdr_exposure(ctx, HDR_SCENES, 0) == NULL);
    CHECK_U64(hdr_exposure_len(NULL, 0), 0);
    CHECK_U64(hdr_exposure_len(ctx, 0), HDR_SCENE_W * HDR_SCENE_H * 3);
    CHECK_U64(hdr_archetype(NULL, 0), 0);
    CHECK_U64(hdr_archetype(ctx, HDR_SCENES), 0);
    b = hdr_hilite_box(NULL, 0);
    CHECK(b.x0 == 0 && b.y0 == 0 && b.x1 == 0 && b.y1 == 0);
    b = hdr_hilite_box(ctx, HDR_SCENES);
    CHECK(b.x0 == 0 && b.y0 == 0 && b.x1 == 0 && b.y1 == 0);
    b = hdr_shadow_box(NULL, HDR_SCENES);
    CHECK(b.x0 == 0 && b.y0 == 0 && b.x1 == 0 && b.y1 == 0);
    CHECK(hdr_hilite_luma(NULL, 0) == 0);
    CHECK(hdr_hilite_luma(ctx, HDR_SCENES) == 0);
    CHECK(hdr_shadow_luma(NULL, 0) == 0);
    CHECK(hdr_shadow_luma(ctx, HDR_SCENES) == 0);
    CHECK(hdr_scratch_init(NULL) == -1);
    hdr_scratch_free(NULL);
    CHECK(hdr_process(NULL, &sc, &r) == -1);
    CHECK(hdr_process(ex, &sc, &r) == -1); /* NULL exposure pointers */
    CHECK(hdr_scratch_init(&sc) == 0);
    CHECK(hdr_process(ex, &sc, NULL) == -1);
    hdr_scratch_free(&sc);
    {
        unsigned t = 1;
        double mpx = 0.0;
        uint64_t px = 0;

        CHECK(hdr_run(NULL, 1, &t, &mpx, &px) == -1);
    }
}

void test_hdr_corpus_golden(void)
{
    /* the golden pins proper: per-scene boxes, luminances, weight and
     * fused digests, exact counters and the task pixel counts */
    hdr_ctx *ctx = shared_hdr();
    static const int32_t HI[HDR_SCENES][4] = {
        { 1185, 139, 1377, 331 }, { 1343, 72, 1535, 264 },
        { 1223, 83, 1415, 275 }, { 1340, 117, 1532, 309 },
    };
    static const int32_t LO[HDR_SCENES][4] = {
        { 141, 840, 333, 1032 }, { 153, 830, 345, 1022 },
        { 146, 879, 338, 1071 }, { 243, 898, 435, 1090 },
    };
    static const int32_t LHI[HDR_SCENES] = { 3634, 3651, 3712, 3614 };
    static const int32_t LLO[HDR_SCENES] = { 197, 212, 219, 212 };
    static const unsigned HIC[HDR_SCENES] = {
        23891u, 0u, 6417u, 7533u,
    };

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned s = 0; s < HDR_SCENES; s++) {
        const hdr_result *r = hdr_reference(ctx, s);
        hdr_box hi = hdr_hilite_box(ctx, s);
        hdr_box lo = hdr_shadow_box(ctx, s);

        CHECK(r != NULL);
        if (!r)
            return;
        CHECK(hi.x0 == HI[s][0] && hi.y0 == HI[s][1] &&
              hi.x1 == HI[s][2] && hi.y1 == HI[s][3]);
        CHECK(lo.x0 == LO[s][0] && lo.y0 == LO[s][1] &&
              lo.x1 == LO[s][2] && lo.y1 == LO[s][3]);
        CHECK(hdr_hilite_luma(ctx, s) == LHI[s]);
        CHECK(hdr_shadow_luma(ctx, s) == LLO[s]);
        CHECK(memcmp(r->weights_digest, HDR_GOLDEN_WEIGHTS[s], 20) == 0);
        CHECK(memcmp(r->fused_digest, HDR_GOLDEN_FUSED[s], 20) == 0);
        CHECK_U64(r->uniform, 0);
        CHECK_U64(r->hi_clip, HIC[s]);
        CHECK_U64(r->lo_black, 0);
    }
    CHECK_U64(hdr_task_pixels(0), HDR_GOLDEN_TASK_PIXELS);
    CHECK_U64(hdr_task_pixels(1), HDR_GOLDEN_TASK_PIXELS_MULTI);
}