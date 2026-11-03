#ifndef WORKLOAD_PHOTOLIBRARY_H
#define WORKLOAD_PHOTOLIBRARY_H

#include <stddef.h>
#include <stdint.h>

/* Workload #5 — Photo Library.
 *
 * Mirrors the fifth Geekbench 6 CPU workload: an import session that
 * decodes each photo, generates thumbnails, runs a classifier for the
 * auto-tag and stores the metadata (Geekbench 6 uses MobileNet 1.0 plus
 * SQLite; LotofBench reproduces the shape dependency-free). Sixteen
 * seeded synthetic photos (1280x960 RGB) in eight content classes run
 * through a fixed per-photo pipeline:
 *
 *   1. three exact box thumbnails at 1/4, 1/8 and 1/16 linear scale
 *      (320x240, 160x120, 80x60), from src/imgops.c;
 *   2. a fixed integer conv pass (8 filters, 3x3, 8.8 fixed point, ReLU
 *      clamp, 2x2 max pool, global average pool) over the 1/8 thumbnail
 *      — the MobileNet stand-in — yielding an 8-dim feature vector;
 *   3. nearest-centroid classification against the per-class reference
 *      features (the auto-tag), which must recover the planted class;
 *   4. a record inserted into a per-slice in-memory store (the SQLite
 *      stand-in), then integrity queries over the store.
 *
 * Everything is documented in docs/benchmarks.md, section "Workload 5". */

typedef struct pl_ctx pl_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the photos are the photos. */
#define PL_PHOTOS        16u  /* the single-core pass ingests these */
#define PL_PHOTOS_MULTI  64u  /* multi-core pass = 4 x the photo set */
#define PL_PHOTO_W     1280u  /* device pixels per photo (RGB888) */
#define PL_PHOTO_H      960u
#define PL_CLASSES        8u  /* content classes, photo p is class p % 8 */

/* Thumbnails: powers-of-two linear scales of the photo. The 1/8
 * thumbnail doubles as the classifier input (PL_CLS_W x PL_CLS_H). */
#define PL_THUMBS         3u
#define PL_THUMB_SCALE0   4u
#define PL_THUMB_SCALE1   8u
#define PL_THUMB_SCALE2  16u
#define PL_THUMB0_W  (PL_PHOTO_W / PL_THUMB_SCALE0)
#define PL_THUMB0_H  (PL_PHOTO_H / PL_THUMB_SCALE0)
#define PL_THUMB1_W  (PL_PHOTO_W / PL_THUMB_SCALE1)
#define PL_THUMB1_H  (PL_PHOTO_H / PL_THUMB_SCALE1)
#define PL_THUMB2_W  (PL_PHOTO_W / PL_THUMB_SCALE2)
#define PL_THUMB2_H  (PL_PHOTO_H / PL_THUMB_SCALE2)
#define PL_CLS_W     PL_THUMB1_W
#define PL_CLS_H     PL_THUMB1_H

/* Feature vector length produced by the fixed conv pass. */
#define PL_FEATURE_DIMS   8u

/* Build the photo corpus (off the clock; fixed seed => byte-identical
 * photos on every machine), the per-class classifier tables and the
 * reference digest of every thumbnail. Returns NULL (after a message
 * on stderr) if the corpus could not be built or a photo failed to
 * classify against its planted class. */
pl_ctx *pl_prepare(unsigned seed);
void    pl_free(pl_ctx *ctx);

/* One workload pass with *threads threads (clamped to the photo set of
 * the pass: 16 single-core, 64 multi-core). Fills *throughput_ips with
 * the median over the runs, in photos per second, and *photos with the
 * exact photo count of the pass (deterministic, identical for every
 * run). Returns 0, or -1 if any thumbnail, tag or record failed to
 * verify. */
int pl_run(pl_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_ips, uint64_t *photos);

/* --- read-only access for tests and tooling ------------------------------*/

const uint8_t *pl_photo(const pl_ctx *ctx, unsigned photo); /* RGB888 */
size_t         pl_photo_len(const pl_ctx *ctx, unsigned photo);
unsigned       pl_photo_class(const pl_ctx *ctx, unsigned photo);
const uint8_t *pl_reference_digest(const pl_ctx *ctx, unsigned photo,
                                   unsigned thumb); /* 20-byte SHA-1 */
/* The classifier reference of a class: the 1/8-scale thumbnail of the
 * clean background (PL_CLS_W x PL_CLS_H, RGB888) and its feature. */
const uint8_t *pl_class_swatch(const pl_ctx *ctx, unsigned cls);
const uint16_t *pl_class_centroid(const pl_ctx *ctx, unsigned cls);
uint64_t       pl_task_photos(unsigned multi);

/* --- public kernels for the /tmp gate and the unit tests -----------------*/

/* The fixed conv pass: 8 filters (3x3, 8.8 fixed point) with edge-
 * clamped windows and ReLU clamp, 2x2 max pool, global average pool.
 * rgb is a packed w x h RGB888 buffer with w and h even; feat receives
 * PL_FEATURE_DIMS values in 0..255. scratch must hold
 * pl_features_scratch_len(w, h) bytes (conv maps + pool maps). */
size_t pl_features_scratch_len(size_t w, size_t h);
void pl_extract_features(const uint8_t *rgb, size_t w, size_t h,
                         uint16_t *feat, uint8_t *scratch);

/* Nearest-centroid classification of a feature vector against the
 * per-class centroids of ctx (integer squared distance, lowest class
 * wins ties). pl_prepare must have run: the centroids live in the ctx. */
unsigned pl_classify(const uint16_t *feat, const pl_ctx *ctx);

#endif /* WORKLOAD_PHOTOLIBRARY_H */
