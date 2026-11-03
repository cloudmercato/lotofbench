/* Unit tests — the shared deterministic image kernels (src/imgops.c).
 *
 * Every vector here is hand-computed (the same vectors gate the kernels
 * in /tmp before the workload lands); they pin the documented rounding
 * rules: half-up at 8/16 fractional bits, edge-clamped windows. */

#include "tests.h"

#include "imgops.h"

#include <stdlib.h>
#include <string.h>

static const int32_t M_IDENT[9] = { 256, 0, 0, 0, 256, 0, 0, 0, 256 };

void test_img_levels(void)
{
    uint8_t a[64], b[64];

    for (int i = 0; i < 64; i++)
        a[i] = (uint8_t)(i * 7 % 256);
    memcpy(b, a, 64);
    img_levels(a, 4, 4, 0, 255); /* full range is the identity */
    CHECK(memcmp(a, b, 64) == 0);

    {
        /* span 51..204: v' = round((v-51)*255/153), half-up */
        uint8_t src[12] = { 51, 100, 128, 200, 204, 0, 255, 10,
                            51, 204, 77, 130 };
        uint8_t want[12] = { 0, 82, 128, 248, 255, 0, 255, 0,
                             0, 255, 43, 132 };

        memcpy(a, src, 12);
        img_levels(a, 2, 2, 51, 204);
        CHECK(memcmp(a, want, 12) == 0);
    }
    {
        /* all-same bytes at the black point collapse to 0 */
        memset(a, 200, 12);
        img_levels(a, 2, 2, 200, 255);
        for (int i = 0; i < 12; i++)
            CHECK_U64(a[i], 0);
    }
}

void test_img_color_matrix(void)
{
    uint8_t a[12], b[12];

    for (int i = 0; i < 12; i += 3) {
        a[i] = 10;
        a[i + 1] = 20;
        a[i + 2] = 30;
    }
    memcpy(b, a, 12);
    img_color_matrix(a, 2, 2, M_IDENT);
    CHECK(memcmp(a, b, 12) == 0);

    {
        /* swap R and B */
        const int32_t m[9] = { 0, 0, 256, 0, 256, 0, 256, 0, 0 };
        uint8_t want[12];

        for (int i = 0; i < 12; i += 3) {
            want[i] = 30;
            want[i + 1] = 20;
            want[i + 2] = 10;
        }
        memcpy(a, b, 12);
        img_color_matrix(a, 2, 2, m);
        CHECK(memcmp(a, want, 12) == 0);
    }
    {
        /* half gain on R: 10 * 0.5 = 5 (floor at the 8.8 boundary) */
        const int32_t m[9] = { 128, 0, 0, 0, 256, 0, 0, 0, 256 };
        uint8_t want[12];

        for (int i = 0; i < 12; i += 3) {
            want[i] = 5;
            want[i + 1] = 20;
            want[i + 2] = 30;
        }
        memcpy(a, b, 12);
        img_color_matrix(a, 2, 2, m);
        CHECK(memcmp(a, want, 12) == 0);
    }
}

void test_img_saturate(void)
{
    uint8_t a[12], b[12];

    for (int i = 0; i < 12; i += 3) {
        a[i] = 10;
        a[i + 1] = 20;
        a[i + 2] = 30;
    }
    memcpy(b, a, 12);

    img_saturate(a, 2, 2, 256); /* s=256 is the identity */
    CHECK(memcmp(a, b, 12) == 0);

    {
        /* s=0 -> grayscale: luma((10,20,30)) = (770+3000+870)>>8 = 18 */
        uint8_t want[12];

        for (int i = 0; i < 12; i += 3)
            want[i] = want[i + 1] = want[i + 2] = 18;
        memcpy(a, b, 12);
        img_saturate(a, 2, 2, 0);
        CHECK(memcmp(a, want, 12) == 0);
    }
    {
        /* s=512 -> out = 2*in - luma: (10,20,30) -> (2,22,42) */
        uint8_t want[12];

        for (int i = 0; i < 12; i += 3) {
            want[i] = 2;
            want[i + 1] = 22;
            want[i + 2] = 42;
        }
        memcpy(a, b, 12);
        img_saturate(a, 2, 2, 512);
        CHECK(memcmp(a, want, 12) == 0);
    }
}

void test_img_box_blur(void)
{
    static uint8_t a[64 * 64 * 3], scratch[64 * 64 * 3];

    /* 1x1 all-same: any radius leaves it unchanged (edge-clamped
     * window, the small-image rule) */
    memset(a, 77, 3);
    img_box_blur(a, 1, 1, 1, scratch);
    for (int i = 0; i < 3; i++)
        CHECK_U64(a[i], 77);

    {
        /* 3x1 gray [0,100,200], r=1: windows [0,0,100] [0,100,200]
         * [100,200,200] -> [33,100,167] */
        uint8_t want[9];

        for (int x = 0; x < 3; x++)
            for (int c = 0; c < 3; c++)
                a[x * 3 + c] = (uint8_t)(x * 100);
        for (int c = 0; c < 3; c++) {
            want[c] = 33;
            want[3 + c] = 100;
            want[6 + c] = 167;
        }
        img_box_blur(a, 3, 1, 1, scratch);
        CHECK(memcmp(a, want, 9) == 0);
    }
    {
        /* 3x3, center=255 gray, r=1: H -> middle row 85, V ->
         * (0+85+0)/3 = 28 everywhere (255 spread over 9 pixels) */
        uint8_t want[27];

        memset(a, 0, 27);
        for (int c = 0; c < 3; c++)
            a[(1 * 3 + 1) * 3 + c] = 255;
        memset(want, 28, 27);
        img_box_blur(a, 3, 3, 1, scratch);
        CHECK(memcmp(a, want, 27) == 0);
    }
}

void test_img_crop(void)
{
    uint8_t src[36], want[12], dst[12];

    /* 4x3 pattern p(x,y) = 10x + y; crop the 2x2 at (1,1) */
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 4; x++)
            for (int c = 0; c < 3; c++)
                src[(y * 4 + x) * 3 + c] = (uint8_t)(10 * x + y);
    {
        size_t k = 0;

        for (int y = 1; y <= 2; y++)
            for (int x = 1; x <= 2; x++)
                for (int c = 0; c < 3; c++)
                    want[k++] = (uint8_t)(10 * x + y);
    }
    img_crop(dst, 2, src, 4, 3, 1, 1, 2, 2);
    CHECK(memcmp(dst, want, 12) == 0);
}

void test_img_scale(void)
{
    uint8_t a[64 * 64 * 3], b[64 * 64 * 3];

    /* identity scale must be byte-identical */
    for (int i = 0; i < 60; i++)
        a[i] = (uint8_t)(i * 37 % 251);
    memcpy(b, a, 60);
    img_scale_bilinear(b, 5, 4, a, 5, 4);
    CHECK(memcmp(a, b, 60) == 0);

    {
        /* 2x upscale of a 1-row ramp [0,255]: samples at -0.25, 0.25,
         * 0.75, 1.25 -> [191, 64, 191, 255] (0.75*255=191.25,
         * 0.25*255=63.75 rounds half-up to 64) */
        uint8_t src[6] = { 0, 0, 0, 255, 255, 255 }, want[12];

        for (int x = 0; x < 4; x++)
            for (int c = 0; c < 3; c++)
                want[x * 3 + c] =
                    (uint8_t)("\xbf\x40\xbf\xff"[x]);
        img_scale_bilinear(a, 4, 1, src, 2, 1);
        CHECK(memcmp(a, want, 12) == 0);
    }
    {
        /* 4->2 downscale of [0,100,200,250]: samples at 0.5 and 2.5
         * -> [50, 225] */
        uint8_t src[12], want[6];

        for (int x = 0; x < 4; x++)
            for (int c = 0; c < 3; c++)
                src[x * 3 + c] = (uint8_t)(x == 3 ? 250 : x * 100);
        for (int x = 0; x < 2; x++)
            for (int c = 0; c < 3; c++)
                want[x * 3 + c] = (uint8_t)(x == 0 ? 50 : 225);
        img_scale_bilinear(a, 2, 1, src, 4, 1);
        CHECK(memcmp(a, want, 6) == 0);
    }
}
