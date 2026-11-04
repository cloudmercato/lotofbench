#ifndef WORKLOAD_SFM_H
#define WORKLOAD_SFM_H

#include <stddef.h>
#include <stdint.h>

/* Workload #16 — Structure from Motion.
 *
 * Mirrors the sixteenth Geekbench 6 CPU workload: reconstruct camera
 * motion and a sparse 3D point cloud from image pairs (Geekbench 6 runs
 * feature matching, essential-matrix estimation and triangulation over
 * photo pairs). LotofBench reproduces the shape dependency-free: a
 * seeded synthetic 3D point cloud (ground, two facades, a back wall and
 * a ceiling) is observed by seeded convergent stereo pairs; each view
 * is a 640x480 grayscale frame built by stamping one dark-centre patch
 * per visible point. Every timed cycle re-runs the whole pipeline on
 * its slice — FAST-9 corner detection with non-max suppression,
 * correspondence pairing, RANSAC over a normalized 8-point essential
 * estimator, pose selection by cheirality and midpoint triangulation —
 * and the reconstruction must reproduce its prepare-time SHA-1 digest,
 * its exact inlier set, the planted relative pose and sub-pixel
 * reprojection bounds; any drift aborts the run instead of scoring.
 *
 * The timed path is float arithmetic restricted to +, -, *, / and
 * sqrtf — IEEE-exact operations, compiled with -ffp-contract=off — so
 * detections, poses and clouds are bit-identical across machines. No
 * transcendental function, no libm call beyond sqrtf, no FMA
 * contraction (camera bases come from cross products, not angles).
 * Documented in docs/benchmarks.md, section "Workload 16". */

typedef struct sfm_ctx sfm_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the pairs are the pairs. */
#define SFM_PAIRS        12u  /* the single-core pass reconstructs these */
#define SFM_PAIRS_MULTI  48u  /* multi-core pass = 4 x the pair set */
#define SFM_VIEW_W      640u  /* grayscale view pixels */
#define SFM_VIEW_H      480u
#define SFM_SCENE_POINTS 544u /* synthetic 3D points before visibility */
#define SFM_MAX_VISIBLE  288u /* visibility cap, one entry per patch */

/* FAST-9: 16-pixel Bresenham ring of radius 3, a corner fires when at
 * least 9 contiguous ring pixels are all brighter or all darker than
 * the centre by this threshold. */
#define SFM_FAST_THRESHOLD 24

/* RANSAC over the 8-point estimator: fixed seeded iteration count (the
 * data is deterministic, so the sample sequence is too), Sampson
 * inlier threshold in pixels. */
#define SFM_RANSAC_ITERS  96u
#define SFM_RANSAC_PX     2.0f

/* Fixed intrinsics, shared by every camera: focal length in pixels and
 * the principal point at the geometric centre of the frame. */
#define SFM_FOCAL      600.0f
#define SFM_CX         ((SFM_VIEW_W - 1u) / 2.0f) /* 319.5 */
#define SFM_CY         ((SFM_VIEW_H - 1u) / 2.0f) /* 239.5 */

/* One detected FAST corner (position is the exact patch centre). */
typedef struct {
    uint16_t x, y;
    uint32_t score; /* sum of |I(ring) - I(centre)| over the 16 ring px */
} sfm_corner;

/* One pixel-space correspondence with its planted identity. */
typedef struct {
    float    u1, v1;  /* left view, pixels */
    float    u2, v2;  /* right view, pixels */
    uint32_t id;      /* scene point index (< SFM_SCENE_POINTS) */
} sfm_corr;

/* A pinhole camera: position C (world, metres) and the world-to-camera
 * rotation R (row-major 3x3; camera looks down +z of its own frame).
 * Projection: u = SFM_FOCAL * x_c / z_c + SFM_CX (v with SFM_CY). */
typedef struct {
    float c[3];
    float R[9];
} sfm_camera;

/* Result of one full pair reconstruction. */
typedef struct {
    uint8_t  digest[20];  /* SHA-1 of the canonical reconstruction */
    uint32_t corners_left, corners_right;
    uint32_t inliers;     /* accepted correspondences */
    uint32_t triangulated;
} sfm_result;

/* Planted ground truth of one correspondence (read-only, for tests). */
typedef struct {
    float    u1, v1, u2, v2; /* patch centres in both views */
    uint32_t scene_id;
    uint8_t  is_outlier;     /* planted mismatch, rejected by RANSAC */
} sfm_planted;

/* --- low-level API (public for the gate and the unit tests) ---------------*/

/* FAST-9 with 3x3 non-max suppression. Scans every interior pixel,
 * scores firers by the ring SAD and keeps local maxima (ties broken by
 * lower y, then lower x — deterministic). Writes at most max_out
 * corners and returns the count, or -1 on NULL arguments. */
int sfm_fast9_detect(const uint8_t *g, unsigned w, unsigned h,
                     int threshold, unsigned max_out, sfm_corner *out);

/* Cyclic Jacobi eigensolvers for symmetric matrices (row-major).
 * sfm_jacobi3 fills vals[3] (descending) and the eigenvector columns
 * vecs[3*c + r] (right-handed). sfm_jacobi9_min fills vec[9] with the
 * unit eigenvector of the smallest eigenvalue. Returns 0, or -1 on
 * NULL. Both are deterministic: fixed sweep count, fixed rotation
 * order, IEEE-exact arithmetic only. */
int sfm_jacobi3(float m[9], float vals[3], float vecs[9]);
int sfm_jacobi9_min(const float m[81], float vec[9]);

/* Squared Sampson distance of one correspondence under a pixel-space
 * essential matrix E. */
float sfm_sampson2(const float E[9], const sfm_corr *c);

/* RANSAC essential-matrix estimator: SFM_RANSAC-style loop over `iters`
 * seeded 8-samples (rng stream seeded with `salt`), normalized 8-point
 * least squares on the best sample's inliers, Hartley-normalized,
 * denormalized back to pixel space. Fills E[9] (pixel space), the
 * inlier mask over the n correspondences and the inlier count.
 * Returns 0, or -1 on NULL / n < 8 / degenerate data. */
int sfm_essential_ransac(const sfm_corr *c, unsigned n, unsigned iters,
                         uint64_t salt, float E[9], uint8_t *inlier,
                         unsigned *n_inliers);

/* Algebraic ground truth: E = [t]x R built from the exact planted
 * relative pose — the independent path the estimated E is
 * differentially tested against. Returns 0, or -1 on NULL. */
int sfm_essential_from_pose(const float R[9], const float t[3],
                            float E[9]);

/* Decompose a normalized essential matrix (scale-immersed, e.g. the
 * output of sfm_essential_from_pose or the K-mapped pixel-space E
 * after Frobenius normalization) into the two rotations R1, R2 and the
 * unit translation t (up to sign: the caller tries +/-t). Returns 0,
 * or -1 on NULL / rank-deficient E. */
int sfm_decompose_essential(const float E[9], float R1[9], float R2[9],
                            float t[3]);

/* Midpoint triangulation of one correspondence in the left camera
 * frame, given the left-to-right pose (X_r = R X_l + t). The
 * reconstruction inherits the translation's unit scale (the classic
 * scale ambiguity) — every check on it is scale-free. Returns 0, or
 * -1 on NULL / near-parallel rays. */
int sfm_triangulate_midpoint(const float R[9], const float t[3],
                             const sfm_corr *c, float X[3]);

/* --- workload API ----------------------------------------------------------*/

/* Build the seeded point cloud, camera pairs and frames (off the
 * clock; fixed seed => bit-identical inputs on every machine) and run
 * the full reconstruction once per pair to build the references. */
sfm_ctx *sfm_prepare(unsigned seed);
void     sfm_free(sfm_ctx *ctx);

/* One workload pass with *threads threads (clamped to the pair set of
 * the pass: 12 single-core, 48 multi-core). Every cycle re-runs the
 * whole pipeline on its slice and verifies each reconstruction against
 * the prepare-time reference and the planted truth. Fills
 * *throughput_pairs with the median over the runs, in image pairs per
 * second, and *pairs with the exact pair count of the pass
 * (deterministic, identical for every run). Returns 0, or -1 if any
 * pair failed to verify. */
int sfm_run(sfm_ctx *ctx, unsigned runs, unsigned *threads,
            double *throughput_pairs, uint64_t *pairs);

/* Read-only access for tests and tooling. */
const uint8_t   *sfm_view(const sfm_ctx *ctx, unsigned pair,
                          int right);
const sfm_result *sfm_reference(const sfm_ctx *ctx, unsigned pair);
const sfm_camera *sfm_camera_left(const sfm_ctx *ctx, unsigned pair);
const sfm_camera *sfm_camera_right(const sfm_ctx *ctx, unsigned pair);
/* Planted relative pose: X_r = R_rel X_l + t_rel (metric scale). */
const float      *sfm_gt_R(const sfm_ctx *ctx, unsigned pair);
const float      *sfm_gt_t(const sfm_ctx *ctx, unsigned pair);
unsigned          sfm_visible(const sfm_ctx *ctx, unsigned pair);
const sfm_planted *sfm_planted_of(const sfm_ctx *ctx, unsigned pair,
                                  unsigned i);

/* Exact pair count of one pass (single- or multi-core). */
uint64_t sfm_task_pairs(unsigned multi);

#endif /* WORKLOAD_SFM_H */
