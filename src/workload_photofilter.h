#ifndef WORKLOAD_PHOTOFILTER_H
#define WORKLOAD_PHOTOFILTER_H

#include <stddef.h>
#include <stdint.h>

/* Workload #13 — Photo Filter.
 *
 * Mirrors the thirteenth Geekbench 6 CPU workload: a stack of color and
 * blur filters, levels, crop and scale applied to a set of photos.
 * Geekbench 6 pushes 10 photos single-core (40 multi-core) through its
 * in-house kernels; LotofBench reproduces the shape dependency-free:
 * ten seeded synthetic photos in six archetype families run through a
 * fixed filter pipeline — saturation, levels, box blur (two-pass
 * Gaussian approximation), central crop and bilinear rescale — all
 * built on the shared integer image kernels of src/imgops.c. Everything
 * is documented in docs/benchmarks.md, section "Workload 13". */

typedef struct pf_ctx pf_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the photos are the photos. */
#define PF_PHOTOS       10u  /* the single-core pass filters these ten */
#define PF_PHOTOS_MULTI 40u  /* multi-core pass = 4 x the photo set */
#define PF_PHOTO_W    1600u  /* device pixels per photo (RGB888) */
#define PF_PHOTO_H    1200u

/* Pipeline geometry: the central crop takes CROP_NUM/CROP_DEN of each
 * dimension, then the rescale brings it back to the full frame. */
#define PF_CROP_NUM      7u
#define PF_CROP_DEN      8u
#define PF_BLUR_RADIUS   2u

/* Build the photo corpus (off the clock; fixed seed => byte-identical
 * photos on every machine), the per-photo filter parameters and the
 * reference digest of every fully filtered photo. */
pf_ctx *pf_prepare(unsigned seed);
void    pf_free(pf_ctx *ctx);

/* One workload pass with *threads threads (clamped to the photo set of
 * the pass: 10 single-core, 40 multi-core). Fills *throughput_mpixels
 * with the median over the runs, in millions of output pixels per
 * second, and *pixels with the exact output-pixel count of the pass
 * (deterministic, identical for every run). Returns 0, or -1 if any
 * photo failed to verify. */
int pf_run(pf_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mpixels, uint64_t *pixels);

/* Read-only access for tests and tooling. */
const uint8_t *pf_photo(const pf_ctx *ctx, unsigned photo); /* RGB888 */
size_t         pf_photo_len(const pf_ctx *ctx, unsigned photo);
const uint8_t *pf_reference_digest(const pf_ctx *ctx, unsigned photo);
int32_t        pf_photo_saturation(const pf_ctx *ctx, unsigned photo);
unsigned       pf_photo_black(const pf_ctx *ctx, unsigned photo);
unsigned       pf_photo_white(const pf_ctx *ctx, unsigned photo);
unsigned       pf_archetype(const pf_ctx *ctx, unsigned photo);
uint64_t       pf_task_pixels(unsigned multi); /* output px of one pass */

#endif /* WORKLOAD_PHOTOFILTER_H */
