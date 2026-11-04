/* Unit tests — workload #5: Photo Library. */

#include "tests.h"

#include "imgops.h"
#include "sha1.h"
#include "workload_photolibrary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.10.0, seed 1). After a
 * DELIBERATE change to the corpus, the thumbnail ladder or the conv
 * pass, re-run the tool and port the printed values here in the same
 * commit. */
#define PL_GOLDEN_PHOTO0_SHA1 "55205d3a1c095b841f4eff7a9e4be78931c1dd0a"
#define PL_GOLDEN_PHOTO0_LEN  3686400u
static const uint8_t PL_GOLDEN_PHOTO0_THUMB[PL_THUMBS][20] = {
    { 0x46, 0x89, 0xa2, 0xb5, 0x2b, 0x43, 0x44, 0x84, 0x0c, 0xbb,
      0x1a, 0x3b, 0xd0, 0x71, 0x66, 0x40, 0x9c, 0x69, 0x10, 0x39 },
    { 0x66, 0x0d, 0xee, 0xec, 0xa7, 0xc2, 0xc0, 0x05, 0x97, 0x35,
      0x40, 0xca, 0x49, 0x49, 0x6a, 0xf0, 0x89, 0xc1, 0x3a, 0xb0 },
    { 0xe2, 0x53, 0x0f, 0x8a, 0x2d, 0xa6, 0x72, 0x62, 0x38, 0x69,
      0xf8, 0x8a, 0x92, 0x43, 0x0a, 0x83, 0xd7, 0x61, 0x80, 0xe3 },
};
static const uint16_t PL_GOLDEN_PHOTO0_FEAT[PL_FEATURE_DIMS] = {
    155, 191, 225, 190, 0, 0, 0, 0
};
static const uint16_t PL_GOLDEN_CENTROID0[PL_FEATURE_DIMS] = {
    151, 190, 226, 188, 0, 0, 0, 0
};
#define PL_GOLDEN_TASK_PHOTOS        16ull
#define PL_GOLDEN_TASK_PHOTOS_MULTI  64ull

/* enough runs to cross a table capacity boundary in run-sane */
#define PL_TEST_RUNS 5u

/* One shared prepare for the read-only tests (the corpus is immutable;
 * the process exits right after the suite anyway). */
static pl_ctx *shared_pl(void)
{
    static pl_ctx *ctx;

    if (!ctx)
        ctx = pl_prepare(1);
    return ctx;
}

static void photo_sha1(const pl_ctx *ctx, unsigned p, char hex[41])
{
    sha1_hex(pl_photo(ctx, p), pl_photo_len(ctx, p), hex);
}

void test_pl_corpus_deterministic(void)
{
    pl_ctx *a = pl_prepare(1), *b = pl_prepare(1);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        pl_free(a);
        pl_free(b);
        return;
    }
    for (unsigned p = 0; p < PL_PHOTOS; p++) {
        CHECK_U64(pl_photo_len(a, p), pl_photo_len(b, p));
        photo_sha1(a, p, ha);
        photo_sha1(b, p, hb);
        CHECK_STR(ha, hb);
        for (unsigned t = 0; t < PL_THUMBS; t++)
            CHECK(memcmp(pl_reference_digest(a, p, t),
                         pl_reference_digest(b, p, t), 20) == 0);
        CHECK_U64(pl_photo_class(a, p), pl_photo_class(b, p));
    }
    for (unsigned c = 0; c < PL_CLASSES; c++)
        CHECK(memcmp(pl_class_centroid(a, c), pl_class_centroid(b, c),
                     PL_FEATURE_DIMS * sizeof(uint16_t)) == 0);
    pl_free(a);
    pl_free(b);
}

void test_pl_input_invariants(void)
{
    pl_ctx *ctx = shared_pl();
    char hex[PL_PHOTOS][41];

    CHECK(ctx);
    if (!ctx)
        return;
    /* the classes cycle p % 8 the way the camera roll does */
    for (unsigned p = 0; p < PL_PHOTOS; p++)
        CHECK_U64(pl_photo_class(ctx, p), p % PL_CLASSES);
    /* fixed geometry */
    for (unsigned p = 0; p < PL_PHOTOS; p++)
        CHECK_U64(pl_photo_len(ctx, p),
                  (size_t)PL_PHOTO_W * PL_PHOTO_H * 3);
    /* the photos are pairwise distinct */
    for (unsigned p = 0; p < PL_PHOTOS; p++) {
        photo_sha1(ctx, p, hex[p]);
        for (unsigned q = 0; q < p; q++)
            CHECK(strcmp(hex[p], hex[q]) != 0);
    }
}

void test_pl_classify_planted(void)
{
    pl_ctx *ctx = shared_pl();
    uint8_t *t1, *scratch;
    uint16_t feat[PL_FEATURE_DIMS];

    CHECK(ctx);
    if (!ctx)
        return;
    t1 = malloc(PL_THUMB1_W * PL_THUMB1_H * 3);
    scratch = malloc(pl_features_scratch_len(PL_CLS_W, PL_CLS_H));
    CHECK(t1 && scratch);
    if (!t1 || !scratch) {
        free(t1);
        free(scratch);
        return;
    }
    /* the classifier recovers the planted class of every photo,
     * re-derived here through the public kernels */
    for (unsigned p = 0; p < PL_PHOTOS; p++) {
        img_box_downscale(t1, PL_CLS_W, PL_CLS_H, pl_photo(ctx, p),
                          PL_PHOTO_W, PL_PHOTO_H);
        pl_extract_features(t1, PL_CLS_W, PL_CLS_H, feat, scratch);
        CHECK_U64(pl_classify(feat, ctx), pl_photo_class(ctx, p));
    }
    /* every centroid classifies to its own class and the swatch
     * feature reproduces the centroid exactly */
    for (unsigned c = 0; c < PL_CLASSES; c++) {
        const uint8_t *sw = pl_class_swatch(ctx, c);

        pl_extract_features(sw, PL_CLS_W, PL_CLS_H, feat, scratch);
        CHECK(memcmp(feat, pl_class_centroid(ctx, c),
                     PL_FEATURE_DIMS * sizeof(uint16_t)) == 0);
        CHECK_U64(pl_classify(pl_class_centroid(ctx, c), ctx), c);
    }
    free(t1);
    free(scratch);
}

void test_pl_pipeline_units(void)
{
    pl_ctx *ctx = shared_pl();
    /* flat gray 4x4: mean probes land on the hand-computed values,
     * the gradient probes stay silent */
    uint8_t flat[4 * 4 * 3], scratch[8 * 4 * 4 + 8 * 2 * 2];
    uint16_t feat[PL_FEATURE_DIMS];

    CHECK(ctx);
    if (!ctx)
        return;
    memset(flat, 100, sizeof flat);
    pl_extract_features(flat, 4, 4, feat, scratch);
    CHECK(feat[0] == 98 && feat[1] == 98 && feat[2] == 98);
    CHECK(feat[3] == 102); /* (9R+17G+3B) = 2900 per tap, x9 -> 102 */
    CHECK(feat[4] == 0 && feat[5] == 0 && feat[6] == 0 && feat[7] == 0);

    /* a horizontal step edge fires gradY, not gradX */
    {
        uint8_t step[4 * 4 * 3];

        for (size_t y = 0; y < 4; y++)
            for (size_t x = 0; x < 4; x++)
                for (int c = 0; c < 3; c++)
                    step[(y * 4 + x) * 3 + c] = y < 2 ? 0 : 255;
        pl_extract_features(step, 4, 4, feat, scratch);
        CHECK(feat[5] > 0);
        CHECK(feat[0] > 0 && feat[4] == 0);
    }
}

void test_pl_seed_variation(void)
{
    pl_ctx *a = pl_prepare(1), *b = pl_prepare(9);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        pl_free(a);
        pl_free(b);
        return;
    }
    /* a different seed yields a different corpus but the same planted
     * class pattern and seed-independent classifier tables */
    photo_sha1(a, 0, ha);
    photo_sha1(b, 0, hb);
    CHECK(strcmp(ha, hb) != 0);
    for (unsigned p = 0; p < PL_PHOTOS; p++)
        CHECK_U64(pl_photo_class(a, p), pl_photo_class(b, p));
    for (unsigned c = 0; c < PL_CLASSES; c++)
        CHECK(memcmp(pl_class_centroid(a, c), pl_class_centroid(b, c),
                     PL_FEATURE_DIMS * sizeof(uint16_t)) == 0);
    pl_free(a);
    pl_free(b);
    /* regression (issue #11): seed 15's first draws pushed photo 7
     * across a centroid boundary — the corpus must still verify via
     * the deterministic redraw */
    a = pl_prepare(15);
    CHECK(a != NULL);
    pl_free(a);
}

void test_pl_run_sane(void)
{
    pl_ctx *ctx = shared_pl();
    unsigned th = 1;
    double tp = 0.0;
    uint64_t ph = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* single-core: the 16-photo set, sane throughput */
    CHECK(pl_run(ctx, 1, &th, &tp, &ph) == 0);
    CHECK_U64(ph, PL_PHOTOS);
    CHECK(tp > 0.0);
    /* multi-core: 4x the set, threads preserved */
    th = 3;
    CHECK(pl_run(ctx, 1, &th, &tp, &ph) == 0);
    CHECK_U64(ph, PL_PHOTOS_MULTI);
    CHECK_U64(th, 3);
    CHECK(tp > 0.0);
    /* a second multi run on the shared ctx still verifies (the store
     * is rebuilt per run, the corpus immutable) */
    th = 2;
    CHECK(pl_run(ctx, 1, &th, &tp, &ph) == 0);
    CHECK_U64(ph, PL_PHOTOS_MULTI);
    CHECK_U64(th, 2);
    /* regression: multiple RUNS of the same pass — the per-slice store
     * must reset between runs (a stale table once hung run 3) */
    th = 3;
    CHECK(pl_run(ctx, PL_TEST_RUNS, &th, &tp, &ph) == 0);
    CHECK_U64(ph, PL_PHOTOS_MULTI);
    CHECK_U64(th, 3);
    CHECK(tp > 0.0);
}

void test_pl_thread_clamp(void)
{
    pl_ctx *ctx = shared_pl();
    unsigned th;
    double tp = 0.0;
    uint64_t ph = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* more threads than photos: clamped to the set of the pass */
    th = 999;
    CHECK(pl_run(ctx, 1, &th, &tp, &ph) == 0);
    CHECK_U64(th, PL_PHOTOS_MULTI);
    CHECK_U64(ph, PL_PHOTOS_MULTI);
    /* zero is meaningless, clamped up to one */
    th = 0;
    CHECK(pl_run(ctx, 1, &th, &tp, &ph) == 0);
    CHECK_U64(th, 1);
    CHECK_U64(ph, PL_PHOTOS);
}

void test_pl_accessors_null_safe(void)
{
    CHECK(pl_photo(NULL, 0) == NULL);
    CHECK_U64(pl_photo_len(NULL, 0), 0);
    CHECK_U64(pl_photo_class(NULL, 0), 0);
    CHECK(pl_reference_digest(NULL, 0, 0) == NULL);
    CHECK(pl_class_swatch(NULL, 0) == NULL);
    CHECK(pl_class_centroid(NULL, 0) == NULL);
    CHECK_U64(pl_task_photos(0), PL_PHOTOS);
    CHECK_U64(pl_task_photos(1), PL_PHOTOS_MULTI);
    CHECK_U64(pl_classify(NULL, NULL), 0);
}

void test_pl_golden(void)
{
    pl_ctx *ctx = shared_pl();
    char hex[41];

    CHECK(ctx);
    if (!ctx)
        return;
    photo_sha1(ctx, 0, hex);
    CHECK_STR(hex, PL_GOLDEN_PHOTO0_SHA1);
    CHECK_U64(pl_photo_len(ctx, 0), PL_GOLDEN_PHOTO0_LEN);
    for (unsigned t = 0; t < PL_THUMBS; t++)
        CHECK(memcmp(pl_reference_digest(ctx, 0, t),
                     PL_GOLDEN_PHOTO0_THUMB[t], 20) == 0);
    /* the pinned feature vector, rebuilt through the public kernels */
    {
        static uint8_t t1[PL_THUMB1_W * PL_THUMB1_H * 3];
        static uint8_t scratch[8 * PL_CLS_W * PL_CLS_H +
                               8 * (PL_CLS_W / 2) * (PL_CLS_H / 2)];
        uint16_t feat[PL_FEATURE_DIMS];

        img_box_downscale(t1, PL_CLS_W, PL_CLS_H, pl_photo(ctx, 0),
                          PL_PHOTO_W, PL_PHOTO_H);
        pl_extract_features(t1, PL_CLS_W, PL_CLS_H, feat, scratch);
        CHECK(memcmp(feat, PL_GOLDEN_PHOTO0_FEAT, sizeof feat) == 0);
        CHECK(memcmp(pl_class_centroid(ctx, 0), PL_GOLDEN_CENTROID0,
                     sizeof feat) == 0);
    }
    CHECK_U64(pl_task_photos(0), PL_GOLDEN_TASK_PHOTOS);
    CHECK_U64(pl_task_photos(1), PL_GOLDEN_TASK_PHOTOS_MULTI);
}
