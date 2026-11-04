#ifndef WORKLOAD_ASSETCOMPRESSION_H
#define WORKLOAD_ASSETCOMPRESSION_H

#include <stddef.h>
#include <stdint.h>

/* Workload #8 — Asset Compression.
 *
 * Mirrors the eighth Geekbench 6 CPU workload: push a set of texture and
 * geometry assets through compression codecs. Geekbench 6 runs 16
 * texture/geometry assets through ASTC, BC7, DXTC and Draco; LotofBench
 * reproduces the shape dependency-free with the DXTC family — BC1 (DXT1)
 * color blocks at two encoder effort levels for opaque textures, BC3
 * (DXT5) for alpha textures — plus a Draco-style geometry codec
 * (position quantization + parallelogram prediction + zigzag + varints)
 * for indexed triangle meshes. Every timed cycle re-encodes, decodes and
 * verifies each asset against prepare-time reference digests (encoded
 * stream, decoded artifact, total squared error); any drift aborts the
 * run instead of scoring. Everything is integer arithmetic, so digests
 * are bit-identical across machines. Documented in docs/benchmarks.md,
 * section "Workload 8". */

/* Asset kinds (ac_asset_kind). */
#define AC_KIND_TEXTURE_OPAQUE 0
#define AC_KIND_TEXTURE_ALPHA  1
#define AC_KIND_MESH           2

/* The asset set: 8 opaque textures + 4 alpha textures + 4 meshes, the
 * GB6 "16 texture/geometry assets" shape. The multi-core pass runs the
 * 4x set (64 assets) split across threads. */
#define AC_TEXTURES        8u
#define AC_ALPHA_TEXTURES  4u
#define AC_MESHES          4u
#define AC_ASSETS          (AC_TEXTURES + AC_ALPHA_TEXTURES + AC_MESHES)
#define AC_ASSETS_MULTI    (4u * AC_ASSETS)

/* Every texture is 256x256 (like the GB6 asset textures, square and
 * block-aligned; BC1/BC3 require multiples of 4). */
#define AC_TEX_W 256u
#define AC_TEX_H 256u

/* Geometry quantization: positions are quantized per component to
 * AC_QUANT_BITS bits (0..4095) over the per-mesh per-component range —
 * the Draco default is 11-14 bits; 12 sits in the middle. */
#define AC_QUANT_BITS   12
#define AC_QUANT_LEVELS 4095 /* 2^12 - 1 */

/* BC1 color-search effort levels (ac_bc1_encode). FAST evaluates the
 * raw bounding-box endpoints; HIGH additionally evaluates three inset
 * variants (each endpoint pulled in by a quarter of the channel range)
 * and keeps the lowest-error pair, first candidate winning ties. */
#define AC_EFFORT_FAST 0
#define AC_EFFORT_HIGH 1

/* --- data primitives (plain structs: the gate and the unit tests build
 *     their own micro-inputs) ------------------------------------------- */

/* One texture: 8-bit RGBA, alpha 255 on the opaque assets. */
typedef struct {
    unsigned w, h;
    const uint8_t *rgba; /* w * h * 4 bytes */
} ac_image;

/* One indexed triangle mesh. Positions are the corpus-domain integers
 * (the generator's fixed-point space); index_count is a multiple of 3.
 * The corpus meshes number vertices in first-appearance order of the
 * triangle list (strip-visit order), with the introduced vertex last in
 * every triangle — the layout the geometry codec's prediction expects. */
typedef struct {
    unsigned        vertex_count;
    unsigned        index_count;
    const int32_t  *positions; /* 3 * vertex_count: x,y,z interleaved */
    const uint32_t *indices;   /* index_count triangle corners */
} ac_mesh;

/* --- BC1 / BC3 texture codecs ---------------------------------------- */

size_t ac_bc1_bound(unsigned w, unsigned h); /* (w/4)*(h/4)*8 */
size_t ac_bc3_bound(unsigned w, unsigned h); /* (w/4)*(h/4)*16 */

/* Encode an RGBA image into BC1 (color only, alpha ignored) or BC3
 * (8-byte alpha block + 8-byte BC1 color block per 4x4 tile). The
 * encoder always emits the 4-color mode (stored color0 word > color1
 * word; flat blocks get a +1 endpoint bump), so decode(alpha) = 255.
 * Returns the compressed size, or 0 on a NULL argument, non-multiple-of-4
 * dimensions or an undersized out_cap. */
size_t ac_bc1_encode(const uint8_t *rgba, unsigned w, unsigned h,
                     int effort, uint8_t *out, size_t out_cap);
size_t ac_bc3_encode(const uint8_t *rgba, unsigned w, unsigned h,
                     uint8_t *out, size_t out_cap);

/* Decode BC1/BC3 into RGBA (w*h*4). BC1 writes alpha 255; the 3-color
 * transparent mode (stored color0 <= color1, index 3) writes alpha 0 —
 * the decoder implements the full DXTC spec rule even though the
 * encoder never emits it. Returns 0, or -1 on NULL/size mismatches. */
int ac_bc1_decode(const uint8_t *blocks, size_t len,
                  unsigned w, unsigned h, uint8_t *out_rgba);
int ac_bc3_decode(const uint8_t *blocks, size_t len,
                  unsigned w, unsigned h, uint8_t *out_rgba);

/* --- Draco-style geometry codec --------------------------------------- */

/* Stream format (all integers little-endian, varints = LEB128):
 *   varint vertex_count, varint triangle_count, u8 quant_bits, u8 flags(=1)
 *   position stream, one vertex at a time (x,y,z interleaved, quantized
 *     domain 0..(2^quant_bits - 1)):
 *     vertex 0: raw varint per component
 *     vertices 1-2: zigzag delta vs the previous vertex
 *     vertex k >= 3: zigzag delta vs the parallelogram prediction
 *       q[k-1] + q[k-2] - q[k-3] — exact on strip-ordered streams (the
 *       corpus layout); merely larger residuals on foreign orders
 *   index stream, one corner at a time:
 *     zigzag varint delta vs the previous corner (seeded with 0)
 * The codec is lossless at the quantized level for ANY indexed mesh —
 * foreign vertex/triangle orders only enlarge residuals. Strip-ordered
 * (affine) vertex sequences quantize to near-affine ones, so their
 * residuals stay within one or two units — one varint byte. */
#define AC_MESH_STRIPS_MAX      6u  /* corpus generator bounds ... */
#define AC_MESH_STRIP_TRIS_MAX 64u /* ... the unit tests size buffers on */
#define AC_MESH_VERT_MAX (AC_MESH_STRIPS_MAX * (AC_MESH_STRIP_TRIS_MAX + 2u))
#define AC_MESH_TRI_MAX  (AC_MESH_STRIPS_MAX * AC_MESH_STRIP_TRIS_MAX)
size_t ac_mesh_bound(unsigned vertex_count, unsigned index_count);
size_t ac_mesh_encode(unsigned vertex_count, unsigned index_count,
                      const int32_t *qpos, const uint32_t *indices,
                      uint8_t *out, size_t out_cap);
int    ac_mesh_decode(const uint8_t *buf, size_t len,
                      int32_t *out_qpos, uint32_t *out_indices,
                      unsigned *out_vertex_count, unsigned *out_index_count);

/* Quantize a mesh's positions per component to 0..4095 over the
 * per-component range (round half up; a constant component maps to 0).
 * Exposed so tests can pin the quantization step independently. */
void ac_mesh_quantize(const ac_mesh *m, int32_t *out_q);

/* --- corpus + run ------------------------------------------------------ */

typedef struct ac_ctx ac_ctx;

/* Build the asset corpus and run every codec once per asset to pin the
 * reference digests and errors (off the clock; fixed seed => a
 * byte-identical corpus on every machine). */
ac_ctx *ac_prepare(unsigned seed);
void    ac_free(ac_ctx *ctx);

unsigned ac_asset_count(const ac_ctx *ctx); /* 16 */
int      ac_asset_kind(const ac_ctx *ctx, unsigned asset);
/* Payload accessors; the image/mesh pointer is NULL for foreign kinds. */
const ac_image *ac_asset_image(const ac_ctx *ctx, unsigned asset);
const ac_mesh  *ac_asset_mesh(const ac_ctx *ctx, unsigned asset);

/* Pass count per asset: 2 for opaque textures (BC1 fast + high), 1 for
 * alpha textures (BC3) and meshes (geometry codec). */
unsigned ac_pass_count(const ac_ctx *ctx, unsigned asset);

/* Prepare-time reference for one asset+pass, verified on every timed
 * cycle: stream_hex pins the encoder, decoded_hex the decoder (BC1/BC3:
 * the decoded RGBA image; meshes: the decoded quantized positions),
 * aux_hex the decoded index list (meshes; "" for textures) and error
 * the total squared RGB error of the decode vs the source (0 for
 * meshes, whose round-trip is exact). */
typedef struct {
    char     stream_hex[41];
    char     decoded_hex[41];
    char     aux_hex[41];
    uint64_t error;
} ac_reference;

const ac_reference *ac_asset_reference(const ac_ctx *ctx, unsigned asset,
                                       unsigned pass);

/* Exact input bytes of one pass, the throughput numerator: opaque
 * textures count w*h*3 per codec pass (2 passes), alpha textures w*h*4
 * (1 pass), meshes vertex_count*12 + index_count*4. Deterministic and
 * machine-independent. multi=1 scores the 4x instance set. */
uint64_t ac_task_bytes(const ac_ctx *ctx, unsigned multi);

/* One workload pass with *threads threads (clamped to the asset set of
 * the pass: 16 single-core, 64 multi-core). Fills *throughput_mbps with
 * the median over the runs and *bytes with the exact task size. Returns
 * 0, or -1 if any verify failed. */
int ac_run(ac_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mbps, uint64_t *bytes);

/* Per-thread scratch, sized by ac_scratch_len() for the largest corpus
 * asset (one allocation, carved; threads never share writable state).
 * qtmp holds the freshly quantized positions of the asset being
 * encoded, qpos/qidx the decoder's output. */
typedef struct {
    uint8_t  *enc;    /* compressed-stream scratch */
    uint8_t  *dec;    /* decoded RGBA scratch */
    int32_t  *qpos;   /* decoded quantized positions */
    int32_t  *qtmp;   /* encode-side quantized positions */
    uint32_t *qidx;   /* decoded indices */
} ac_scratch;

size_t ac_scratch_len(void);
int    ac_scratch_init(ac_scratch *s);
void   ac_scratch_free(ac_scratch *s);

#endif /* WORKLOAD_ASSETCOMPRESSION_H */
