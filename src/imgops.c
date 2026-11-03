/* Deterministic image kernels — see imgops.h for the contract. */

#include "imgops.h"

/* Scale-and-clamp helper shared by the per-pixel remaps. */
static inline uint8_t clamp_u8(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

void img_levels(uint8_t *img, size_t w, size_t h,
                unsigned black, unsigned white)
{
    /* (v - black) * 255 / (white - black), half-up; the numerator fits
     * 32 bits and the denominator is <= 255, so plain integer division
     * is exact and machine-independent. white > black per contract. */
    uint32_t span = (uint32_t)white - black;
    size_t n = w * h * 3;

    for (size_t i = 0; i < n; i++) {
        uint32_t v = img[i];
        uint32_t out;

        if (v <= black)
            out = 0;
        else if (v >= white)
            out = 255;
        else
            out = ((v - black) * 255u + span / 2u) / span;
        img[i] = (uint8_t)out;
    }
}

void img_color_matrix(uint8_t *img, size_t w, size_t h,
                      const int32_t m[9])
{
    size_t n = w * h; /* per-pixel, 3 channels */

    for (size_t p = 0; p < n; p++) {
        uint8_t *px = img + p * 3;
        int32_t r = px[0], g = px[1], b = px[2];
        int32_t c[3];
        int ch;

        c[0] = r * m[0] + g * m[1] + b * m[2];
        c[1] = r * m[3] + g * m[4] + b * m[5];
        c[2] = r * m[6] + g * m[7] + b * m[8];
        for (ch = 0; ch < 3; ch++) {
            /* half-up rounding at 8 fractional bits (weights are 8.8) */
            int32_t v = (c[ch] + 128) >> 8;

            px[ch] = clamp_u8(v);
        }
    }
}

void img_saturate(uint8_t *img, size_t w, size_t h, int32_t s)
{
    /* Rec.601 luma weights in 8.8 (sum = 256): R 0.301->77, G 0.586->150,
     * B 0.113->29. With S the 8.8 saturation (256 = identity):
     *   out_c = L + (S/256)(in_c - L)
     * expanded as a 3x3 8.8 matrix with t = 256 - S:
     *   m_cc = S + (t * W_c) >> 8,  m_cj = (t * W_j) >> 8  (j != c)
     * (t may be negative for S > 256; >> on int32 is floor, which is
     * deterministic). */
    static const int32_t LR = 77, LG = 150, LB = 29;
    int32_t t = 256 - s;
    int32_t m[9];

    m[0] = s + ((t * LR) >> 8);
    m[1] = (t * LG) >> 8;
    m[2] = (t * LB) >> 8;
    m[3] = (t * LR) >> 8;
    m[4] = s + ((t * LG) >> 8);
    m[5] = (t * LB) >> 8;
    m[6] = (t * LR) >> 8;
    m[7] = (t * LG) >> 8;
    m[8] = s + ((t * LB) >> 8);
    img_color_matrix(img, w, h, m);
}

void img_box_blur(uint8_t *img, size_t w, size_t h, unsigned r,
                  uint8_t *scratch)
{
    size_t row_bytes = w * 3;
    uint32_t win = 2 * r + 1;
    uint32_t half = win / 2;

    /* horizontal pass: img -> scratch */
    for (size_t y = 0; y < h; y++) {
        const uint8_t *row = img + y * row_bytes;
        uint8_t *out = scratch + y * row_bytes;

        for (int ch = 0; ch < 3; ch++) {
            uint32_t sum = 0;

            /* seed the window at x=0 with edge-clamped columns (the
             * image can be smaller than the window on either side) */
            for (unsigned k = 0; k <= r; k++)
                sum += row[(k < w ? k : w - 1) * 3 + ch];
            for (unsigned k = 0; k < r; k++)
                sum += row[0 * 3 + ch];
            out[0 * 3 + ch] = (uint8_t)((sum + half) / win);
            for (size_t x = 1; x < w; x++) {
                size_t add_x = (x + r < w) ? x + r : w - 1;
                size_t sub_x = (x >= r + 1) ? x - r - 1 : 0;

                sum += row[add_x * 3 + ch];
                sum -= row[sub_x * 3 + ch];
                out[x * 3 + ch] = (uint8_t)((sum + half) / win);
            }
        }
    }

    /* vertical pass: scratch -> img */
    for (size_t x = 0; x < w; x++) {
        for (int ch = 0; ch < 3; ch++) {
            uint32_t sum = 0;

            /* seed the window at y=0 with edge-clamped rows (same
             * small-image rule as the horizontal pass) */
            for (unsigned k = 0; k <= r; k++)
                sum += scratch[(k < h ? k : h - 1) * row_bytes + x * 3 + ch];
            for (unsigned k = 0; k < r; k++)
                sum += scratch[0 * row_bytes + x * 3 + ch];
            img[0 * row_bytes + x * 3 + ch] =
                (uint8_t)((sum + half) / win);
            for (size_t y = 1; y < h; y++) {
                size_t add_y = (y + r < h) ? y + r : h - 1;
                size_t sub_y = (y >= r + 1) ? y - r - 1 : 0;

                sum += scratch[add_y * row_bytes + x * 3 + ch];
                sum -= scratch[sub_y * row_bytes + x * 3 + ch];
                img[y * row_bytes + x * 3 + ch] =
                    (uint8_t)((sum + half) / win);
            }
        }
    }
}

void img_crop(uint8_t *dst, size_t dw,
              const uint8_t *src, size_t sw, size_t sh,
              size_t x, size_t y, size_t cw, size_t ch)
{
    (void)sh;
    for (size_t row = 0; row < ch; row++)
        for (size_t col = 0; col < cw; col++)
            for (int c = 0; c < 3; c++)
                dst[row * dw * 3 + col * 3 + c] =
                    src[(y + row) * sw * 3 + (x + col) * 3 + c];
}

void img_scale_bilinear(uint8_t *dst, size_t dw, size_t dh,
                        const uint8_t *src, size_t sw, size_t sh)
{
    /* Source coordinate of destination pixel x: (x + 0.5) * sw / dw -
     * 0.5. Stepped in 16.16 fixed point: dx = sw * 65536 / dw, start
     * sx0 = sw * 32768 / dw - 32768 (may be negative; floor shifts keep
     * the fraction in [0, 65536) and the edge clamp keeps x0 >= 0). */
    uint64_t dx = ((uint64_t)sw << 16) / dw;
    uint64_t dy = ((uint64_t)sh << 16) / dh;
    int64_t sx0 = (int64_t)(((uint64_t)sw * 32768u) / dw) - 32768;
    int64_t sy0 = (int64_t)(((uint64_t)sh * 32768u) / dh) - 32768;

    for (size_t y = 0; y < dh; y++) {
        int64_t sy = sy0 + (int64_t)(dy * y);
        int64_t y0 = sy >> 16; /* floor, handles the negative start */
        int64_t fy = sy - (y0 << 16);
        size_t ya = (size_t)(y0 < 0 ? 0 : y0);
        size_t yb = (ya + 1 < sh) ? ya + 1 : sh - 1;

        for (size_t x = 0; x < dw; x++) {
            int64_t sx = sx0 + (int64_t)(dx * x);
            int64_t x0 = sx >> 16;
            int64_t fx = sx - (x0 << 16);
            size_t xa = (size_t)(x0 < 0 ? 0 : x0);
            size_t xb = (xa + 1 < sw) ? xa + 1 : sw - 1;
            uint8_t *out = dst + (y * dw + x) * 3;
            uint32_t wx0 = 65536u - (uint32_t)fx;
            uint32_t wx1 = (uint32_t)fx;
            uint32_t wy0 = 65536u - (uint32_t)fy;
            uint32_t wy1 = (uint32_t)fy;

            for (int c = 0; c < 3; c++) {
                uint32_t p00 = src[(ya * sw + xa) * 3 + c];
                uint32_t p10 = src[(ya * sw + xb) * 3 + c];
                uint32_t p01 = src[(yb * sw + xa) * 3 + c];
                uint32_t p11 = src[(yb * sw + xb) * 3 + c];
                /* top/bot are 16.16 blends of the source values; the
                 * vertical blend multiplies two 16.16 weights, so the
                 * product lives at 32.32 — 255*65536*65536 overflows
                 * uint32 and the final shift is >> 32. */
                uint64_t top = (uint64_t)p00 * wx0 + (uint64_t)p10 * wx1;
                uint64_t bot = (uint64_t)p01 * wx0 + (uint64_t)p11 * wx1;
                uint64_t v = (top * wy0 + bot * wy1 + (1ull << 31)) >> 32;

                out[c] = (uint8_t)v;
            }
        }
    }
}
