#ifndef IMGOPS_H
#define IMGOPS_H

#include <stddef.h>
#include <stdint.h>

/* Deterministic image kernels (workload #13 Photo Filter, reused by the
 * later image workloads).
 *
 * Everything is plain integer arithmetic on packed RGB888 buffers
 * (width * height * 3 bytes, row-major, top-left origin), so the same
 * input produces byte-identical output on every machine: fixed-point
 * weights with explicit rounding, no floating point anywhere. */

/* Per-channel in-place level stretch: v' = (v - black) * 255 / (white -
 * black), clamped to [0, 255]. white must be > black. */
void img_levels(uint8_t *img, size_t w, size_t h,
                unsigned black, unsigned white);

/* In-place 3x3 color matrix, weights in 8.8 fixed point (row-major,
 * rows are R/G/B outputs, columns R/G/B inputs). Each output channel is
 * sum(v_i * m_ij) with half-up rounding at 8 fractional bits, clamped. */
void img_color_matrix(uint8_t *img, size_t w, size_t h,
                      const int32_t m[9]);

/* Saturation adjustment around Rec.601 luma, s in 8.8 fixed point
 * (256 = identity, 0 = grayscale, 512 = double saturation). Same
 * rounding rules as img_color_matrix. */
void img_saturate(uint8_t *img, size_t w, size_t h, int32_t s);

/* Separable box blur, radius r (>= 1), in place with a caller-provided
 * scratch buffer of at least w * h * 3 bytes. Two passes (horizontal,
 * then vertical), each a running sum over a (2r+1) window with the
 * window average in half-up rounding. Two box passes over the same
 * radius approximate a Gaussian. */
void img_box_blur(uint8_t *img, size_t w, size_t h, unsigned r,
                  uint8_t *scratch);

/* Convert an RGB888 image to single-channel Rec.601 luma (weights
 * 77/150/29 in 8.8, sum 256, half-up rounding). dst holds w * h bytes;
 * src and dst must not alias. */
void img_grayscale(const uint8_t *rgb, size_t w, size_t h, uint8_t *gray);

/* Separable [1 4 6 4 1]/16 Gaussian blur (two passes, sigma ~ 1.4 px) of
 * a single-channel image, in place, with a caller-provided scratch
 * buffer of at least w * h bytes. Edge-clamped windows, half-up
 * rounding. The smoothing stage of a Canny edge pipeline. */
void img_gray_gaussian_blur5(uint8_t *img, size_t w, size_t h,
                             uint8_t *scratch);

/* Extract the (cw x ch) rectangle at (x, y) of src into dst (packed
 * rows). Caller guarantees the rectangle is inside the source. */
void img_crop(uint8_t *dst, size_t dw,
              const uint8_t *src, size_t sw, size_t sh,
              size_t x, size_t y, size_t cw, size_t ch);

/* Bilinear resample of src (sw x sh) into dst (dw x dh), packed RGB.
 * Weights are 16.16 fixed point: each destination pixel samples the
 * source at ((x + 0.5) * sw / dw - 0.5, same for y), the four
 * neighbours are edge-clamped and blended with half-up rounding. */
void img_scale_bilinear(uint8_t *dst, size_t dw, size_t dh,
                        const uint8_t *src, size_t sw, size_t sh);

/* Exact box downscale for integer ratios: the dst (dw x dh) pixel at
 * (x, y) is the half-up-rounded mean of the (sw/dw) x (sh/dh) source
 * block that covers it. Packed RGB in, packed RGB out. The caller
 * guarantees dw, dh >= 1 and that sw and sh divide by dw and dh
 * (the workload thumbnails use power-of-two ratios). Every source
 * pixel contributes to exactly one destination pixel, so the pass
 * reads each source pixel once per thumbnail. */
void img_box_downscale(uint8_t *dst, size_t dw, size_t dh,
                       const uint8_t *src, size_t sw, size_t sh);

#endif /* IMGOPS_H */
