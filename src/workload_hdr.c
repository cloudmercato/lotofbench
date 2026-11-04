/* LotofBench — HDR workload (workload #14).
 *
 * Mirrors the fourteenth Geekbench 6 CPU workload: blend six 16 MP SDR
 * exposures of one scene into an HDR image with exposure fusion
 * (Mertens et al.'s pyramid blending). LotofBench reproduces the shape
 * dependency-free, with the same honesty rules as the other workloads:
 *
 *   - deterministic synthetic input — a seeded set of four scenes
 *     (1600x1200, the stand-in for the 16 MP capture size) in four
 *     archetype families (backlit window, sunset, gallery, night
 *     street). Every scene is a 12-bit luminance field — a two-stop
 *     vertical gradient plus planted features (bright windows, a sun
 *     disc, spotlight discs, Chebyshev lamp falloffs, lit windows —
 *     all analytic integer shapes) and per-pixel noise (+-24 of 4096)
 *     — mapped through a per-archetype tint (8.8 channel gains) onto
 *     six exposures: byte = clamp((V * 2^k * 255 + 2048) >> 12 + n_k,
 *     0, 255) for k = 0..5, a bracketed burst 1 EV apart whose short
 *     exposure maps the full 12-bit range onto [0, 255] and whose
 *     brighter stops clip the highlights. One +-3 noise draw per pixel
 *     per exposure (applied to all three channels — the correlated
 *     sensor-noise stand-in). Every scene also plants two flat
 *     ground-truth patches — a highlight patch (3584-3712 of 4096,
 *     top-right) and a shadow patch (192-224, bottom-left), 192x192,
 *     at least 64/96 px inside the frame — whose boxes and clean
 *     luminances are exposed through accessors. Fixed --seed means
 *     byte-identical exposures on every machine; generation is off the
 *     clock — the stand-in for the RAW loader;
 *   - a deterministic fusion pass — the Mertens pipeline in 100%
 *     integer arithmetic: per-exposure weight maps (contrast: the
 *     absolute 4-neighbour Laplacian of the Rec.601 luma, quartered,
 *     floor 16 cap 256 — the floor is the epsilon of Mertens et al.,
 *     keeping flat regions from collapsing to uniform blending;
 *     saturation: max-min, floor 16 cap 256; well-exposedness:
 *     255 * (1 - sum_c (v_c - 128)^2 / (3 * 128^2)), half-up — a
 *     linear decay from the midtone peak standing in for their
 *     Gaussian kernel), combined as W16 = (contrast * saturation *
 *     well) >> 8; per-pixel normalization across the six exposures
 *     (floor division, last exposure carries the remainder — the
 *     stack sums to exactly 255 everywhere; the all-zero pixel takes
 *     the documented uniform fallback 42,42,42,42,42,45); a 6-level
 *     Laplacian pyramid of every exposure and a 6-level Gaussian
 *     pyramid of every normalized weight plane (2x2 half-up box
 *     downsample, the shared img_scale_bilinear as the Laplacian's
 *     upsample); per-level blending — the accumulator sums the raw
 *     weight*Laplacian products and one symmetric half-up division by
 *     255 happens at collapse; the deepest level blends the Gaussian
 *     residuals directly; collapse — bilinear expand of the signed
 *     accumulator (16.16, half-up toward +infinity) plus the level
 *     accumulator, clamped to bytes. All bounds are documented (the
 *     collapse plane stays within +-1540), so int16 planes are exact;
 *   - correctness inside the timed cycle — a pass only counts if every
 *     scene reproduces its prepare-time reference pipeline: SHA-1 of
 *     the normalized weight stack and of the fused frame, plus the
 *     exact uniform/hi_clip/lo_black counters, all bit-identical, with
 *     the degeneracy guards uniform == 0 and lo_black == 0 (the scene
 *     floor keeps every pixel visible in the longest exposure). Any
 *     drift aborts the run instead of scoring;
 *   - shared-task multi-core through the common harness — the
 *     multi-core pass fuses 16 scene instances (4x the set) split into
 *     equal contiguous slices, one per thread.
 *
 * The throughput metric is fused output pixels: every scene
 * contributes exactly one full 1600x1200 fused frame, exact and
 * machine-independent, the image-workload analogue of the corpus size.
 *
 * Deliberate divergences from Geekbench 6 (all documented in
 * docs/benchmarks.md): 1600x1200 synthetic exposures instead of 16 MP
 * photos (the pipeline shape and the Mpixels/s metric are the
 * workload's), a linear well-exposedness kernel instead of the
 * Gaussian, floor-16 contrast/saturation weights instead of
 * floating-point epsilons, aligned noise-free brackets instead of
 * handheld frames (no ghost-step correction stage), and synthetic
 * seeded scenes instead of real RAW bursts.
 */

#include "workload_hdr.h"

#include "harness.h"
#include "imgops.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <stdlib.h>
#include <string.h>

#define HDR_SCENE_PX   ((size_t)HDR_SCENE_W * HDR_SCENE_H)
#define HDR_SCENE_BYTES (HDR_SCENE_PX * 3)
#define HDR_ARCHETYPES 4u

/* Symmetric half-up division by 255: q(v) = sign(v) * (|v| + 127) / 255.
 * The one rounding of the blend accumulator (raw products are summed
 * exactly, the division happens once per pixel per level). */
static int32_t q255(int32_t v)
{
    if (v >= 0)
        return (v + 127) / 255;
    return -((-v + 127) / 255);
}

/* --- corpus generation ------------------------------------------------------*/

typedef struct {
    const char *name;
    uint16_t tint[3]; /* 8.8 channel gains applied to the 12-bit scene */
} hdr_scene_def;

/* The tints keep every archetype's channels distinguishable (the
 * saturation factor has something to score) while the highlight patch
 * (<= 3712 of 4096) stays below the first exposure's clip point
 * (3712 * 282 >> 8 = 4095.2 — the strongest gain just fits) so the
 * shortest exposure always keeps patch detail. */
static const hdr_scene_def ARCH[HDR_ARCHETYPES] = {
    { "window",  { 262, 252, 235 } },
    { "sunset",  { 282, 236, 198 } },
    { "gallery", { 247, 253, 266 } },
    { "night",   { 226, 238, 285 } },
};

struct hdr_ctx {
    uint8_t *exposure[HDR_SCENES][HDR_EXPOSURES];
    hdr_result ref[HDR_SCENES];
    unsigned  arch[HDR_SCENES];
    hdr_box   hilite[HDR_SCENES];
    hdr_box   shadow[HDR_SCENES];
    int32_t   hilite_l[HDR_SCENES];
    int32_t   shadow_l[HDR_SCENES];
};

/* Half-up two-stop vertical gradient over rows [0, rows), the same
 * expression as the Background Blur corpus (deterministic; the falling
 * case truncates toward zero, which is fine — the value is pinned by
 * the seed, not by a rounding claim). */
static void fill_grad(uint16_t *L, uint32_t rows, uint32_t top,
                      uint32_t bottom)
{
    for (uint32_t y = 0; y < rows; y++) {
        int32_t v = (int32_t)top +
                    (((int32_t)bottom - (int32_t)top) * (int32_t)y * 2 +
                     (int32_t)rows) / ((int32_t)rows * 2);

        for (uint32_t x = 0; x < HDR_SCENE_W; x++)
            L[(size_t)y * HDR_SCENE_W + x] = (uint16_t)v;
    }
}

static uint16_t clamp12(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > (int32_t)HDR_LUMA_MAX)
        return (uint16_t)HDR_LUMA_MAX;
    return (uint16_t)v;
}

/* Archetype 0 — backlit window: a dark room gradient, one bright
 * window rectangle (512 frame, 3840 interior), a floor band. The
 * window interior clips in every exposure past k = 0 but keeps detail
 * in the shortest one (3840 * 262 >> 8 = 3930 < 4096). */
static void paint_window(uint16_t *L, rng_t *r)
{
    uint32_t wx = 256 + rng_below(r, 769);
    uint32_t wy = 128 + rng_below(r, 321);
    uint32_t ww = 288 + rng_below(r, 161);
    uint32_t wh = 192 + rng_below(r, 129);

    fill_grad(L, HDR_SCENE_H, 160, 384);
    for (uint32_t y = wy; y < wy + wh; y++)
        for (uint32_t x = wx; x < wx + ww; x++) {
            int frame = x < wx + 8 || x >= wx + ww - 8 ||
                        y < wy + 8 || y >= wy + wh - 8;

            L[(size_t)y * HDR_SCENE_W + x] = frame ? 512 : 3840;
        }
    for (uint32_t y = HDR_SCENE_H - 200; y < HDR_SCENE_H; y++)
        for (uint32_t x = 0; x < HDR_SCENE_W; x++) {
            size_t i = (size_t)y * HDR_SCENE_W + x;

            L[i] = clamp12((int32_t)L[i] + 96);
        }
}

/* Archetype 1 — sunset: bright sky against a dark ground, a sun disc
 * (the only corpus feature whose core reaches 4095 on every channel —
 * the source of the suite's hi_clip pixels) and a dark sloped ridge
 * band silhouetted across the ground. */
static void paint_sunset(uint16_t *L, rng_t *r)
{
    uint32_t horizon = 520 + rng_below(r, 161);
    uint32_t cx = 320 + rng_below(r, 961);
    uint32_t rad = 96 + rng_below(r, 65);
    int32_t cy = (int32_t)horizon - 48 - (int32_t)rng_below(r, 97);
    int32_t slope = (int32_t)rng_below(r, 129) - 64;

    fill_grad(L, horizon, 512, 3584);
    fill_grad(L + (size_t)horizon * HDR_SCENE_W, HDR_SCENE_H - horizon,
              384, 128);
    for (int32_t dy = -(int32_t)rad - 16; dy <= (int32_t)rad + 16; dy++) {
        int32_t y = cy + dy;

        if (y < 0 || y >= (int32_t)HDR_SCENE_H)
            continue;
        for (int32_t dx = -(int32_t)rad - 16; dx <= (int32_t)rad + 16;
             dx++) {
            int32_t x = (int32_t)cx + dx;
            int32_t d2 = dx * dx + dy * dy;
            size_t i;

            if (x < 0 || x >= (int32_t)HDR_SCENE_W)
                continue;
            i = (size_t)y * HDR_SCENE_W + (size_t)x;
            if (d2 <= (int32_t)(rad * rad))
                L[i] = HDR_LUMA_MAX;
            else if (d2 <= (int32_t)((rad + 16) * (rad + 16)))
                L[i] = 3584;
        }
    }
    for (uint32_t x = 0; x < HDR_SCENE_W; x++) {
        int32_t ry = (int32_t)horizon + 48 +
                     ((((int32_t)x - (int32_t)HDR_SCENE_W / 2) * slope) >> 8);

        for (int32_t y = ry; y < ry + 24; y++)
            if (y >= 0 && y < (int32_t)HDR_SCENE_H)
                L[(size_t)y * HDR_SCENE_W + x] = 256;
    }
}

/* Archetype 2 — gallery: a mid-lit wall with three spotlight discs
 * (bright cores, quadratic falloff rings — no square roots, the falloff
 * is a function of the squared distance) and a dark wainscot band the
 * spots punch through. */
static void paint_gallery(uint16_t *L, rng_t *r)
{
    fill_grad(L, HDR_SCENE_H, 896, 1280);
    for (uint32_t y = HDR_SCENE_H - 240; y < HDR_SCENE_H; y++)
        for (uint32_t x = 0; x < HDR_SCENE_W; x++)
            L[(size_t)y * HDR_SCENE_W + x] = 320;
    for (unsigned k = 0; k < 3; k++) {
        uint32_t cx = 192 + rng_below(r, 1217);
        uint32_t cy = 160 + rng_below(r, 721);
        uint32_t rad = 128 + rng_below(r, 97);
        uint32_t out2 = (rad + 64) * (rad + 64);
        uint32_t in2 = rad * rad;
        int32_t y0 = (int32_t)cy - (int32_t)rad - 64;
        int32_t y1 = (int32_t)cy + (int32_t)rad + 64;

        for (int32_t y = y0; y <= y1; y++) {
            int32_t dy = y - (int32_t)cy;

            if (y < 0 || y >= (int32_t)HDR_SCENE_H)
                continue;
            for (int32_t dx = -(int32_t)rad - 64;
                 dx <= (int32_t)rad + 64; dx++) {
                int32_t x = (int32_t)cx + dx;
                int32_t d2, base, span, v;
                size_t i;

                if (x < 0 || x >= (int32_t)HDR_SCENE_W)
                    continue;
                d2 = dy * dy + dx * dx;
                if (d2 <= (int32_t)in2) {
                    L[(size_t)y * HDR_SCENE_W + (size_t)x] = 3840;
                    continue;
                }
                if (d2 > (int32_t)out2)
                    continue;
                base = 896 + (((1280 - 896) * (int32_t)y * 2 +
                               (int32_t)HDR_SCENE_H) /
                              ((int32_t)HDR_SCENE_H * 2));
                span = (int32_t)out2 - (int32_t)in2;
                v = base + ((3840 - base) *
                            (int32_t)(out2 - (uint32_t)d2)) / span;
                i = (size_t)y * HDR_SCENE_W + (size_t)x;
                if (v > (int32_t)L[i])
                    L[i] = (uint16_t)v;
            }
        }
    }
}

/* Archetype 3 — night street: a dark base, a seeded lattice of lit
 * windows, and five lamps whose Chebyshev-distance falloff (4095 core,
 * -96 per step of square distance) is the brightest feature in the
 * corpus, each with a lit pool on the ground below. */
static void paint_night(uint16_t *L, rng_t *r)
{
    fill_grad(L, HDR_SCENE_H, 96, 192);
    for (unsigned gy = 0; gy < 4; gy++)
        for (unsigned gx = 0; gx < 8; gx++) {
            uint32_t wx, wy, ww, wh, v;

            if (rng_below(r, 4) != 0)
                continue;
            wx = 64 + gx * 192 + rng_below(r, 97);
            wy = 96 + gy * 96 + rng_below(r, 49);
            ww = 24 + rng_below(r, 25);
            wh = 16 + rng_below(r, 17);
            v = 2048 + rng_below(r, 1025);
            for (uint32_t y = wy; y < wy + wh && y < HDR_SCENE_H; y++)
                for (uint32_t x = wx; x < wx + ww && x < HDR_SCENE_W;
                     x++)
                    L[(size_t)y * HDR_SCENE_W + x] = (uint16_t)v;
        }
    for (unsigned k = 0; k < 5; k++) {
        uint32_t cx = 128 + rng_below(r, 1345);
        uint32_t cy = 128 + rng_below(r, 449);
        uint32_t half = 48 + rng_below(r, 49);

        for (uint32_t y = cy - 48; y <= cy + 48 && y < HDR_SCENE_H; y++)
            for (uint32_t x = cx - 48; x <= cx + 48 && x < HDR_SCENE_W;
                 x++) {
                uint32_t dx = x >= cx ? x - cx : cx - x;
                uint32_t dy = y >= cy ? y - cy : cy - y;
                uint32_t d = dx > dy ? dx : dy;
                size_t i = (size_t)y * HDR_SCENE_W + x;
                int32_t v = 4095 - (int32_t)(d * 96);

                if (d <= 42 && v > (int32_t)L[i])
                    L[i] = (uint16_t)v;
            }
        for (uint32_t y = cy + half + 8;
             y < cy + half * 3 + 8 && y < HDR_SCENE_H; y++)
            for (uint32_t x = cx - half;
                 x <= cx + half && x < HDR_SCENE_W; x++) {
                size_t i = (size_t)y * HDR_SCENE_W + x;

                if (L[i] < 320)
                    L[i] = 320;
            }
    }
}

static void (*const PAINT[HDR_ARCHETYPES])(uint16_t *, rng_t *) = {
    paint_window, paint_sunset, paint_gallery, paint_night
};

static void gen_scene(hdr_ctx *ctx, unsigned s, unsigned arch, rng_t *rng)
{
    uint16_t *L = malloc(HDR_SCENE_PX * sizeof(uint16_t));
    hdr_box *hi = &ctx->hilite[s];
    hdr_box *lo = &ctx->shadow[s];
    int32_t lhi = (int32_t)(HDR_HILITE_MIN + rng_below(rng, 129));
    int32_t llo = (int32_t)(HDR_SHADOW_MIN + rng_below(rng, 33));

    if (!L)
        abort(); /* a benchmark must not run with half-built scenes */
    /* the ground-truth patches: top-right / bottom-left, at least
     * 64 / 96 px inside the frame (exclusive x1/y1) */
    hi->x0 = 1184 + (int32_t)rng_below(rng, 161);
    hi->y0 = 64 + (int32_t)rng_below(rng, 97);
    hi->x1 = hi->x0 + HDR_PATCH;
    hi->y1 = hi->y0 + HDR_PATCH;
    ctx->hilite_l[s] = lhi;
    lo->x0 = 96 + (int32_t)rng_below(rng, 161);
    lo->y0 = 816 + (int32_t)rng_below(rng, 97);
    lo->x1 = lo->x0 + HDR_PATCH;
    lo->y1 = lo->y0 + HDR_PATCH;
    ctx->shadow_l[s] = llo;

    PAINT[arch % HDR_ARCHETYPES](L, rng);
    /* per-pixel scene noise: +-24 of 4096 (the patches overwrite it) */
    for (size_t i = 0; i < HDR_SCENE_PX; i++)
        L[i] = clamp12((int32_t)L[i] + (int32_t)rng_below(rng, 49) - 24);
    for (int32_t y = hi->y0; y < hi->y1; y++)
        for (int32_t x = hi->x0; x < hi->x1; x++)
            L[(size_t)y * HDR_SCENE_W + (size_t)x] = (uint16_t)lhi;
    for (int32_t y = lo->y0; y < lo->y1; y++)
        for (int32_t x = lo->x0; x < lo->x1; x++)
            L[(size_t)y * HDR_SCENE_W + (size_t)x] = (uint16_t)llo;

    /* the six exposures of the bracketed burst: byte = clamp((V * 2^k
     * * 255 + 2048) >> 12 + n_k, 0, 255), one +-3 noise draw per
     * pixel per exposure, shared by the three channels */
    for (unsigned k = 0; k < HDR_EXPOSURES; k++) {
        uint32_t e = 1u << k;
        uint8_t *dst = ctx->exposure[s][k];

        for (size_t p = 0; p < HDR_SCENE_PX; p++) {
            int32_t nk = (int32_t)rng_below(rng, 7) - 3;

            for (int c = 0; c < 3; c++) {
                int32_t v = ((int32_t)L[p] * (int32_t)ARCH[arch].tint[c])
                            >> 8;
                int32_t b;

                if (v > (int32_t)HDR_LUMA_MAX)
                    v = (int32_t)HDR_LUMA_MAX;
                b = ((v * (int32_t)e * 255 + 2048) >> 12) + nk;
                if (b < 0)
                    b = 0;
                if (b > 255)
                    b = 255;
                dst[p * 3 + c] = (uint8_t)b;
            }
        }
    }
    free(L);
}

/* --- pyramid geometry --------------------------------------------------------*/

/* Level sizes and per-plane offsets, derived once from the fixed scene
 * size (level l + 1 halves level l, half-up). gpyr holds HDR_LEVELS + 1
 * RGB planes (byte offsets), lpyr holds HDR_LEVELS int16 RGB planes and
 * acc holds HDR_LEVELS + 1 int32 RGB planes (element offsets — levels
 * 0..5 blend the Laplacians, level 6 the Gaussian residual), wgnorm
 * holds HDR_LEVELS + 1 single-channel planes (byte offsets). */
static struct {
    size_t w[HDR_LEVELS + 1], h[HDR_LEVELS + 1];
    size_t goff[HDR_LEVELS + 2];
    size_t loff[HDR_LEVELS + 2];
    size_t aoff[HDR_LEVELS + 2];
    size_t woff[HDR_LEVELS + 2];
    size_t ltotal, atotal;
} GEO;

static void geo_init(void)
{
    static int done;

    if (done)
        return;
    GEO.w[0] = HDR_SCENE_W;
    GEO.h[0] = HDR_SCENE_H;
    GEO.goff[0] = 0;
    GEO.loff[0] = 0;
    GEO.aoff[0] = 0;
    GEO.woff[0] = 0;
    for (unsigned l = 0; l <= HDR_LEVELS; l++) {
        size_t px = GEO.w[l] * GEO.h[l];

        if (l < HDR_LEVELS) {
            GEO.w[l + 1] = (GEO.w[l] + 1) / 2;
            GEO.h[l + 1] = (GEO.h[l] + 1) / 2;
        }
        GEO.goff[l + 1] = GEO.goff[l] + px * 3;
        GEO.loff[l + 1] = GEO.loff[l] + px * 3;
        GEO.aoff[l + 1] = GEO.aoff[l] + px * 3;
        GEO.woff[l + 1] = GEO.woff[l] + px;
    }
    GEO.ltotal = GEO.loff[HDR_LEVELS];
    GEO.atotal = GEO.aoff[HDR_LEVELS + 1];
    done = 1;
}

/* --- the integer kernels ---------------------------------------------------- */

void hdr_weights(const uint8_t *rgb, size_t w, size_t h,
                 uint8_t *gray, uint16_t *w16)
{
    img_grayscale(rgb, w, h, gray);
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
            size_t i = y * w + x;
            size_t iu = (y ? y - 1 : 0) * w + x;
            size_t id = (y + 1 < h ? y + 1 : h - 1) * w + x;
            size_t il = y * w + (x ? x - 1 : 0);
            size_t ir = y * w + (x + 1 < w ? x + 1 : w - 1);
            int32_t lap = 4 * (int32_t)gray[i] -
                          (int32_t)gray[iu] - (int32_t)gray[id] -
                          (int32_t)gray[il] - (int32_t)gray[ir];
            int32_t al = lap < 0 ? -lap : lap;
            int32_t wc = 16 + (al + 2) / 4;
            const uint8_t *px = rgb + i * 3;
            int32_t mn = px[0], mx = px[0], ws, sq = 0, we;
            uint32_t prod;

            if (wc > 256)
                wc = 256;
            for (int c = 0; c < 3; c++) {
                int32_t v = px[c], d = v - 128;

                if (v < mn)
                    mn = v;
                if (v > mx)
                    mx = v;
                if (d < 0)
                    d = -d;
                sq += d * d;
            }
            ws = 16 + (mx - mn);
            if (ws > 256)
                ws = 256;
            /* linear decay from the midtone peak: 255 at sq = 0,
             * 0 at sq = 3 * 128^2 = 49152, half-up */
            we = 255 - (sq * 255 + 24576) / 49152;
            prod = (uint32_t)wc * (uint32_t)ws * (uint32_t)we;
            w16[i] = (uint16_t)(prod >> 8);
        }
}

void hdr_normalize(const uint16_t *const *w16, size_t n_px,
                   uint8_t *const *out, uint32_t *uniform)
{
    for (size_t p = 0; p < n_px; p++) {
        uint32_t s = 0;

        for (unsigned k = 0; k < HDR_EXPOSURES; k++)
            s += w16[k][p];
        if (s == 0) {
            /* every exposure fully unexposed: the documented uniform
             * fallback (the floor split of 255 over six planes) */
            for (unsigned k = 0; k < HDR_EXPOSURES - 1; k++)
                out[k][p] = 42;
            out[HDR_EXPOSURES - 1][p] = 45;
            if (uniform)
                (*uniform)++;
            continue;
        }
        {
            uint32_t acc = 0;

            for (unsigned k = 0; k < HDR_EXPOSURES - 1; k++) {
                uint32_t v = (uint32_t)w16[k][p] * 255u / s;

                out[k][p] = (uint8_t)v;
                acc += v;
            }
            out[HDR_EXPOSURES - 1][p] = (uint8_t)(255u - acc);
        }
    }
}

void hdr_down_rgb(uint8_t *dst, size_t dw, size_t dh,
                  const uint8_t *src, size_t sw, size_t sh)
{
    for (size_t y = 0; y < dh; y++) {
        size_t y0 = y * 2, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        const uint8_t *ra = src + y0 * sw * 3;
        const uint8_t *rb = src + y1 * sw * 3;

        for (size_t x = 0; x < dw; x++) {
            size_t x0 = x * 2, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;

            for (int c = 0; c < 3; c++)
                dst[(y * dw + x) * 3 + c] =
                    (uint8_t)((ra[x0 * 3 + c] + ra[x1 * 3 + c] +
                               rb[x0 * 3 + c] + rb[x1 * 3 + c] + 2) >> 2);
        }
    }
}

void hdr_down_gray(uint8_t *dst, size_t dw, size_t dh,
                   const uint8_t *src, size_t sw, size_t sh)
{
    for (size_t y = 0; y < dh; y++) {
        size_t y0 = y * 2, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        const uint8_t *ra = src + y0 * sw;
        const uint8_t *rb = src + y1 * sw;

        for (size_t x = 0; x < dw; x++) {
            size_t x0 = x * 2, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;

            dst[y * dw + x] =
                (uint8_t)((ra[x0] + ra[x1] + rb[x0] + rb[x1] + 2) >> 2);
        }
    }
}

void hdr_expand16(int16_t *dst, size_t dw, size_t dh,
                  const int16_t *src, size_t sw, size_t sh)
{
    /* Same source coordinate convention as img_scale_bilinear
     * ((x + 0.5) * sw / dw - 0.5 in 16.16, edge-clamped neighbours),
     * signed 16-bit values, half-up rounding toward +infinity: the
     * four products live at 16.16 * 16.16 = 32.32 and the final shift
     * is >> 32 with the +2^31 bias (arithmetic shift = floor, so
     * exact halves round up). Bilinear weights are convex, so |dst| is
     * bounded by max |src|. */
    uint64_t dx = ((uint64_t)sw << 16) / dw;
    uint64_t dy = ((uint64_t)sh << 16) / dh;
    int64_t sx0 = (int64_t)(((uint64_t)sw * 32768u) / dw) - 32768;
    int64_t sy0 = (int64_t)(((uint64_t)sh * 32768u) / dh) - 32768;

    for (size_t y = 0; y < dh; y++) {
        int64_t sy = sy0 + (int64_t)(dy * y);
        int64_t iy = sy >> 16; /* floor, handles the negative start */
        int64_t fy = sy - iy * 65536; /* no <<: iy can be negative (UB) */
        size_t ya = (size_t)(iy < 0 ? 0 : iy);
        size_t yb = (ya + 1 < sh) ? ya + 1 : sh - 1;

        for (size_t x = 0; x < dw; x++) {
            int64_t sx = sx0 + (int64_t)(dx * x);
            int64_t ix = sx >> 16;
            int64_t fx = sx - ix * 65536; /* no <<: ix can be negative (UB) */
            size_t xa = (size_t)(ix < 0 ? 0 : ix);
            size_t xb = (xa + 1 < sw) ? xa + 1 : sw - 1;
            int64_t wx0 = 65536 - fx, wx1 = fx;
            int64_t wy0 = 65536 - fy, wy1 = fy;

            for (int c = 0; c < 3; c++) {
                int64_t top = (int64_t)src[(ya * sw + xa) * 3 + c] * wx0 +
                              (int64_t)src[(ya * sw + xb) * 3 + c] * wx1;
                int64_t bot = (int64_t)src[(yb * sw + xa) * 3 + c] * wx0 +
                              (int64_t)src[(yb * sw + xb) * 3 + c] * wx1;
                int64_t v = (top * wy0 + bot * wy1 +
                             ((int64_t)1 << 31)) >> 32;

                dst[(y * dw + x) * 3 + c] = (int16_t)v;
            }
        }
    }
}

/* --- the pipeline ------------------------------------------------------------*/

/* Buffer roles: the six exposures (read-only inputs); wprod <- raw
 * weight products, wnorm <- normalized stack (digest); then per
 * exposure: gpyr <- its Gaussian pyramid, lpyr <- its Laplacian
 * pyramid, wgnorm <- the Gaussian pyramid of its weight plane, all
 * accumulated into acc; finally the collapse walks acc top-down into
 * cur/up16 and writes s->fused. */

int hdr_process(const uint8_t *const *exposures, hdr_scratch *s,
                hdr_result *out)
{
    size_t n = HDR_SCENE_PX;
    const uint16_t *wp[HDR_EXPOSURES];
    uint8_t *wn[HDR_EXPOSURES];
    int16_t *cur;
    uint32_t uniform = 0, hi_clip = 0, lo_black = 0;

    if (!exposures || !s || !out || !s->gray || !s->wprod || !s->wnorm ||
        !s->wgnorm || !s->gpyr || !s->lpyr || !s->up8 || !s->acc ||
        !s->cur || !s->up16 || !s->fused)
        return -1;
    for (unsigned k = 0; k < HDR_EXPOSURES; k++) {
        if (!exposures[k])
            return -1;
        wp[k] = s->wprod + (size_t)k * n;
        wn[k] = s->wnorm + (size_t)k * n;
    }

    geo_init();
    memset(s->acc, 0, GEO.atotal * sizeof(int32_t));

    for (unsigned k = 0; k < HDR_EXPOSURES; k++)
        hdr_weights(exposures[k], HDR_SCENE_W, HDR_SCENE_H, s->gray,
                    s->wprod + (size_t)k * n);
    hdr_normalize(wp, n, wn, &uniform);
    sha1(s->wnorm, HDR_EXPOSURES * n, out->weights_digest);

    for (unsigned k = 0; k < HDR_EXPOSURES; k++) {
        /* the exposure's Gaussian pyramid (levels 0..HDR_LEVELS) */
        memcpy(s->gpyr, exposures[k], HDR_SCENE_BYTES);
        for (unsigned l = 0; l < HDR_LEVELS; l++)
            hdr_down_rgb(s->gpyr + GEO.goff[l + 1], GEO.w[l + 1],
                         GEO.h[l + 1], s->gpyr + GEO.goff[l],
                         GEO.w[l], GEO.h[l]);
        /* its Laplacian pyramid (levels 0..HDR_LEVELS-1): level l is
         * the level-l plane minus the bilinear expand of level l + 1 */
        for (unsigned l = 0; l < HDR_LEVELS; l++) {
            size_t px = GEO.w[l] * GEO.h[l];

            img_scale_bilinear(s->up8, GEO.w[l], GEO.h[l],
                               s->gpyr + GEO.goff[l + 1],
                               GEO.w[l + 1], GEO.h[l + 1]);
            for (size_t i = 0; i < px * 3; i++)
                s->lpyr[GEO.loff[l] + i] =
                    (int16_t)((int32_t)s->gpyr[GEO.goff[l] + i] -
                              (int32_t)s->up8[i]);
        }
        /* the Gaussian pyramid of this exposure's weight plane */
        memcpy(s->wgnorm, s->wnorm + (size_t)k * n, n);
        for (unsigned l = 0; l < HDR_LEVELS; l++)
            hdr_down_gray(s->wgnorm + GEO.woff[l + 1], GEO.w[l + 1],
                          GEO.h[l + 1], s->wgnorm + GEO.woff[l],
                          GEO.w[l], GEO.h[l]);
        /* accumulate the level blends: raw weight*value products, the
         * division by 255 happens once per pixel at collapse */
        for (unsigned l = 0; l < HDR_LEVELS; l++) {
            const uint8_t *wg = s->wgnorm + GEO.woff[l];
            const int16_t *lp = s->lpyr + GEO.loff[l];
            int32_t *ac = s->acc + GEO.aoff[l];
            size_t px = GEO.w[l] * GEO.h[l];

            for (size_t p = 0; p < px; p++) {
                int32_t wv = wg[p];

                ac[p * 3] += wv * lp[p * 3];
                ac[p * 3 + 1] += wv * lp[p * 3 + 1];
                ac[p * 3 + 2] += wv * lp[p * 3 + 2];
            }
        }
        {
            /* the deepest level blends the Gaussian residuals */
            const uint8_t *wg = s->wgnorm + GEO.woff[HDR_LEVELS];
            const uint8_t *gp = s->gpyr + GEO.goff[HDR_LEVELS];
            int32_t *ac = s->acc + GEO.aoff[HDR_LEVELS];
            size_t px = GEO.w[HDR_LEVELS] * GEO.h[HDR_LEVELS];

            for (size_t p = 0; p < px; p++) {
                int32_t wv = wg[p];

                ac[p * 3] += wv * gp[p * 3];
                ac[p * 3 + 1] += wv * gp[p * 3 + 1];
                ac[p * 3 + 2] += wv * gp[p * 3 + 2];
            }
        }
    }

    /* collapse: cur = q(acc(6)); level l = 5..0: cur = expand16(cur) +
     * q(acc(l)). The accumulator products are bounded (six planes of
     * weight 255 times +-255 Laplacians), so every q(acc(l)) stays in
     * +-257 and the running plane within +-1540 — int16 exact. */
    cur = s->cur;
    {
        size_t px = GEO.w[HDR_LEVELS] * GEO.h[HDR_LEVELS];
        const int32_t *ac = s->acc + GEO.aoff[HDR_LEVELS];

        for (size_t i = 0; i < px * 3; i++)
            cur[i] = (int16_t)q255(ac[i]);
    }
    for (unsigned l = HDR_LEVELS; l-- > 0;) {
        size_t px = GEO.w[l] * GEO.h[l];
        const int32_t *ac = s->acc + GEO.aoff[l];

        hdr_expand16(s->up16, GEO.w[l], GEO.h[l], cur,
                     GEO.w[l + 1], GEO.h[l + 1]);
        for (size_t i = 0; i < px * 3; i++)
            cur[i] = (int16_t)(s->up16[i] + q255(ac[i]));
    }

    for (size_t p = 0; p < n; p++) {
        uint8_t rgb[3];
        int clipped = 1, black = 1;

        for (int c = 0; c < 3; c++) {
            int32_t v = cur[p * 3 + c];

            if (v < 0)
                v = 0;
            if (v > 255)
                v = 255;
            rgb[c] = (uint8_t)v;
            if (v != 255)
                clipped = 0;
            if (v != 0)
                black = 0;
        }
        s->fused[p * 3] = rgb[0];
        s->fused[p * 3 + 1] = rgb[1];
        s->fused[p * 3 + 2] = rgb[2];
        if (clipped)
            hi_clip++;
        if (black)
            lo_black++;
    }
    sha1(s->fused, HDR_SCENE_BYTES, out->fused_digest);
    out->uniform = uniform;
    out->hi_clip = hi_clip;
    out->lo_black = lo_black;
    s->res = *out; /* the scratch keeps the result of the last pipeline */
    return 0;
}

/* --- scratch / prepare / run -------------------------------------------------*/

int hdr_scratch_init(hdr_scratch *s)
{
    if (!s)
        return -1;
    geo_init();
    memset(s, 0, sizeof(*s));
    s->gray = malloc(HDR_SCENE_PX);
    s->wprod = malloc(HDR_EXPOSURES * HDR_SCENE_PX * sizeof(uint16_t));
    s->wnorm = malloc(HDR_EXPOSURES * HDR_SCENE_PX);
    s->wgnorm = malloc(GEO.woff[HDR_LEVELS + 1]);
    s->gpyr = malloc(GEO.goff[HDR_LEVELS + 1]);
    s->lpyr = malloc(GEO.ltotal * sizeof(int16_t));
    s->up8 = malloc(HDR_SCENE_BYTES);
    s->acc = malloc(GEO.atotal * sizeof(int32_t));
    s->cur = malloc(HDR_SCENE_PX * 3 * sizeof(int16_t));
    s->up16 = malloc(HDR_SCENE_PX * 3 * sizeof(int16_t));
    s->fused = malloc(HDR_SCENE_BYTES);
    if (!s->gray || !s->wprod || !s->wnorm || !s->wgnorm || !s->gpyr ||
        !s->lpyr || !s->up8 || !s->acc || !s->cur || !s->up16 ||
        !s->fused) {
        hdr_scratch_free(s);
        return -1;
    }
    return 0;
}

void hdr_scratch_free(hdr_scratch *s)
{
    if (!s)
        return;
    free(s->gray);
    free(s->wprod);
    free(s->wnorm);
    free(s->wgnorm);
    free(s->gpyr);
    free(s->lpyr);
    free(s->up8);
    free(s->acc);
    free(s->cur);
    free(s->up16);
    free(s->fused);
    memset(s, 0, sizeof(*s));
}

hdr_ctx *hdr_prepare(unsigned seed)
{
    hdr_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t prng;

    if (!ctx)
        return NULL;
    geo_init();
    rng_init(&prng, seed);
    for (unsigned s = 0; s < HDR_SCENES; s++) {
        unsigned arch = s % HDR_ARCHETYPES;
        hdr_scratch sc;

        for (unsigned k = 0; k < HDR_EXPOSURES; k++) {
            ctx->exposure[s][k] = malloc(HDR_SCENE_BYTES);
            if (!ctx->exposure[s][k]) {
                hdr_free(ctx);
                return NULL;
            }
        }
        ctx->arch[s] = arch;
        gen_scene(ctx, s, arch, &prng);

        /* reference pipeline, off the clock */
        if (hdr_scratch_init(&sc) != 0) {
            hdr_free(ctx);
            return NULL;
        }
        if (hdr_process((const uint8_t *const *)ctx->exposure[s], &sc,
                        &ctx->ref[s]) != 0 ||
            ctx->ref[s].uniform != 0 || ctx->ref[s].lo_black != 0) {
            hdr_scratch_free(&sc);
            hdr_free(ctx);
            return NULL;
        }
        hdr_scratch_free(&sc);
    }
    return ctx;
}

void hdr_free(hdr_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned s = 0; s < HDR_SCENES; s++)
        for (unsigned k = 0; k < HDR_EXPOSURES; k++)
            free(ctx->exposure[s][k]);
    free(ctx);
}

/* One thread's slice: contiguous scene instances, own scratch —
 * threads never share writable state (shared-task harness contract). */
typedef struct {
    hdr_ctx *ctx;
    hdr_scratch sc;
    size_t begin, end; /* instance range [begin, end) of the pass */
} hdr_slice;

static int hdr_cycle(void *arg)
{
    hdr_slice *sl = arg;
    hdr_ctx *ctx = sl->ctx;

    for (size_t i = sl->begin; i < sl->end; i++) {
        unsigned s = (unsigned)(i % HDR_SCENES);
        hdr_result r;
        const hdr_result *ref = &ctx->ref[s];

        if (hdr_process((const uint8_t *const *)ctx->exposure[s], &sl->sc,
                        &r) != 0)
            return -1;
        if (memcmp(r.weights_digest, ref->weights_digest, 20) != 0 ||
            memcmp(r.fused_digest, ref->fused_digest, 20) != 0 ||
            r.uniform != ref->uniform ||
            r.hi_clip != ref->hi_clip ||
            r.lo_black != ref->lo_black ||
            r.uniform != 0 || /* the corpus keeps every pixel alive */
            r.lo_black != 0) /* the fusion must not crush to black */
            return -1;
    }
    return 0;
}

uint64_t hdr_task_pixels(unsigned multi)
{
    return (multi ? HDR_SCENES_MULTI : HDR_SCENES) *
           (uint64_t)HDR_SCENE_PX;
}

int hdr_run(hdr_ctx *ctx, unsigned runs, unsigned *threads,
            double *throughput_mpx, uint64_t *pixels)
{
    unsigned t_count;
    unsigned scene_set;
    hdr_slice *slices;
    void **ctxs;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    int rc = -1;

    if (!ctx || !threads || !throughput_mpx || !pixels)
        return -1;
    t_count = *threads;
    scene_set = t_count > 1 ? HDR_SCENES_MULTI : HDR_SCENES;

    if (t_count > scene_set) /* one scene instance per thread at least */
        t_count = scene_set;
    if (t_count < 1)
        t_count = 1;

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;

    {
        size_t per = scene_set / t_count;
        unsigned extra = scene_set % t_count;
        size_t begin = 0;

        for (unsigned t = 0; t < t_count; t++) {
            size_t count = per + (t < extra ? 1u : 0u);

            slices[t].ctx = ctx;
            slices[t].begin = begin;
            slices[t].end = begin + count;
            begin += count;
            if (hdr_scratch_init(&slices[t].sc) != 0)
                goto out;
            ctxs[t] = &slices[t];
        }
    }

    rc = harness_run(t_count, runs, hdr_cycle, ctxs, elapsed);
    if (rc == 0) {
        uint64_t npx = hdr_task_pixels(t_count > 1 ? 1u : 0u);

        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0
                           ? (double)npx / elapsed[r] / 1e6
                           : -1.0;
        *throughput_mpx = lb_median(rates, runs);
        *threads = t_count;
        *pixels = npx;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++)
            hdr_scratch_free(&slices[t].sc);
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors ---------------------------------------------------------------*/

const uint8_t *hdr_exposure(const hdr_ctx *ctx, unsigned scene,
                            unsigned k)
{
    return ctx && scene < HDR_SCENES && k < HDR_EXPOSURES
               ? ctx->exposure[scene][k]
               : NULL;
}

size_t hdr_exposure_len(const hdr_ctx *ctx, unsigned scene)
{
    (void)scene;
    return ctx ? HDR_SCENE_BYTES : 0;
}

const hdr_result *hdr_reference(const hdr_ctx *ctx, unsigned scene)
{
    return ctx && scene < HDR_SCENES ? &ctx->ref[scene] : NULL;
}

unsigned hdr_archetype(const hdr_ctx *ctx, unsigned scene)
{
    return ctx && scene < HDR_SCENES ? ctx->arch[scene] : 0;
}

static hdr_box zero_box(void)
{
    hdr_box zero = { 0, 0, 0, 0 };

    return zero;
}

hdr_box hdr_hilite_box(const hdr_ctx *ctx, unsigned scene)
{
    if (!ctx || scene >= HDR_SCENES)
        return zero_box();
    return ctx->hilite[scene];
}

int32_t hdr_hilite_luma(const hdr_ctx *ctx, unsigned scene)
{
    return ctx && scene < HDR_SCENES ? ctx->hilite_l[scene] : 0;
}

hdr_box hdr_shadow_box(const hdr_ctx *ctx, unsigned scene)
{
    if (!ctx || scene >= HDR_SCENES)
        return zero_box();
    return ctx->shadow[scene];
}

int32_t hdr_shadow_luma(const hdr_ctx *ctx, unsigned scene)
{
    return ctx && scene < HDR_SCENES ? ctx->shadow_l[scene] : 0;
}
