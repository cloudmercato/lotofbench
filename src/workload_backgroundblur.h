#ifndef WORKLOAD_BACKGROUNDBLUR_H
#define WORKLOAD_BACKGROUNDBLUR_H

#include <stddef.h>
#include <stdint.h>

/* Workload #10 — Background Blur.
 *
 * Mirrors the tenth Geekbench 6 CPU workload: run image segmentation
 * over a video clip and blur the background behind the detected
 * subject, the portrait-mode video-call effect. Geekbench 6 runs the
 * trained DeepLabV3+ TFLite model; LotofBench reproduces the *shape*
 * of the workload dependency-free: a deterministic segmentation pass
 * (exact background subtraction against a prepare-time background
 * model — the trained-model stand-in, see the divergences note in
 * docs/benchmarks.md) over seeded synthetic 1920x1080 video frames,
 * followed by the per-pixel-blur-strength composite that consumes the
 * mask. The per-frame pipeline is:
 *
 *   1. downscale the frame 1/4 (the DeepLab-style low-res inference
 *      input, the shared img_box_downscale kernel);
 *   2. segment the 480x270 tap: per-cell max channel difference
 *      against the frame's background model, thresholded — the
 *      subject stands out by 50+ levels, the noise floor is +-6;
 *   3. a 3x3 majority filter (the morphological/CRF cleanup stand-in)
 *      smooths the binary mask;
 *   4. a bilinear x4 upsample to full resolution feathers the mask;
 *   5. a two-pass box blur (radius 9, the shared img_box_blur kernel)
 *      produces the blurred frame;
 *   6. the composite out = (mask*sharp + (255-mask)*blurred + 127)/255
 *      per channel — the per-pixel blur strength is the mask: a pixel
 *      with mask 255 stays bit-identical to the input, a pixel with
 *      mask 0 is exactly the blurred value, and the feathered edge
 *      interpolates between the two.
 *
 * Everything is integer arithmetic with documented rounding, so the
 * mask and the composited frame are bit-identical across machines and
 * compilers. Every timed cycle re-runs the full pipeline on every
 * frame and verifies the SHA-1 of the mask and of the output frame,
 * plus two exact counters (fully-foreground pixels, changed pixels),
 * against the prepare-time references; any divergence aborts the run
 * instead of scoring. Everything is documented in
 * docs/benchmarks.md, section "Workload 10".
 *
 * The segmentation is deliberately MODEL-FREE: it subtracts the exact
 * background (known per scene archetype) instead of running a trained
 * net, so the mask provably recovers the planted subject — the
 * workload verifies real segmentation quality (subject interior sharp,
 * background blurred) and mask + composite throughput (frames per
 * second), the contract the roadmap prescribes for this workload. */

typedef struct bb_ctx bb_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the clip is the clip. */
#define BB_FRAMES        12u  /* the single-core pass blurs these twelve */
#define BB_FRAMES_MULTI  48u  /* multi-core pass = 4 x the frame set */
#define BB_FRAME_W      1920u /* Full HD, the stand-in for the capture size */
#define BB_FRAME_H      1080u

/* Segmentation geometry: the pipeline runs on a 1/4 tap (480x270),
 * like DeepLab's low-resolution inference, and the mask is upsampled
 * back to the frame. */
#define BB_SEG_SCALE      4u
#define BB_SEG_W (BB_FRAME_W / BB_SEG_SCALE) /* 480 */
#define BB_SEG_H (BB_FRAME_H / BB_SEG_SCALE) /* 270 */

/* Segmentation threshold on the per-cell max channel difference
 * against the background model. The noise floor is bounded by +-6 per
 * byte (a 4x4 block mean of the worst-case noise is +-6), the planted
 * subject step is at least 50 (see workload_backgroundblur.c), so
 * every pure-background cell scores <= 6 and every pure-subject cell
 * >= 44: the threshold separates them with a documented margin. */
#define BB_SEG_THRESH    24u

/* The composite blur radius (two-pass box blur, the shared
 * imgops kernel). The per-pixel blur strength interpolates between
 * the sharp frame (mask 255) and this blur (mask 0). */
#define BB_BLUR_RADIUS    9u

/* Result of one full frame pipeline. The two digests pin the
 * byte-exact state of the pipeline: the upsampled mask and the
 * composited output frame. mask_ones counts the mask==255 pixels
 * (the deep-foreground plateau); changed counts the output pixels
 * that differ from the input frame in any channel (the work the blur
 * did — zero would mean the composite collapsed to a copy). */
typedef struct {
    uint8_t  mask_digest[20];
    uint8_t  out_digest[20];
    uint32_t mask_ones;
    uint32_t changed;
} bb_result;

/* Planted-subject bounding box (exclusive x1/y1) in frame coordinates;
 * the ground truth the mask must recover. */
typedef struct {
    int32_t x0, y0, x1, y1;
} bb_bbox;

/* Per-frame scratch: one per thread, allocated once, reused for every
 * frame and run (threads never share writable state). blurred holds
 * the blurred frame on entry to the composite and the composited
 * output on return from bb_process. */
typedef struct {
    uint8_t  *tap;     /* 480*270*3: downscaled frame */
    uint8_t  *coarse;  /* 480*270: thresholded segmentation */
    uint8_t  *refined; /* 480*270: majority-filtered mask */
    uint8_t  *mask;    /* 1920*1080: upsampled soft mask */
    uint8_t  *blurred; /* 1920*1080*3: blurred frame, then output */
    uint8_t  *blur_sc; /* 1920*1080*3: img_box_blur scratch */
    bb_result res;     /* result of the last pipeline */
} bb_scratch;

int  bb_scratch_init(bb_scratch *s);
void bb_scratch_free(bb_scratch *s);

/* --- the integer kernels (public for the differential tests) ------------- */

/* Segment one tap against its background model: coarse[i] = 255 when
 * the max per-channel absolute difference reaches BB_SEG_THRESH, else
 * 0. n is the cell count (both buffers hold n bytes; the tap holds
 * 3 * n bytes). */
void bb_segment(const uint8_t *tap, const uint8_t *model, size_t n,
                uint8_t *coarse);

/* 3x3 majority filter over a binary plane, zero padding outside:
 * out[i] = 255 when at least 5 of the 9 neighbours are 255. w, h >= 1;
 * out must not alias in. */
void bb_majority3(const uint8_t *in, size_t w, size_t h, uint8_t *out);

/* Bilinear resample of a single-channel plane (the mask), same
 * fixed-point convention as img_scale_bilinear: source coordinate
 * (x + 0.5) * sw / dw - 0.5 in 16.16, edge-clamped neighbours,
 * half-up rounding. dst holds dw * dh bytes. */
void bb_mask_upscale(uint8_t *dst, size_t dw, size_t dh,
                     const uint8_t *src, size_t sw, size_t sh);

/* The per-pixel-blur-strength composite: out = (mask*sharp +
 * (255-mask)*blurred + 127) / 255 per channel (half-up), n_px pixels.
 * out may alias blurred (pixel-local); it must not alias frame or
 * mask. */
void bb_composite(uint8_t *out, const uint8_t *frame,
                  const uint8_t *blurred, const uint8_t *mask,
                  size_t n_px);

/* The full per-frame pipeline: 1/4 downscale, background-subtraction
 * segmentation against the frame's archetype model, 3x3 majority
 * filter, bilinear x4 mask upsample, radius-9 box blur and the
 * per-pixel composite (written over the blurred frame in s->blurred),
 * SHA-1 of the mask and of the output frame, exact counters. Returns
 * 0 and fills *out, or -1 on a NULL/mismatch. */
int bb_process(const uint8_t *frame, const uint8_t *model, bb_scratch *s,
               bb_result *out);

/* --- corpus / models / run ------------------------------------------------ */

/* Build the background models (one per scene archetype, off the
 * clock; seed-independent) and the frame corpus (fixed seed =>
 * byte-identical frames on every machine), then run the reference
 * pipeline on every frame. */
bb_ctx *bb_prepare(unsigned seed);
void    bb_free(bb_ctx *ctx);

/* One workload pass with *threads threads (clamped to the frame set
 * of the pass: 12 single-core, 48 multi-core). Fills *throughput_fps
 * with the median over the runs, in frames per second, and *frames
 * with the exact frame count of the pass (12 or 48). Returns 0, or -1
 * if any frame failed to verify. */
int bb_run(bb_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_fps, uint64_t *frames);

/* Read-only access for tests and tooling. */
const uint8_t   *bb_frame(const bb_ctx *ctx, unsigned frame);
size_t           bb_frame_len(const bb_ctx *ctx, unsigned frame);
const bb_result *bb_reference(const bb_ctx *ctx, unsigned frame);
unsigned         bb_archetype(const bb_ctx *ctx, unsigned frame);

/* The planted subject of one frame (documented scene structure). */
bb_bbox bb_subject_bbox(const bb_ctx *ctx, unsigned frame);
int32_t bb_subject_tone(const bb_ctx *ctx, unsigned frame);

/* The background model of one archetype (BB_ARCHETYPES planes of
 * BB_SEG_W x BB_SEG_H RGB), and the SHA-1 of the whole model table —
 * the models derive from the archetype palette alone, so the digest
 * is seed-independent (the analogue of the fixed net's digest). */
const uint8_t *bb_model(const bb_ctx *ctx, unsigned arch);
const uint8_t *bb_model_digest(const bb_ctx *ctx);

uint64_t bb_task_frames(unsigned multi); /* frame count of one pass */

#endif /* WORKLOAD_BACKGROUNDBLUR_H */
