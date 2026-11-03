/* LotofBench — Horizon Detection workload (workload #12).
 *
 * Mirrors the twelfth Geekbench 6 CPU workload: detect the horizon line
 * of a photo by reducing it to edges with the Canny detector, then
 * applying the Hough transform. LotofBench reproduces the shape
 * dependency-free, with the same honesty rules as the other workloads:
 *
 *   - deterministic synthetic input — a seeded set of 8 photos
 *     (1600x1200 RGB) in six landscape archetype families (ocean,
 *     mountains, desert, city skyline, forest, fields) that cycle the
 *     way a real import session does. Every photo carries a planted
 *     horizon: an exact straight boundary between a bright sky gradient
 *     and a darker ground gradient (luma gap >= 113 everywhere), with a
 *     seeded slope in [-0.5, 0.5] and a position that keeps the
 *     boundary inside the frame over the full width. Short dark
 *     silhouettes (peaks, buildings, trees, cacti — every straight run
 *     <= 80 px, per-bin Hough votes <= 133) and sub-threshold texture
 *     (tone deltas <= 20, noise <= 8) are the distractors. Fixed --seed
 *     means byte-identical photos on every machine; generation is off
 *     the clock — the stand-in for the camera import;
 *   - a from-scratch Canny pipeline per photo — Rec.601 grayscale and a
 *     separable [1 4 6 4 1]/16 Gaussian blur (shared src/imgops.c
 *     kernels), Sobel gradients with the |gx| + |gy| magnitude and a
 *     4-sector direction quantization, non-maximum suppression, double
 *     threshold (weak 100, strong 200) with 8-connected hysteresis, a
 *     720-bin (0.25 degrees) x 1-px-rho Hough transform and a
 *     deterministic first-max peak. Integer arithmetic throughout — no
 *     floating point — so edge maps and detected cells are
 *     bit-identical across machines and compilers;
 *   - correctness inside the timed cycle — a pass only counts if every
 *     photo reproduces its prepare-time reference detection (edge-map
 *     SHA-1, theta bin, rho bin, vote count) AND the detected line
 *     matches the planted ground truth within documented tolerances
 *     (1.0 degree of normal angle, 3 px of rho, votes >= width/4). Any
 *     divergence aborts the run instead of scoring;
 *   - shared-task multi-core through the common harness — the
 *     multi-core pass detects 32 photos (4x the set) split into equal
 *     contiguous slices, one per thread.
 *
 * The throughput metric is input pixels: every photo contributes its
 * full 1600x1200 frame, exact and machine-independent, the
 * photo-workload analogue of the corpus size.
 *
 * Deliberate divergences from Geekbench 6 (all documented in
 * docs/benchmarks.md): synthetic photos with planted straight horizons
 * instead of real photographs (the ground truth needs a known line),
 * 0.25-degree Hough bins instead of OpenCV's 1 degree (keeps tilted
 * horizons above the distractor votes), and our own thresholds — the
 * pipeline structure is Canny + Hough, as in GB6.
 */

#include "workload_horizon.h"

#include "harness.h"
#include "imgops.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <stdlib.h>
#include <string.h>

#define PHOTO_BYTES ((size_t)HD_PHOTO_W * HD_PHOTO_H * 3)

/* --- small integer helpers ------------------------------------------------ */

static int32_t div_round_half_away(int32_t num, int32_t den)
{
    /* C division truncates toward zero; this rounds half away from
     * zero, deterministically on every platform. */
    if (num >= 0)
        return (num + den / 2) / den;
    return -((-num + den / 2) / den);
}

static uint8_t clamp_u8(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

/* Exact integer square root (bit-by-bit), for the rho range and the
 * disk shapes. */
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

/* cos/sin of the 720 bin centers (bin k = k * 0.25 degrees),
 * scaled by 1024 and rounded half away from zero. The true
 * values sit at least 0.0011 away from every .5 rounding
 * boundary, so any libm's trigonometry rounds to this exact
 * table — it is machine-independent by construction. */
static const int16_t HD_COS1024[HD_THETA_STEPS] = {
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1023,  1023,  1023,  1023,
     1023,  1022,  1022,  1022,  1022,  1021,  1021,  1020,  1020,  1020,  1019,  1019,
     1018,  1018,  1017,  1017,  1016,  1016,  1015,  1015,  1014,  1013,  1013,  1012,
     1011,  1011,  1010,  1009,  1008,  1008,  1007,  1006,  1005,  1004,  1003,  1003,
     1002,  1001,  1000,   999,   998,   997,   996,   995,   994,   992,   991,   990,
      989,   988,   987,   986,   984,   983,   982,   981,   979,   978,   977,   975,
      974,   972,   971,   970,   968,   967,   965,   964,   962,   961,   959,   958,
      956,   954,   953,   951,   949,   948,   946,   944,   943,   941,   939,   937,
      935,   934,   932,   930,   928,   926,   924,   922,   920,   918,   916,   914,
      912,   910,   908,   906,   904,   902,   900,   898,   896,   893,   891,   889,
      887,   885,   882,   880,   878,   875,   873,   871,   868,   866,   864,   861,
      859,   856,   854,   851,   849,   846,   844,   841,   839,   836,   834,   831,
      828,   826,   823,   820,   818,   815,   812,   810,   807,   804,   801,   799,
      796,   793,   790,   787,   784,   782,   779,   776,   773,   770,   767,   764,
      761,   758,   755,   752,   749,   746,   743,   740,   737,   733,   730,   727,
      724,   721,   718,   715,   711,   708,   705,   702,   698,   695,   692,   689,
      685,   682,   679,   675,   672,   668,   665,   662,   658,   655,   651,   648,
      644,   641,   637,   634,   630,   627,   623,   620,   616,   613,   609,   606,
      602,   598,   595,   591,   587,   584,   580,   576,   573,   569,   565,   561,
      558,   554,   550,   546,   543,   539,   535,   531,   527,   524,   520,   516,
      512,   508,   504,   500,   496,   493,   489,   485,   481,   477,   473,   469,
      465,   461,   457,   453,   449,   445,   441,   437,   433,   429,   425,   421,
      416,   412,   408,   404,   400,   396,   392,   388,   384,   379,   375,   371,
      367,   363,   359,   354,   350,   346,   342,   338,   333,   329,   325,   321,
      316,   312,   308,   304,   299,   295,   291,   287,   282,   278,   274,   269,
      265,   261,   256,   252,   248,   243,   239,   235,   230,   226,   222,   217,
      213,   209,   204,   200,   195,   191,   187,   182,   178,   173,   169,   165,
      160,   156,   151,   147,   143,   138,   134,   129,   125,   120,   116,   111,
      107,   103,    98,    94,    89,    85,    80,    76,    71,    67,    63,    58,
       54,    49,    45,    40,    36,    31,    27,    22,    18,    13,     9,     4,
        0,    -4,    -9,   -13,   -18,   -22,   -27,   -31,   -36,   -40,   -45,   -49,
      -54,   -58,   -63,   -67,   -71,   -76,   -80,   -85,   -89,   -94,   -98,  -103,
     -107,  -111,  -116,  -120,  -125,  -129,  -134,  -138,  -143,  -147,  -151,  -156,
     -160,  -165,  -169,  -173,  -178,  -182,  -187,  -191,  -195,  -200,  -204,  -209,
     -213,  -217,  -222,  -226,  -230,  -235,  -239,  -243,  -248,  -252,  -256,  -261,
     -265,  -269,  -274,  -278,  -282,  -287,  -291,  -295,  -299,  -304,  -308,  -312,
     -316,  -321,  -325,  -329,  -333,  -338,  -342,  -346,  -350,  -354,  -359,  -363,
     -367,  -371,  -375,  -379,  -384,  -388,  -392,  -396,  -400,  -404,  -408,  -412,
     -416,  -421,  -425,  -429,  -433,  -437,  -441,  -445,  -449,  -453,  -457,  -461,
     -465,  -469,  -473,  -477,  -481,  -485,  -489,  -493,  -496,  -500,  -504,  -508,
     -512,  -516,  -520,  -524,  -527,  -531,  -535,  -539,  -543,  -546,  -550,  -554,
     -558,  -561,  -565,  -569,  -573,  -576,  -580,  -584,  -587,  -591,  -595,  -598,
     -602,  -606,  -609,  -613,  -616,  -620,  -623,  -627,  -630,  -634,  -637,  -641,
     -644,  -648,  -651,  -655,  -658,  -662,  -665,  -668,  -672,  -675,  -679,  -682,
     -685,  -689,  -692,  -695,  -698,  -702,  -705,  -708,  -711,  -715,  -718,  -721,
     -724,  -727,  -730,  -733,  -737,  -740,  -743,  -746,  -749,  -752,  -755,  -758,
     -761,  -764,  -767,  -770,  -773,  -776,  -779,  -782,  -784,  -787,  -790,  -793,
     -796,  -799,  -801,  -804,  -807,  -810,  -812,  -815,  -818,  -820,  -823,  -826,
     -828,  -831,  -834,  -836,  -839,  -841,  -844,  -846,  -849,  -851,  -854,  -856,
     -859,  -861,  -864,  -866,  -868,  -871,  -873,  -875,  -878,  -880,  -882,  -885,
     -887,  -889,  -891,  -893,  -896,  -898,  -900,  -902,  -904,  -906,  -908,  -910,
     -912,  -914,  -916,  -918,  -920,  -922,  -924,  -926,  -928,  -930,  -932,  -934,
     -935,  -937,  -939,  -941,  -943,  -944,  -946,  -948,  -949,  -951,  -953,  -954,
     -956,  -958,  -959,  -961,  -962,  -964,  -965,  -967,  -968,  -970,  -971,  -972,
     -974,  -975,  -977,  -978,  -979,  -981,  -982,  -983,  -984,  -986,  -987,  -988,
     -989,  -990,  -991,  -992,  -994,  -995,  -996,  -997,  -998,  -999, -1000, -1001,
    -1002, -1003, -1003, -1004, -1005, -1006, -1007, -1008, -1008, -1009, -1010, -1011,
    -1011, -1012, -1013, -1013, -1014, -1015, -1015, -1016, -1016, -1017, -1017, -1018,
    -1018, -1019, -1019, -1020, -1020, -1020, -1021, -1021, -1022, -1022, -1022, -1022,
    -1023, -1023, -1023, -1023, -1023, -1024, -1024, -1024, -1024, -1024, -1024, -1024,
};

/* cos/sin of the 720 bin centers (bin k = k * 0.25 degrees),
 * scaled by 1024 and rounded half away from zero. The true
 * values sit at least 0.0011 away from every .5 rounding
 * boundary, so any libm's trigonometry rounds to this exact
 * table — it is machine-independent by construction. */
static const int16_t HD_SIN1024[HD_THETA_STEPS] = {
        0,     4,     9,    13,    18,    22,    27,    31,    36,    40,    45,    49,
       54,    58,    63,    67,    71,    76,    80,    85,    89,    94,    98,   103,
      107,   111,   116,   120,   125,   129,   134,   138,   143,   147,   151,   156,
      160,   165,   169,   173,   178,   182,   187,   191,   195,   200,   204,   209,
      213,   217,   222,   226,   230,   235,   239,   243,   248,   252,   256,   261,
      265,   269,   274,   278,   282,   287,   291,   295,   299,   304,   308,   312,
      316,   321,   325,   329,   333,   338,   342,   346,   350,   354,   359,   363,
      367,   371,   375,   379,   384,   388,   392,   396,   400,   404,   408,   412,
      416,   421,   425,   429,   433,   437,   441,   445,   449,   453,   457,   461,
      465,   469,   473,   477,   481,   485,   489,   493,   496,   500,   504,   508,
      512,   516,   520,   524,   527,   531,   535,   539,   543,   546,   550,   554,
      558,   561,   565,   569,   573,   576,   580,   584,   587,   591,   595,   598,
      602,   606,   609,   613,   616,   620,   623,   627,   630,   634,   637,   641,
      644,   648,   651,   655,   658,   662,   665,   668,   672,   675,   679,   682,
      685,   689,   692,   695,   698,   702,   705,   708,   711,   715,   718,   721,
      724,   727,   730,   733,   737,   740,   743,   746,   749,   752,   755,   758,
      761,   764,   767,   770,   773,   776,   779,   782,   784,   787,   790,   793,
      796,   799,   801,   804,   807,   810,   812,   815,   818,   820,   823,   826,
      828,   831,   834,   836,   839,   841,   844,   846,   849,   851,   854,   856,
      859,   861,   864,   866,   868,   871,   873,   875,   878,   880,   882,   885,
      887,   889,   891,   893,   896,   898,   900,   902,   904,   906,   908,   910,
      912,   914,   916,   918,   920,   922,   924,   926,   928,   930,   932,   934,
      935,   937,   939,   941,   943,   944,   946,   948,   949,   951,   953,   954,
      956,   958,   959,   961,   962,   964,   965,   967,   968,   970,   971,   972,
      974,   975,   977,   978,   979,   981,   982,   983,   984,   986,   987,   988,
      989,   990,   991,   992,   994,   995,   996,   997,   998,   999,  1000,  1001,
     1002,  1003,  1003,  1004,  1005,  1006,  1007,  1008,  1008,  1009,  1010,  1011,
     1011,  1012,  1013,  1013,  1014,  1015,  1015,  1016,  1016,  1017,  1017,  1018,
     1018,  1019,  1019,  1020,  1020,  1020,  1021,  1021,  1022,  1022,  1022,  1022,
     1023,  1023,  1023,  1023,  1023,  1024,  1024,  1024,  1024,  1024,  1024,  1024,
     1024,  1024,  1024,  1024,  1024,  1024,  1024,  1024,  1023,  1023,  1023,  1023,
     1023,  1022,  1022,  1022,  1022,  1021,  1021,  1020,  1020,  1020,  1019,  1019,
     1018,  1018,  1017,  1017,  1016,  1016,  1015,  1015,  1014,  1013,  1013,  1012,
     1011,  1011,  1010,  1009,  1008,  1008,  1007,  1006,  1005,  1004,  1003,  1003,
     1002,  1001,  1000,   999,   998,   997,   996,   995,   994,   992,   991,   990,
      989,   988,   987,   986,   984,   983,   982,   981,   979,   978,   977,   975,
      974,   972,   971,   970,   968,   967,   965,   964,   962,   961,   959,   958,
      956,   954,   953,   951,   949,   948,   946,   944,   943,   941,   939,   937,
      935,   934,   932,   930,   928,   926,   924,   922,   920,   918,   916,   914,
      912,   910,   908,   906,   904,   902,   900,   898,   896,   893,   891,   889,
      887,   885,   882,   880,   878,   875,   873,   871,   868,   866,   864,   861,
      859,   856,   854,   851,   849,   846,   844,   841,   839,   836,   834,   831,
      828,   826,   823,   820,   818,   815,   812,   810,   807,   804,   801,   799,
      796,   793,   790,   787,   784,   782,   779,   776,   773,   770,   767,   764,
      761,   758,   755,   752,   749,   746,   743,   740,   737,   733,   730,   727,
      724,   721,   718,   715,   711,   708,   705,   702,   698,   695,   692,   689,
      685,   682,   679,   675,   672,   668,   665,   662,   658,   655,   651,   648,
      644,   641,   637,   634,   630,   627,   623,   620,   616,   613,   609,   606,
      602,   598,   595,   591,   587,   584,   580,   576,   573,   569,   565,   561,
      558,   554,   550,   546,   543,   539,   535,   531,   527,   524,   520,   516,
      512,   508,   504,   500,   496,   493,   489,   485,   481,   477,   473,   469,
      465,   461,   457,   453,   449,   445,   441,   437,   433,   429,   425,   421,
      416,   412,   408,   404,   400,   396,   392,   388,   384,   379,   375,   371,
      367,   363,   359,   354,   350,   346,   342,   338,   333,   329,   325,   321,
      316,   312,   308,   304,   299,   295,   291,   287,   282,   278,   274,   269,
      265,   261,   256,   252,   248,   243,   239,   235,   230,   226,   222,   217,
      213,   209,   204,   200,   195,   191,   187,   182,   178,   173,   169,   165,
      160,   156,   151,   147,   143,   138,   134,   129,   125,   120,   116,   111,
      107,   103,    98,    94,    89,    85,    80,    76,    71,    67,    63,    58,
       54,    49,    45,    40,    36,    31,    27,    22,    18,    13,     9,     4,
};

void hd_trig(unsigned theta_bin, int32_t *cos1024, int32_t *sin1024)
{
    if (theta_bin >= HD_THETA_STEPS) {
        *cos1024 = 0;
        *sin1024 = 0;
        return;
    }
    *cos1024 = HD_COS1024[theta_bin];
    *sin1024 = HD_SIN1024[theta_bin];
}

/* --- corpus generation -----------------------------------------------------*/

/* Archetype palettes: sky gradient above the horizon, ground gradient
 * below (RGB). The luma gap between the two sides of the boundary is
 * >= 113 at every row in [26, 1174] — the horizon is always the
 * strongest edge by a wide margin (verified in the unit tests). */
typedef struct {
    uint8_t sky_top[3], sky_bottom[3];
    uint8_t ground_top[3], ground_bottom[3];
    unsigned noise_amp;
    const char *name;
} hd_scene;

static const hd_scene ARCHETYPES[6] = {
    { { 135, 190, 235 }, { 200, 225, 245 }, { 10, 40, 60 }, { 20, 60, 85 },
      8, "ocean" },
    { { 250, 235, 200 }, { 220, 205, 175 }, { 60, 55, 50 }, { 30, 28, 25 },
      8, "mountains" },
    { { 250, 225, 180 }, { 245, 218, 170 }, { 120, 85, 50 }, { 85, 55, 30 },
      8, "desert" },
    { { 180, 160, 200 }, { 230, 190, 160 }, { 35, 35, 40 }, { 20, 20, 24 },
      8, "city" },
    { { 210, 230, 240 }, { 190, 215, 230 }, { 70, 110, 45 }, { 40, 75, 30 },
      8, "forest" },
    { { 235, 240, 250 }, { 215, 225, 240 }, { 95, 130, 50 }, { 60, 90, 35 },
      8, "fields" },
};

/* The strong shapes (peaks, buildings, trees, cacti) are dark
 * silhouettes: each channel is darkened by 200 (clamped), so against a
 * sky of luma >= 170 the step clears the strong threshold by >= 4x. */

/* Apply a tone delta to one pixel if it lies in the given region (sky
 * for bright shapes, ground for dark ones) — shapes never shrink the
 * horizon step, they only move each side further from the boundary. */
static void tint_pixel(uint8_t *img, size_t x, size_t y, int32_t tone)
{
    size_t off = (y * HD_PHOTO_W + x) * 3;

    for (int c = 0; c < 3; c++)
        img[off + c] = clamp_u8((int32_t)img[off + c] + tone);
}

/* Bright disk (sun / cloud puff) in the sky region. */
static void draw_disk(uint8_t *img, int32_t dcx, int32_t dcy, uint32_t r,
                      int32_t tone, const int32_t *ybcol)
{
    int32_t y0 = dcy - (int32_t)r > 0 ? dcy - (int32_t)r : 0;
    int32_t y1 = dcy + (int32_t)r < (int32_t)HD_PHOTO_H
                     ? dcy + (int32_t)r : (int32_t)HD_PHOTO_H;

    for (int32_t y = y0; y < y1; y++) {
        int64_t dy = y - dcy;
        uint32_t half = isqrt64((uint64_t)r * r - (uint64_t)(dy * dy));
        int32_t x0 = dcx - (int32_t)half > 0 ? dcx - (int32_t)half : 0;
        int32_t x1 = dcx + (int32_t)half < (int32_t)HD_PHOTO_W
                         ? dcx + (int32_t)half : (int32_t)HD_PHOTO_W;

        for (int32_t x = x0; x < x1; x++)
            if (y < ybcol[x])
                tint_pixel(img, (size_t)x, (size_t)y, tone);
    }
}

/* Dark full-width band in the ground region (dune shading, field rows). */
static void draw_band(uint8_t *img, int32_t b0, int32_t bh, int32_t tone,
                      const int32_t *ybcol)
{
    int32_t y0 = b0 > 0 ? b0 : 0;
    int32_t y1 = b0 + bh < (int32_t)HD_PHOTO_H ? b0 + bh : (int32_t)HD_PHOTO_H;

    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = 0; x < (int32_t)HD_PHOTO_W; x++)
            if (y >= ybcol[x])
                tint_pixel(img, (size_t)x, (size_t)y, tone);
}

/* Dark triangle (peak / tree) with its base on the horizon, rising into
 * the sky; side runs are <= sqrt(36^2 + 56^2) ~= 67 px. */
static void draw_triangle(uint8_t *img, int32_t xc, int32_t bw, int32_t ht,
                          const int32_t *ybcol)
{
    int32_t half_w = bw / 2;
    int32_t base = ybcol[xc];

    for (int32_t dy = 0; dy < ht; dy++) {
        int32_t y = base - 1 - dy;
        int32_t half = dy * half_w / ht;
        int32_t x0 = xc - half > 0 ? xc - half : 0;
        int32_t x1 = xc + half + 1 < (int32_t)HD_PHOTO_W
                         ? xc + half + 1 : (int32_t)HD_PHOTO_W;

        if (y < 0)
            break;
        for (int32_t x = x0; x < x1; x++)
            if (y < ybcol[x])
                tint_pixel(img, (size_t)x, (size_t)y, -200);
    }
}

/* Dark rectangle (building / cactus) standing on the horizon, w_ <= 40
 * and h <= 80, so every straight run stays <= 80 px. */
static void draw_rect(uint8_t *img, int32_t xc, int32_t w_, int32_t ht,
                      const int32_t *ybcol)
{
    int32_t x0 = xc - w_ / 2 > 0 ? xc - w_ / 2 : 0;
    int32_t x1 = xc + (w_ - w_ / 2) < (int32_t)HD_PHOTO_W
                     ? xc + (w_ - w_ / 2) : (int32_t)HD_PHOTO_W;
    int32_t y0 = ybcol[xc] - ht > 0 ? ybcol[xc] - ht : 0;
    int32_t y1 = ybcol[xc];

    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = x0; x < x1; x++)
            if (y < ybcol[x])
                tint_pixel(img, (size_t)x, (size_t)y, -200);
}

static void gen_photo(uint8_t *img, unsigned arch, int32_t cx, int32_t cy,
                      int32_t slope, rng_t *rng)
{
    const hd_scene *a = &ARCHETYPES[arch % 6];
    int32_t *ybcol = malloc(HD_PHOTO_W * sizeof(int32_t));

    if (!ybcol) /* the caller treats a NULL photo as fatal anyway */
        abort();

    /* the planted horizon: an exact straight boundary, per column */
    for (int32_t x = 0; x < (int32_t)HD_PHOTO_W; x++)
        ybcol[x] = cy + div_round_half_away((x - cx) * slope, 128);

    /* sky above the boundary, ground below, vertical gradients */
    for (int32_t y = 0; y < (int32_t)HD_PHOTO_H; y++) {
        uint8_t *row = img + (size_t)y * HD_PHOTO_W * 3;
        int32_t t = y * 255 / ((int32_t)HD_PHOTO_H - 1);

        for (int32_t x = 0; x < (int32_t)HD_PHOTO_W; x++) {
            const uint8_t *top = y < ybcol[x] ? a->sky_top : a->ground_top;
            const uint8_t *bot = y < ybcol[x] ? a->sky_bottom
                                              : a->ground_bottom;

            for (int c = 0; c < 3; c++)
                row[(size_t)x * 3 + c] =
                    (uint8_t)((int32_t)top[c] +
                              (((int32_t)bot[c] - (int32_t)top[c]) * t +
                               127) / 255);
        }
    }

    /* weak texture: bright disks in the sky, dark bands in the ground;
     * every tone delta <= 20 luma, below the weak edge threshold */
    unsigned n_disks = 1 + rng_below(rng, 2); /* 1..2 */

    for (unsigned s = 0; s < n_disks; s++) {
        uint32_t r = 30 + rng_below(rng, 51);          /* 30..80 */
        int32_t dcx = (int32_t)rng_below(rng, HD_PHOTO_W);
        int32_t dcy = (int32_t)rng_below(rng, HD_PHOTO_H);
        int32_t tone = 12 + (int32_t)rng_below(rng, 9); /* 12..20 */

        draw_disk(img, dcx, dcy, r, tone, ybcol);
    }
    unsigned n_bands = 2 + rng_below(rng, 3); /* 2..4 */

    for (unsigned s = 0; s < n_bands; s++) {
        int32_t b0 = (int32_t)rng_below(rng, HD_PHOTO_H);
        int32_t bh = 24 + (int32_t)rng_below(rng, 49); /* 24..72 */
        int32_t tone = -(int32_t)(8 + rng_below(rng, 13)); /* -20..-8 */

        draw_band(img, b0, bh, tone, ybcol);
    }

    /* strong silhouettes per archetype: short dark shapes standing on
     * the horizon (their bottom edges merge with it) — every straight
     * run <= 80 px, so no distractor out-votes a tilted horizon */
    switch (arch % 6) {
    case 1: { /* mountains: 2..3 peaks */
        unsigned n = 2 + rng_below(rng, 2);

        for (unsigned s = 0; s < n; s++) {
            int32_t bw = 48 + (int32_t)rng_below(rng, 25);  /* 48..72 */
            int32_t ht = 40 + (int32_t)rng_below(rng, 17);  /* 40..56 */
            int32_t lo = bw / 2 + 1;
            int32_t xc = lo + (int32_t)rng_below(rng, HD_PHOTO_W - 2u * lo);

            draw_triangle(img, xc, bw, ht, ybcol);
        }
        break;
    }
    case 2: { /* desert: 2..3 cacti */
        unsigned n = 2 + rng_below(rng, 2);

        for (unsigned s = 0; s < n; s++) {
            int32_t w_ = 10 + (int32_t)rng_below(rng, 7);   /* 10..16 */
            int32_t ht = 50 + (int32_t)rng_below(rng, 31);  /* 50..80 */
            int32_t xc = (int32_t)rng_below(rng, HD_PHOTO_W);

            draw_rect(img, xc, w_, ht, ybcol);
        }
        break;
    }
    case 3: { /* city: 3..5 spaced buildings */
        unsigned n = 3 + rng_below(rng, 3);
        int32_t placed[5];
        unsigned nplaced = 0;

        for (unsigned s = 0; s < n; s++) {
            int32_t w_ = 24 + (int32_t)rng_below(rng, 17);  /* 24..40 */
            int32_t ht = 50 + (int32_t)rng_below(rng, 31);  /* 50..80 */
            int32_t xc = 0;

            for (unsigned attempt = 0; attempt < 8; attempt++) {
                xc = (int32_t)rng_below(rng, HD_PHOTO_W);
                int ok = 1;

                for (unsigned q = 0; q < nplaced; q++)
                    if (abs(xc - placed[q]) < w_ / 2 + 4)
                        ok = 0;
                if (ok)
                    break;
            }
            if (nplaced < 5)
                placed[nplaced++] = xc;
            draw_rect(img, xc, w_, ht, ybcol);
        }
        break;
    }
    case 4: { /* forest: 4..6 trees */
        unsigned n = 4 + rng_below(rng, 3);

        for (unsigned s = 0; s < n; s++) {
            int32_t bw = 24 + (int32_t)rng_below(rng, 17);  /* 24..40 */
            int32_t ht = 24 + (int32_t)rng_below(rng, 17);  /* 24..40 */
            int32_t lo = bw / 2 + 1;
            int32_t xc = lo + (int32_t)rng_below(rng, HD_PHOTO_W - 2u * lo);

            draw_triangle(img, xc, bw, ht, ybcol);
        }
        break;
    }
    case 5: { /* fields: one lone tree */
        int32_t bw = 40 + (int32_t)rng_below(rng, 25);      /* 40..64 */
        int32_t ht = 36 + (int32_t)rng_below(rng, 17);      /* 36..52 */
        int32_t lo = bw / 2 + 1;
        int32_t xc = lo + (int32_t)rng_below(rng, HD_PHOTO_W - 2u * lo);

        draw_triangle(img, xc, bw, ht, ybcol);
        break;
    }
    default: /* ocean: open water, the horizon is the only strong edge */
        break;
    }

    /* per-pixel noise, amplitude per archetype — one rng draw per
     * pixel, low three bytes become the channel deltas */
    for (size_t y = 0; y < HD_PHOTO_H; y++) {
        uint8_t *row = img + y * HD_PHOTO_W * 3;

        for (size_t x = 0; x < HD_PHOTO_W; x++) {
            uint64_t n = rng_next(rng);
            int32_t amp = (int32_t)a->noise_amp;

            for (int c = 0; c < 3; c++) {
                int32_t d = (int32_t)((n >> (c * 8)) & 0xFFu);

                d = (d * (2 * amp + 1) >> 8) - amp;
                row[x * 3 + c] = clamp_u8((int32_t)row[x * 3 + c] + d);
            }
        }
    }
    free(ybcol);
}

/* --- the Canny + Hough pipeline --------------------------------------------*/

/* Sobel gradients with the |gx| + |gy| magnitude (OpenCV's default L1
 * metric) and the gradient direction quantized to 4 sectors at the
 * 22.5-degree boundaries (tan(22.5) ~= 0.414 -> 106, tan(67.5) ~=
 * 2.414 -> 618, scaled by 256). The cell packs mag << 2 | sector
 * (mag <= 2040 fits 12 bits); the 1-px border stays 0. */
static void sobel_sectors(const uint8_t *gray, size_t w, size_t h,
                          uint16_t *cells)
{
    memset(cells, 0, w * h * sizeof(uint16_t));
    for (size_t y = 1; y + 1 < h; y++) {
        const uint8_t *up = gray + (y - 1) * w;
        const uint8_t *mid = gray + y * w;
        const uint8_t *dn = gray + (y + 1) * w;
        uint16_t *out = cells + y * w;

        for (size_t x = 1; x + 1 < w; x++) {
            int32_t gx = (up[x + 1] + 2 * mid[x + 1] + dn[x + 1]) -
                         (up[x - 1] + 2 * mid[x - 1] + dn[x - 1]);
            int32_t gy = (dn[x - 1] + 2 * dn[x] + dn[x + 1]) -
                         (up[x - 1] + 2 * up[x] + up[x + 1]);
            int32_t ax = gx < 0 ? -gx : gx;
            int32_t ay = gy < 0 ? -gy : gy;
            int32_t mag = ax + ay;
            unsigned sector;

            if (mag == 0)
                sector = 0;
            else if (ay * 256 <= ax * 106)
                sector = 0; /* gradient ~ horizontal, edge ~ vertical */
            else if (ay * 256 < ax * 618)
                sector = ((gx >= 0) == (gy >= 0)) ? 1 : 3; /* diagonals */
            else
                sector = 2; /* gradient ~ vertical, edge ~ horizontal */
            out[x] = (uint16_t)((mag << 2) | sector);
        }
    }
}

/* Non-maximum suppression along each pixel's gradient sector: keep the
 * magnitude when it is >= both neighbors along the gradient direction
 * (>=, so the 2-px plateau of a clean step edge survives as a ridge). */
static void nonmax_suppress(const uint16_t *cells, size_t w, size_t h,
                            uint16_t *nms)
{
    static const int32_t DX[4][2] = {
        { -1, 1 }, { -1, 1 }, { 0, 0 }, { -1, 1 }
    };
    static const int32_t DY[4][2] = {
        { 0, 0 }, { -1, 1 }, { -1, 1 }, { 1, -1 }
    };

    memset(nms, 0, w * h * sizeof(uint16_t));
    for (size_t y = 1; y + 1 < h; y++) {
        const uint16_t *row = cells + y * w;
        uint16_t *out = nms + y * w;

        for (size_t x = 1; x + 1 < w; x++) {
            uint16_t cell = row[x];

            if (!cell)
                continue;
            int32_t m = cell >> 2;
            unsigned sec = cell & 3u;
            /* neighbors along the gradient direction, across rows */
            size_t a = (size_t)((int64_t)(y * w + x) + DY[sec][0] * (int64_t)w +
                                DX[sec][0]);
            size_t b = (size_t)((int64_t)(y * w + x) + DY[sec][1] * (int64_t)w +
                                DX[sec][1]);
            int32_t n1 = cells[a] >> 2;
            int32_t n2 = cells[b] >> 2;

            if (m >= n1 && m >= n2)
                out[x] = (uint16_t)m;
        }
    }
}

/* Double threshold + 8-connected hysteresis: the strong set, grown
 * through weak pixels until closure. The resulting set is the unique
 * fixpoint of a monotone operator, so it is independent of the BFS
 * order — deterministic by construction. */
static void hysteresis(const uint16_t *nms, size_t w, size_t h,
                       uint8_t *edges, uint32_t *stack)
{
    static const int32_t NB[8][2] = {
        { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 },
        { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 }
    };
    size_t top = 0;

    memset(edges, 0, w * h);
    for (size_t i = 0; i < w * h; i++) {
        if (nms[i] >= HD_EDGE_HIGH) {
            edges[i] = 1;
            stack[top++] = (uint32_t)i;
        }
    }
    while (top) {
        uint32_t i = stack[--top];
        int64_t x = i % w;
        int64_t y = i / w;

        for (int k = 0; k < 8; k++) {
            int64_t nx = x + NB[k][0];
            int64_t ny = y + NB[k][1];

            if (nx < 0 || ny < 0 || nx >= (int64_t)w || ny >= (int64_t)h)
                continue;
            size_t j = (size_t)ny * w + (size_t)nx;

            if (!edges[j] && nms[j] >= HD_EDGE_LOW) {
                edges[j] = 1;
                stack[top++] = (uint32_t)j;
            }
        }
    }
}

int hd_scratch_init(hd_scratch *s, size_t w, size_t h)
{
    memset(s, 0, sizeof(*s));
    if (w < 3 || h < 3)
        return -1;
    s->w = w;
    s->h = h;
    /* |rho| bound: image diagonal, rounded up (+1 for the sqrt floor,
     * +1 for the trig-table rounding, +1 for the rho rounding) */
    s->rho_bound = (int32_t)isqrt64((uint64_t)(w - 1) * (w - 1) +
                                    (uint64_t)(h - 1) * (h - 1)) + 3;
    s->gray = malloc(w * h);
    s->blur = malloc(w * h);
    s->mag = malloc(w * h * sizeof(uint16_t));
    s->nms = malloc(w * h * sizeof(uint16_t));
    s->edges = malloc(w * h);
    s->stack = malloc(w * h * sizeof(uint32_t));
    s->accum = malloc((size_t)HD_THETA_STEPS *
                      (size_t)(2 * s->rho_bound + 1) * sizeof(int32_t));
    if (!s->gray || !s->blur || !s->mag || !s->nms || !s->edges ||
        !s->stack || !s->accum) {
        hd_scratch_free(s);
        return -1;
    }
    return 0;
}

void hd_scratch_free(hd_scratch *s)
{
    if (!s)
        return;
    free(s->gray);
    free(s->blur);
    free(s->mag);
    free(s->nms);
    free(s->edges);
    free(s->stack);
    free(s->accum);
    memset(s, 0, sizeof(*s));
}

int hd_detect(const uint8_t *rgb, size_t w, size_t h,
              hd_scratch *s, hd_result *out)
{
    int32_t cols;
    uint32_t best = 0;
    unsigned best_t = 0;
    int32_t best_r = 0;

    if (!rgb || !s || !out || !s->gray || !s->blur || !s->mag ||
        !s->nms || !s->edges || !s->stack || !s->accum ||
        s->w != w || s->h != h)
        return -1;

    img_grayscale(rgb, w, h, s->gray);
    img_gray_gaussian_blur5(s->gray, w, h, s->blur);
    sobel_sectors(s->gray, w, h, s->mag);
    nonmax_suppress(s->mag, w, h, s->nms);
    hysteresis(s->nms, w, h, s->edges, s->stack);
    sha1(s->edges, w * h, out->edge_digest);

    /* Hough transform: one row of rho cells per theta bin */
    cols = 2 * s->rho_bound + 1;
    memset(s->accum, 0, (size_t)HD_THETA_STEPS * (size_t)cols *
                         sizeof(int32_t));
    for (size_t y = 0; y < h; y++) {
        const uint8_t *erow = s->edges + y * w;

        for (size_t x = 0; x < w; x++) {
            if (!erow[x])
                continue;
            for (int t = 0; t < HD_THETA_STEPS; t++) {
                int32_t v = (int32_t)x * HD_COS1024[t] +
                            (int32_t)y * HD_SIN1024[t];
                int32_t r = div_round_half_away(v, 1024);

                if (r < -s->rho_bound)
                    r = -s->rho_bound;
                if (r > s->rho_bound)
                    r = s->rho_bound;
                s->accum[(size_t)t * (size_t)cols +
                         (size_t)(r + s->rho_bound)]++;
            }
        }
    }

    /* peak: first cell in (theta asc, rho asc) order with strictly the
     * highest vote count — deterministic tie-break */
    for (int t = 0; t < HD_THETA_STEPS; t++) {
        const int32_t *row = s->accum + (size_t)t * (size_t)cols;

        for (int32_t ri = 0; ri < cols; ri++) {
            if ((uint32_t)row[ri] > best) {
                best = (uint32_t)row[ri];
                best_t = (unsigned)t;
                best_r = ri - s->rho_bound;
            }
        }
    }
    out->theta_bin = best_t;
    out->rho_bin = best_r;
    out->votes = best;
    return 0;
}

/* --- prepare / run ----------------------------------------------------------*/

struct hd_ctx {
    uint8_t *photo[HD_PHOTOS];
    hd_result ref[HD_PHOTOS];
    int32_t  cx[HD_PHOTOS], cy[HD_PHOTOS], slope[HD_PHOTOS];
    unsigned arch[HD_PHOTOS];
};

/* Ground-truth check of one detection against the planted horizon:
 * angle within 1.0 degree (cross product of the detected unit normal
 * with the planted direction), rho within HD_GT_RHO_PX of the planted
 * line's rho at the detected theta, votes >= HD_MIN_VOTES. */
static int ground_truth_ok(const hd_ctx *ctx, unsigned p,
                           const hd_result *r)
{
    int32_t c, s, n = ctx->slope[p];

    if (r->votes < HD_MIN_VOTES)
        return 0;
    hd_trig(r->theta_bin, &c, &s);
    /* planted normal (-n, 128); |cross| <= 1024 * |dir| * sin(1 deg) */
    {
        int32_t cross = 128 * c + n * s;
        int32_t dot = 128 * s - n * c;
        int32_t rho_planted = div_round_half_away(ctx->cx[p] * c +
                                                  ctx->cy[p] * s, 1024);

        if (cross < -HD_GT_ANGLE_CROSS || cross > HD_GT_ANGLE_CROSS)
            return 0;
        if (dot <= 0)
            return 0;
        {
            int32_t dr = r->rho_bin - rho_planted;

            if (dr < -HD_GT_RHO_PX || dr > HD_GT_RHO_PX)
                return 0;
        }
    }
    return 1;
}

hd_ctx *hd_prepare(unsigned seed)
{
    hd_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t rng;

    if (!ctx)
        return NULL;
    rng_init(&rng, seed);
    for (unsigned p = 0; p < HD_PHOTOS; p++) {
        unsigned arch = p % 6;
        int32_t cx = 751 + (int32_t)rng_below(&rng, 99);  /* 751..849 */
        int32_t cy = 451 + (int32_t)rng_below(&rng, 299); /* 451..749 */
        int32_t slope = (int32_t)rng_below(&rng, 129) - 64; /* -64..64 */
        hd_scratch sc;

        ctx->photo[p] = malloc(PHOTO_BYTES);
        if (!ctx->photo[p]) {
            hd_free(ctx);
            return NULL;
        }
        ctx->arch[p] = arch;
        ctx->cx[p] = cx;
        ctx->cy[p] = cy;
        ctx->slope[p] = slope;
        gen_photo(ctx->photo[p], arch, cx, cy, slope, &rng);

        /* reference detection, off the clock */
        if (hd_scratch_init(&sc, HD_PHOTO_W, HD_PHOTO_H) != 0) {
            hd_free(ctx);
            return NULL;
        }
        if (hd_detect(ctx->photo[p], HD_PHOTO_W, HD_PHOTO_H, &sc,
                      &ctx->ref[p]) != 0) {
            hd_scratch_free(&sc);
            hd_free(ctx);
            return NULL;
        }
        hd_scratch_free(&sc);
    }
    return ctx;
}

void hd_free(hd_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned p = 0; p < HD_PHOTOS; p++)
        free(ctx->photo[p]);
    free(ctx);
}

/* One thread's slice: contiguous photo instances, own scratch —
 * threads never share writable state (shared-task harness contract). */
typedef struct {
    hd_ctx *ctx;
    hd_scratch sc;
    size_t begin, end; /* instance range [begin, end) of the pass */
} hd_slice;

static int hd_cycle(void *arg)
{
    hd_slice *sl = arg;
    hd_ctx *ctx = sl->ctx;

    for (size_t i = sl->begin; i < sl->end; i++) {
        unsigned p = (unsigned)(i % HD_PHOTOS);
        hd_result r;
        const hd_result *ref = &ctx->ref[p];

        if (hd_detect(ctx->photo[p], HD_PHOTO_W, HD_PHOTO_H, &sl->sc,
                      &r) != 0)
            return -1;
        if (memcmp(r.edge_digest, ref->edge_digest, 20) != 0 ||
            r.theta_bin != ref->theta_bin ||
            r.rho_bin != ref->rho_bin || r.votes != ref->votes)
            return -1;
        if (!ground_truth_ok(ctx, p, &r))
            return -1;
    }
    return 0;
}

uint64_t hd_task_pixels(unsigned multi)
{
    unsigned set = multi ? HD_PHOTOS_MULTI : HD_PHOTOS;

    return (uint64_t)set * HD_PHOTO_W * HD_PHOTO_H;
}

int hd_run(hd_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mpixels, uint64_t *pixels)
{
    unsigned t_count = *threads;
    unsigned photo_set = t_count > 1 ? HD_PHOTOS_MULTI : HD_PHOTOS;
    hd_slice *slices;
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
            if (hd_scratch_init(&slices[t].sc, HD_PHOTO_W,
                                HD_PHOTO_H) != 0)
                goto out;
            ctxs[t] = &slices[t];
        }
    }

    rc = harness_run(t_count, runs, hd_cycle, ctxs, elapsed);
    if (rc == 0) {
        uint64_t px = hd_task_pixels(t_count > 1 ? 1u : 0u);

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
        for (unsigned t = 0; t < t_count; t++)
            hd_scratch_free(&slices[t].sc);
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors ---------------------------------------------------------------*/

const uint8_t *hd_photo(const hd_ctx *ctx, unsigned photo)
{
    return ctx && photo < HD_PHOTOS ? ctx->photo[photo] : NULL;
}

size_t hd_photo_len(const hd_ctx *ctx, unsigned photo)
{
    return ctx && photo < HD_PHOTOS ? PHOTO_BYTES : 0;
}

const hd_result *hd_reference(const hd_ctx *ctx, unsigned photo)
{
    return ctx && photo < HD_PHOTOS ? &ctx->ref[photo] : NULL;
}

unsigned hd_archetype(const hd_ctx *ctx, unsigned photo)
{
    return ctx && photo < HD_PHOTOS ? ctx->arch[photo] : 0;
}

int32_t hd_plant_cx(const hd_ctx *ctx, unsigned photo)
{
    return ctx && photo < HD_PHOTOS ? ctx->cx[photo] : 0;
}

int32_t hd_plant_cy(const hd_ctx *ctx, unsigned photo)
{
    return ctx && photo < HD_PHOTOS ? ctx->cy[photo] : 0;
}

int32_t hd_plant_slope(const hd_ctx *ctx, unsigned photo)
{
    return ctx && photo < HD_PHOTOS ? ctx->slope[photo] : 0;
}
