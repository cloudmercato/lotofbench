/* Unit tests — workload #12: Horizon Detection. */

#include "tests.h"

#include "imgops.h"
#include "sha1.h"
#include "workload_horizon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.8.0, seed 1). After a
 * DELIBERATE change to the corpus or the pipeline, re-run the tool and
 * port the printed values here in the same commit. */
#define HD_GOLDEN_PHOTO0_SHA1 "f1b427a5b70b66ba63b86201c665b727f69903e9"
#define HD_GOLDEN_PHOTO0_LEN  5760000u
static const uint8_t HD_GOLDEN_PHOTO0_REF[20] = {
    0xf6, 0xe4, 0xa8, 0x01, 0x61, 0xa3, 0x80, 0xbd, 0x11, 0xb1,
    0xd9, 0x18, 0xcc, 0xa3, 0x12, 0xe4, 0xd7, 0x2c, 0xaf, 0x5d
};
#define HD_GOLDEN_PHOTO0_THETA 421u
#define HD_GOLDEN_PHOTO0_RHO   509
#define HD_GOLDEN_PHOTO0_VOTES 824u
#define HD_GOLDEN_TASK_PIXELS        15360000ull
#define HD_GOLDEN_TASK_PIXELS_MULTI  61440000ull

/* One shared prepare for the read-only tests (the corpus is immutable;
 * the process exits right after the suite anyway). */
static hd_ctx *shared_hd(void)
{
    static hd_ctx *ctx;

    if (!ctx)
        ctx = hd_prepare(1);
    return ctx;
}

/* The planted boundary row of column x, drawn by the generator with
 * division rounding half away from zero — re-derived independently. */
static int32_t boundary_row(int32_t cx, int32_t cy, int32_t slope,
                            int32_t x)
{
    int64_t num = (int64_t)(x - cx) * slope;

    return cy + (int32_t)(num >= 0 ? (num + 64) / 128
                                   : -((-num + 64) / 128));
}

/* Independent ground-truth check of a reference detection: the
 * detected normal must align with the planted normal (direction
 * (128, slope)) within 1 degree, rho must sit within HD_GT_RHO_PX of
 * the planted line's rho at the detected theta, and the votes must
 * clear HD_MIN_VOTES. */
static int ground_truth(int32_t cx, int32_t cy, int32_t slope,
                        const hd_result *r)
{
    int32_t c, s;

    if (r->votes < HD_MIN_VOTES)
        return 0;
    hd_trig(r->theta_bin, &c, &s);
    {
        int32_t cross = 128 * c + slope * s;
        int32_t dot = 128 * s - slope * c;
        int32_t rho_planted = (cx * c + cy * s >= 0)
            ? (cx * c + cy * s + 512) / 1024
            : -((-cx * c - cy * s + 512) / 1024);
        int32_t dr = r->rho_bin - rho_planted;

        if (cross < -HD_GT_ANGLE_CROSS || cross > HD_GT_ANGLE_CROSS)
            return 0;
        if (dot <= 0)
            return 0;
        if (dr < -HD_GT_RHO_PX || dr > HD_GT_RHO_PX)
            return 0;
    }
    return 1;
}

void test_hd_photos_deterministic(void)
{
    hd_ctx *a = hd_prepare(1), *b = hd_prepare(1);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        hd_free(a);
        hd_free(b);
        return;
    }
    for (unsigned p = 0; p < HD_PHOTOS; p++) {
        CHECK_U64(hd_photo_len(a, p), hd_photo_len(b, p));
        sha1_hex(hd_photo(a, p), hd_photo_len(a, p), ha);
        sha1_hex(hd_photo(b, p), hd_photo_len(b, p), hb);
        CHECK_STR(ha, hb);
        CHECK(memcmp(hd_reference(a, p), hd_reference(b, p),
                     sizeof(hd_result)) == 0);
    }
    hd_free(a);
    hd_free(b);
}

void test_hd_input_invariants(void)
{
    hd_ctx *ctx = shared_hd();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned p = 0; p < HD_PHOTOS; p++) {
        int32_t cx = hd_plant_cx(ctx, p);
        int32_t cy = hd_plant_cy(ctx, p);
        int32_t slope = hd_plant_slope(ctx, p);

        CHECK_U64(hd_photo_len(ctx, p),
                  (size_t)HD_PHOTO_W * HD_PHOTO_H * 3);
        CHECK_U64(hd_archetype(ctx, p), p % 6); /* six archetypes cycle */
        CHECK(cx >= 751 && cx <= 849);
        CHECK(cy >= 451 && cy <= 749);
        CHECK(slope >= -64 && slope <= 64);
        /* the boundary stays inside the frame over the full width */
        CHECK(boundary_row(cx, cy, slope, 0) >= 26);
        CHECK(boundary_row(cx, cy, slope, 0) <= 1174);
        CHECK(boundary_row(cx, cy, slope, (int32_t)HD_PHOTO_W - 1) >= 26);
        CHECK(boundary_row(cx, cy, slope, (int32_t)HD_PHOTO_W - 1) <=
              1174);
        CHECK(hd_reference(ctx, p) != NULL);
        CHECK(hd_reference(ctx, p)->votes >= HD_MIN_VOTES);
    }
}

void test_hd_ground_truth(void)
{
    /* the differential test: the pipeline's detection must match the
     * planted horizon on every photo of several seeds */
    for (unsigned seed = 1; seed <= 3; seed++) {
        hd_ctx *ctx = hd_prepare(seed);

        CHECK(ctx);
        if (!ctx)
            continue;
        for (unsigned p = 0; p < HD_PHOTOS; p++) {
            const hd_result *r = hd_reference(ctx, p);

            CHECK(ground_truth(hd_plant_cx(ctx, p), hd_plant_cy(ctx, p),
                               hd_plant_slope(ctx, p), r));
        }
        hd_free(ctx);
    }
}

void test_hd_detect_units(void)
{
    hd_scratch sc;
    hd_result r;

    /* uniform image: no edges, no votes */
    {
        size_t w = 96, h = 64;
        uint8_t *img = malloc(w * h * 3);

        CHECK(img);
        if (img) {
            memset(img, 120, w * h * 3);
            CHECK(hd_scratch_init(&sc, w, h) == 0);
            CHECK(hd_detect(img, w, h, &sc, &r) == 0);
            CHECK_U64(r.votes, 0);
            hd_scratch_free(&sc);
            free(img);
        }
    }
    /* a perfect horizontal line: normal at 90 degrees (bin 360),
     * rho at the boundary +-1 (the 2-px NMS ridge straddles it) */
    {
        size_t w = 200, h = 150;
        uint8_t *img = malloc(w * h * 3);

        CHECK(img);
        if (img) {
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < w; x++) {
                    uint8_t v = y < 75 ? 210 : 40;

                    img[(y * w + x) * 3 + 0] = v;
                    img[(y * w + x) * 3 + 1] = v;
                    img[(y * w + x) * 3 + 2] = v;
                }
            CHECK(hd_scratch_init(&sc, w, h) == 0);
            CHECK(hd_detect(img, w, h, &sc, &r) == 0);
            /* the 2-px ridge (rows 74/75) ties between the 89.75 and
             * 90-degree bins; both are the true normal */
            CHECK(r.theta_bin == HD_THETA_STEPS / 2 ||
                  r.theta_bin == HD_THETA_STEPS / 2 - 1);
            CHECK(r.rho_bin >= 74 && r.rho_bin <= 76);
            CHECK(r.votes >= h / 2);
            hd_scratch_free(&sc);
            free(img);
        }
    }
    /* a perfect vertical line: normal at 0 degrees */
    {
        size_t w = 200, h = 150;
        uint8_t *img = malloc(w * h * 3);

        CHECK(img);
        if (img) {
            for (size_t y = 0; y < h; y++)
                for (size_t x = 0; x < w; x++) {
                    uint8_t v = x < 100 ? 210 : 40;

                    img[(y * w + x) * 3 + 0] = v;
                    img[(y * w + x) * 3 + 1] = v;
                    img[(y * w + x) * 3 + 2] = v;
                }
            CHECK(hd_scratch_init(&sc, w, h) == 0);
            CHECK(hd_detect(img, w, h, &sc, &r) == 0);
            CHECK_U64(r.theta_bin, 0);
            CHECK(r.rho_bin >= 99 && r.rho_bin <= 101);
            hd_scratch_free(&sc);
            free(img);
        }
    }
    /* a scratch built for another size is refused, nulls are refused */
    {
        uint8_t img[8 * 8 * 3];

        memset(img, 120, sizeof img);
        CHECK(hd_scratch_init(&sc, 8, 8) == 0);
        CHECK(hd_detect(img, 8, 8, &sc, &r) == 0);
        CHECK(hd_detect(img, 8, 8, NULL, &r) == -1);
        CHECK(hd_detect(NULL, 8, 8, &sc, &r) == -1);
        CHECK(hd_detect(img, 16, 8, &sc, &r) == -1); /* size mismatch */
        hd_scratch_free(&sc);
        CHECK(hd_scratch_init(&sc, 2, 2) == -1); /* too small */
        hd_scratch_free(&sc);
    }
}

void test_hd_corpus_golden(void)
{
    hd_ctx *ctx = shared_hd();
    char hex[41];
    const hd_result *r;

    CHECK(ctx);
    if (!ctx)
        return;
    sha1_hex(hd_photo(ctx, 0), hd_photo_len(ctx, 0), hex);
    CHECK_STR(hex, HD_GOLDEN_PHOTO0_SHA1);
    CHECK_U64(hd_photo_len(ctx, 0), HD_GOLDEN_PHOTO0_LEN);
    r = hd_reference(ctx, 0);
    CHECK(r != NULL);
    if (!r)
        return;
    CHECK(memcmp(r->edge_digest, HD_GOLDEN_PHOTO0_REF, 20) == 0);
    CHECK_U64(r->theta_bin, HD_GOLDEN_PHOTO0_THETA);
    CHECK_U64(r->rho_bin, (uint64_t)(int64_t)HD_GOLDEN_PHOTO0_RHO);
    CHECK_U64(r->votes, HD_GOLDEN_PHOTO0_VOTES);
    CHECK_U64(hd_task_pixels(0), HD_GOLDEN_TASK_PIXELS);
    CHECK_U64(hd_task_pixels(1), HD_GOLDEN_TASK_PIXELS_MULTI);
}

void test_hd_seed_variation(void)
{
    hd_ctx *a = hd_prepare(1), *b = hd_prepare(2);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        hd_free(a);
        hd_free(b);
        return;
    }
    sha1_hex(hd_photo(a, 0), hd_photo_len(a, 0), ha);
    sha1_hex(hd_photo(b, 0), hd_photo_len(b, 0), hb);
    CHECK(strcmp(ha, hb) != 0); /* different seed => different corpus */
    hd_free(a);
    hd_free(b);
}

void test_hd_run_sane(void)
{
    hd_ctx *ctx = shared_hd();
    unsigned threads = 1;
    double mpixels = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(hd_run(ctx, 1, &threads, &mpixels, &pixels) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(pixels, (uint64_t)HD_PHOTOS * HD_PHOTO_W * HD_PHOTO_H);
    CHECK(mpixels > 0.0);
}

void test_hd_thread_clamp(void)
{
    hd_ctx *ctx = shared_hd();
    unsigned threads = 999;
    double mpixels = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* more threads than photos: clamped to the 32-photo multi-core set */
    CHECK(hd_run(ctx, 1, &threads, &mpixels, &pixels) == 0);
    CHECK_U64(threads, HD_PHOTOS_MULTI);
    CHECK_U64(pixels,
              (uint64_t)HD_PHOTOS_MULTI * HD_PHOTO_W * HD_PHOTO_H);
}

void test_hd_accessors_null_safe(void)
{
    CHECK(hd_photo(NULL, 0) == NULL);
    CHECK_U64(hd_photo_len(NULL, 0), 0);
    CHECK(hd_reference(NULL, 0) == NULL);
    CHECK_U64(hd_archetype(NULL, 0), 0);
    CHECK_U64(hd_plant_cx(NULL, 0), 0);
    CHECK_U64(hd_plant_cy(NULL, 0), 0);
    CHECK_U64(hd_plant_slope(NULL, 0), 0);
    hd_trig(HD_THETA_STEPS, &(int32_t){ 0 }, &(int32_t){ 0 }); /* no crash */

    hd_ctx *ctx = shared_hd();

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(hd_photo(ctx, HD_PHOTOS) == NULL);
    CHECK_U64(hd_photo_len(ctx, HD_PHOTOS), 0);
    CHECK(hd_reference(ctx, HD_PHOTOS) == NULL);
}
