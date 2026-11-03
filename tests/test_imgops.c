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

void test_img_grayscale(void)
{
    /* Rec.601 luma, weights 77/150/29 (sum 256), half-up:
     * (255,255,255) -> 255, (0,0,0) -> 0, primaries 77/149/29,
     * (100,150,200) -> (7700+22500+5800+128)>>8 = 141 */
    {
        const uint8_t src[18] = { 255, 255, 255, 0, 0, 0, 255, 0, 0,
                                  0, 255, 0, 0, 0, 255, 100, 150, 200 };
        const uint8_t want[6] = { 255, 0, 77, 149, 29, 141 };
        uint8_t dst[6];

        img_grayscale(src, 6, 1, dst);
        CHECK(memcmp(dst, want, 6) == 0);
    }
    {
        /* every pixel maps independently (3x2, row-major) */
        uint8_t src[18], dst[6];

        for (int i = 0; i < 18; i++)
            src[i] = (uint8_t)(i * 13 % 256);
        img_grayscale(src, 3, 2, dst);
        for (int p = 0; p < 6; p++) {
            int32_t v = src[p * 3] * 77 + src[p * 3 + 1] * 150 +
                        src[p * 3 + 2] * 29 + 128;

            CHECK_U64(dst[p], (uint32_t)(v >> 8));
        }
    }
}

void test_img_gray_gaussian_blur5(void)
{
    static uint8_t a[64 * 64], scratch[64 * 64];

    /* 1x1 all-same: unchanged (edge-clamped window, small-image rule) */
    a[0] = 77;
    img_gray_gaussian_blur5(a, 1, 1, scratch);
    CHECK_U64(a[0], 77);

    /* 9x1 impulse 160 at x=4: [1 4 6 4 1]/16, half-up at 4 bits ->
     * [10, 40, 60, 40, 10] */
    {
        const uint8_t want[9] = { 0, 0, 10, 40, 60, 40, 10, 0, 0 };

        memset(a, 0, 9);
        a[4] = 160;
        img_gray_gaussian_blur5(a, 9, 1, scratch);
        CHECK(memcmp(a, want, 9) == 0);
    }
    /* 5x5 impulse 160 at (2,2): the separable kernel squared, with
     * edge-clamped windows folding the response back into the frame —
     * exact expected grid (verified by hand, two half-up passes) */
    {
        static const uint8_t want[25] = {
            1, 3, 4, 3, 1,
            3, 10, 15, 10, 3,
            4, 15, 23, 15, 4,
            3, 10, 15, 10, 3,
            1, 3, 4, 3, 1
        };

        memset(a, 0, 25);
        a[2 * 5 + 2] = 160;
        img_gray_gaussian_blur5(a, 5, 5, scratch);
        CHECK(memcmp(a, want, 25) == 0);
    }
    /* constant image stays constant through both passes */
    memset(a, 200, 64 * 64);
    img_gray_gaussian_blur5(a, 64, 64, scratch);
    for (int i = 0; i < 64 * 64; i++)
        CHECK_U64(a[i], 200);
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

void test_img_box_downscale(void)
{
    /* exact vectors, hand-computed (the /tmp gate runs the same) */
    {
        /* 2x2 -> 1x1: mean 127.5 rounds half-up to 128 */
        uint8_t src[12] = { 0, 0, 0, 255, 255, 255,
                            255, 255, 255, 0, 0, 0 };
        uint8_t dst[3];

        img_box_downscale(dst, 1, 1, src, 2, 2);
        CHECK(dst[0] == 128 && dst[1] == 128 && dst[2] == 128);
    }
    {
        /* 1x1 -> 1x1 identity */
        uint8_t src[3] = { 7, 8, 9 }, dst[3];

        img_box_downscale(dst, 1, 1, src, 1, 1);
        CHECK(dst[0] == 7 && dst[1] == 8 && dst[2] == 9);
    }
    {
        /* 4x4 -> 2x2: rows 0-1 are {0,1,2,3}, rows 2-3 all 9. Block
         * (0,0) = {0,1,0,1} mean 0.5 -> 1; block (1,0) = {2,3,2,3}
         * mean 2.5 -> 3; bottom blocks stay 9. */
        uint8_t src[48], dst[12];

        for (int i = 0; i < 16; i++)
            for (int c = 0; c < 3; c++)
                src[i * 3 + c] = (uint8_t)(i < 8 ? i % 4 : 9);
        img_box_downscale(dst, 2, 2, src, 4, 4);
        CHECK(dst[0] == 1);
        CHECK(dst[3] == 3);
        CHECK(dst[6] == 9 && dst[9] == 9);
    }
    {
        /* 3x3 -> 1x1, non-power-of-two ratio: mean 0..8 = 4 exactly */
        uint8_t src[27], dst[3];

        for (int i = 0; i < 9; i++)
            for (int c = 0; c < 3; c++)
                src[i * 3 + c] = (uint8_t)i;
        img_box_downscale(dst, 1, 1, src, 3, 3);
        CHECK(dst[0] == 4 && dst[1] == 4 && dst[2] == 4);
    }
    {
        /* constant image at every ratio of the photo ladder */
        uint8_t src[1280 * 960 * 3], t0[320 * 240 * 3];
        uint8_t t1[160 * 120 * 3], t2[80 * 60 * 3];
        int ok = 1;

        memset(src, 0xC7, sizeof src);
        img_box_downscale(t0, 320, 240, src, 1280, 960);
        img_box_downscale(t1, 160, 120, src, 1280, 960);
        img_box_downscale(t2, 80, 60, src, 1280, 960);
        for (size_t i = 0; i < sizeof t0; i++)
            if (t0[i] != 0xC7) ok = 0;
        for (size_t i = 0; i < sizeof t1; i++)
            if (t1[i] != 0xC7) ok = 0;
        for (size_t i = 0; i < sizeof t2; i++)
            if (t2[i] != 0xC7) ok = 0;
        CHECK(ok);
    }
    {
        /* differential vs an independent per-window recomputation on
         * pseudo-random buffers, assorted ratio pairs */
        static const size_t cases[5][4] = {
            { 64, 48, 16, 12 }, { 32, 32, 8, 8 }, { 40, 20, 20, 10 },
            { 16, 16, 2, 2 }, { 100, 60, 25, 15 },
        };
        uint8_t src[100 * 60 * 3], a[25 * 15 * 3], b[25 * 15 * 3];
        uint64_t state = 777;

        for (unsigned ci = 0; ci < 5; ci++) {
            size_t sw = cases[ci][0], sh = cases[ci][1];
            size_t dw = cases[ci][2], dh = cases[ci][3];

            for (size_t i = 0; i < sw * sh * 3; i++) {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                src[i] = (uint8_t)(state >> 32);
            }
            img_box_downscale(a, dw, dh, src, sw, sh);
            for (size_t y = 0; y < dh; y++)
                for (size_t x = 0; x < dw; x++)
                    for (int c = 0; c < 3; c++) {
                        uint32_t sum = 0, area =
                            (uint32_t)((sw / dw) * (sh / dh));

                        for (size_t yy = y * (sh / dh);
                             yy < (y + 1) * (sh / dh); yy++)
                            for (size_t xx = x * (sw / dw);
                                 xx < (x + 1) * (sw / dw); xx++)
                                sum += src[(yy * sw + xx) * 3 + c];
                        b[(y * dw + x) * 3 + c] =
                            (uint8_t)((sum + area / 2) / area);
                    }
            CHECK(memcmp(a, b, dw * dh * 3) == 0);
        }
    }
}
