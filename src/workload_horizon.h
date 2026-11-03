#ifndef WORKLOAD_HORIZON_H
#define WORKLOAD_HORIZON_H

#include <stddef.h>
#include <stdint.h>

/* Workload #12 — Horizon Detection.
 *
 * Mirrors the twelfth Geekbench 6 CPU workload: detect the horizon line
 * of a photo by reducing it to edges with the Canny detector, then
 * applying the Hough transform. Geekbench 6 runs the OpenCV-style
 * pipeline over real photos; LotofBench reproduces the shape
 * dependency-free: eight seeded synthetic landscape photos (1600x1200
 * RGB, six archetype families) each carry a planted straight horizon
 * with a known slope and position, and every timed cycle re-runs the
 * full grayscale -> Gaussian blur -> Sobel -> non-maximum suppression ->
 * double threshold + hysteresis -> Hough -> peak pipeline on every
 * photo. Everything is integer arithmetic, so the detected cells and
 * edge-map digests are bit-identical across machines. The detection is
 * verified against the prepare-time reference AND against the planted
 * ground-truth line; any divergence aborts the run instead of scoring.
 * Everything is documented in docs/benchmarks.md, section "Workload 12". */

typedef struct hd_ctx hd_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the photos are the photos. */
#define HD_PHOTOS        8u  /* the single-core pass detects these eight */
#define HD_PHOTOS_MULTI 32u  /* multi-core pass = 4 x the photo set */
#define HD_PHOTO_W     1600u /* device pixels per photo (RGB888) */
#define HD_PHOTO_H     1200u

/* Hough geometry: 720 theta bins of 0.25 degrees; rho in 1-pixel bins.
 * The rho range covers the image diagonal (2000 px for 1600x1200) plus
 * the trig-table rounding (<= 1.4 px), so |rho| <= 2001. The fine theta
 * step keeps the horizon's votes concentrated: at 1-degree bins a
 * tilted line (normal angle mid-bin, slope up to 0.5) would spread its
 * ~1600 ridge pixels over ~14 rho cells and peak at ~100 votes —
 * weaker than a single 160-px building edge. At 0.25 degrees the same
 * line peaks at ~400+ votes, above every documented distractor. */
#define HD_THETA_STEPS  720
#define HD_RHO_MAX      2001

/* Canny thresholds on the |gx| + |gy| magnitude (0..4080 scale): a
 * candidate edge is strong at >= HIGH, weak at >= LOW; hysteresis keeps
 * weak pixels connected to the strong set. The corpus guarantees the
 * horizon step >= 110 luma and every other luminance change <= 25, so
 * both thresholds sit in the documented gap (see docs/benchmarks.md). */
#define HD_EDGE_LOW    100u
#define HD_EDGE_HIGH   200u

/* A Hough peak must gather at least 300 votes to count as the horizon.
 * The generator bounds every distractor straight edge at <= 80 px per
 * run (<= 67 px for triangle sides), doubled at most by the 2-px NMS
 * ridge — a per-bin distractor ceiling of ~134 votes — while the
 * horizon ridge concentrates >= 549 votes over seeds 1..5 (0.25-degree
 * bins). Both margins are >= 1.8x. */
#define HD_MIN_VOTES   300u

/* Ground-truth tolerances for the planted horizon (checked inside every
 * timed cycle, not just at prepare time):
 *  - the detected theta bin's normal must align with the planted normal
 *    to within 1.0 degree: |cross(unit_normal, planted_dir)| <=
 *    1024 * |dir| * sin(1 deg) ~= 2560 for the worst seeded slope;
 *  - the detected rho must sit within 3 px of the planted line's rho at
 *    the detected theta (trig-table rounding + staircase + NMS ridge);
 *  - the vote count must clear HD_MIN_VOTES. */
#define HD_GT_ANGLE_CROSS 2600
#define HD_GT_RHO_PX      3

/* Result of one full detection. theta_bin is a 0.25-degree bin index
 * (0..719); the normal angle is theta_bin / 4 degrees. */
typedef struct {
    unsigned theta_bin;        /* 0..719, 0.25-degree normal-angle bins */
    int32_t  rho_bin;          /* signed rho in pixels */
    uint32_t votes;            /* accumulator cells at the peak */
    uint8_t  edge_digest[20];  /* SHA-1 of the final edge map (w*h, 0/1) */
} hd_result;

/* Per-image scratch: one per thread, allocated once, reused for every
 * photo and run (threads never share writable state). w and h pin the
 * scratch to one image size; hd_detect refuses a mismatch. */
typedef struct {
    size_t   w, h;       /* image size the buffers were sized for */
    uint8_t  *gray;      /* w * h: luma after grayscale */
    uint8_t  *blur;      /* w * h: gaussian scratch */
    uint16_t *mag;       /* w * h: |gx| + |gy|, packed with the sector */
    uint16_t *nms;       /* w * h: suppressed magnitude */
    uint8_t  *edges;     /* w * h: final edge map (0/1) */
    uint32_t *stack;     /* w * h: hysteresis BFS stack */
    int32_t  *accum;     /* HD_THETA_STEPS * (2 * rho_bound + 1) cells */
    int32_t   rho_bound; /* |rho| bound for this size (diag + 3) */
} hd_scratch;

int  hd_scratch_init(hd_scratch *s, size_t w, size_t h);
void hd_scratch_free(hd_scratch *s);

/* The full detection pipeline on one RGB888 image: grayscale, Gaussian
 * blur, Sobel gradients with 4-sector quantization, non-maximum
 * suppression, double threshold + hysteresis, Hough transform, peak.
 * Returns 0 and fills *out, or -1 on a NULL/scratch mismatch. The 1-px
 * image border never becomes an edge. */
int hd_detect(const uint8_t *rgb, size_t w, size_t h,
              hd_scratch *s, hd_result *out);

/* The integer cosine/sine of one theta bin, scaled by 1024 (rounded
 * half away from zero; provably stable across libm implementations —
 * see docs/benchmarks.md). Exposed for tests and tooling. */
void hd_trig(unsigned theta_bin, int32_t *cos1024, int32_t *sin1024);

/* Build the photo corpus (off the clock; fixed seed => byte-identical
 * photos on every machine), run the reference detection on every photo
 * and store the planted ground-truth parameters. */
hd_ctx *hd_prepare(unsigned seed);
void    hd_free(hd_ctx *ctx);

/* One workload pass with *threads threads (clamped to the photo set of
 * the pass: 8 single-core, 32 multi-core). Fills *throughput_mpixels
 * with the median over the runs, in millions of input pixels processed
 * per second, and *pixels with the exact pixel count of the pass
 * (deterministic, identical for every run). Returns 0, or -1 if any
 * detection failed to verify. */
int hd_run(hd_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mpixels, uint64_t *pixels);

/* Read-only access for tests and tooling. */
const uint8_t   *hd_photo(const hd_ctx *ctx, unsigned photo);
size_t           hd_photo_len(const hd_ctx *ctx, unsigned photo);
const hd_result *hd_reference(const hd_ctx *ctx, unsigned photo);
unsigned         hd_archetype(const hd_ctx *ctx, unsigned photo);

/* The planted horizon: an exact straight line through (cx, cy) with
 * direction (128, slope) — slope in [-64, 64], so |dy/dx| <= 0.5. The
 * visible segment always spans the full photo width. */
int32_t hd_plant_cx(const hd_ctx *ctx, unsigned photo);
int32_t hd_plant_cy(const hd_ctx *ctx, unsigned photo);
int32_t hd_plant_slope(const hd_ctx *ctx, unsigned photo);

uint64_t hd_task_pixels(unsigned multi); /* input px of one pass */

#endif /* WORKLOAD_HORIZON_H */
