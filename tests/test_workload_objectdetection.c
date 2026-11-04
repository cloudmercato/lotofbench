/* Unit tests — workload #9: Object Detection. */

#include "tests.h"

#include "sha1.h"
#include "workload_objectdetection.h"

#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.13.0, seed 1). After a
 * DELIBERATE change to the corpus, the net or the decode, re-run the
 * tool and port the printed values here in the same commit. */
#define OD_GOLDEN_NET_SHA1 "3782f78c92f1f10fdbc219f25f708b6f46ec6f2a"
#define OD_GOLDEN_PHOTO0_SHA1 "d80e9341c8203c628435608a2e95c141218db933"
static const uint8_t OD_GOLDEN_FEAT[20] = {0x11, 0x90, 0x3a, 0x34, 0xda, 0x5f, 0xc0, 0x4b, 0x6b, 0x10, 0x9a, 0x4f, 0x1e, 0xe4, 0x68, 0x1b, 0xbf, 0x26, 0x2c, 0x40};
static const uint8_t OD_GOLDEN_TENS[20] = {0x93, 0xd5, 0xfb, 0x24, 0x6c, 0x70, 0xf7, 0xb8, 0x51, 0xc0, 0xed, 0xa7, 0x20, 0xdc, 0x25, 0x08, 0x89, 0xf4, 0x0c, 0x08};
static const uint8_t OD_GOLDEN_DET[20] = {0x38, 0x27, 0x78, 0x0c, 0x8f, 0x21, 0x65, 0x04, 0x11, 0xda, 0xfb, 0x5a, 0xbf, 0x0d, 0xe7, 0x7b, 0x9c, 0xf3, 0x0e, 0xf7};
#define OD_GOLDEN_MACS 316225344ull
#define OD_GOLDEN_IMAGES       8ull
#define OD_GOLDEN_IMAGES_MULTI 32ull

/* One shared prepare for the read-only tests (the corpus is immutable;
 * the process exits right after the suite anyway). */
static od_ctx *shared_od(void)
{
    static od_ctx *ctx;

    if (!ctx)
        ctx = od_prepare(1);
    return ctx;
}

/* Rec.601 luma of one RGB pixel (integer, the imgops convention). */
static unsigned luma(const uint8_t *px)
{
    return (77 * px[0] + 150 * px[1] + 29 * px[2] + 128) >> 8;
}

/* --- determinism and corpus -------------------------------------------------- */

void test_od_photos_deterministic(void)
{
    od_ctx *a = od_prepare(1), *b = od_prepare(1);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        od_free(a);
        od_free(b);
        return;
    }
    for (unsigned p = 0; p < OD_PHOTOS; p++) {
        CHECK_U64(od_photo_len(a, p), od_photo_len(b, p));
        sha1_hex(od_photo(a, p), od_photo_len(a, p), ha);
        sha1_hex(od_photo(b, p), od_photo_len(b, p), hb);
        CHECK_STR(ha, hb);
        CHECK(memcmp(od_reference(a, p), od_reference(b, p),
                     sizeof(od_result)) == 0);
    }
    od_free(a);
    od_free(b);
}

void test_od_input_invariants(void)
{
    od_ctx *ctx = shared_od();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned p = 0; p < OD_PHOTOS; p++) {
        unsigned n = od_object_count(ctx, p);

        CHECK_U64(od_photo_len(ctx, p),
                  (size_t)OD_INPUT_W * OD_INPUT_H * 3);
        CHECK_U64(od_archetype(ctx, p), p % 6); /* archetypes cycle */
        CHECK(n >= 1 && n <= 4);
        for (unsigned k = 0; k < n; k++) {
            od_bbox b = od_object_bbox(ctx, p, k);
            int32_t tone = od_object_tone(ctx, p, k);
            int32_t cy = (b.y0 + b.y1) / 2;
            const uint8_t *outp, *inn;
            int32_t step;

            CHECK(b.x0 >= 0 && b.y0 >= 0);
            CHECK(b.x1 <= (int32_t)OD_INPUT_W);
            CHECK(b.y1 <= (int32_t)OD_INPUT_H);
            CHECK(b.x1 > b.x0 && b.y1 > b.y0);
            CHECK(b.x1 - b.x0 >= 30 && b.y1 - b.y0 >= 30);
            CHECK(od_object_class(ctx, p, k) < 6);
            CHECK(tone <= -50 && tone >= -90);
            /* the planted step is real across the left edge: the luma
             * jumps by |tone| +- noise (sampled 2 px off the edge;
             * non-overlap keeps the outside sample on background) */
            outp = od_photo(ctx, p) +
                   ((size_t)cy * OD_INPUT_W + (size_t)(b.x0 - 2)) * 3;
            inn = od_photo(ctx, p) +
                  ((size_t)cy * OD_INPUT_W + (size_t)(b.x0 + 2)) * 3;
            step = (int32_t)luma(inn) - (int32_t)luma(outp);
            if (step < 0)
                step = -step;
            CHECK(step >= 30);
        }
        CHECK(od_reference(ctx, p) != NULL);
    }
}

void test_od_seed_variation(void)
{
    od_ctx *a = od_prepare(1), *b = od_prepare(2);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        od_free(a);
        od_free(b);
        return;
    }
    sha1_hex(od_photo(a, 0), od_photo_len(a, 0), ha);
    sha1_hex(od_photo(b, 0), od_photo_len(b, 0), hb);
    CHECK(strcmp(ha, hb) != 0); /* different seed => different corpus */
    od_free(a);
    od_free(b);
}

void test_od_net_seed_independent(void)
{
    od_ctx *a = od_prepare(1), *b = od_prepare(2);
    uint8_t da[20], db[20];

    CHECK(a && b);
    if (!a || !b) {
        od_free(a);
        od_free(b);
        return;
    }
    memcpy(da, od_net_digest(a), 20);
    memcpy(db, od_net_digest(b), 20);
    CHECK(memcmp(da, db, 20) == 0); /* the net does not move with seeds */
    od_free(a);
    od_free(b);
}

/* --- integer kernels ------------------------------------------------------------ */

void test_od_quantize_units(void)
{
    /* endpoints, the 127/128 midpoint (both map to 0), monotone */
    {
        uint8_t in[6] = { 0, 1, 127, 128, 255, 64 };
        int8_t out[6];

        od_quantize_input(in, 2, out);
        CHECK_U64(out[0], -127);
        CHECK_U64(out[1], -126);
        CHECK_U64(out[2], 0);
        CHECK_U64(out[3], 0);
        CHECK_U64(out[4], 127);
        CHECK_U64(out[5], -63);
    }
    {
        uint8_t in[255];
        int8_t out[255];

        for (unsigned v = 0; v < 255; v++)
            in[v] = (uint8_t)v;
        od_quantize_input(in, 85, out); /* 255 bytes = 85 px */
        for (unsigned v = 1; v < 255; v++)
            CHECK(out[v] >= out[v - 1]);
    }
    /* requantize: half-away-from-zero, ReLU, symmetric saturation */
    CHECK_U64(od_requant(0, 9, 0), 0);
    CHECK_U64(od_requant(1, 1, 0), 1);
    CHECK_U64(od_requant(2, 1, 0), 1);
    CHECK_U64(od_requant(3, 1, 0), 2);
    CHECK_U64(od_requant(-3, 1, 0), -2);
    CHECK_U64(od_requant(-1, 1, 0), -1);
    CHECK_U64(od_requant(1000000, 1, 0), 127);
    CHECK_U64(od_requant(-1000000, 1, 0), -127);
    CHECK_U64(od_requant(-5, 1, 1), 0);
    CHECK_U64(od_requant(5, 1, 1), 3);
    CHECK_U64(od_requant(2, 1, 1), 1);
}

void test_od_conv_units(void)
{
    /* pointwise identity: weights = I, out == in */
    {
        int8_t in[12] = { 1, -2, 3, 4, 5, -6, 7, 8, 9, -10, 11, 12 };
        int8_t wt[9] = { 2, 0, 0, 0, 2, 0, 0, 0, 2 };
        int8_t out[12];

        od_conv_pw1x1(in, 2, 2, 3, 3, wt, 1, 0, out);
        CHECK(memcmp(out, in, sizeof in) == 0);
    }
    /* pointwise sums: one out channel, weights all 1 (2x2x4 NHWC) */
    {
        int8_t in[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                          9, 10, 11, 12, 13, 14, 15, 16 };
        int8_t wt[4] = { 8, 8, 8, 8 };
        int8_t out[4];

        od_conv_pw1x1(in, 2, 2, 4, 1, wt, 3, 0, out);
        /* per-pixel channel sums 10, 26, 42, 58, times 8, requant >>3 */
        CHECK_U64(out[0], 10);
        CHECK_U64(out[1], 26);
        CHECK_U64(out[2], 42);
        CHECK_U64(out[3], 58);
    }
    /* depthwise identity (center tap), stride 1: out == in */
    {
        int8_t in[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        int8_t wt[18];
        int8_t out[12];

        memset(wt, 0, sizeof wt);
        wt[0 * 9 + 4] = 2;
        wt[1 * 9 + 4] = 2;
        od_conv_dw3x3(in, 3, 2, 2, wt, 1, 1, 0, out);
        CHECK(memcmp(out, in, sizeof in) == 0);
    }
    /* depthwise box sum, zero padding visible: 3x3 image, all taps 8,
     * shift 3 -> out = the sum of the in-bounds window */
    {
        int8_t in[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        int8_t wt[9], out[9];

        memset(wt, 8, sizeof wt);
        od_conv_dw3x3(in, 3, 3, 1, wt, 1, 3, 0, out);
        CHECK_U64(out[0], 12); /* 1+2+4+5 */
        CHECK_U64(out[1], 21); /* 1+2+3+4+5+6 */
        CHECK_U64(out[2], 16); /* 2+3+5+6 */
        CHECK_U64(out[3], 27); /* 1+2+4+5+7+8 */
        CHECK_U64(out[4], 45); /* all nine */
        CHECK_U64(out[5], 33); /* 2+3+5+6+8+9 */
        CHECK_U64(out[6], 24); /* 4+5+7+8 */
        CHECK_U64(out[7], 39); /* 4+5+6+7+8+9 */
        CHECK_U64(out[8], 28); /* 5+6+8+9 */
    }
    /* depthwise stride 2 on a 4x4: out 2x2, center taps sample the
     * even rows/columns */
    {
        int8_t in[16], wt[9], out[4];

        for (unsigned i = 0; i < 16; i++)
            in[i] = (int8_t)(i + 1);
        memset(wt, 0, sizeof wt);
        wt[4] = 2;
        od_conv_dw3x3(in, 4, 4, 1, wt, 2, 1, 0, out);
        CHECK_U64(out[0], in[0]);
        CHECK_U64(out[1], in[2]);
        CHECK_U64(out[2], in[8]);
        CHECK_U64(out[3], in[10]);
    }
    /* stem: channel independence + 2x2 output geometry on a 4x4 input */
    {
        int8_t in[48], wt[864], out[128]; /* 4x4x3 in, 2x2x32 out */

        memset(in, 0, sizeof in);
        memset(wt, 0, sizeof wt);
        for (unsigned i = 0; i < 16; i++)
            in[i * 3] = 1; /* channel 0 of every pixel (NHWC) */
        wt[(0 * 3 + 0) * 9 + 4] = 1; /* co=0, ci=0, center tap */
        od_conv3x3_stem(in, 4, 4, wt, 1, out);
        /* channel 0 is 1 everywhere, so every center-tap sample of
         * co=0 is 1; the 30 other channels have zero weights */
        CHECK_U64(out[0], 1);    /* (0,0) */
        CHECK_U64(out[32], 1);   /* (0,1) */
        CHECK_U64(out[64], 1);   /* (1,0) */
        CHECK_U64(out[96], 1);   /* (1,1) */
        for (unsigned co = 1; co < 32; co++) {
            CHECK_U64(out[co], 0);
            CHECK_U64(out[96 + co], 0);
        }
    }
    /* od_infer refuses nulls */
    {
        od_scratch sc;
        od_result r;

        static uint8_t photo[OD_INPUT_W * OD_INPUT_H * 3];

        CHECK(od_scratch_init(&sc) == 0);
        CHECK(od_infer(NULL, &sc, &r) == -1);
        CHECK(od_infer(photo, NULL, &r) == -1);
        CHECK(od_infer(photo, &sc, NULL) == -1);
        od_scratch_free(&sc);
    }
}

void test_od_anchor_units(void)
{
    /* the documented endpoints and centre of the 19-cell grid */
    CHECK_U64(od_anchor_q8(0), 63);
    CHECK_U64(od_anchor_q8(9), 1200);
    CHECK_U64(od_anchor_q8(18), 2337);
    /* strictly increasing, inside the frame */
    for (unsigned c = 0; c + 1 < OD_GRID; c++)
        CHECK(od_anchor_q8(c) < od_anchor_q8(c + 1));
    for (unsigned c = 0; c < OD_GRID; c++) {
        CHECK(od_anchor_q8(c) >= 0);
        CHECK(od_anchor_q8(c) <= OD_Q8_MAX);
    }
    CHECK_U64(od_anchor_q8(OD_GRID), 0); /* out of range: 0, no crash */
}

void test_od_decode_units(void)
{
    int8_t head[(size_t)OD_GRID * OD_GRID * OD_HEAD_COLS];
    uint8_t det[(size_t)OD_GRID * OD_GRID * 10];
    od_result r;
    unsigned total = 0;

    /* all zeros: class 0 everywhere (first argmax of equal logits),
     * every box at its anchor with the 8-px base square */
    memset(head, 0, sizeof head);
    memset(&r, 0xAA, sizeof r);
    od_decode_head(head, det, &r);
    for (unsigned c = 0; c < OD_CLASSES; c++)
        total += r.classes[c];
    CHECK_U64(total, OD_GRID * OD_GRID);
    CHECK_U64(r.classes[0], OD_GRID * OD_GRID);
    CHECK_U64(r.classes[1], 0);
    {
        const uint8_t *d = det; /* cell (0,0) */

        CHECK_U64((d[0] << 8) | d[1], od_anchor_q8(0));
        CHECK_U64((d[2] << 8) | d[3], od_anchor_q8(0));
        CHECK_U64((d[4] << 8) | d[5], od_anchor_q8(0) + 64);
        CHECK_U64((d[6] << 8) | d[7], od_anchor_q8(0) + 64);
        CHECK_U64(d[8], 0);
        CHECK_U64(d[9], 0);
        d = det + ((size_t)OD_GRID * OD_GRID - 1) * 10; /* cell (18,18) */
        CHECK_U64((d[0] << 8) | d[1], od_anchor_q8(18));
        CHECK_U64((d[4] << 8) | d[5], OD_Q8_MAX); /* 2337+64 -> clamp */
        CHECK_U64((d[6] << 8) | d[7], OD_Q8_MAX);
    }
    /* first-argmax tie-break and clamping: cell (2,3) gets logits
     * {5, 9, 9, 1} (tie -> class 1) and +127 deltas (clamped to the
     * frame); cell (0,0) gets -128 deltas (clamped to 0) */
    {
        int8_t *t = head + ((size_t)3 * OD_GRID + 2) * OD_HEAD_COLS;
        uint8_t *d = det + ((size_t)3 * OD_GRID + 2) * 10;

        t[0] = 5; t[1] = 9; t[2] = 9; t[3] = 1;
        t[4] = 127; t[5] = 127; t[6] = 127; t[7] = 127;
        t = head; /* cell (0,0) */
        t[0] = 1; t[1] = 2; t[2] = 3; t[3] = 4; /* class 3 wins */
        t[4] = -128; t[5] = -128; t[6] = -128; t[7] = -128;
        memset(&r, 0, sizeof r);
        od_decode_head(head, det, &r);
        d = det + ((size_t)3 * OD_GRID + 2) * 10;
        CHECK_U64(d[8], 1);
        CHECK_U64((d[0] << 8) | d[1], 2348); /* 316 + 127*16, no clamp */
        CHECK_U64((d[2] << 8) | d[3], OD_Q8_MAX);        /* y0 clamped */
        CHECK_U64((d[4] << 8) | d[5], OD_Q8_MAX);        /* x1 clamped */
        CHECK_U64((d[6] << 8) | d[7], OD_Q8_MAX);        /* y1 clamped */
        d = det; /* cell (0,0) */
        CHECK_U64(d[8], 3);
        CHECK_U64((d[0] << 8) | d[1], 0);                /* x0 clamped */
        CHECK_U64((d[2] << 8) | d[3], 0);                /* y0 clamped */
        CHECK_U64((d[4] << 8) | d[5], 0);                /* x1 clamped */
        CHECK_U64((d[6] << 8) | d[7], 0);                /* y1 clamped */
    }
}

/* --- the differential test --------------------------------------------------- */

/* An independent naive inference, plane-major (CHW) with no double
 * buffering, re-derived from the documented net: if the workload's
 * kernels ever drift (layout mixup, rounding change, wrong layer
 * order), the digests stop matching. The layer table below is the
 * test's own copy of the documented net. */

static int8_t nv_requant(int32_t acc, unsigned shift, int relu)
{
    int32_t half = (int32_t)1 << (shift - 1);
    int32_t q = acc >= 0 ? (acc + half) >> shift
                         : -((-acc + half) >> shift);

    if (relu && q < 0)
        q = 0;
    if (q > 127)
        q = 127;
    if (q < -127)
        q = -127;
    return (int8_t)q;
}

static void nv_stem(const int8_t *in, size_t w, size_t h,
                    const int8_t *wt, unsigned shift, int8_t *out)
{
    size_t ow = (w + 1) / 2, oh = (h + 1) / 2;

    for (size_t co = 0; co < 32; co++)
        for (size_t oy = 0; oy < oh; oy++)
            for (size_t ox = 0; ox < ow; ox++) {
                int32_t acc = 0;

                for (unsigned dy = 0; dy < 3; dy++)
                    for (unsigned dx = 0; dx < 3; dx++) {
                        int64_t iy = (int64_t)oy * 2 + dy - 1;
                        int64_t ix = (int64_t)ox * 2 + dx - 1;

                        if (iy < 0 || ix < 0 ||
                            iy >= (int64_t)h || ix >= (int64_t)w)
                            continue;
                        for (size_t ci = 0; ci < 3; ci++)
                            acc += (int32_t)in[ci * w * h +
                                               (size_t)iy * w +
                                               (size_t)ix] *
                                   wt[co * 27 + ci * 9 + dy * 3 + dx];
                    }
                out[co * ow * oh + oy * ow + ox] = nv_requant(acc, shift, 1);
            }
}

static void nv_dw(const int8_t *in, size_t w, size_t h, size_t c,
                  const int8_t *wt, unsigned stride, unsigned shift,
                  int8_t *out)
{
    size_t ow = (w + stride - 1) / stride, oh = (h + stride - 1) / stride;

    for (size_t ch = 0; ch < c; ch++)
        for (size_t oy = 0; oy < oh; oy++)
            for (size_t ox = 0; ox < ow; ox++) {
                int32_t acc = 0;

                for (unsigned dy = 0; dy < 3; dy++)
                    for (unsigned dx = 0; dx < 3; dx++) {
                        int64_t iy = (int64_t)oy * stride + dy - 1;
                        int64_t ix = (int64_t)ox * stride + dx - 1;

                        if (iy < 0 || ix < 0 ||
                            iy >= (int64_t)h || ix >= (int64_t)w)
                            continue;
                        acc += (int32_t)in[ch * w * h + (size_t)iy * w +
                                           (size_t)ix] *
                               wt[ch * 9 + dy * 3 + dx];
                    }
                out[ch * ow * oh + oy * ow + ox] = nv_requant(acc, shift, 1);
            }
}

static void nv_pw(const int8_t *in, size_t w, size_t h, size_t cin,
                  size_t cout, const int8_t *wt, unsigned shift, int relu,
                  int8_t *out)
{
    for (size_t co = 0; co < cout; co++)
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++) {
                int32_t acc = 0;

                for (size_t ci = 0; ci < cin; ci++)
                    acc += (int32_t)in[ci * w * h + y * w + x] *
                           wt[co * cin + ci];
                out[co * w * h + y * w + x] = nv_requant(acc, shift, relu);
            }
}

void test_od_differential_naive(void)
{
    /* the test's own layer table: kind (0 stem, 1 dw, 2 pw, 3 head),
     * out channels, stride, requant shift, relu */
    static const unsigned KIND[12] = { 0, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 3 };
    static const unsigned CIN[12] = { 3, 32, 32, 64, 64, 128,
                                      128, 256, 256, 512, 512, 512 };
    static const unsigned COUT[12] = { 32, 32, 64, 64, 128, 128,
                                       256, 256, 512, 512, 512, 8 };
    static const unsigned STR[12] = { 2, 1, 1, 2, 1, 2, 1, 2, 1, 1, 1, 1 };
    static const unsigned SHF[12] = { 7, 8, 8, 7, 8, 7, 9, 7, 10, 7, 10,
                                      10 };
    od_ctx *ctx = shared_od();
    od_scratch sc;
    od_result r;
    const int8_t *weights;
    int8_t *cur, *nxt, *feat_chw, *head_chw, *nhwc;
    uint8_t *det;
    size_t woff = 0;
    size_t w = OD_INPUT_W, h = OD_INPUT_H;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(od_scratch_init(&sc) == 0);
    CHECK(od_infer(od_photo(ctx, 0), &sc, &r) == 0);
    weights = od_net_weights();
    CHECK(weights != NULL);

    cur = malloc(150 * 150 * 64);
    nxt = malloc(150 * 150 * 64);
    feat_chw = malloc((size_t)OD_GRID * OD_GRID * 512);
    head_chw = malloc((size_t)OD_GRID * OD_GRID * OD_HEAD_COLS);
    nhwc = malloc((size_t)OD_GRID * OD_GRID * 512);
    det = malloc((size_t)OD_GRID * OD_GRID * 10);
    CHECK(cur && nxt && feat_chw && head_chw && nhwc && det);
    if (!cur || !nxt || !feat_chw || !head_chw || !nhwc || !det)
        goto out;

    /* quantize + de-interleave the input to CHW (independent formula) */
    for (size_t c = 0; c < 3; c++)
        for (size_t y = 0; y < h; y++)
            for (size_t x = 0; x < w; x++)
                cur[c * w * h + y * w + x] =
                    (int8_t)(((int32_t)od_photo(ctx, 0)
                                  [(y * w + x) * 3 + c] * 254 + 127) /
                                 255 - 127);

    for (unsigned li = 0; li < 12; li++) {
        switch (KIND[li]) {
        case 0:
            nv_stem(cur, w, h, weights + woff, SHF[li], nxt);
            woff += (size_t)COUT[li] * CIN[li] * 9;
            break;
        case 1:
            nv_dw(cur, w, h, CIN[li], weights + woff, STR[li], SHF[li],
                  nxt);
            woff += (size_t)COUT[li] * 9;
            break;
        default:
            nv_pw(cur, w, h, CIN[li], COUT[li], weights + woff, SHF[li],
                  KIND[li] == 3 ? 0 : 1,
                  KIND[li] == 3 ? head_chw : nxt);
            woff += (size_t)COUT[li] * CIN[li];
            break;
        }
        if (KIND[li] != 3) {
            int8_t *t = cur;

            cur = nxt;
            nxt = t;
            w = (w + STR[li] - 1) / STR[li];
            h = (h + STR[li] - 1) / STR[li];
        }
    }
    /* degeneracy guard: the shifts were RMS-calibrated; a future
     * over-aggressive shift would silently zero the activations and
     * keep every digest green on a vacuous net */
    {
        size_t nz = 0, tot = (size_t)512 * 361;

        for (size_t i = 0; i < tot; i++)
            if (cur[i])
                nz++;
        CHECK(nz * 10 > tot); /* > 10% nonzero activations */
        nz = 0;
        for (size_t i = 0; i < (size_t)8 * 361; i++)
            if (head_chw[i])
                nz++;
        CHECK(nz > 8 * 361 / 10); /* the head fires too */
    }

    /* cur now holds the last feature map, CHW 512 x 19 x 19 */
    for (size_t ch = 0; ch < 512; ch++)
        for (size_t y = 0; y < OD_GRID; y++)
            for (size_t x = 0; x < OD_GRID; x++)
                nhwc[(y * OD_GRID + x) * 512 + ch] =
                    (uint8_t)cur[ch * 361 + y * OD_GRID + x];
    sha1((const uint8_t *)nhwc, (size_t)OD_GRID * OD_GRID * 512,
         r.feature_digest);
    CHECK(memcmp(r.feature_digest, sc.res.feature_digest, 20) == 0);
    CHECK(memcmp(r.feature_digest,
                 od_reference(ctx, 0)->feature_digest, 20) == 0);

    for (size_t co = 0; co < 8; co++)
        for (size_t y = 0; y < OD_GRID; y++)
            for (size_t x = 0; x < OD_GRID; x++)
                nhwc[(y * OD_GRID + x) * 8 + co] =
                    (uint8_t)head_chw[co * 361 + y * OD_GRID + x];
    sha1((const uint8_t *)nhwc,
         (size_t)OD_GRID * OD_GRID * OD_HEAD_COLS,
         r.tensor_digest);
    CHECK(memcmp(r.tensor_digest, sc.res.tensor_digest, 20) == 0);

    /* and the naive decode must agree with the workload's decoded rows */
    {
        od_result rd;

        od_decode_head((const int8_t *)nhwc, det, &rd);
        CHECK(memcmp(det, sc.det, (size_t)OD_GRID * OD_GRID * 10) == 0);
        CHECK(memcmp(rd.classes, r.classes, sizeof rd.classes) == 0);
    }

out:
    free(cur);
    free(nxt);
    free(feat_chw);
    free(head_chw);
    free(nhwc);
    free(det);
    od_scratch_free(&sc);
}

/* --- golden pin, run sanity, clamping, accessors ------------------------------ */

void test_od_corpus_golden(void)
{
    od_ctx *ctx = shared_od();
    char hex[41];
    const od_result *r;

    CHECK(ctx);
    if (!ctx)
        return;
    {
        uint8_t nd[20];

        memcpy(nd, od_net_digest(ctx), 20);
        sha1_hex(nd, 20, hex);
        CHECK_STR(hex, OD_GOLDEN_NET_SHA1);
    }
    sha1_hex(od_photo(ctx, 0), od_photo_len(ctx, 0), hex);
    CHECK_STR(hex, OD_GOLDEN_PHOTO0_SHA1);
    r = od_reference(ctx, 0);
    CHECK(r != NULL);
    if (!r)
        return;
    CHECK(memcmp(r->feature_digest, OD_GOLDEN_FEAT, 20) == 0);
    CHECK(memcmp(r->tensor_digest, OD_GOLDEN_TENS, 20) == 0);
    CHECK(memcmp(r->det_digest, OD_GOLDEN_DET, 20) == 0);
    CHECK_U64(od_task_macs(), OD_GOLDEN_MACS);
    CHECK_U64(od_task_images(0), OD_GOLDEN_IMAGES);
    CHECK_U64(od_task_images(1), OD_GOLDEN_IMAGES_MULTI);
}

void test_od_run_sane(void)
{
    od_ctx *ctx = shared_od();
    unsigned threads = 1;
    double ips = 0.0;
    uint64_t images = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(od_run(ctx, 1, &threads, &ips, &images) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(images, OD_PHOTOS);
    CHECK(ips > 0.0);
}

void test_od_thread_clamp(void)
{
    od_ctx *ctx = shared_od();
    unsigned threads = 999;
    double ips = 0.0;
    uint64_t images = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* more threads than photos: clamped to the 32-photo multi-core set */
    CHECK(od_run(ctx, 1, &threads, &ips, &images) == 0);
    CHECK_U64(threads, OD_PHOTOS_MULTI);
    CHECK_U64(images, OD_PHOTOS_MULTI);
}

void test_od_accessors_null_safe(void)
{
    od_ctx *ready = shared_od(); /* build the net before the NULL checks */

    CHECK(ready != NULL);
    CHECK(od_photo(NULL, 0) == NULL);
    CHECK_U64(od_photo_len(NULL, 0), 0);
    CHECK(od_reference(NULL, 0) == NULL);
    CHECK_U64(od_archetype(NULL, 0), 0);
    CHECK_U64(od_object_count(NULL, 0), 0);
    CHECK_U64(od_object_class(NULL, 0, 0), 0);
    CHECK_U64(od_object_tone(NULL, 0, 0), 0);
    CHECK_U64(od_object_bbox(NULL, 0, 0).x1, 0);
    CHECK(od_net_digest(NULL) != NULL); /* the net exists without a ctx */
    CHECK(od_net_weights() != NULL);

    CHECK(od_photo(ready, OD_PHOTOS) == NULL);
    CHECK_U64(od_photo_len(ready, OD_PHOTOS), 0);
    CHECK(od_reference(ready, OD_PHOTOS) == NULL);
    CHECK_U64(od_object_count(ready, OD_PHOTOS), 0);
    CHECK_U64(od_object_bbox(ready, 0, 99).x1, 0); /* out-of-range object */
    CHECK_U64(od_object_class(ready, 0, 99), 0);
    CHECK_U64(od_object_tone(ready, 0, 99), 0);
}
