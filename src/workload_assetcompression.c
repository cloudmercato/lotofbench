/* LotofBench — Asset Compression workload (workload #8).
 *
 * Mirrors the eighth Geekbench 6 CPU workload: push a set of texture and
 * geometry assets through compression codecs. Geekbench 6 runs 16
 * texture/geometry assets through ASTC, BC7, DXTC and Draco; LotofBench
 * reproduces the shape dependency-free with the DXTC family — BC1 (DXT1)
 * color blocks at two encoder effort levels for opaque textures, BC3
 * (DXT5) for alpha textures — plus a Draco-style geometry codec
 * (12-bit position quantization + parallelogram prediction + zigzag +
 * varints) for indexed triangle meshes. Every timed cycle re-encodes,
 * decodes and verifies each asset against prepare-time reference digests
 * (encoded stream, decoded artifact, total squared RGB error); any drift
 * aborts the run instead of scoring. Everything is integer arithmetic,
 * so digests are bit-identical across machines.
 *
 * Methodology lives in the shared harness (harness.c): deterministic
 * corpus generated off the clock, wall-clock timing around the full
 * encode + decode + verify cycle, start-gated runs, median over runs.
 * Documented in docs/benchmarks.md, section "Workload 8". */

#include "workload_assetcompression.h"

#include "harness.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <stdlib.h>
#include <string.h>

/* --- scratch sizing ---------------------------------------------------- */

#define AC_ENC_BYTES ((size_t)(AC_TEX_W / 4) * (AC_TEX_H / 4) * 16)
#define AC_DEC_BYTES ((size_t)AC_TEX_W * AC_TEX_H * 4)
#define AC_QPOS_BYTES ((size_t)AC_MESH_VERT_MAX * 3 * sizeof(int32_t))
#define AC_QIDX_BYTES ((size_t)AC_MESH_TRI_MAX * 3 * sizeof(uint32_t))

size_t ac_scratch_len(void)
{
    return AC_ENC_BYTES + AC_DEC_BYTES + AC_QPOS_BYTES * 2 + AC_QIDX_BYTES;
}

int ac_scratch_init(ac_scratch *s)
{
    if (!s)
        return -1;
    s->enc = malloc(AC_ENC_BYTES);
    s->dec = malloc(AC_DEC_BYTES);
    s->qpos = malloc(AC_QPOS_BYTES);
    s->qtmp = malloc(AC_QPOS_BYTES);
    s->qidx = malloc(AC_QIDX_BYTES);
    if (!s->enc || !s->dec || !s->qpos || !s->qtmp || !s->qidx) {
        ac_scratch_free(s);
        return -1;
    }
    return 0;
}

void ac_scratch_free(ac_scratch *s)
{
    if (!s)
        return;
    free(s->enc);
    free(s->dec);
    free(s->qpos);
    free(s->qtmp);
    free(s->qidx);
    s->enc = s->dec = NULL;
    s->qpos = s->qtmp = NULL;
    s->qidx = NULL;
}

/* --- zigzag + LEB128 varints ------------------------------------------- */

static uint32_t zig_encode(int32_t v)
{
    return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

static int32_t zig_decode(uint32_t v)
{
    return (int32_t)(v >> 1) ^ -(int32_t)(v & 1u);
}

static size_t put_varint(uint8_t *out, uint32_t v)
{
    size_t n = 0;

    while (v >= 0x80u) {
        out[n++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

static int get_varint(const uint8_t *buf, size_t len, size_t *pos,
                      uint32_t *out)
{
    uint32_t v = 0;
    unsigned shift = 0;

    for (;;) {
        uint8_t b;

        if (*pos >= len || shift > 28)
            return -1;
        b = buf[(*pos)++];
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u))
            break;
        shift += 7;
    }
    *out = v;
    return 0;
}

/* --- BC1 / BC3 color blocks --------------------------------------------- */

/* 5-6-5 word of an 8-bit RGB triple. */
static unsigned color_word(unsigned r, unsigned g, unsigned b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* 8-bit RGB back from a 5-6-5 word (full-scale expansion x * 255 / max,
 * rounded). Shared by the encoder's error search and the decoder. */
static void word_rgb(unsigned word, unsigned rgb[3])
{
    unsigned r5 = (word >> 11) & 31u, g6 = (word >> 5) & 63u, b5 = word & 31u;

    rgb[0] = (r5 * 255u + 15u) / 31u;
    rgb[1] = (g6 * 255u + 31u) / 63u;
    rgb[2] = (b5 * 255u + 15u) / 31u;
}

/* The 4-color BC1 palette (round-half-up 1/3 and 2/3 interpolants). */
static unsigned color_pal(unsigned k, const unsigned r0[3],
                          const unsigned r1[3], int c)
{
    if (k == 0)
        return r0[c];
    if (k == 1)
        return r1[c];
    if (k == 2)
        return (2 * r0[c] + r1[c] + 1) / 3;
    return (r0[c] + 2 * r1[c] + 1) / 3;
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t res = 0, bit = 1uLL << 62;

    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

/* The 16 block pixels, RGB only. Texture sizes are multiples of 4, so a
 * whole block is always inside the image and no clamp is needed — but
 * keep the reads bounds-safe anyway (defensive; the encoders reject
 * non-multiple-of-4 sizes up front). */
static void dilate_block(const ac_image *img, unsigned bx, unsigned by,
                         unsigned px[16][3])
{
    for (unsigned i = 0; i < 4; i++) {
        unsigned yy = by * 4 + i;

        if (yy >= img->h)
            yy = img->h - 1;
        for (unsigned j = 0; j < 4; j++) {
            unsigned xx = bx * 4 + j;
            const uint8_t *p;

            if (xx >= img->w)
                xx = img->w - 1;
            p = img->rgba + ((size_t)yy * img->w + xx) * 4;
            px[i * 4 + j][0] = p[0];
            px[i * 4 + j][1] = p[1];
            px[i * 4 + j][2] = p[2];
        }
    }
}

/* Total squared RGB error of the block against the 4-color palette of
 * (w0, w1); fills *idx_out with the per-texel nearest entry, first
 * candidate winning ties. (No const on px: C before C23 rejects the
 * array-pointer qualifier conversion at the call site.) */
static unsigned block_error(unsigned px[16][3], unsigned w0,
                            unsigned w1, unsigned *idx_out)
{
    unsigned r0[3], r1[3], err = 0, idx = 0;

    word_rgb(w0, r0);
    word_rgb(w1, r1);
    for (int i = 0; i < 16; i++) {
        unsigned be = 0, bi = 0;
        int first = 1;

        for (unsigned k = 0; k < 4; k++) {
            unsigned e = 0;

            for (int c = 0; c < 3; c++) {
                int d = (int)px[i][c] - (int)color_pal(k, r0, r1, c);

                e += (unsigned)(d * d);
            }
            if (first || e < be) {
                first = 0;
                be = e;
                bi = k;
            }
        }
        err += be;
        idx |= bi << (2 * i);
    }
    *idx_out = idx;
    return err;
}

/* One 4x4 color block: bounding-box endpoints (+ the HIGH-effort inset
 * variants), 4-color mode only, first candidate winning ties. */
static void encode_color_block(const ac_image *img, unsigned bx, unsigned by,
                               int effort, uint8_t out[8])
{
    unsigned px[16][3], mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
    unsigned c0[3], c1[3], best_err = 0, bw0 = 0, bw1 = 0, bidx = 0;

    dilate_block(img, bx, by, px);
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) {
            if (px[i][c] < mn[c])
                mn[c] = px[i][c];
            if (px[i][c] > mx[c])
                mx[c] = px[i][c];
        }
    for (int c = 0; c < 3; c++) {
        c0[c] = mx[c];
        c1[c] = mn[c];
    }

    for (unsigned var = 0; var < 4u; var++) {
        unsigned p0[3], p1[3], w0, w1, lo, hi, err, idx;

        if (var == 0) {
            for (int c = 0; c < 3; c++) {
                p0[c] = c0[c];
                p1[c] = c1[c];
            }
        } else {
            if (effort != AC_EFFORT_HIGH)
                break;
            for (int c = 0; c < 3; c++) {
                unsigned range = mx[c] - mn[c], step = 0;

                if (var == 1)
                    step = range / 4;
                else if (var == 2)
                    step = range / 2;
                else
                    step = 3 * range / 4;
                p0[c] = c0[c] - step > 255u ? 0u : c0[c] - step;
                p1[c] = c1[c] + step > 255u ? 255u : c1[c] + step;
            }
        }

        w0 = color_word(p0[0], p0[1], p0[2]);
        w1 = color_word(p1[0], p1[1], p1[2]);
        /* Stored color0 word > color1 word (4-color mode): a flat block
         * gets a +1 endpoint bump so the words always differ. */
        if (w0 == w1)
            w0++;
        lo = w0 < w1 ? w0 : w1;
        hi = w0 < w1 ? w1 : w0;
        err = block_error(px, lo, hi, &idx);
        if (var == 0 || err < best_err) {
            best_err = err;
            bw0 = lo;
            bw1 = hi;
            bidx = idx;
        }
    }

    out[0] = (uint8_t)(bw0 & 0xffu);
    out[1] = (uint8_t)(bw0 >> 8);
    out[2] = (uint8_t)(bw1 & 0xffu);
    out[3] = (uint8_t)(bw1 >> 8);
    for (int i = 0; i < 4; i++)
        out[4 + i] = (uint8_t)(bidx >> (8 * i));
}

size_t ac_bc1_bound(unsigned w, unsigned h)
{
    return (size_t)(w / 4) * (h / 4) * 8;
}

size_t ac_bc3_bound(unsigned w, unsigned h)
{
    return (size_t)(w / 4) * (h / 4) * 16;
}

size_t ac_bc1_encode(const uint8_t *rgba, unsigned w, unsigned h, int effort,
                     uint8_t *out, size_t out_cap)
{
    ac_image img;
    size_t need;
    unsigned bw = w / 4, bh = h / 4;
    uint8_t blk[8];

    if (!rgba || !out || !w || !h || (w % 4) || (h % 4))
        return 0;
    img.w = w;
    img.h = h;
    img.rgba = rgba;
    need = ac_bc1_bound(w, h);
    if (out_cap < need)
        return 0;
    for (unsigned by = 0; by < bh; by++)
        for (unsigned bx = 0; bx < bw; bx++) {
            encode_color_block(&img, bx, by, effort, blk);
            memcpy(out + ((size_t)by * bw + bx) * 8, blk, 8);
        }
    return need;
}

/* --- BC3 alpha blocks ---------------------------------------------------- */

/* The 8-level BC3 alpha palette. Shared by the encoder's search and the
 * decoder so both compute identical interpolants. */
static unsigned alpha_pal(unsigned k, unsigned a0, unsigned a1)
{
    if (k == 0)
        return a0;
    if (k == 1)
        return a1;
    if (a0 > a1)
        return ((8u - k) * a0 + (k - 1u) * a1) / 7u;
    if (k == 6)
        return 0;
    if (k == 7)
        return 255;
    return ((8u - k) * a0 + (k - 1u) * a1) / 7u;
}

static uint8_t read_alpha(const ac_image *img, unsigned x, unsigned y)
{
    return img->rgba[((size_t)y * img->w + x) * 4 + 3];
}

/* One 4x4 alpha block: exact min/max endpoints, nearest-of-8 search,
 * first candidate winning ties. */
static void encode_alpha_block(const ac_image *img, unsigned bx, unsigned by,
                               uint8_t out[8])
{
    uint8_t px[16];
    uint8_t mn = 255, mx = 0;
    uint64_t idx = 0;

    for (unsigned i = 0; i < 4; i++)
        for (unsigned j = 0; j < 4; j++) {
            uint8_t v = read_alpha(img, bx * 4 + j, by * 4 + i);

            px[i * 4 + j] = v;
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }

    unsigned a0 = mx, a1 = mn;

    /* Stored a0 > a1 (8-interpolant mode): a flat block bumps a1 down. */
    if (a0 == a1)
        a1 = a0 ? (uint8_t)(a0 - 1) : 1;
    for (int i = 0; i < 16; i++) {
        unsigned be = 0;
        unsigned best = 0;
        int first = 1;

        for (unsigned k = 0; k < 8; k++) {
            unsigned p = alpha_pal(k, a0, a1);
            unsigned v = px[i];
            unsigned e = v > p ? v - p : p - v;

            if (first || e < be) {
                first = 0;
                be = e;
                best = k;
            }
        }
        idx |= (uint64_t)best << (3 * i);
    }

    out[0] = (uint8_t)a0;
    out[1] = (uint8_t)a1;
    for (int i = 0; i < 6; i++)
        out[2 + i] = (uint8_t)(idx >> (8 * i));
}

size_t ac_bc3_encode(const uint8_t *rgba, unsigned w, unsigned h, uint8_t *out,
                     size_t out_cap)
{
    ac_image img;
    size_t need;
    unsigned bw = w / 4, bh = h / 4;

    if (!rgba || !out || !w || !h || (w % 4) || (h % 4))
        return 0;
    img.w = w;
    img.h = h;
    img.rgba = rgba;
    need = ac_bc3_bound(w, h);
    if (out_cap < need)
        return 0;
    for (unsigned by = 0; by < bh; by++)
        for (unsigned bx = 0; bx < bw; bx++) {
            uint8_t blk[16];

            encode_alpha_block(&img, bx, by, blk);
            encode_color_block(&img, bx, by, AC_EFFORT_FAST, blk + 8);
            memcpy(out + ((size_t)by * bw + bx) * 16, blk, 16);
        }
    return need;
}

/* --- BC1 / BC3 decode ----------------------------------------------------- */

static int decode_blocks(const uint8_t *blocks, size_t len, unsigned w,
                         unsigned h, unsigned with_alpha, uint8_t *out_rgba)
{
    unsigned bw = w / 4, bh = h / 4;
    size_t stride = with_alpha ? 16u : 8u;

    if (!blocks || !out_rgba || !w || !h || (w % 4) || (h % 4))
        return -1;
    if (len < (size_t)bw * bh * stride)
        return -1;
    for (unsigned by = 0; by < bh; by++)
        for (unsigned bx = 0; bx < bw; bx++) {
            const uint8_t *blk =
                blocks + ((size_t)by * bw + bx) * stride;
            const uint8_t *cb = with_alpha ? blk + 8 : blk;
            unsigned a0 = 0, a1 = 0, w0, w1, idx, rgb0[3], rgb1[3];
            uint64_t aidx = 0;

            if (with_alpha) {
                a0 = blk[0];
                a1 = blk[1];
                for (int i = 0; i < 6; i++)
                    aidx |= (uint64_t)blk[2 + i] << (8 * i);
            }
            w0 = (unsigned)cb[0] | ((unsigned)cb[1] << 8);
            w1 = (unsigned)cb[2] | ((unsigned)cb[3] << 8);
            idx = (unsigned)cb[4] | ((unsigned)cb[5] << 8) |
                  ((unsigned)cb[6] << 16) | ((unsigned)cb[7] << 24);
            word_rgb(w0, rgb0);
            word_rgb(w1, rgb1);
            for (unsigned i = 0; i < 4; i++)
                for (unsigned j = 0; j < 4; j++) {
                    unsigned k = (idx >> (2 * (i * 4 + j))) & 3u;
                    uint8_t *o = out_rgba +
                        (((size_t)(by * 4 + i) * w) + bx * 4 + j) * 4;

                    o[0] = (uint8_t)color_pal(k, rgb0, rgb1, 0);
                    o[1] = (uint8_t)color_pal(k, rgb0, rgb1, 1);
                    o[2] = (uint8_t)color_pal(k, rgb0, rgb1, 2);
                    if (with_alpha) {
                        unsigned t =
                            (unsigned)((aidx >> (3 * (i * 4 + j))) & 7u);

                        o[3] = (uint8_t)alpha_pal(t, a0, a1);
                    } else {
                        /* The 3-color transparent mode (stored color0
                         * word <= color1 word, index 3) decodes alpha 0;
                         * the encoder never emits it but the decoder
                         * implements the full DXTC rule. */
                        o[3] = (w0 <= w1 && k == 3) ? 0 : 255;
                    }
                }
        }
    return 0;
}

int ac_bc1_decode(const uint8_t *blocks, size_t len, unsigned w, unsigned h,
                  uint8_t *out_rgba)
{
    return decode_blocks(blocks, len, w, h, 0, out_rgba);
}

int ac_bc3_decode(const uint8_t *blocks, size_t len, unsigned w, unsigned h,
                  uint8_t *out_rgba)
{
    return decode_blocks(blocks, len, w, h, 1, out_rgba);
}

/* --- geometry codec -------------------------------------------------------- */

void ac_mesh_quantize(const ac_mesh *m, int32_t *out_q)
{
    unsigned vc = m->vertex_count;

    for (int c = 0; c < 3; c++) {
        int32_t mn = m->positions[3 * (size_t)0 + c];
        int32_t mx = mn;

        for (unsigned i = 1; i < vc; i++) {
            int32_t v = m->positions[3 * (size_t)i + c];

            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }
        {
            int32_t range = mx - mn;

            for (unsigned i = 0; i < vc; i++) {
                int32_t v = m->positions[3 * (size_t)i + c];
                int32_t q = range ? (int32_t)(((int64_t)(v - mn) *
                                                   AC_QUANT_LEVELS +
                                               range / 2) / range)
                                  : 0;

                if (q > AC_QUANT_LEVELS)
                    q = AC_QUANT_LEVELS;
                out_q[3 * (size_t)i + c] = q;
            }
        }
    }
}

size_t ac_mesh_bound(unsigned vertex_count, unsigned index_count)
{
    /* Generous upper bound: no varint exceeds 5 bytes. */
    return 10u + 5u * 3u * (size_t)vertex_count +
           5u * (size_t)index_count;
}

size_t ac_mesh_encode(unsigned vertex_count, unsigned index_count,
                      const int32_t *qpos, const uint32_t *indices,
                      uint8_t *out, size_t out_cap)
{
    size_t n = 0;

    if (!qpos || !out || !vertex_count || (index_count % 3) ||
        (!indices && index_count))
        return 0;
    if (out_cap < ac_mesh_bound(vertex_count, index_count))
        return 0;

    n += put_varint(out + n, vertex_count);
    n += put_varint(out + n, index_count / 3);
    out[n++] = (uint8_t)AC_QUANT_BITS;
    out[n++] = 1; /* stream flags: version 1 */

    int32_t px = 0, py = 0, pz = 0;
    int32_t p2x = 0, p2y = 0, p2z = 0, p3x = 0, p3y = 0, p3z = 0;

    for (unsigned i = 0; i < vertex_count; i++) {
        int32_t x = qpos[3 * (size_t)i], y = qpos[3 * (size_t)i + 1],
                z = qpos[3 * (size_t)i + 2];
        int32_t ex, ey, ez;

        if (i == 0) {
            ex = x;
            ey = y;
            ez = z;
        } else if (i < 3) {
            ex = x - px;
            ey = y - py;
            ez = z - pz;
        } else {
            /* Parallelogram prediction through the previous three
             * vertices — exact on affine (strip) streams. */
            ex = x - (px + p2x - p3x);
            ey = y - (py + p2y - p3y);
            ez = z - (pz + p2z - p3z);
        }
        n += put_varint(out + n, zig_encode(ex));
        n += put_varint(out + n, zig_encode(ey));
        n += put_varint(out + n, zig_encode(ez));
        p3x = p2x;
        p3y = p2y;
        p3z = p2z;
        p2x = px;
        p2y = py;
        p2z = pz;
        px = x;
        py = y;
        pz = z;
    }

    uint32_t prev = 0;

    for (unsigned i = 0; i < index_count; i++) {
        uint32_t idx = indices[i];

        n += put_varint(out + n, zig_encode((int32_t)idx - (int32_t)prev));
        prev = idx;
    }
    return n;
}

int ac_mesh_decode(const uint8_t *buf, size_t len, int32_t *out_qpos,
                   uint32_t *out_indices, unsigned *out_vertex_count,
                   unsigned *out_index_count)
{
    size_t pos = 0;
    uint32_t vc, tc;

    if (!buf || !out_qpos || !out_indices || !out_vertex_count ||
        !out_index_count)
        return -1;
    if (get_varint(buf, len, &pos, &vc) ||
        get_varint(buf, len, &pos, &tc))
        return -1;
    if (pos + 2 > len || buf[pos] != (uint8_t)AC_QUANT_BITS || buf[pos + 1] != 1)
        return -1;
    pos += 2;
    if (!vc || tc > (unsigned)-1 / 3 || (uint64_t)vc * 3 > (uint64_t)-1 / 4)
        return -1;

    unsigned ic = tc * 3;
    int32_t px = 0, py = 0, pz = 0;
    int32_t p2x = 0, p2y = 0, p2z = 0, p3x = 0, p3y = 0, p3z = 0;

    for (unsigned i = 0; i < vc; i++) {
        uint32_t d;
        int32_t dx, dy, dz, x, y, z;

        if (get_varint(buf, len, &pos, &d))
            return -1;
        dx = zig_decode(d);
        if (get_varint(buf, len, &pos, &d))
            return -1;
        dy = zig_decode(d);
        if (get_varint(buf, len, &pos, &d))
            return -1;
        dz = zig_decode(d);
        if (i == 0) {
            x = dx;
            y = dy;
            z = dz;
        } else if (i < 3) {
            x = px + dx;
            y = py + dy;
            z = pz + dz;
        } else {
            x = px + p2x - p3x + dx;
            y = py + p2y - p3y + dy;
            z = pz + p2z - p3z + dz;
        }
        out_qpos[3 * (size_t)i] = x;
        out_qpos[3 * (size_t)i + 1] = y;
        out_qpos[3 * (size_t)i + 2] = z;
        p3x = p2x;
        p3y = p2y;
        p3z = p2z;
        p2x = px;
        p2y = py;
        p2z = pz;
        px = x;
        py = y;
        pz = z;
    }

    uint32_t prev = 0;

    for (unsigned i = 0; i < ic; i++) {
        uint32_t d;
        int64_t next;

        if (get_varint(buf, len, &pos, &d))
            return -1;
        next = (int64_t)prev + zig_decode(d);
        if (next < 0 || next >= (int64_t)vc)
            return -1;
        prev = (uint32_t)next;
        out_indices[i] = prev;
    }
    if (pos != len)
        return -1; /* trailing bytes are corruption */
    *out_vertex_count = vc;
    *out_index_count = ic;
    return 0;
}

/* --- corpus generator ------------------------------------------------------- */

/* Four seeded texture families cycle: 0 vertical gradient + banding,
 * 1 flat mosaic tiles, 2 concentric rings, 3 two-tone checker + diagonal
 * ramp. Alpha textures add seeded disks and a blocky checker to the
 * alpha channel (the stand-in for a UI sprite sheet). All integer. */
static void gen_texture(rng_t *rng, uint8_t *rgba, unsigned w, unsigned h,
                        unsigned with_alpha, unsigned family)
{
    unsigned base[3], alt[3];
    unsigned tile_colors[64] = { 0 };
    unsigned cx = 0, cy = 0, ndisks = 0;
    unsigned dxs[4], dys[4], rads[4];

    family %= 4; /* the 12 textures cycle the four families */

    for (int c = 0; c < 3; c++)
        base[c] = 40 + rng_below(rng, 176);
    for (int c = 0; c < 3; c++) {
        int v = (int)base[c] + (int)rng_below(rng, 121) - 60;

        alt[c] = (unsigned)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    if (family == 1)
        for (int i = 0; i < 64; i++) {
            unsigned r = 30 + rng_below(rng, 196);
            unsigned g = 30 + rng_below(rng, 196);
            unsigned b = 30 + rng_below(rng, 196);

            tile_colors[i] = (r << 16) | (g << 8) | b;
        }
    if (family == 2) {
        cx = w / 4 + rng_below(rng, w / 2);
        cy = h / 4 + rng_below(rng, h / 2);
    }
    if (with_alpha) {
        ndisks = 2 + rng_below(rng, 3);
        for (unsigned i = 0; i < ndisks; i++) {
            dxs[i] = rng_below(rng, w);
            dys[i] = rng_below(rng, h);
            rads[i] = 16 + rng_below(rng, 81);
        }
    }

    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            unsigned rgb[3];
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;

            switch (family % 4) {
            case 0:
                for (int c = 0; c < 3; c++) {
                    int v = (int)base[c] + (((int)alt[c] - (int)base[c]) *
                                            (int)y) / (int)(h - 1);

                    v += ((x / 8 + y / 8) % 2) ? 10 : -10;
                    rgb[c] = (unsigned)(v < 0 ? 0 : v > 255 ? 255 : v);
                }
                break;
            case 1: {
                /* 8x8 mosaic of 32-px tiles */
                unsigned t = tile_colors[(y / 32) * 8 + (x / 32)];

                rgb[0] = (t >> 16) & 255u;
                rgb[1] = (t >> 8) & 255u;
                rgb[2] = t & 255u;
                break;
            }
            case 2: {
                unsigned ax = x > cx ? x - cx : cx - x;
                unsigned ay = y > cy ? y - cy : cy - y;
                unsigned ring =
                    (isqrt64((uint64_t)ax * ax + (uint64_t)ay * ay) / 8) % 2;

                for (int c = 0; c < 3; c++)
                    rgb[c] = ring ? base[c] : alt[c];
                break;
            }
            default:
                for (int c = 0; c < 3; c++) {
                    int v = (int)(((x / 32) + (y / 32)) % 2 ? base[c]
                                                            : alt[c]);

                    v += (int)((x + y) / 16);
                    rgb[c] = (unsigned)(v > 255 ? 255 : v);
                }
                break;
            }

            for (int c = 0; c < 3; c++) {
                int v = (int)rgb[c] + (int)rng_below(rng, 17) - 8;

                o[c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }

            if (!with_alpha) {
                o[3] = 255;
            } else {
                int alpha = 0;

                for (unsigned i = 0; i < ndisks; i++) {
                    unsigned ax = x > dxs[i] ? x - dxs[i] : dxs[i] - x;
                    unsigned ay = y > dys[i] ? y - dys[i] : dys[i] - y;

                    if ((uint64_t)ax * ax + (uint64_t)ay * ay <=
                        (uint64_t)rads[i] * rads[i])
                        alpha = 255;
                }
                if (((x / 32) + (y / 32)) % 2)
                    alpha = 255;
                {
                    int av = alpha + (int)rng_below(rng, 9) - 4;

                    o[3] = (uint8_t)(av < 0 ? 0 : av > 255 ? 255 : av);
                }
            }
        }
}

/* Seeded triangle strips: strip s walks an affine vertex sequence
 * v_k = A_s + k * D_s (seeded origin and direction per strip, seeded
 * strip length), so the codec's parallelogram prediction is near-exact
 * the way a real strip-ordered mesh is. Winding alternates per triangle
 * like a real strip. */
static void gen_mesh(rng_t *rng, int32_t *pos, uint32_t *idx, unsigned *out_vc,
                     unsigned *out_ic)
{
    unsigned vc = 0, ic = 0;
    unsigned strips = 3 + rng_below(rng, 4); /* 3..6 */

    for (unsigned s = 0; s < strips; s++) {
        unsigned tris = 16 + rng_below(rng, 49); /* 16..64 */
        int32_t ax = (int32_t)rng_below(rng, 1u << 20) - (1 << 19);
        int32_t ay = (int32_t)rng_below(rng, 1u << 20) - (1 << 19);
        int32_t az = (int32_t)rng_below(rng, 1u << 20) - (1 << 19);
        int32_t dx = (int32_t)rng_below(rng, 4097) - 2048;
        int32_t dy = (int32_t)rng_below(rng, 4097) - 2048;
        int32_t dz = (int32_t)rng_below(rng, 4097) - 2048;
        uint32_t base = vc;

        for (unsigned k = 0; k <= tris + 1; k++) {
            pos[3 * (size_t)vc] =
                ax + (int32_t)((int64_t)k * dx);
            pos[3 * (size_t)vc + 1] =
                ay + (int32_t)((int64_t)k * dy);
            pos[3 * (size_t)vc + 2] =
                az + (int32_t)((int64_t)k * dz);
            vc++;
        }
        for (unsigned t = 0; t < tris; t++) {
            if (t % 2 == 0) {
                idx[ic++] = base + t;
                idx[ic++] = base + t + 1;
                idx[ic++] = base + t + 2;
            } else {
                idx[ic++] = base + t + 1;
                idx[ic++] = base + t;
                idx[ic++] = base + t + 2;
            }
        }
    }
    *out_vc = vc;
    *out_ic = ic;
}

/* --- context, references, accessors ------------------------------------------ */

enum { AC_CODEC_BC1 = 0, AC_CODEC_BC3, AC_CODEC_MESH };

struct ac_asset {
    int kind;
    unsigned passes;
    ac_image img;        /* textures */
    ac_mesh mesh;        /* meshes */
    int32_t *pos;        /* mesh, owned */
    uint32_t *idx;       /* mesh, owned */
};

struct ac_ctx {
    unsigned seed;
    struct ac_asset assets[AC_ASSETS];
    ac_reference refs[AC_ASSETS][2];
};

/* Which codec and effort runs (asset, pass). */
static int pass_codec(const struct ac_asset *as, unsigned pass, int *effort)
{
    *effort = AC_EFFORT_FAST;
    if (as->kind == AC_KIND_MESH)
        return AC_CODEC_MESH;
    if (as->kind == AC_KIND_TEXTURE_ALPHA)
        return AC_CODEC_BC3;
    *effort = pass ? AC_EFFORT_HIGH : AC_EFFORT_FAST;
    return AC_CODEC_BC1;
}

static uint64_t sq_error(const uint8_t *src, const uint8_t *dec, size_t px)
{
    uint64_t e = 0;

    for (size_t i = 0; i < px; i++)
        for (int c = 0; c < 3; c++) {
            int d = (int)src[i * 4 + c] - (int)dec[i * 4 + c];

            e += (uint64_t)(int64_t)d * (uint64_t)(int64_t)d;
        }
    return e;
}

/* Encode + decode + verify one asset pass. ref_out == NULL compares
 * against the stored reference (the timed path); otherwise it stores it
 * (the prepare-time pin). Returns 0 on success. */
static int run_asset(const ac_ctx *ctx, unsigned a, unsigned p,
                     ac_scratch *sc, ac_reference *ref_out)
{
    const struct ac_asset *as = &ctx->assets[a];
    const ac_reference *ref = &ctx->refs[a][p];
    int codec, effort = 0;
    char hex[41];

    codec = pass_codec(as, p, &effort);

    if (codec == AC_CODEC_MESH) {
        unsigned vc = as->mesh.vertex_count, ic = as->mesh.index_count;
        unsigned dvc, dic;
        size_t n;

        ac_mesh_quantize(&as->mesh, sc->qtmp);
        n = ac_mesh_encode(vc, ic, sc->qtmp, as->mesh.indices, sc->enc,
                           AC_ENC_BYTES);
        if (n == 0 || ac_mesh_decode(sc->enc, n, sc->qpos, sc->qidx, &dvc,
                                     &dic) != 0)
            return -1;
        if (dvc != vc || dic != ic ||
            memcmp(sc->qpos, sc->qtmp,
                   (size_t)vc * 3 * sizeof(int32_t)) != 0 ||
            memcmp(sc->qidx, as->mesh.indices,
                   (size_t)ic * sizeof(uint32_t)) != 0)
            return -1;
        sha1_hex(sc->enc, n, hex);
        if (ref_out)
            memcpy(ref_out->stream_hex, hex, 41);
        else if (memcmp(hex, ref->stream_hex, 41) != 0)
            return -1;
        sha1_hex((const uint8_t *)sc->qpos,
                 (size_t)vc * 3 * sizeof(int32_t), hex);
        if (ref_out)
            memcpy(ref_out->decoded_hex, hex, 41);
        else if (memcmp(hex, ref->decoded_hex, 41) != 0)
            return -1;
        sha1_hex((const uint8_t *)sc->qidx,
                 (size_t)ic * sizeof(uint32_t), hex);
        if (ref_out) {
            memcpy(ref_out->aux_hex, hex, 41);
            ref_out->error = 0;
        } else if (memcmp(hex, ref->aux_hex, 41) != 0) {
            return -1;
        }
        return 0;
    }

    unsigned w = as->img.w, h = as->img.h;
    size_t n;

    if (codec == AC_CODEC_BC3)
        n = ac_bc3_encode(as->img.rgba, w, h, sc->enc, AC_ENC_BYTES);
    else
        n = ac_bc1_encode(as->img.rgba, w, h, effort, sc->enc, AC_ENC_BYTES);
    if (n == 0)
        return -1;
    sha1_hex(sc->enc, n, hex);
    if (ref_out)
        memcpy(ref_out->stream_hex, hex, 41);
    else if (memcmp(hex, ref->stream_hex, 41) != 0)
        return -1;

    int rc = codec == AC_CODEC_BC3
                 ? ac_bc3_decode(sc->enc, n, w, h, sc->dec)
                 : ac_bc1_decode(sc->enc, n, w, h, sc->dec);

    if (rc != 0)
        return -1;
    uint64_t err = sq_error(as->img.rgba, sc->dec, (size_t)w * h);

    sha1_hex(sc->dec, (size_t)w * h * 4, hex);
    if (ref_out) {
        memcpy(ref_out->decoded_hex, hex, 41);
        ref_out->aux_hex[0] = '\0';
        ref_out->error = err;
    } else {
        if (memcmp(hex, ref->decoded_hex, 41) != 0)
            return -1;
        if (err != ref->error)
            return -1;
    }
    return 0;
}

ac_ctx *ac_prepare(unsigned seed)
{
    ac_ctx *ctx = calloc(1, sizeof(*ctx));
    ac_scratch sc;

    if (!ctx)
        return NULL;
    ctx->seed = seed;

    rng_t rng;

    rng_init(&rng, seed);
    for (unsigned i = 0; i < AC_TEXTURES; i++) {
        struct ac_asset *a = &ctx->assets[i];

        a->kind = AC_KIND_TEXTURE_OPAQUE;
        a->passes = 2;
        a->img.w = AC_TEX_W;
        a->img.h = AC_TEX_H;
        a->img.rgba = malloc((size_t)AC_TEX_W * AC_TEX_H * 4);
        if (!a->img.rgba)
            goto fail;
        gen_texture(&rng, (uint8_t *)a->img.rgba, AC_TEX_W, AC_TEX_H, 0, i);
    }
    for (unsigned i = 0; i < AC_ALPHA_TEXTURES; i++) {
        struct ac_asset *a = &ctx->assets[AC_TEXTURES + i];

        a->kind = AC_KIND_TEXTURE_ALPHA;
        a->passes = 1;
        a->img.w = AC_TEX_W;
        a->img.h = AC_TEX_H;
        a->img.rgba = malloc((size_t)AC_TEX_W * AC_TEX_H * 4);
        if (!a->img.rgba)
            goto fail;
        gen_texture(&rng, (uint8_t *)a->img.rgba, AC_TEX_W, AC_TEX_H, 1, i);
    }
    for (unsigned i = 0; i < AC_MESHES; i++) {
        struct ac_asset *a = &ctx->assets[AC_TEXTURES + AC_ALPHA_TEXTURES + i];

        a->kind = AC_KIND_MESH;
        a->passes = 1;
        a->pos = malloc((size_t)AC_MESH_VERT_MAX * 3 * sizeof(int32_t));
        a->idx = malloc((size_t)AC_MESH_TRI_MAX * 3 * sizeof(uint32_t));
        if (!a->pos || !a->idx)
            goto fail;
        gen_mesh(&rng, a->pos, a->idx, &a->mesh.vertex_count,
                 &a->mesh.index_count);
        a->mesh.positions = a->pos;
        a->mesh.indices = a->idx;
    }

    if (ac_scratch_init(&sc) != 0)
        goto fail;
    for (unsigned a = 0; a < AC_ASSETS; a++)
        for (unsigned p = 0; p < ctx->assets[a].passes; p++)
            if (run_asset(ctx, a, p, &sc, &ctx->refs[a][p]) != 0) {
                ac_scratch_free(&sc);
                goto fail;
            }
    ac_scratch_free(&sc);
    return ctx;

fail:
    ac_free(ctx);
    return NULL;
}

void ac_free(ac_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned i = 0; i < AC_ASSETS; i++) {
        free((void *)ctx->assets[i].img.rgba);
        free(ctx->assets[i].pos);
        free(ctx->assets[i].idx);
    }
    free(ctx);
}

unsigned ac_asset_count(const ac_ctx *ctx)
{
    return ctx ? AC_ASSETS : 0;
}

int ac_asset_kind(const ac_ctx *ctx, unsigned asset)
{
    if (!ctx || asset >= AC_ASSETS)
        return -1;
    return ctx->assets[asset].kind;
}

const ac_image *ac_asset_image(const ac_ctx *ctx, unsigned asset)
{
    if (!ctx || asset >= AC_ASSETS)
        return NULL;
    if (ctx->assets[asset].kind == AC_KIND_MESH)
        return NULL;
    return &ctx->assets[asset].img;
}

const ac_mesh *ac_asset_mesh(const ac_ctx *ctx, unsigned asset)
{
    if (!ctx || asset >= AC_ASSETS)
        return NULL;
    if (ctx->assets[asset].kind != AC_KIND_MESH)
        return NULL;
    return &ctx->assets[asset].mesh;
}

unsigned ac_pass_count(const ac_ctx *ctx, unsigned asset)
{
    if (!ctx || asset >= AC_ASSETS)
        return 0;
    return ctx->assets[asset].passes;
}

const ac_reference *ac_asset_reference(const ac_ctx *ctx, unsigned asset,
                                       unsigned pass)
{
    if (!ctx || asset >= AC_ASSETS || pass >= 2 ||
        pass >= ctx->assets[asset].passes)
        return NULL;
    return &ctx->refs[asset][pass];
}

uint64_t ac_task_bytes(const ac_ctx *ctx, unsigned multi)
{
    uint64_t total = 0;

    if (!ctx)
        return 0;
    for (unsigned a = 0; a < AC_ASSETS; a++) {
        const struct ac_asset *as = &ctx->assets[a];
        uint64_t per = 0;

        if (as->kind == AC_KIND_MESH)
            per = (uint64_t)as->mesh.vertex_count * 12 +
                  (uint64_t)as->mesh.index_count * 4;
        else if (as->kind == AC_KIND_TEXTURE_ALPHA)
            per = (uint64_t)as->img.w * as->img.h * 4;
        else
            per = (uint64_t)as->img.w * as->img.h * 3 * as->passes;
        total += per;
    }
    return multi ? total * 4 : total;
}

/* --- the timed cycle ----------------------------------------------------------- */

typedef struct {
    ac_ctx *ctx;
    unsigned begin, end; /* asset-instance range of this thread */
    ac_scratch sc;
} ac_slice;

static int ac_cycle(void *arg)
{
    ac_slice *s = arg;

    for (unsigned a = s->begin; a < s->end; a++) {
        unsigned ai = a % AC_ASSETS; /* the 4x multi set repeats the corpus */

        for (unsigned p = 0; p < s->ctx->assets[ai].passes; p++)
            if (run_asset(s->ctx, ai, p, &s->sc, NULL) != 0)
                return -1;
    }
    return 0;
}

int ac_run(ac_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mbps, uint64_t *bytes)
{
    unsigned t_count = *threads;
    unsigned set = t_count > 1 ? AC_ASSETS_MULTI : AC_ASSETS;
    ac_slice *slices = NULL;
    void **ctxs = NULL;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    int rc = -1;

    if (!ctx || !threads || !throughput_mbps || !bytes)
        return -1;
    if (t_count > set)
        t_count = set;
    if (t_count < 1)
        t_count = 1;

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;

    {
        size_t per = set / t_count, extra = set % t_count, begin = 0;

        for (unsigned t = 0; t < t_count; t++) {
            size_t count = per + (t < extra ? 1u : 0u);

            slices[t].ctx = ctx;
            slices[t].begin = begin;
            slices[t].end = begin + count;
            begin += count;
            if (ac_scratch_init(&slices[t].sc) != 0)
                goto out;
            ctxs[t] = &slices[t];
        }
    }

    rc = harness_run(t_count, runs, ac_cycle, ctxs, elapsed);
    if (rc == 0) {
        uint64_t task = ac_task_bytes(ctx, t_count > 1 ? 1u : 0u);

        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0
                           ? (double)task / elapsed[r] / 1e6
                           : -1.0;
        *throughput_mbps = lb_median(rates, runs);
        *threads = t_count;
        *bytes = task;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++)
            ac_scratch_free(&slices[t].sc);
    free(slices);
    free(ctxs);
    return rc;
}
