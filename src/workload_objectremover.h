#ifndef WORKLOAD_OBJECTREMOVER_H
#define WORKLOAD_OBJECTREMOVER_H

#include <stddef.h>
#include <stdint.h>

/* Workload #11 — Object Remover.
 *
 * Mirrors the eleventh Geekbench 6 CPU workload: remove a masked object
 * from a photo with content-aware fill (inpainting). Geekbench 6 feeds
 * real photos and lasso masks through a production fill stack; LotofBench
 * reproduces the *shape* of the workload dependency-free: seeded synthetic
 * photos with a planted two-shape mask (a disc plus an overlapping
 * rectangle — the lasso scribble), erased by a 100%-integer push-pull
 * pyramid inpainting:
 *
 *   1. push: a 6-level pyramid (level 0 = the photo, level l + 1 halves
 *      level l) where every coarse cell is the half-up mean of its
 *      DISTINCT known children (mask byte == 0, the clamped corner
 *      taps are deduplicated) and the coarse mask marks the fully
 *      masked cells (every child unknown) — masked bytes are never
 *      read, at any level;
 *   2. coarsest-level fill: the unknown cells of the coarsest plane take
 *      the per-channel half-up mean of that plane's known cells (counted
 *      in coarse_fill; an all-unknown plane is a contract failure);
 *   3. pull and solve, coarse to fine: every level's unknown cells are
 *      initialized with the shared img_scale_bilinear of the level
 *      below's FILLED plane, then refined by OR_ITERS in-place
 *      Gauss-Seidel sweeps of the discrete 4-neighbour Laplace equation
 *      (raster order, edge-clamped neighbours, half-up
 *      (l + r + u + d + 2) >> 2) over the unknown cells only — the
 *      coarse levels carry the low-frequency fill, the per-level sweeps
 *      blend in the local boundary detail; known cells stay fixed
 *      (Dirichlet boundary);
 *   4. composite: the level-0 plane after its own solve — known pixels
 *      are byte-identical to the input, masked pixels carry the fill.
 *
 * Everything is integer arithmetic with documented rounding, so the fill
 * is bit-identical across machines and compilers. Every timed cycle
 * re-runs the full pipeline on every photo and verifies the SHA-1 of the
 * mask and of the filled frame, plus three exact counters (masked
 * pixels, coarsest-level filled cells, Gauss-Seidel sweeps executed),
 * against the prepare-time
 * references; any divergence aborts the run instead of scoring. Everything
 * is documented in docs/benchmarks.md, section "Workload 11".
 *
 * The corpus carries ground truth: the masks are the union of one disc
 * and one rectangle (shapes and exact pixel counts exposed through
 * accessors), every disc is big enough to leave fully-masked 32x32 blocks
 * at the coarsest level, so coarse_fill >= 1 on the whole corpus. The
 * tests drive the kernels on hand-built planes, pin a closed-form fill
 * (a constant photo is its own harmonic fill, to the byte), and diff the
 * whole pipeline against an independently written naive reference. */

typedef struct or_ctx or_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the photo set is the photo set. */
#define OR_PHOTOS        4u  /* the single-core pass fills these four */
#define OR_PHOTOS_MULTI 16u  /* multi-core pass = 4 x the photo set */
#define OR_PHOTO_W    1600u
#define OR_PHOTO_H    1200u

/* Pyramid geometry: levels 0..OR_LEVELS, level l + 1 halves level l
 * (half-up). The coarsest plane (25x19) stays larger than the holes:
 * every photo's mask leaves known cells there, and every disc (radius
 * >= 64) covers at least one fully-masked 64x64 block, so coarse_fill
 * >= 1 on the whole corpus. */
#define OR_LEVELS        6u

/* The refinement: in-place Gauss-Seidel sweeps of the discrete Laplace
 * per level, over the unknown cells only. The per-level sweeps run
 * coarse to fine; the whole fill executes OR_LEVELS * OR_ITERS sweeps
 * per photo (the corpus holes reach level 0). */
#define OR_ITERS        24u

/* The mask placement contract: the disc (radius OR_DISC_MIN..OR_DISC_MAX)
 * and the rectangle are both at least OR_MASK_MARGIN pixels inside the
 * frame, and the rectangle always contains the disc's centre column and
 * row, so the two shapes always overlap. */
#define OR_DISC_MIN      64u
#define OR_DISC_MAX      96u
#define OR_DISC_RNG      (OR_DISC_MAX - OR_DISC_MIN + 1u)
#define OR_MASK_MARGIN   48u

/* Result of one full photo fill. The two digests pin the byte-exact
 * state of the pipeline: the mask plane and the filled frame. unknown
 * counts the masked pixels (the fill's exact work set), coarse_fill the
 * coarsest-level cells that took the known-mean fallback, iters the
 * exact number of Gauss-Seidel sweeps executed (OR_ITERS * OR_LEVELS
 * when the hole reaches level 0, pinned to catch silent drift). */
typedef struct {
    uint8_t  mask_digest[20];
    uint8_t  filled_digest[20];
    uint32_t unknown;
    uint32_t coarse_fill;
    uint32_t iters;
} or_result;

/* Box in photo coordinates, exclusive x1/y1. */
typedef struct {
    int32_t x0, y0, x1, y1;
} or_box;

/* The mask's two planted shapes (the ground truth the tests rebuild). */
typedef struct {
    int32_t cx, cy, r; /* the disc */
} or_disc;
typedef struct {
    int32_t x0, y0, x1, y1; /* the rectangle, exclusive x1/y1 */
} or_rect;

/* Per-photo scratch: one per thread, allocated once, reused for every
 * photo and run (threads never share writable state). The filled frame
 * is the pyramid's level 0 after or_process. */
typedef struct {
    uint8_t  *pyr;    /* packed RGB pyramid, levels 0..OR_LEVELS */
    uint8_t  *maskp;  /* mask pyramid, levels 0..OR_LEVELS */
    uint8_t  *up;     /* full-size RGB plane: img_scale_bilinear target */
    or_result res;    /* result of the last pipeline */
} or_scratch;

int  or_scratch_init(or_scratch *s);
void or_scratch_free(or_scratch *s);

/* --- the integer kernels (public for the differential tests) ------- */

/* One push step: dst (dw x dh, packed RGB) and dmask receive the
 * half-up mean of the distinct known children of every cell (mask
 * byte == 0) and the coarse mask (1 iff every child is unknown — a
 * fully masked cell; 0 otherwise).
 * src is (sw x sh) packed RGB, smask its mask plane. dst must not
 * alias src. */
void or_push_rgb(uint8_t *dst, uint8_t *dmask, size_t dw, size_t dh,
                 const uint8_t *src, const uint8_t *smask,
                 size_t sw, size_t sh);

/* The coarsest-level fill: unknown cells of plane (w x h, packed RGB)
 * take the per-channel half-up mean of the known cells (mask byte
 * == 0); *filled (may be NULL) accumulates the number of filled cells.
 * Returns 0, or -1 if the plane holds no known cell at all. */
int or_coarse_fill(uint8_t *plane, const uint8_t *mask, size_t w,
                   size_t h, uint32_t *filled);

/* The pull: img_scale_bilinear of coarse (cw x ch) into up scratch
 * (w x h), then every unknown cell (mask byte != 0) of plane takes the
 * upsampled value; known cells are left untouched. up must hold
 * w * h * 3 bytes and must not alias plane. */
void or_pull(uint8_t *plane, uint8_t *up, const uint8_t *mask,
             size_t w, size_t h, const uint8_t *coarse,
             size_t cw, size_t ch);

/* One in-place Gauss-Seidel sweep of the discrete Laplace over the
 * region [x0, x1) x [y0, y1) of the (w x h, packed RGB) plane: unknown
 * cells (mask byte != 0) take (l + r + u + d + 2) >> 2 per channel in
 * raster order — the freshly written left and up neighbours are part of
 * the documented semantics — with the neighbour indices clamped to the
 * plane's edges; known cells are left untouched. The region must stay
 * inside the plane. */
void or_sweep(uint8_t *plane, const uint8_t *mask, size_t w, size_t h,
              size_t x0, size_t y0, size_t x1, size_t y1);

/* The full per-photo pipeline: push, coarsest-level fill, pull and
 * solve per level, composite (written over the pyramid's level 0),
 * SHA-1 of the mask and of the filled frame, exact counters. photo is
 * 1600x1200 packed RGB, mask 1600x1200 bytes (nonzero = fill me: the
 * object; 0 = keep). Returns 0 and fills *out, or -1 on a NULL/
 * mismatch or a degenerate plane. */
int or_process(const uint8_t *photo, const uint8_t *mask, or_scratch *s,
               or_result *out);

/* --- corpus / run ---------------------------------------------------- */

/* Build the photo corpus (fixed seed => byte-identical photos on every
 * machine; generation off the clock), then run the reference pipeline
 * on every photo. */
or_ctx *or_prepare(unsigned seed);
void    or_free(or_ctx *ctx);

/* One workload pass with *threads threads (clamped to the photo set of
 * the pass: 4 single-core, 16 multi-core). Fills *throughput_mpx with
 * the median over the runs, in millions of filled output pixels per
 * second, and *pixels with the exact output-pixel count of the pass
 * (4 or 16 photos). Returns 0, or -1 if any photo failed to verify. */
int or_run(or_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mpx, uint64_t *pixels);

/* Read-only access for tests and tooling. */
const uint8_t *or_photo(const or_ctx *ctx, unsigned scene);
size_t         or_photo_len(const or_ctx *ctx, unsigned scene);
const uint8_t *or_mask(const or_ctx *ctx, unsigned scene);
const or_result *or_reference(const or_ctx *ctx, unsigned scene);
unsigned       or_archetype(const or_ctx *ctx, unsigned scene);

/* The planted mask shapes and their exact geometry. */
or_disc  or_mask_disc(const or_ctx *ctx, unsigned scene);
or_rect  or_mask_rect(const or_ctx *ctx, unsigned scene);
or_box   or_mask_box(const or_ctx *ctx, unsigned scene);
uint32_t or_mask_pixels(const or_ctx *ctx, unsigned scene);

uint64_t or_task_pixels(unsigned multi); /* output pixels of one pass */

#endif /* WORKLOAD_OBJECTREMOVER_H */
