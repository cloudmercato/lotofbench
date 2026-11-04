#ifndef WORKLOAD_HDR_H
#define WORKLOAD_HDR_H

#include <stddef.h>
#include <stdint.h>

/* Workload #14 — HDR.
 *
 * Mirrors the fourteenth Geekbench 6 CPU workload: blend six 16 MP SDR
 * exposures of one scene into an HDR image with exposure fusion
 * (Mertens-style pyramid blending). Geekbench 6 feeds real bracketed
 * photos through the fusion stack; LotofBench reproduces the *shape*
 * of the workload dependency-free: six seeded synthetic exposures of a
 * 12-bit synthetic scene — a bracketed burst 1 EV apart whose bright
 * frames clip the highlights and whose dark frames drown the shadows —
 * fused by a 100%-integer exposure-fusion pipeline:
 *
 *   1. per-exposure weight maps, three Mertens factors in fixed point
 *      (the formulas are the workload's contract, restated in
 *      workload_hdr.c and pinned by the tests):
 *        - contrast:   the absolute 4-neighbour Laplacian of the
 *          exposure's Rec.601 luma, quartered, floor 16, cap 256;
 *        - saturation: max(R,G,B) - min(R,G,B), floor 16, cap 256;
 *        - well-exposedness: 255 * (1 - sum_c (v_c - 128)^2 / (3 *
 *          128^2)), half-up — a linear decay from the midtone peak
 *          (the integer stand-in for Mertens' Gaussian kernel);
 *      combined as W16 = (contrast * saturation * well) >> 8 (16-bit);
 *      contrast and saturation each floor at 16 and cap at 256;
 *   2. per-pixel normalization across the six exposures (floor
 *      division, the last exposure carries the remainder, so the six
 *      normalized weights sum to exactly 255 everywhere; an all-zero
 *      pixel falls back to the documented uniform split);
 *   3. a 6-level Laplacian pyramid of every exposure and a 6-level
 *      Gaussian pyramid of every normalized weight plane (2x2 half-up
 *      box downsample, the shared img_scale_bilinear as the upsample);
 *   4. per-level blending: the level accumulator is the weight-summed
 *      Laplacian (one symmetric half-up division by 255 per pixel), the
 *      deepest level blends the Gaussian residuals directly;
 *   5. collapse: bilinear expand (signed 16.16, half-up) + add per
 *      level, clamped to bytes — the fused HDR frame.
 *
 * Everything is integer arithmetic with documented rounding, so the
 * weight stack and the fused frame are bit-identical across machines
 * and compilers. Every timed cycle re-runs the full pipeline on every
 * scene and verifies the SHA-1 of the normalized weight stack and of
 * the fused frame, plus three exact counters (weight-starved pixels,
 * clipped output pixels, black output pixels), against the
 * prepare-time references; any divergence aborts the run instead of
 * scoring. Everything is documented in docs/benchmarks.md, section
 * "Workload 14".
 *
 * The corpus carries ground truth: every scene plants one flat
 * highlight patch and one flat shadow patch (boxes and clean 12-bit
 * luminances exposed through accessors) whose bytes are predictable
 * in every exposure — clipped in the bright stops for the highlight,
 * near-black in the shortest exposure for the shadow. The fused
 * output must recover both: de-clip the highlight patch toward its
 * best-exposed value, lift the shadow patch far above what the dark
 * exposures show. The tests pin that recovery with the fused digest
 * and the exact counters per scene, and drive hdr_process through
 * constant planes where every pyramid level stays constant, so the
 * fused value reduces to the closed-form weight math and is checked
 * to the bit. */

typedef struct hdr_ctx hdr_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the burst is the burst. */
#define HDR_SCENES        4u  /* the single-core pass fuses these four */
#define HDR_SCENES_MULTI 16u  /* multi-core pass = 4 x the scene set */
#define HDR_EXPOSURES     6u  /* the bracketed burst, 1 EV apart */
#define HDR_SCENE_W    1600u  /* the stand-in for the 16 MP capture */
#define HDR_SCENE_H    1200u

/* Pyramid geometry: the Gaussian pyramids hold HDR_LEVELS + 1 planes
 * (levels 0..HDR_LEVELS), the Laplacian/accumulator pyramids hold
 * HDR_LEVELS planes (levels 0..HDR_LEVELS-1); the deepest accumulator
 * level blends the Gaussian residuals directly. Level l halves level
 * l-1 (half-up). */
#define HDR_LEVELS       6u

/* The 12-bit scene radiance range and the exposure mapping: byte =
 * clamp((V * 2^k * 255 + 2048) >> 12 + noise, 0, 255) for exposure
 * k = 0..5 — a bracketed burst 1 EV apart where exposure 0 maps the
 * full 12-bit range onto [0, 255] and every further stop doubles it
 * (bright frames clip the highlights, dark frames drown the shadows). */
#define HDR_LUMA_MAX   4095u

/* The planted ground-truth patches: flat squares of clean 12-bit
 * luminance (no noise, no features), 192x192, the highlight patch in
 * the top-right region (at least 64 px inside the frame), the shadow
 * patch in the bottom-left region (at least 96 px inside). Their
 * luminances are drawn from the seed within these bands. */
#define HDR_PATCH       192u
#define HDR_HILITE_MIN 3584u
#define HDR_HILITE_MAX 3712u
#define HDR_SHADOW_MIN  192u
#define HDR_SHADOW_MAX  224u

/* Result of one full scene fusion. The two digests pin the byte-exact
 * state of the pipeline: the normalized six-plane weight stack and the
 * fused output frame. uniform counts the pixels whose raw weights were
 * all zero (the documented uniform fallback; 0 for the whole corpus —
 * the scene floor keeps every pixel visible in the longest exposure).
 * hi_clip counts fused pixels clamped at 255 — bright cores no
 * exposure recovers, plus the pyramid's overshoot at hard edges (the
 * corpus measures both: 0 on the smooth sunset scene, ~24k on the
 * window scene) — lo_black counts fused pixels at 0 (the corpus floor
 * makes these impossible). */
typedef struct {
    uint8_t  weights_digest[20];
    uint8_t  fused_digest[20];
    uint32_t uniform;
    uint32_t hi_clip;
    uint32_t lo_black;
} hdr_result;

/* Planted patch box (exclusive x1/y1) in scene coordinates and its
 * clean 12-bit luminance; the ground truth the fusion must recover. */
typedef struct {
    int32_t x0, y0, x1, y1;
} hdr_box;

/* Per-scene scratch: one per thread, allocated once, reused for every
 * scene and run (threads never share writable state). */
typedef struct {
    uint8_t  *gray;    /* 1600*1200: one exposure's luma */
    uint16_t *wprod;   /* 6 * 1600*1200: raw weight products */
    uint8_t  *wnorm;   /* 6 * 1600*1200: normalized weight stack */
    uint8_t  *wgnorm;  /* Gaussian pyramid of one weight plane */
    uint8_t  *gpyr;    /* Gaussian pyramid of one exposure (RGB) */
    int16_t  *lpyr;    /* Laplacian pyramid of one exposure (RGB) */
    uint8_t  *up8;     /* 1600*1200*3: img_scale_bilinear scratch */
    int32_t  *acc;     /* blend accumulator pyramid (RGB) */
    int16_t  *cur;     /* 1600*1200*3: collapse running plane */
    int16_t  *up16;    /* 1600*1200*3: expand16 scratch */
    uint8_t  *fused;   /* 1600*1200*3: the fused output */
    hdr_result res;    /* result of the last pipeline */
} hdr_scratch;

int  hdr_scratch_init(hdr_scratch *s);
void hdr_scratch_free(hdr_scratch *s);

/* --- the integer kernels (public for the differential tests) ------- */

/* One exposure's raw weight plane: gray receives the exposure's
 * Rec.601 luma (the shared imgops convention), w16 the per-pixel
 * weight product W16 = (contrast * saturation * well) >> 8. w, h are
 * the plane dimensions; gray and w16 hold w * h entries each. */
void hdr_weights(const uint8_t *rgb, size_t w, size_t h,
                 uint8_t *gray, uint16_t *w16);

/* Normalize the six raw weight planes (exposure order) into the six
 * output planes: floor division by the per-pixel sum, the last
 * exposure carrying the remainder so the stack sums to exactly 255 per
 * pixel; an all-zero pixel takes the uniform fallback 42,42,42,42,42,45
 * and is counted in *uniform (may be NULL). out must not alias w16. */
void hdr_normalize(const uint16_t *const *w16, size_t n_px,
                   uint8_t *const *out, uint32_t *uniform);

/* 2x2 box downsample (half-up, edge-clamped second tap) of a packed
 * RGB888 plane (hdr_down_rgb) or a single-channel plane
 * (hdr_down_gray). dst holds dw * dh (pixels / bytes). */
void hdr_down_rgb(uint8_t *dst, size_t dw, size_t dh,
                  const uint8_t *src, size_t sw, size_t sh);
void hdr_down_gray(uint8_t *dst, size_t dw, size_t dh,
                   const uint8_t *src, size_t sw, size_t sh);

/* Bilinear x~2 expand of a signed 16-bit plane, the same source
 * coordinate convention as img_scale_bilinear ((x + 0.5) * sw / dw -
 * 0.5 in 16.16, edge-clamped neighbours) with half-up rounding toward
 * +infinity. dst holds dw * dh * 3 int16s; dst must not alias src. */
void hdr_expand16(int16_t *dst, size_t dw, size_t dh,
                  const int16_t *src, size_t sw, size_t sh);

/* The full per-scene pipeline: six per-exposure weight planes,
 * normalization, the Laplacian/Gaussian pyramids, the per-level blend
 * and the collapse (written to s->fused), SHA-1 of the normalized
 * weight stack and of the fused frame, exact counters. exposures holds
 * HDR_EXPOSURES pointers to 1600x1200 RGB888 planes in exposure order.
 * Returns 0 and fills *out, or -1 on a NULL/mismatch. */
int hdr_process(const uint8_t *const *exposures, hdr_scratch *s,
                hdr_result *out);

/* --- corpus / run ---------------------------------------------------- */

/* Build the scene corpus (fixed seed => byte-identical exposures on
 * every machine; generation off the clock), then run the reference
 * pipeline on every scene. */
hdr_ctx *hdr_prepare(unsigned seed);
void     hdr_free(hdr_ctx *ctx);

/* One workload pass with *threads threads (clamped to the scene set of
 * the pass: 4 single-core, 16 multi-core). Fills *throughput_mpx with
 * the median over the runs, in millions of fused output pixels per
 * second, and *pixels with the exact output-pixel count of the pass
 * (4 or 16 scenes). Returns 0, or -1 if any scene failed to verify. */
int hdr_run(hdr_ctx *ctx, unsigned runs, unsigned *threads,
            double *throughput_mpx, uint64_t *pixels);

/* Read-only access for tests and tooling. */
const uint8_t *hdr_exposure(const hdr_ctx *ctx, unsigned scene,
                            unsigned k);
size_t         hdr_exposure_len(const hdr_ctx *ctx, unsigned scene);
const hdr_result *hdr_reference(const hdr_ctx *ctx, unsigned scene);
unsigned       hdr_archetype(const hdr_ctx *ctx, unsigned scene);

/* The planted patches (documented scene structure). */
hdr_box  hdr_hilite_box(const hdr_ctx *ctx, unsigned scene);
int32_t  hdr_hilite_luma(const hdr_ctx *ctx, unsigned scene);
hdr_box  hdr_shadow_box(const hdr_ctx *ctx, unsigned scene);
int32_t  hdr_shadow_luma(const hdr_ctx *ctx, unsigned scene);

uint64_t hdr_task_pixels(unsigned multi); /* output pixels of one pass */

#endif /* WORKLOAD_HDR_H */
