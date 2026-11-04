#ifndef WORKLOAD_OBJECTDETECTION_H
#define WORKLOAD_OBJECTDETECTION_H

#include <stddef.h>
#include <stdint.h>

/* Workload #9 — Object Detection.
 *
 * Mirrors the ninth Geekbench 6 CPU workload: run a MobileNetV1-shaped
 * conv net with an SSD-style detection head over a set of photos.
 * Geekbench 6 runs the trained TFLite model; LotofBench reproduces the
 * *shape* of the dependency dependency-free: a fixed, untrained,
 * quantized (int8) conv net — the same stem + depthwise-separable stack
 * + slim detection head skeleton, every multiply int8 and every
 * accumulator int32 — over eight seeded synthetic 300x300 photos (the
 * SSD300 native input, the stand-in for the resize step). The net is
 * FIXED: its int8 weights are drawn once from a dedicated PRNG stream
 * with a hard-coded seed, so the same bytes run on every machine, and
 * the --seed argument only varies the photos. The whole per-image
 * pipeline (input quantization, 12 convolutions, detection decode) is
 * integer arithmetic with documented rounding, so the feature maps, the
 * output tensor and the decoded detections are bit-identical across
 * machines and compilers. Every timed cycle re-runs the full inference
 * on every photo and verifies the last feature map, the output tensor
 * and the decoded detections against the prepare-time references by
 * SHA-1; any divergence aborts the run instead of scoring. Everything
 * is documented in docs/benchmarks.md, section "Workload 9".
 *
 * The net is deliberately UNTRAINED: it has no learned parameters, so
 * the decoded labels are not real detections — what the workload
 * measures is quantized conv-net throughput (images per second), and
 * what it verifies is the exact byte-level reproducibility of the whole
 * inference, the contract the roadmap prescribes for this workload. */

typedef struct od_ctx od_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the photos are the photos. */
#define OD_PHOTOS        8u  /* the single-core pass detects these eight */
#define OD_PHOTOS_MULTI 32u  /* multi-core pass = 4 x the photo set */
#define OD_INPUT_W      300u /* SSD300: the native input size */
#define OD_INPUT_H      300u

/* The fixed net's shape. The stack is: a 3x3 stride-2 stem (3 -> 32),
 * five depthwise-separable blocks (dw 3x3 + pw 1x1) at channels
 * 32 -> 64 -> 128 -> 256 -> 512 -> 512 with strides 1,2,2,2,1, and a
 * slim detection head (pw 1x1, 512 -> 8) on the final 19x19 grid:
 * 300 -> 150 -> 150 -> 75 -> 38 -> 19 -> 19. 316,225,344 int8 MACs per
 * image (the exact count is pinned by the unit tests and documented in
 * docs/benchmarks.md). */
#define OD_CLASSES 4u /* per-cell class logits in the head */
#define OD_DELTAS  4u /* per-cell box deltas in the head */
#define OD_HEAD_COLS (OD_CLASSES + OD_DELTAS)
#define OD_GRID     19u /* final feature-map side (the detection grid) */

/* Quantization: activations and weights are symmetric int8 (zero point
 * 0, range [-127, 127]); every convolution accumulates in int32 and is
 * requantized by an arithmetic right shift with half-away-from-zero
 * rounding, then ReLU (hidden layers) and symmetric saturation. The
 * per-layer shifts are part of the fixed net (see workload_objectdetection.c). */

/* Detection decode (a simplified SSD-lite head), all integer:
 *  - the anchor of cell (x, y) is its centre in input coordinates,
 *    kept in 1/8-px fixed point: od_anchor_q8(x) = ((2x+1)*1200 + 9)/19
 *    (half-up; the same formula serves rows and columns);
 *  - each delta has the fixed unit 16 q8 = 2 px; box centre = anchor +
 *    delta*16, box side = 64 + delta*16 (an 8-px base square), every
 *    value clamped to [0, 2400] (the frame in 1/8-px units);
 *  - the class is the first argmax of the four signed logits. */
#define OD_Q8_MAX 2400 /* the frame side in 1/8-px units */

/* Result of one full inference. The three digests pin the byte-exact
 * state of the pipeline: the last feature map (19x19x512), the head's
 * raw output tensor (19x19x8) and the decoded detections. classes[]
 * counts the grid cells whose first-argmax is each class (a
 * human-readable summary of the decoded tensor). */
typedef struct {
    uint8_t  feature_digest[20];
    uint8_t  tensor_digest[20];
    uint8_t  det_digest[20];
    uint32_t classes[OD_CLASSES];
} od_result;

/* Planted-object bounding box (exclusive x1/y1) in photo coordinates. */
typedef struct {
    int32_t x0, y0, x1, y1;
} od_bbox;

/* Objects are drawn as darker rectangles: od_object_tone() returns the
 * (negative) per-channel delta in [-90, -50]; even in the darkest
 * corner of the corpus the applied step stays >= 50, far above the
 * +-6 per-pixel noise. */

/* Per-image scratch: one per thread, allocated once, reused for every
 * photo and run (threads never share writable state). */
typedef struct {
    int8_t   *input_q; /* 300*300*3: quantized input */
    int8_t   *buf_a;   /* conv interleave buffer (max 150*150*64) */
    int8_t   *buf_b;   /* conv interleave buffer (max 150*150*64) */
    int8_t   *head;    /* 19*19*8: raw head output tensor */
    uint8_t  *det;     /* 19*19*10: decoded detections */
    od_result res;     /* result of the last inference */
} od_scratch;

int  od_scratch_init(od_scratch *s);
void od_scratch_free(od_scratch *s);

/* The per-image pipeline: RGB888 -> int8 quantization, the 12
 * convolutions of the fixed net, SHA-1 of the last feature map and of
 * the head output tensor, the SSD-lite decode, SHA-1 of the decoded
 * detections. Returns 0 and fills *out, or -1 on a NULL/mismatch. */
int od_infer(const uint8_t *rgb, od_scratch *s, od_result *out);

/* --- the integer kernels (public for the differential tests) ------------- */

/* RGB888 -> symmetric int8: q = round(p * 254 / 255) - 127, half-up.
 * n_px is the pixel count (the buffer holds 3 * n_px bytes). */
void od_quantize_input(const uint8_t *rgb, size_t n_px, int8_t *out);

/* Requantize one int32 accumulator: half-away-from-zero division by
 * 2^shift, optional ReLU, symmetric saturation to [-127, 127]. */
int8_t od_requant(int32_t acc, unsigned shift, int relu);

/* Depthwise 3x3 convolution (out channels == in channels), zero
 * padding, SAME ceil output geometry (out = (in + 1) / 2 at stride 2).
 * weights hold c * 9 int8 taps, tap-major (dy, dx, channel). NHWC
 * buffers throughout. */
void od_conv_dw3x3(const int8_t *in, size_t w, size_t h, size_t c,
                   const int8_t *weights, unsigned stride, unsigned shift,
                   int relu, int8_t *out);

/* Pointwise 1x1 convolution. weights hold cout * cin int8 weights,
 * out-channel-major. */
void od_conv_pw1x1(const int8_t *in, size_t w, size_t h, size_t cin,
                   size_t cout, const int8_t *weights, unsigned shift,
                   int relu, int8_t *out);

/* Full 3x3 convolution (the stem: 3 -> 32), zero padding, stride 2.
 * weights hold cout * cin * 9 int8 taps, out-channel-major. */
void od_conv3x3_stem(const int8_t *in, size_t w, size_t h,
                     const int8_t *weights, unsigned shift, int8_t *out);

/* The anchor centre of one detection-grid row/column, in 1/8-px units
 * (half-up; identical for rows and columns). Exposed for tests. */
int32_t od_anchor_q8(unsigned cell);

/* The SSD-lite decode of one head output tensor (19x19x8, NHWC): 10
 * bytes per grid cell into det (x0, y0, x1, y1 in 1/8-px units as
 * big-endian u16, class, argmax logit) and the per-class cell counts
 * into out->classes. Exposed for tests. */
void od_decode_head(const int8_t *head, uint8_t *det, od_result *out);

/* --- corpus / net / run ------------------------------------------------------ */

/* Build the photo corpus (off the clock; fixed seed => byte-identical
 * photos on every machine) and the fixed net, then run the reference
 * inference on every photo. The net is independent of the seed. */
od_ctx *od_prepare(unsigned seed);
void    od_free(od_ctx *ctx);

/* One workload pass with *threads threads (clamped to the photo set of
 * the pass: 8 single-core, 32 multi-core). Fills *throughput_ips with
 * the median over the runs, in images per second, and *images with the
 * exact image count of the pass (8 or 32). Returns 0, or -1 if any
 * inference failed to verify. */
int od_run(od_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_ips, uint64_t *images);

/* Read-only access for tests and tooling. */
const uint8_t   *od_photo(const od_ctx *ctx, unsigned photo);
size_t           od_photo_len(const od_ctx *ctx, unsigned photo);
const od_result *od_reference(const od_ctx *ctx, unsigned photo);
unsigned         od_archetype(const od_ctx *ctx, unsigned photo);

/* The planted objects of one photo (documented scene structure). */
unsigned od_object_count(const od_ctx *ctx, unsigned photo);
od_bbox  od_object_bbox(const od_ctx *ctx, unsigned photo, unsigned obj);
unsigned od_object_class(const od_ctx *ctx, unsigned photo, unsigned obj);
int32_t  od_object_tone(const od_ctx *ctx, unsigned photo, unsigned obj);

/* SHA-1 of the fixed net's weight blob (seed-independent pin). */
const uint8_t *od_net_digest(const od_ctx *ctx);

/* The fixed net's int8 weight blob (layer-major, in the execution order
 * of the layer table: stem 32*3*9, then per dw layer out*9 taps
 * (dy,dx,channel), per pw layer out*in, head 8*512) — exposed so the
 * tests can run an independent, naive inference and compare. */
const int8_t *od_net_weights(void);

uint64_t od_task_images(unsigned multi); /* image count of one pass */

/* Exact multiply-accumulate count of one full inference (every conv of
 * the fixed net, including the head) — deterministic, pinned by the
 * unit tests, the MAC analogue of the Ray Tracer's exact ray count. */
uint64_t od_task_macs(void);

#endif /* WORKLOAD_OBJECTDETECTION_H */
