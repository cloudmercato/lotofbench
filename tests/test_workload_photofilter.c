/* Unit tests — workload #13: Photo Filter. */

#include "tests.h"

#include "imgops.h"
#include "sha1.h"
#include "workload_photofilter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.7.0, seed 1). After a
 * DELIBERATE change to the corpus or the pipeline, re-run the tool and
 * port the printed values here in the same commit. */
#define PF_GOLDEN_PHOTO0_SHA1 "a451fb3deff7eed5b073b8888dc0b292699c40cf"
#define PF_GOLDEN_PHOTO0_LEN  5760000u
static const uint8_t PF_GOLDEN_PHOTO0_REF[20] = {
    0xac, 0x90, 0xfc, 0xe1, 0x7c, 0x31, 0xe5, 0x14, 0x02, 0x41,
    0xea, 0x3d, 0x6e, 0x91, 0xb2, 0x87, 0x7d, 0x84, 0xec, 0x79
};
#define PF_GOLDEN_TASK_PIXELS        19200000ull
#define PF_GOLDEN_TASK_PIXELS_MULTI  76800000ull

/* One shared prepare for the read-only tests (the corpus is immutable;
 * the process exits right after the suite anyway). */
static pf_ctx *shared_pf(void)
{
    static pf_ctx *ctx;

    if (!ctx)
        ctx = pf_prepare(1);
    return ctx;
}

static void photo_sha1(const pf_ctx *ctx, unsigned p, char hex[41])
{
    sha1_hex(pf_photo(ctx, p), pf_photo_len(ctx, p), hex);
}

void test_pf_photos_deterministic(void)
{
    pf_ctx *a = pf_prepare(1), *b = pf_prepare(1);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        pf_free(a);
        pf_free(b);
        return;
    }
    for (unsigned p = 0; p < PF_PHOTOS; p++) {
        CHECK_U64(pf_photo_len(a, p), pf_photo_len(b, p));
        photo_sha1(a, p, ha);
        photo_sha1(b, p, hb);
        CHECK_STR(ha, hb);
    }
    pf_free(a);
    pf_free(b);
}

void test_pf_input_invariants(void)
{
    pf_ctx *ctx = shared_pf();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned p = 0; p < PF_PHOTOS; p++) {
        CHECK_U64(pf_photo_len(ctx, p),
                  (size_t)PF_PHOTO_W * PF_PHOTO_H * 3);
        CHECK_U64(pf_archetype(ctx, p), p % 6); /* six archetypes cycle */
        CHECK(pf_photo_saturation(ctx, p) >= 180 &&
              pf_photo_saturation(ctx, p) <= 330);
        CHECK(pf_photo_black(ctx, p) <= 40);
        CHECK(pf_photo_white(ctx, p) >= 215 &&
              pf_photo_white(ctx, p) <= 255);
        CHECK(pf_photo_white(ctx, p) > pf_photo_black(ctx, p));
        CHECK(pf_reference_digest(ctx, p) != NULL);
    }
}

void test_pf_reference_reproducible(void)
{
    pf_ctx *ctx = shared_pf();
    uint8_t *work, *blur_tmp, *crop_buf, dig[20];
    size_t cw = (size_t)PF_PHOTO_W * PF_CROP_NUM / PF_CROP_DEN;
    size_t chh = (size_t)PF_PHOTO_H * PF_CROP_NUM / PF_CROP_DEN;
    int ok;

    CHECK(ctx);
    if (!ctx)
        return;
    /* an independent re-run of the pipeline on photo 0 must reproduce
     * the prepare-time reference digest byte for byte */
    work = malloc((size_t)PF_PHOTO_W * PF_PHOTO_H * 3);
    blur_tmp = malloc((size_t)PF_PHOTO_W * PF_PHOTO_H * 3);
    crop_buf = malloc(cw * chh * 3);
    CHECK(work && blur_tmp && crop_buf);
    if (!work || !blur_tmp || !crop_buf) {
        free(work);
        free(blur_tmp);
        free(crop_buf);
        return;
    }
    memcpy(work, pf_photo(ctx, 0), (size_t)PF_PHOTO_W * PF_PHOTO_H * 3);
    img_saturate(work, PF_PHOTO_W, PF_PHOTO_H, pf_photo_saturation(ctx, 0));
    img_levels(work, PF_PHOTO_W, PF_PHOTO_H, pf_photo_black(ctx, 0),
               pf_photo_white(ctx, 0));
    img_box_blur(work, PF_PHOTO_W, PF_PHOTO_H, PF_BLUR_RADIUS, blur_tmp);
    img_crop(crop_buf, cw, work, PF_PHOTO_W, PF_PHOTO_H,
             ((size_t)PF_PHOTO_W - cw) / 2, ((size_t)PF_PHOTO_H - chh) / 2,
             cw, chh);
    img_scale_bilinear(work, PF_PHOTO_W, PF_PHOTO_H, crop_buf, cw, chh);
    sha1(work, (size_t)PF_PHOTO_W * PF_PHOTO_H * 3, dig);
    ok = memcmp(dig, pf_reference_digest(ctx, 0), 20) == 0;
    CHECK(ok);
    free(work);
    free(blur_tmp);
    free(crop_buf);
}

void test_pf_corpus_golden(void)
{
    pf_ctx *ctx = shared_pf();
    char hex[41];

    CHECK(ctx);
    if (!ctx)
        return;
    photo_sha1(ctx, 0, hex);
    CHECK_STR(hex, PF_GOLDEN_PHOTO0_SHA1);
    CHECK_U64(pf_photo_len(ctx, 0), PF_GOLDEN_PHOTO0_LEN);
    CHECK(memcmp(pf_reference_digest(ctx, 0), PF_GOLDEN_PHOTO0_REF, 20) ==
          0);
    CHECK_U64(pf_task_pixels(0), PF_GOLDEN_TASK_PIXELS);
    CHECK_U64(pf_task_pixels(1), PF_GOLDEN_TASK_PIXELS_MULTI);
}

void test_pf_seed_variation(void)
{
    pf_ctx *a = pf_prepare(1), *b = pf_prepare(2);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        pf_free(a);
        pf_free(b);
        return;
    }
    photo_sha1(a, 0, ha);
    photo_sha1(b, 0, hb);
    CHECK(strcmp(ha, hb) != 0); /* different seed => different corpus */
    pf_free(a);
    pf_free(b);
}

void test_pf_run_sane(void)
{
    pf_ctx *ctx = shared_pf();
    unsigned threads = 1;
    double mpixels = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(pf_run(ctx, 1, &threads, &mpixels, &pixels) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(pixels, (uint64_t)PF_PHOTOS * PF_PHOTO_W * PF_PHOTO_H);
    CHECK(mpixels > 0.0);
}

void test_pf_thread_clamp(void)
{
    pf_ctx *ctx = shared_pf();
    unsigned threads = 999;
    double mpixels = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* more threads than photos: clamped to the 40-photo multi-core set */
    CHECK(pf_run(ctx, 1, &threads, &mpixels, &pixels) == 0);
    CHECK_U64(threads, PF_PHOTOS_MULTI);
    CHECK_U64(pixels,
              (uint64_t)PF_PHOTOS_MULTI * PF_PHOTO_W * PF_PHOTO_H);
}

void test_pf_accessors_null_safe(void)
{
    CHECK(pf_photo(NULL, 0) == NULL);
    CHECK_U64(pf_photo_len(NULL, 0), 0);
    CHECK(pf_reference_digest(NULL, 0) == NULL);
    CHECK_U64(pf_photo_saturation(NULL, 0), 0);
    CHECK_U64(pf_photo_black(NULL, 0), 0);
    CHECK_U64(pf_photo_white(NULL, 0), 0);
    CHECK_U64(pf_archetype(NULL, 0), 0);

    pf_ctx *ctx = shared_pf();

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(pf_photo(ctx, PF_PHOTOS) == NULL);
    CHECK_U64(pf_photo_len(ctx, PF_PHOTOS), 0);
    CHECK(pf_reference_digest(ctx, PF_PHOTOS) == NULL);
}
