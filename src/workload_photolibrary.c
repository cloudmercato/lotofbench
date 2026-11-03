/* LotofBench — Photo Library workload (workload #5).
 *
 * Mirrors the fifth Geekbench 6 CPU workload: an import session that
 * decodes each photo, generates thumbnails, classifies it for the
 * auto-tag and stores the metadata. Geekbench 6 uses MobileNet 1.0 and
 * SQLite over real photos; LotofBench reproduces the shape
 * dependency-free, with the same honesty rules as the other workloads:
 *
 *   - deterministic synthetic input — a seeded set of 16 photos
 *     (1280x960 RGB) in eight content classes (beach, forest, desert,
 *     ocean, snow, volcano, meadow, night) that cycle the way a real
 *     camera roll does. Each photo is a vertical class gradient, a
 *     banding texture, seeded shapes and per-pixel noise, all integer
 *     arithmetic — fixed --seed means byte-identical corpora on every
 *     machine. Generated off the clock — the stand-in for the card
 *     import (and for the JPEG decode Geekbench measures; see the
 *     divergences in docs/benchmarks.md);
 *   - three exact box thumbnails per photo at 1/4, 1/8 and 1/16 linear
 *     scale via the shared img_box_downscale kernel (a real importer
 *     builds the same ladder), each one SHA-1-checked against its
 *     prepare-time reference;
 *   - a fixed integer conv pass over the 1/8 thumbnail — the MobileNet
 *     stand-in: 8 filters (3x3, 8.8 fixed point), ReLU clamp, 2x2 max
 *     pool, global average pool — producing an 8-dim feature vector;
 *   - the auto-tag: nearest-centroid classification against the
 *     per-class reference features. The class is planted in the
 *     corpus generator, so a tag that does not match the planted class
 *     aborts the run — the classifier has to actually read the image;
 *   - a per-slice in-memory record store (the SQLite stand-in): one
 *     record per ingested photo in an open-addressing hash table,
 *     then integrity queries — every id present, per-class counts and
 *     per-class id lists matching the planted ground truth;
 *   - shared-task multi-core through the common harness — the
 *     multi-core pass ingests 64 photos (4x the set) split into equal
 *     contiguous slices, one per thread; threads never share writable
 *     state.
 *
 * The throughput metric is photos per second — the photo-workload
 * analogue of HTML5 Browser's pages/s; the photo geometry is fixed by
 * the workload constants, so the metric is machine-independent.
 *
 * Deliberate divergences from Geekbench 6 (all documented in
 * docs/benchmarks.md): synthetic photos instead of a real photo
 * library, no JPEG codec (the import stage is off the clock), a fixed
 * 8-filter integer conv pass instead of MobileNet v1, and an
 * in-memory record store instead of SQLite.
 */

#include "workload_photolibrary.h"

#include "harness.h"
#include "imgops.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PHOTO_BYTES  ((size_t)PL_PHOTO_W * PL_PHOTO_H * 3)
#define THUMB0_BYTES ((size_t)PL_THUMB0_W * PL_THUMB0_H * 3)
#define THUMB1_BYTES ((size_t)PL_THUMB1_W * PL_THUMB1_H * 3)
#define THUMB2_BYTES ((size_t)PL_THUMB2_W * PL_THUMB2_H * 3)
#define CLS_BYTES    ((size_t)PL_CLS_W * PL_CLS_H * 3)

/* --- small helpers --------------------------------------------------------*/

static inline uint8_t clamp_u8(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

/* Exact integer square root (bit-by-bit, machine-independent) — the
 * disk rasterizer of the corpus generator needs it. */
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

/* --- corpus generation -----------------------------------------------------*/

/* Content classes: {gradient top, gradient bottom, accent}, each RGB,
 * plus the noise amplitude (0..255 scale). Eight families whose mean
 * colors sit far apart — the classifier has to tell them apart after
 * thumbnails, noise and shapes. */
typedef struct {
    uint8_t top[3], bottom[3], accent[3];
    unsigned noise_amp;
    const char *name;
} pl_scene;

static const pl_scene CLASSES[PL_CLASSES] = {
    { { 175, 208, 238 }, { 130, 175, 220 }, { 244, 226, 178 }, 6,  "beach" },
    { { 52, 104, 58 },   { 28, 66, 36 },    { 148, 188, 108 }, 8,  "forest" },
    { { 220, 186, 130 }, { 190, 152, 98 },  { 242, 214, 160 }, 5,  "desert" },
    { { 38, 92, 172 },   { 18, 58, 128 },   { 120, 168, 220 }, 6,  "ocean" },
    { { 236, 241, 248 }, { 214, 224, 236 }, { 255, 255, 255 }, 4,  "snow" },
    { { 100, 54, 42 },   { 64, 30, 24 },    { 208, 106, 60 },  9,  "volcano" },
    { { 122, 192, 92 },  { 88, 148, 62 },   { 226, 242, 158 }, 6,  "meadow" },
    { { 30, 32, 56 },    { 12, 14, 28 },    { 176, 178, 214 }, 10, "night" },
};

/* The class background: vertical gradient between the class colors plus
 * a smooth banding texture. Resolution-independent — the classifier
 * swatches run it at photo scale too, then follow the exact same
 * downscale path as the photos. */
static void gen_background(uint8_t *img, size_t w, size_t h, unsigned cls)
{
    const pl_scene *a = &CLASSES[cls % PL_CLASSES];

    for (size_t y = 0; y < h; y++) {
        uint32_t t = (uint32_t)(y * 255u / (h - 1));
        uint8_t *row = img + y * w * 3;

        for (size_t x = 0; x < w; x++) {
            int32_t band = (int32_t)((x * x / 211u + y * y / 149u) % 13u) - 6;

            for (int c = 0; c < 3; c++) {
                int32_t base = (int32_t)a->top[c] +
                    (((int32_t)a->bottom[c] - (int32_t)a->top[c]) *
                     (int32_t)t + 127) / 255;

                row[x * 3 + c] = clamp_u8(base + band);
            }
        }
    }
}

static void gen_photo(uint8_t *img, unsigned cls, rng_t *rng)
{
    const pl_scene *a = &CLASSES[cls % PL_CLASSES];
    unsigned n_shapes = 2 + rng_below(rng, 2); /* 2..3 subjects */

    gen_background(img, PL_PHOTO_W, PL_PHOTO_H, cls);

    /* seeded shapes: disks and rectangles in the accent color
     * (lightness-variated), the photo's "subjects" */
    for (unsigned s = 0; s < n_shapes; s++) {
        uint32_t cx = rng_below(rng, PL_PHOTO_W);
        uint32_t cy = rng_below(rng, PL_PHOTO_H);
        uint32_t rad = 40 + rng_below(rng, 111); /* 40..150 */
        int32_t tone = (int32_t)rng_below(rng, 33) - 16; /* -16..15 */
        uint8_t col[3];
        int rect = (s & 1u) != 0;

        for (int c = 0; c < 3; c++)
            col[c] = clamp_u8((int32_t)a->accent[c] + tone);
        if (rect) {
            uint32_t x0 = cx > rad ? cx - rad : 0;
            uint32_t x1 = cx + rad < PL_PHOTO_W ? cx + rad : PL_PHOTO_W;
            uint32_t y0 = cy > rad ? cy - rad : 0;
            uint32_t y1 = cy + rad < PL_PHOTO_H ? cy + rad : PL_PHOTO_H;

            for (uint32_t y = y0; y < y1; y++)
                for (uint32_t x = x0; x < x1; x++)
                    for (int c = 0; c < 3; c++)
                        img[(y * PL_PHOTO_W + x) * 3 + c] = col[c];
        } else {
            uint32_t y0 = cy > rad ? cy - rad : 0;
            uint32_t y1 = cy + rad < PL_PHOTO_H ? cy + rad : PL_PHOTO_H;

            for (uint32_t y = y0; y < y1; y++) {
                int64_t dy = (int64_t)y - cy;
                int64_t disc = (int64_t)rad * rad - dy * dy;
                uint32_t half = disc > 0 ? isqrt64((uint64_t)disc) : 0;
                uint32_t x0 = cx > half ? cx - half : 0;
                uint32_t x1 = cx + half < PL_PHOTO_W ? cx + half
                                                     : PL_PHOTO_W;

                for (uint32_t x = x0; x < x1; x++)
                    for (int c = 0; c < 3; c++)
                        img[(y * PL_PHOTO_W + x) * 3 + c] = col[c];
            }
        }
    }

    /* per-pixel noise, amplitude per class — one rng draw per pixel,
     * low three bytes become the channel deltas */
    for (size_t y = 0; y < PL_PHOTO_H; y++) {
        uint8_t *row = img + y * PL_PHOTO_W * 3;

        for (size_t x = 0; x < PL_PHOTO_W; x++) {
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

/* --- the fixed conv pass (the MobileNet stand-in) --------------------------*/

/* 8 output channels x 9 taps (3x3, row-major, tap = dy*3+dx) x 3 input
 * channels (R, G, B), 8.8 fixed point. Channels 0-2 are per-channel
 * mean probes (gain 252/256), channel 3 a luma probe (9R+17G+3B per
 * tap), channels 4-6 signed gradient probes on the luma mix, channel 7
 * a zero-sum Laplacian edge probe. Workload constants, not learned
 * weights — documented in docs/benchmarks.md. */
static const int16_t CONV1[PL_FEATURE_DIMS][9][3] = {
    /* 0: R mean */ {
        { 28, 0, 0 }, { 28, 0, 0 }, { 28, 0, 0 },
        { 28, 0, 0 }, { 28, 0, 0 }, { 28, 0, 0 },
        { 28, 0, 0 }, { 28, 0, 0 }, { 28, 0, 0 },
    },
    /* 1: G mean */ {
        { 0, 28, 0 }, { 0, 28, 0 }, { 0, 28, 0 },
        { 0, 28, 0 }, { 0, 28, 0 }, { 0, 28, 0 },
        { 0, 28, 0 }, { 0, 28, 0 }, { 0, 28, 0 },
    },
    /* 2: B mean */ {
        { 0, 0, 28 }, { 0, 0, 28 }, { 0, 0, 28 },
        { 0, 0, 28 }, { 0, 0, 28 }, { 0, 0, 28 },
        { 0, 0, 28 }, { 0, 0, 28 }, { 0, 0, 28 },
    },
    /* 3: luma mean */ {
        { 9, 17, 3 }, { 9, 17, 3 }, { 9, 17, 3 },
        { 9, 17, 3 }, { 9, 17, 3 }, { 9, 17, 3 },
        { 9, 17, 3 }, { 9, 17, 3 }, { 9, 17, 3 },
    },
    /* 4: gradient X (luma mix) */ {
        { -9, -17, -3 }, { 0, 0, 0 }, { 9, 17, 3 },
        { -9, -17, -3 }, { 0, 0, 0 }, { 9, 17, 3 },
        { -9, -17, -3 }, { 0, 0, 0 }, { 9, 17, 3 },
    },
    /* 5: gradient Y (luma mix) */ {
        { -9, -17, -3 }, { -9, -17, -3 }, { -9, -17, -3 },
        { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
        { 9, 17, 3 }, { 9, 17, 3 }, { 9, 17, 3 },
    },
    /* 6: gradient diagonal (luma mix) */ {
        { -9, -17, -3 }, { 0, 0, 0 }, { 0, 0, 0 },
        { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
        { 0, 0, 0 }, { 0, 0, 0 }, { 9, 17, 3 },
    },
    /* 7: Laplacian (luma mix), zero sum */ {
        { 0, 0, 0 }, { -9, -17, -3 }, { 0, 0, 0 },
        { -9, -17, -3 }, { 36, 68, 12 }, { -9, -17, -3 },
        { 0, 0, 0 }, { -9, -17, -3 }, { 0, 0, 0 },
    },
};

size_t pl_features_scratch_len(size_t w, size_t h)
{
    return PL_FEATURE_DIMS * w * h +
           PL_FEATURE_DIMS * (w / 2) * (h / 2);
}

void pl_extract_features(const uint8_t *rgb, size_t w, size_t h,
                         uint16_t *feat, uint8_t *scratch)
{
    uint8_t *conv = scratch;                          /* 8 x w x h */
    uint8_t *pool = scratch + PL_FEATURE_DIMS * w * h; /* 8 x w/2 x h/2 */

    /* conv1: 3x3, edge-clamped windows, 8.8 fixed point with half-up
     * rounding, clamped to 0..255 (the low clamp is the ReLU) */
    for (size_t y = 0; y < h; y++) {
        size_t ym = y > 0 ? y - 1 : 0;
        size_t yp = y + 1 < h ? y + 1 : h - 1;

        for (size_t x = 0; x < w; x++) {
            size_t xm = x > 0 ? x - 1 : 0;
            size_t xp = x + 1 < w ? x + 1 : w - 1;
            const uint8_t *px[3][3] = {
                { rgb + (ym * w + xm) * 3, rgb + (ym * w + x) * 3,
                  rgb + (ym * w + xp) * 3 },
                { rgb + (y * w + xm) * 3, rgb + (y * w + x) * 3,
                  rgb + (y * w + xp) * 3 },
                { rgb + (yp * w + xm) * 3, rgb + (yp * w + x) * 3,
                  rgb + (yp * w + xp) * 3 },
            };

            for (unsigned k = 0; k < PL_FEATURE_DIMS; k++) {
                int32_t acc = 0;

                for (unsigned t = 0; t < 9; t++)
                    for (int c = 0; c < 3; c++)
                        acc += (int32_t)CONV1[k][t][c] *
                               (int32_t)px[t / 3][t % 3][c];
                conv[(k * h + y) * w + x] = clamp_u8((acc + 128) >> 8);
            }
        }
    }

    /* 2x2 max pool */
    for (unsigned k = 0; k < PL_FEATURE_DIMS; k++)
        for (size_t y = 0; y < h / 2; y++)
            for (size_t x = 0; x < w / 2; x++) {
                const uint8_t *c00 = conv + (k * h + 2 * y) * w + 2 * x;
                const uint8_t *c10 = c00 + w;
                uint8_t m = c00[0] > c00[1] ? c00[0] : c00[1];
                uint8_t m1 = c10[0] > c10[1] ? c10[0] : c10[1];

                pool[(k * (h / 2) + y) * (w / 2) + x] = m > m1 ? m : m1;
            }

    /* global average pool per channel, half-up */
    {
        size_t n = (w / 2) * (h / 2);
        uint32_t half = (uint32_t)(n / 2);

        for (unsigned k = 0; k < PL_FEATURE_DIMS; k++) {
            uint32_t sum = 0;
            const uint8_t *p = pool + k * (h / 2) * (w / 2);

            for (size_t i = 0; i < n; i++)
                sum += p[i];
            feat[k] = (uint16_t)((sum + half) / (uint32_t)n);
        }
    }
}

/* --- context -----------------------------------------------------------------*/

struct pl_ctx {
    uint8_t *photo[PL_PHOTOS];
    uint8_t  ref_digest[PL_PHOTOS][PL_THUMBS][20];
    unsigned cls[PL_PHOTOS];
    uint8_t  swatch[PL_CLASSES][CLS_BYTES]; /* clean 1/8-scale class thumb */
    uint16_t centroid[PL_CLASSES][PL_FEATURE_DIMS];
};

/* Nearest centroid, integer squared distance, lowest class wins ties. */
unsigned pl_classify(const uint16_t *feat, const pl_ctx *ctx)
{
    uint64_t best = UINT64_MAX;
    unsigned best_cls = 0;

    if (!ctx)
        return 0;
    for (unsigned c = 0; c < PL_CLASSES; c++) {
        uint64_t d = 0;

        for (unsigned k = 0; k < PL_FEATURE_DIMS; k++) {
            int32_t e = (int32_t)feat[k] - (int32_t)ctx->centroid[c][k];

            d += (uint64_t)((int64_t)e * e);
        }
        if (d < best) {
            best = d;
            best_cls = c;
        }
    }
    return best_cls;
}

/* Build the classifier tables: the clean background of every class,
 * taken through the exact same downscale + feature path as the photos,
 * becomes the class swatch and centroid. The tables are a pure function
 * of the class constants — no seed, no rng. */
static int build_classifier(pl_ctx *ctx)
{
    uint8_t *full = malloc(PHOTO_BYTES);
    uint8_t *scratch = malloc(pl_features_scratch_len(PL_CLS_W, PL_CLS_H));

    if (!full || !scratch) {
        free(full);
        free(scratch);
        return -1;
    }
    for (unsigned c = 0; c < PL_CLASSES; c++) {
        gen_background(full, PL_PHOTO_W, PL_PHOTO_H, c);
        img_box_downscale(ctx->swatch[c], PL_CLS_W, PL_CLS_H, full,
                          PL_PHOTO_W, PL_PHOTO_H);
        pl_extract_features(ctx->swatch[c], PL_CLS_W, PL_CLS_H,
                            ctx->centroid[c], scratch);
    }
    free(full);
    free(scratch);

    /* self-consistency: every centroid must classify to its own class
     * (duplicate centroids would let a lower class steal the argmin) */
    for (unsigned c = 0; c < PL_CLASSES; c++)
        if (pl_classify(ctx->centroid[c], ctx) != c) {
            fprintf(stderr,
                    "lotofbench: photo-library: class %u centroid is not "
                    "distinct\n", c);
            return -1;
        }
    return 0;
}

pl_ctx *pl_prepare(unsigned seed)
{
    pl_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t rng;
    uint8_t *thumb[PL_THUMBS];
    uint8_t *scratch;
    static const unsigned tdim[PL_THUMBS][2] = {
        { PL_THUMB0_W, PL_THUMB0_H },
        { PL_THUMB1_W, PL_THUMB1_H },
        { PL_THUMB2_W, PL_THUMB2_H },
    };
    static const size_t tbytes[PL_THUMBS] = {
        THUMB0_BYTES, THUMB1_BYTES, THUMB2_BYTES
    };

    if (!ctx)
        return NULL;
    if (build_classifier(ctx) != 0) {
        free(ctx);
        return NULL;
    }

    thumb[0] = malloc(THUMB0_BYTES);
    thumb[1] = malloc(THUMB1_BYTES);
    thumb[2] = malloc(THUMB2_BYTES);
    scratch = malloc(pl_features_scratch_len(PL_CLS_W, PL_CLS_H));
    if (!thumb[0] || !thumb[1] || !thumb[2] || !scratch) {
        free(thumb[0]);
        free(thumb[1]);
        free(thumb[2]);
        free(scratch);
        pl_free(ctx);
        return NULL;
    }

    rng_init(&rng, seed);
    for (unsigned p = 0; p < PL_PHOTOS; p++) {
        ctx->photo[p] = malloc(PHOTO_BYTES);
        if (!ctx->photo[p]) {
            free(thumb[0]);
            free(thumb[1]);
            free(thumb[2]);
            free(scratch);
            pl_free(ctx);
            return NULL;
        }
        ctx->cls[p] = p % PL_CLASSES;
        gen_photo(ctx->photo[p], ctx->cls[p], &rng);

        /* the thumbnail ladder + references, exactly as the timed
         * cycle will rebuild them */
        for (unsigned t = 0; t < PL_THUMBS; t++) {
            img_box_downscale(thumb[t], tdim[t][0], tdim[t][1],
                              ctx->photo[p], PL_PHOTO_W, PL_PHOTO_H);
            sha1(thumb[t], tbytes[t], ctx->ref_digest[p][t]);
        }

        /* the corpus is only valid if the classifier recovers the
         * planted class of every photo — abort loudly otherwise */
        {
            uint16_t feat[PL_FEATURE_DIMS];

            pl_extract_features(thumb[1], PL_CLS_W, PL_CLS_H, feat,
                                scratch);
            if (pl_classify(feat, ctx) != ctx->cls[p]) {
                fprintf(stderr,
                        "lotofbench: photo-library: photo %u classified "
                        "as class %u, planted %u — corpus broken\n",
                        p, pl_classify(feat, ctx), ctx->cls[p]);
                free(thumb[0]);
                free(thumb[1]);
                free(thumb[2]);
                free(scratch);
                pl_free(ctx);
                return NULL;
            }
        }
    }
    free(thumb[0]);
    free(thumb[1]);
    free(thumb[2]);
    free(scratch);
    return ctx;
}

void pl_free(pl_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned p = 0; p < PL_PHOTOS; p++)
        free(ctx->photo[p]);
    free(ctx);
}

/* --- the per-slice record store (the SQLite stand-in) ----------------------*/

typedef struct {
    uint32_t id;    /* instance index within the pass */
    uint32_t tag;   /* first 4 bytes of the 1/4-thumb digest */
    uint8_t  cls;   /* the auto-tag */
    uint8_t  lum;   /* luma-probe feature of the photo */
    uint16_t w, h;  /* largest thumbnail size */
} pl_record;

#define PL_RECORD_EMPTY UINT32_MAX

/* One thread's slice: contiguous photo instances, own scratch, own
 * store — threads never share writable state (harness contract). */
typedef struct {
    pl_ctx *ctx;
    uint8_t *thumb0, *thumb1, *thumb2;
    uint8_t *scratch;  /* pl_features_scratch_len(PL_CLS_W, PL_CLS_H) */
    pl_record *records;
    uint32_t *table;   /* open addressing, stores record indexes */
    uint32_t cap;      /* table capacity, power of two */
    size_t begin, end; /* instance range [begin, end) of the pass */
} pl_slice;

static uint32_t store_hash(uint32_t id, uint32_t cap)
{
    return (uint32_t)(id * 2654435761u) & (cap - 1);
}

static void store_insert(pl_slice *sl, const pl_record *rec)
{
    uint32_t h = store_hash(rec->id, sl->cap);

    while (sl->table[h] != PL_RECORD_EMPTY)
        h = (h + 1) & (sl->cap - 1);
    sl->table[h] = (uint32_t)(rec - sl->records);
}

static const pl_record *store_find(const pl_slice *sl, uint32_t id)
{
    uint32_t h = store_hash(id, sl->cap);

    while (sl->table[h] != PL_RECORD_EMPTY) {
        const pl_record *r = &sl->records[sl->table[h]];

        if (r->id == id)
            return r;
        h = (h + 1) & (sl->cap - 1);
    }
    return NULL;
}

static uint32_t next_pow2(uint32_t v)
{
    uint32_t p = 8;

    while (p < v)
        p <<= 1;
    return p;
}

/* The store queries: every id present with the expected fields, then
 * the per-class summary (count + ascending ids) matching the planted
 * ground truth of the slice. Returns 0, or -1 on any divergence. */
static int store_verify(pl_slice *sl)
{
    uint32_t ids_found[PL_PHOTOS_MULTI];
    uint32_t ids_want[PL_PHOTOS_MULTI];

    /* point queries: every instance of the slice, field by field */
    for (size_t i = sl->begin; i < sl->end; i++) {
        unsigned p = (unsigned)(i % PL_PHOTOS);
        const pl_record *r = store_find(sl, (uint32_t)i);

        if (!r || r->cls != sl->ctx->cls[p] ||
            r->tag != (uint32_t)(((uint32_t)sl->ctx->ref_digest[p][0][0] << 24) |
                                 ((uint32_t)sl->ctx->ref_digest[p][0][1] << 16) |
                                 ((uint32_t)sl->ctx->ref_digest[p][0][2] << 8) |
                                 sl->ctx->ref_digest[p][0][3]) ||
            r->w != PL_THUMB0_W || r->h != PL_THUMB0_H)
            return -1;
    }

    /* per-class summary: ids collected from the table, sorted, against
     * the ground-truth list built from the planted classes */
    for (unsigned c = 0; c < PL_CLASSES; c++) {
        unsigned nf = 0, nw = 0;

        for (uint32_t slot = 0; slot < sl->cap; slot++)
            if (sl->table[slot] != PL_RECORD_EMPTY &&
                sl->records[sl->table[slot]].cls == c)
                ids_found[nf++] = sl->records[sl->table[slot]].id;
        for (size_t i = sl->begin; i < sl->end; i++)
            if (sl->ctx->cls[i % PL_PHOTOS] == c)
                ids_want[nw++] = (uint32_t)i;
        if (nf != nw)
            return -1;
        /* insertion sort both, then compare (ids are small) */
        for (unsigned a = 1; a < nf; a++) {
            uint32_t v = ids_found[a];
            unsigned b = a;

            while (b > 0 && ids_found[b - 1] > v) {
                ids_found[b] = ids_found[b - 1];
                b--;
            }
            ids_found[b] = v;
        }
        for (unsigned a = 1; a < nw; a++) {
            uint32_t v = ids_want[a];
            unsigned b = a;

            while (b > 0 && ids_want[b - 1] > v) {
                ids_want[b] = ids_want[b - 1];
                b--;
            }
            ids_want[b] = v;
        }
        for (unsigned a = 0; a < nf; a++)
            if (ids_found[a] != ids_want[a])
                return -1;
    }
    return 0;
}

/* --- run ---------------------------------------------------------------------*/

static int pl_cycle(void *arg)
{
    pl_slice *sl = arg;
    pl_ctx *ctx = sl->ctx;
    static const unsigned tdim[PL_THUMBS][2] = {
        { PL_THUMB0_W, PL_THUMB0_H },
        { PL_THUMB1_W, PL_THUMB1_H },
        { PL_THUMB2_W, PL_THUMB2_H },
    };

    /* each run starts from an empty store — the harness calls the
     * cycle once per run, so this is the per-run reset point */
    memset(sl->table, 0xFF, (size_t)sl->cap * sizeof(uint32_t));

    for (size_t i = sl->begin; i < sl->end; i++) {
        unsigned p = (unsigned)(i % PL_PHOTOS);
        const uint8_t *src = ctx->photo[p];
        const uint8_t *ref0 = ctx->ref_digest[p][0];
        const uint8_t *ref1 = ctx->ref_digest[p][1];
        const uint8_t *ref2 = ctx->ref_digest[p][2];
        uint8_t dig[20];
        uint16_t feat[PL_FEATURE_DIMS];
        pl_record rec;

        /* 1. the thumbnail ladder */
        img_box_downscale(sl->thumb0, tdim[0][0], tdim[0][1], src,
                          PL_PHOTO_W, PL_PHOTO_H);
        img_box_downscale(sl->thumb1, tdim[1][0], tdim[1][1], src,
                          PL_PHOTO_W, PL_PHOTO_H);
        img_box_downscale(sl->thumb2, tdim[2][0], tdim[2][1], src,
                          PL_PHOTO_W, PL_PHOTO_H);

        /* 2. every thumbnail verifies against its prepare-time
         * reference — a failed digest aborts the run */
        sha1(sl->thumb0, THUMB0_BYTES, dig);
        if (memcmp(dig, ref0, 20) != 0)
            return -1;
        sha1(sl->thumb1, THUMB1_BYTES, dig);
        if (memcmp(dig, ref1, 20) != 0)
            return -1;
        sha1(sl->thumb2, THUMB2_BYTES, dig);
        if (memcmp(dig, ref2, 20) != 0)
            return -1;

        /* 3. the auto-tag: conv pass over the 1/8 thumb, then
         * nearest-centroid; must recover the planted class */
        pl_extract_features(sl->thumb1, PL_CLS_W, PL_CLS_H, feat,
                            sl->scratch);
        if (pl_classify(feat, ctx) != ctx->cls[p])
            return -1;

        /* 4. ingest the metadata record */
        rec.id = (uint32_t)i;
        rec.cls = (uint8_t)ctx->cls[p];
        rec.lum = (uint8_t)feat[3];
        rec.w = PL_THUMB0_W;
        rec.h = PL_THUMB0_H;
        rec.tag = (uint32_t)(((uint32_t)ref0[0] << 24) |
                             ((uint32_t)ref0[1] << 16) |
                             ((uint32_t)ref0[2] << 8) | ref0[3]);
        sl->records[i - sl->begin] = rec;
        store_insert(sl, &sl->records[i - sl->begin]);
    }

    return store_verify(sl);
}

uint64_t pl_task_photos(unsigned multi)
{
    return multi ? PL_PHOTOS_MULTI : PL_PHOTOS;
}

int pl_run(pl_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_ips, uint64_t *photos)
{
    unsigned t_count = *threads;
    unsigned photo_set = t_count > 1 ? PL_PHOTOS_MULTI : PL_PHOTOS;
    pl_slice *slices;
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
            pl_slice *sl = &slices[t];

            sl->ctx = ctx;
            sl->begin = begin;
            sl->end = begin + count;
            begin += count;
            sl->thumb0 = malloc(THUMB0_BYTES);
            sl->thumb1 = malloc(THUMB1_BYTES);
            sl->thumb2 = malloc(THUMB2_BYTES);
            sl->scratch =
                malloc(pl_features_scratch_len(PL_CLS_W, PL_CLS_H));
            sl->records = malloc(count * sizeof(pl_record));
            sl->cap = next_pow2((uint32_t)(2 * count));
            sl->table = malloc((size_t)sl->cap * sizeof(uint32_t));
            if (!sl->thumb0 || !sl->thumb1 || !sl->thumb2 ||
                !sl->scratch || !sl->records || !sl->table)
                goto out;
            ctxs[t] = sl;
        }
    }

    rc = harness_run(t_count, runs, pl_cycle, ctxs, elapsed);
    if (rc == 0) {
        uint64_t set = (uint64_t)photo_set;

        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0 ? set / elapsed[r] : -1.0;
        *throughput_ips = lb_median(rates, runs);
        *threads = t_count;
        *photos = set;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++) {
            free(slices[t].thumb0);
            free(slices[t].thumb1);
            free(slices[t].thumb2);
            free(slices[t].scratch);
            free(slices[t].records);
            free(slices[t].table);
        }
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors ----------------------------------------------------------------*/

const uint8_t *pl_photo(const pl_ctx *ctx, unsigned photo)
{
    return ctx && photo < PL_PHOTOS ? ctx->photo[photo] : NULL;
}

size_t pl_photo_len(const pl_ctx *ctx, unsigned photo)
{
    return ctx && photo < PL_PHOTOS ? PHOTO_BYTES : 0;
}

unsigned pl_photo_class(const pl_ctx *ctx, unsigned photo)
{
    return ctx && photo < PL_PHOTOS ? ctx->cls[photo] : 0;
}

const uint8_t *pl_reference_digest(const pl_ctx *ctx, unsigned photo,
                                   unsigned thumb)
{
    if (!ctx || photo >= PL_PHOTOS || thumb >= PL_THUMBS)
        return NULL;
    return ctx->ref_digest[photo][thumb];
}

const uint8_t *pl_class_swatch(const pl_ctx *ctx, unsigned cls)
{
    return ctx && cls < PL_CLASSES ? ctx->swatch[cls] : NULL;
}

const uint16_t *pl_class_centroid(const pl_ctx *ctx, unsigned cls)
{
    return ctx && cls < PL_CLASSES ? ctx->centroid[cls] : NULL;
}
