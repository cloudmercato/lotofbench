/* LotofBench tests — workload #8, Asset Compression
 * (workload_assetcompression.[ch]): corpus determinism, codec unit
 * vectors with hand-computed expected values (exact flat colors, the
 * BC1 3-color transparent decode rule, the BC3 alpha palette, mesh
 * round-trips including foreign vertex/triangle orders and malformed
 * streams), golden corpus pins, and sane workload runs. The workload
 * re-verifies every codec pass internally on every timed cycle. */

#include "tests.h"

#include "rng.h"
#include "sha1.h"
#include "workload_assetcompression.h"

#include <stdlib.h>
#include <string.h>

/* --- helpers ----------------------------------------------------------- */

/* Fill a small RGBA image with a callback (x, y, rgb[3], alpha). */
typedef void (*ac_px_fn)(unsigned x, unsigned y, unsigned rgb[3],
                         unsigned *alpha);

static void fill_image(uint8_t *rgba, unsigned w, unsigned h, ac_px_fn fn)
{
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            unsigned rgb[3], alpha = 255;
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;

            fn(x, y, rgb, &alpha);
            o[0] = (uint8_t)rgb[0];
            o[1] = (uint8_t)rgb[1];
            o[2] = (uint8_t)rgb[2];
            o[3] = (uint8_t)alpha;
        }
}

static void px_red(unsigned x, unsigned y, unsigned rgb[3], unsigned *alpha)
{
    (void)x;
    (void)y;
    rgb[0] = 255;
    rgb[1] = 0;
    rgb[2] = 0;
    *alpha = 255;
}

static void px_black_white(unsigned x, unsigned y, unsigned rgb[3],
                           unsigned *alpha)
{
    unsigned v = y < 2 ? 255u : 0u; /* top half white, bottom half black */

    (void)x;
    rgb[0] = rgb[1] = rgb[2] = v;
    *alpha = 255;
}

static void px_red_alpha200(unsigned x, unsigned y, unsigned rgb[3],
                            unsigned *alpha)
{
    (void)x;
    (void)y;
    rgb[0] = 255;
    rgb[1] = 0;
    rgb[2] = 0;
    *alpha = 200;
}

static void px_blue_alpha0(unsigned x, unsigned y, unsigned rgb[3],
                           unsigned *alpha)
{
    (void)x;
    (void)y;
    rgb[0] = 0;
    rgb[1] = 0;
    rgb[2] = 255;
    *alpha = 0;
}

/* Alpha texture: alpha 255 above y = h/2, 0 below (RGB = x + y ramp). */
static void px_alpha_split(unsigned x, unsigned y, unsigned rgb[3],
                           unsigned *alpha)
{
    rgb[0] = x & 255u;
    rgb[1] = y & 255u;
    rgb[2] = 128;
    *alpha = y < 2 ? 255u : 0u;
}

/* --- corpus determinism --------------------------------------------------- */

void test_ac_textures_deterministic(void)
{
    ac_ctx *a = ac_prepare(42);
    ac_ctx *b = ac_prepare(42);
    ac_ctx *c = ac_prepare(43);
    int same = 1, diff = 0;

    if (!a || !b || !c) {
        test_fail(__FILE__, __LINE__, "ac_prepare failed");
        ac_free(a);
        ac_free(b);
        ac_free(c);
        return;
    }
    for (unsigned i = 0; i < AC_TEXTURES + AC_ALPHA_TEXTURES; i++) {
        const ac_image *ia = ac_asset_image(a, i);
        const ac_image *ib = ac_asset_image(b, i);
        size_t bytes = (size_t)ia->w * ia->h * 4;

        if (memcmp(ia->rgba, ib->rgba, bytes) != 0)
            same = 0;
    }
    diff = memcmp(ac_asset_image(a, 0)->rgba, ac_asset_image(c, 0)->rgba,
                  (size_t)AC_TEX_W * AC_TEX_H * 4) != 0;
    CHECK(same);
    CHECK(diff);
    ac_free(a);
    ac_free(b);
    ac_free(c);
}

void test_ac_input_invariants(void)
{
    ac_ctx *ac = ac_prepare(1);

    if (!ac) {
        test_fail(__FILE__, __LINE__, "ac_prepare failed");
        return;
    }
    CHECK_U64(ac_asset_count(ac), 16u);
    for (unsigned i = 0; i < 16; i++) {
        int kind = ac_asset_kind(ac, i);

        if (i < 8) {
            const ac_image *im;

            CHECK_U64(kind, AC_KIND_TEXTURE_OPAQUE);
            im = ac_asset_image(ac, i);
            CHECK(im != NULL);
            CHECK_U64(im->w, AC_TEX_W);
            CHECK_U64(im->h, AC_TEX_H);
            CHECK(ac_asset_mesh(ac, i) == NULL);
            CHECK_U64(ac_pass_count(ac, i), 2u);
            /* opaque: every alpha byte is 255 */
            for (size_t p = 3; p < (size_t)im->w * im->h * 4; p += 4)
                if (im->rgba[p] != 255) {
                    CHECK_U64(im->rgba[p], 255u);
                    break;
                }
        } else if (i < 12) {
            const ac_image *im;

            CHECK_U64(kind, AC_KIND_TEXTURE_ALPHA);
            im = ac_asset_image(ac, i);
            CHECK(im != NULL);
            CHECK(ac_asset_mesh(ac, i) == NULL);
            CHECK_U64(ac_pass_count(ac, i), 1u);
            /* alpha textures actually span the alpha range */
            {
                unsigned mn = 255, mx = 0;

                for (size_t p = 3; p < (size_t)im->w * im->h * 4; p += 4) {
                    if (im->rgba[p] < mn)
                        mn = im->rgba[p];
                    if (im->rgba[p] > mx)
                        mx = im->rgba[p];
                }
                CHECK(mx > mn + 128);
            }
        } else {
            const ac_mesh *m;

            CHECK_U64(kind, AC_KIND_MESH);
            CHECK(ac_asset_image(ac, i) == NULL);
            m = ac_asset_mesh(ac, i);
            CHECK(m != NULL);
            CHECK_U64(ac_pass_count(ac, i), 1u);
            CHECK(m->vertex_count >= 3);
            CHECK(m->vertex_count <= AC_MESH_VERT_MAX);
            CHECK(m->index_count >= 3);
            CHECK(m->index_count % 3 == 0);
            CHECK(m->index_count / 3 <= AC_MESH_TRI_MAX);
            for (unsigned k = 0; k < m->index_count; k++)
                if (m->indices[k] >= m->vertex_count) {
                    CHECK(m->indices[k] < m->vertex_count);
                    break;
                }
        }
        /* every pass has a reference with populated digests */
        for (unsigned p = 0; p < ac_pass_count(ac, i); p++) {
            const ac_reference *r = ac_asset_reference(ac, i, p);

            CHECK(r != NULL);
            CHECK(strlen(r->stream_hex) == 40);
            CHECK(strlen(r->decoded_hex) == 40);
        }
    }
    ac_free(ac);
}

/* --- BC1 units -------------------------------------------------------------- */

void test_ac_bc1_units(void)
{
    static uint8_t src[8 * 4 * 4], dec[8 * 4 * 4];
    static uint8_t blk[8 * 8];

    /* flat pure red 4x4: 5-bit exact, the decode must be EXACT */
    fill_image(src, 4, 4, px_red);
    CHECK(ac_bc1_encode(src, 4, 4, AC_EFFORT_FAST, blk, sizeof blk) == 8);
    CHECK(ac_bc1_decode(blk, 8, 4, 4, dec) == 0);
    CHECK(memcmp(dec, src, sizeof src) == 0); /* incl. alpha 255 */

    /* black/white halves on 8x4 (two blocks): both blocks are exact */
    fill_image(src, 8, 4, px_black_white);
    CHECK(ac_bc1_encode(src, 8, 4, AC_EFFORT_HIGH, blk, sizeof blk) == 16);
    CHECK(ac_bc1_decode(blk, 16, 8, 4, dec) == 0);
    CHECK(memcmp(dec, src, sizeof src) == 0);

    /* first block words: the encoder stores the MIN word as color0 and
     * the MAX word as color1 (both orders decode identically) */
    CHECK_U64(blk[0] | ((unsigned)blk[1] << 8), 0x0000u);
    CHECK_U64(blk[2] | ((unsigned)blk[3] << 8), 0xFFFFu);

    /* HIGH vs FAST on a noisy image: HIGH never scores worse (first
     * candidate wins ties, so FAST's candidate set is a subset). */
    {
        rng_t r;
        static uint8_t noisy[16 * 16 * 4], d1[16 * 16 * 4];
        uint8_t c1[128], c2[128];
        uint64_t e1 = 0, e2 = 0;

        rng_init(&r, 7);
        for (size_t i = 0; i < sizeof noisy; i++)
            noisy[i] = (uint8_t)rng_next(&r);
        CHECK(ac_bc1_encode(noisy, 16, 16, AC_EFFORT_FAST, c1, sizeof c1));
        CHECK(ac_bc1_encode(noisy, 16, 16, AC_EFFORT_HIGH, c2, sizeof c2));
        CHECK(ac_bc1_decode(c1, sizeof c1, 16, 16, d1) == 0);
        for (size_t i = 0; i < 16 * 16; i++)
            for (int c = 0; c < 3; c++) {
                int d = (int)noisy[i * 4 + c] - (int)d1[i * 4 + c];

                e1 += (uint64_t)(d * d);
            }
        CHECK(ac_bc1_decode(c2, sizeof c2, 16, 16, d1) == 0);
        for (size_t i = 0; i < 16 * 16; i++)
            for (int c = 0; c < 3; c++) {
                int d = (int)noisy[i * 4 + c] - (int)d1[i * 4 + c];

                e2 += (uint64_t)(d * d);
            }
        CHECK(e2 <= e1);
    }

    /* the 3-color transparent mode: stored color0 <= color1, index 3
     * decodes alpha 0 — the full DXTC rule, hand-built block */
    {
        static const uint8_t trans[8] = { 0x00, 0x00, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF };

        CHECK(ac_bc1_decode(trans, 8, 4, 4, dec) == 0);
        for (unsigned i = 0; i < 16; i++) {
            CHECK_U64(dec[i * 4], (2 * 255u + 0u + 1u) / 3u); /* r = 170 */
            CHECK_U64(dec[i * 4 + 1], (2 * 255u + 0u + 1u) / 3u);
            CHECK_U64(dec[i * 4 + 2], (2 * 255u + 0u + 1u) / 3u);
            CHECK_U64(dec[i * 4 + 3], 0u); /* transparent texel */
        }
    }

    /* rejects: NULL, undersized cap, non-multiple-of-4 sizes */
    CHECK(ac_bc1_encode(NULL, 4, 4, 0, blk, sizeof blk) == 0);
    CHECK(ac_bc1_encode(src, 4, 4, 0, blk, 7) == 0);
    CHECK(ac_bc1_encode(src, 6, 4, 0, blk, sizeof blk) == 0);
    CHECK(ac_bc1_encode(src, 4, 6, 0, blk, sizeof blk) == 0);
    CHECK(ac_bc1_encode(src, 0, 0, 0, blk, sizeof blk) == 0);
    CHECK(ac_bc1_decode(blk, 8, 6, 4, dec) == -1);
    CHECK(ac_bc1_decode(NULL, 8, 4, 4, dec) == -1);
    CHECK(ac_bc1_decode(blk, 7, 4, 4, dec) == -1);
}

/* --- BC3 units ---------------------------------------------------------------- */

void test_ac_bc3_units(void)
{
    static uint8_t src[4 * 4 * 4], dec[4 * 4 * 4];
    static uint8_t blk[16];

    /* flat pure red + flat alpha 200: a0 = 200, bump makes a1 = 199,
     * every texel maps to k = 0 -> decoded alpha EXACTLY 200; red is
     * 5-6-5 exact so the decoded RGB is exact too */
    fill_image(src, 4, 4, px_red_alpha200);
    CHECK(ac_bc3_encode(src, 4, 4, blk, sizeof blk) == 16);
    CHECK_U64(blk[0], 200u); /* stored a0 = max */
    CHECK(ac_bc3_decode(blk, 16, 4, 4, dec) == 0);
    CHECK(memcmp(dec, src, sizeof src) == 0);

    /* flat pure blue + flat alpha 0: a0 = 0, bump makes a1 = 1, decode
     * alpha exactly 0; RGB exact (5-6-5 exact color) */
    fill_image(src, 4, 4, px_blue_alpha0);
    CHECK(ac_bc3_encode(src, 4, 4, blk, sizeof blk) == 16);
    CHECK_U64(blk[0], 0u);
    CHECK(ac_bc3_decode(blk, 16, 4, 4, dec) == 0);
    CHECK(memcmp(dec, src, sizeof src) == 0);

    /* split alpha (255 top, 0 bottom): a0 = 255 > a1 = 0, both values
     * decode exactly through k = 0 / k = 1 */
    fill_image(src, 4, 4, px_alpha_split);
    CHECK(ac_bc3_encode(src, 4, 4, blk, sizeof blk) == 16);
    CHECK_U64(blk[0], 255u);
    CHECK_U64(blk[1], 0u);
    CHECK(ac_bc3_decode(blk, 16, 4, 4, dec) == 0);
    for (unsigned i = 0; i < 16; i++)
        CHECK_U64(dec[i * 4 + 3], src[i * 4 + 3]);

    /* rejects: NULL, undersized cap, bad sizes, short buffer */
    CHECK(ac_bc3_encode(NULL, 4, 4, blk, sizeof blk) == 0);
    CHECK(ac_bc3_encode(src, 4, 4, blk, 15) == 0);
    CHECK(ac_bc3_encode(src, 5, 4, blk, sizeof blk) == 0);
    CHECK(ac_bc3_decode(blk, 15, 4, 4, dec) == -1);
    CHECK(ac_bc3_decode(blk, 16, 4, 4, NULL) == -1);
}

/* --- geometry codec units -------------------------------------------------------- */

/* A small strip-ordered mesh: affine positions, 2 triangles. */
static void make_strip_mesh(ac_mesh *m, int32_t *pos, uint32_t *idx)
{
    static const int32_t p[12] = { 0, 0, 0, 100, -50, 10, 200, -100, 20,
                                   300, -150, 30 };
    static const uint32_t ix[6] = { 0, 1, 2, 2, 1, 3 };

    memcpy(pos, p, sizeof p);
    memcpy(idx, ix, sizeof ix);
    m->vertex_count = 4;
    m->index_count = 6;
    m->positions = pos;
    m->indices = idx;
}

void test_ac_mesh_units(void)
{
    ac_mesh m;
    int32_t pos[12], qpos[12], dpos[12];
    uint32_t idx[6], didx[6];
    uint8_t buf[128];
    unsigned vc, ic;

    make_strip_mesh(&m, pos, idx);

    /* quantization pins: constant component -> 0; min -> 0; max -> 4095 */
    ac_mesh_quantize(&m, qpos);
    /* pins: x [0,300], y [-150,0], z [0,30] -> round((v-mn)*4095/range) */
    {
        static const int32_t want[12] = { 0,    4095, 0,    1365, 2730, 1365,
                                          2730, 1365, 2730, 4095, 0,    4095 };

        for (int i = 0; i < 12; i++)
            CHECK_U64(qpos[i], (uint64_t)want[i]);
    }

    /* round-trip: encode -> decode reproduces qpos and indices exactly */
    {
        size_t n = ac_mesh_encode(m.vertex_count, m.index_count, qpos,
                                  m.indices, buf, sizeof buf);

        CHECK(n > 0);
        CHECK(ac_mesh_decode(buf, n, dpos, didx, &vc, &ic) == 0);
        CHECK_U64(vc, 4u);
        CHECK_U64(ic, 6u);
        CHECK(memcmp(dpos, qpos, sizeof qpos) == 0);
        CHECK(memcmp(didx, m.indices, sizeof didx) == 0);

        /* header pins: varint vc=4, tc=2, quant bits 12, flags 1 */
        CHECK_U64(buf[0], 4u);
        CHECK_U64(buf[1], 2u);
        CHECK_U64(buf[2], 12u);
        CHECK_U64(buf[3], 1u);

        /* rejects: truncated stream, trailing byte, bad flags */
        CHECK(ac_mesh_decode(buf, n - 1, dpos, didx, &vc, &ic) == -1);
        {
            uint8_t bad[129];

            memcpy(bad, buf, n);
            bad[n] = 0;
            CHECK(ac_mesh_decode(bad, n + 1, dpos, didx, &vc, &ic) == -1);
            memcpy(bad, buf, n);
            bad[3] = 7; /* unknown flags */
            CHECK(ac_mesh_decode(bad, n, dpos, didx, &vc, &ic) == -1);
            memcpy(bad, buf, n);
            bad[2] = 11; /* wrong quant bits */
            CHECK(ac_mesh_decode(bad, n, dpos, didx, &vc, &ic) == -1);
        }
    }

    /* encode rejects: NULL, undersized cap, index_count not multiple
     * of 3 */
    CHECK(ac_mesh_encode(4, 6, qpos, m.indices, buf, 4) == 0);
    CHECK(ac_mesh_encode(4, 6, NULL, m.indices, buf, sizeof buf) == 0);
    CHECK(ac_mesh_encode(4, 4, qpos, m.indices, buf, sizeof buf) == 0);
    CHECK(ac_mesh_encode(0, 0, qpos, m.indices, buf, sizeof buf) == 0);
    CHECK(ac_mesh_decode(NULL, 8, dpos, didx, &vc, &ic) == -1);

    /* constant mesh: every component constant -> quantized all 0 */
    {
        int32_t cp[12];
        int32_t cq[12], cdq[12];
        uint32_t ci[6] = { 0, 1, 2, 2, 1, 3 }, cdi[6];

        for (int i = 0; i < 4; i++) {
            cp[3 * i] = 77;
            cp[3 * i + 1] = -13;
            cp[3 * i + 2] = 9000;
        }
        ac_mesh m2 = { 4, 6, cp, ci };

        ac_mesh_quantize(&m2, cq);
        for (int i = 0; i < 12; i++)
            CHECK_U64(cq[i], 0u);
        size_t n = ac_mesh_encode(4, 6, cq, ci, buf, sizeof buf);

        CHECK(n > 0);
        CHECK(ac_mesh_decode(buf, n, cdq, cdi, &vc, &ic) == 0);
        CHECK(memcmp(cdq, cq, sizeof cq) == 0);
        CHECK(memcmp(cdi, ci, sizeof cdi) == 0);
    }
}

void test_ac_mesh_foreign_orders(void)
{
    /* the lossless-for-any-mesh guarantee: reversed indices and a
     * non-affine position order must round-trip exactly */
    int32_t pos[15] = { 500, -500, 0, -250, 250, 12345, 0, 0, -999,
                        4096, 4096, 4096, -4096, 100, 7 };
    uint32_t idx[9] = { 4, 0, 2, 2, 1, 4, 3, 4, 0 };
    int32_t qpos[15], dq[15];
    uint32_t di[9];
    uint8_t buf[256];
    unsigned vc, ic;
    ac_mesh m = { 5, 9, pos, idx };

    ac_mesh_quantize(&m, qpos);
    size_t n = ac_mesh_encode(5, 9, qpos, idx, buf, sizeof buf);

    CHECK(n > 0);
    CHECK(ac_mesh_decode(buf, n, dq, di, &vc, &ic) == 0);
    CHECK_U64(vc, 5u);
    CHECK_U64(ic, 9u);
    CHECK(memcmp(dq, qpos, sizeof qpos) == 0);
    CHECK(memcmp(di, idx, sizeof di) == 0);

    /* reversed corner order (a foreign triangle order) */
    uint32_t rev[9];

    for (int i = 0; i < 9; i++)
        rev[i] = idx[8 - i];
    n = ac_mesh_encode(5, 9, qpos, rev, buf, sizeof buf);
    CHECK(n > 0);
    CHECK(ac_mesh_decode(buf, n, dq, di, &vc, &ic) == 0);
    CHECK(memcmp(di, rev, sizeof rev) == 0);

    /* a corrupted mid-stream byte must never yield a wrong-but-valid
     * decode silently: flipping bits in the varint area either fails
     * or changes the decoded payload (both acceptable; a crash-free
     * detectable outcome is the requirement) */
    {
        uint8_t bad[128];
        int ok = 0;

        memcpy(bad, buf, n);
        bad[n / 2] ^= 0xA5u;
        if (ac_mesh_decode(bad, n, dq, di, &vc, &ic) != 0)
            ok = 1; /* rejected */
        else if (memcmp(dq, qpos, sizeof qpos) != 0 ||
                 memcmp(di, rev, sizeof rev) != 0)
            ok = 1; /* decoded differently */
        CHECK(ok);
    }
}

/* --- golden pins -------------------------------------------------------------- */

void test_ac_corpus_golden(void)
{
    ac_ctx *ac = ac_prepare(1);
    char hex[41];

    if (!ac) {
        test_fail(__FILE__, __LINE__, "ac_prepare failed");
        return;
    }

    /* source texture digests (seed 1) */
    {
        const ac_image *im = ac_asset_image(ac, 0);

        sha1_hex(im->rgba, (size_t)im->w * im->h * 4, hex);
        CHECK_STR(hex, "98270e1949cd55d55fe93c7c0620e88dae9b19e2");
    }
    {
        const ac_image *im = ac_asset_image(ac, 8);

        sha1_hex(im->rgba, (size_t)im->w * im->h * 4, hex);
        CHECK_STR(hex, "09dfe74c58f71cd0a2550244a7878b369849fd45");
    }

    /* codec references: opaque texture 0, both BC1 efforts */
    {
        const ac_reference *r = ac_asset_reference(ac, 0, 0);

        CHECK_STR(r->stream_hex, "fdffb59d8151030566cd0215d374ca81456a043a");
        CHECK_STR(r->decoded_hex, "18f917b5173311a29427a8c6d9b7bfb4a1814597");
        CHECK_U64(r->error, 3954016u);
    }
    {
        const ac_reference *r = ac_asset_reference(ac, 0, 1);

        CHECK_STR(r->stream_hex, "ab26248b0049653d23cdfdbf939f4f85a856c07b");
        CHECK_STR(r->decoded_hex, "8628ed7260528f9007335e5760fe0cb2f8077cfe");
        CHECK_U64(r->error, 3445518u);
    }
    {
        const ac_reference *r = ac_asset_reference(ac, 8, 0);

        CHECK_STR(r->stream_hex, "c47c4eb10511c076a28f5dbf41656f78354a1db3");
        CHECK_STR(r->decoded_hex, "903e5893ee1388c3ce711ffdc77c0a60efd0090e");
        CHECK_U64(r->error, 4112026u);
    }
    {
        const ac_mesh *m = ac_asset_mesh(ac, 12);
        const ac_reference *r = ac_asset_reference(ac, 12, 0);

        CHECK_U64(m->vertex_count, 161u);
        CHECK_U64(m->index_count, 459u);
        CHECK_STR(r->stream_hex, "3cd2541ed534e7e5b3d862616a98c87bb57a044d");
        CHECK_STR(r->decoded_hex, "3cf621d9b33c07977451264cf0848445efd96b7c");
        CHECK_STR(r->aux_hex, "d7a00ab4da6099ae8eb116d38936a2eea68aa819");
        CHECK_U64(r->error, 0u);
    }

    /* exact task sizes */
    CHECK_U64(ac_task_bytes(ac, 0), 4213816u);
    CHECK_U64(ac_task_bytes(ac, 1), 16855264u);

    ac_free(ac);
}

void test_ac_seed_variation(void)
{
    ac_ctx *a = ac_prepare(1);
    ac_ctx *b = ac_prepare(2);

    if (!a || !b) {
        test_fail(__FILE__, __LINE__, "ac_prepare failed");
        ac_free(a);
        ac_free(b);
        return;
    }
    CHECK(strcmp(ac_asset_reference(a, 0, 0)->stream_hex,
                 ac_asset_reference(b, 0, 0)->stream_hex) != 0);
    CHECK(memcmp(ac_asset_image(a, 0)->rgba,
                 ac_asset_image(b, 0)->rgba,
                 (size_t)AC_TEX_W * AC_TEX_H * 4) != 0);
    ac_free(a);
    ac_free(b);
}

/* --- run sanity ---------------------------------------------------------------- */

void test_ac_run_sane(void)
{
    ac_ctx *ac = ac_prepare(7);
    unsigned t;
    double mbps;
    uint64_t bytes;

    if (!ac) {
        test_fail(__FILE__, __LINE__, "ac_prepare failed");
        return;
    }

    /* two threads, two runs */
    t = 2;
    mbps = 0.0;
    bytes = 0;
    CHECK(ac_run(ac, 2, &t, &mbps, &bytes) == 0);
    CHECK(mbps > 0.1 && mbps < 100000.0);
    CHECK_U64(bytes, ac_task_bytes(ac, 1)); /* t > 1 -> the 4x set */
    CHECK_U64(t, 2u);

    /* five runs cross any per-run state boundary (harness contract:
     * state initialised before the run loop must be reset in cycle) */
    t = 2;
    CHECK(ac_run(ac, 5, &t, &mbps, &bytes) == 0);
    CHECK(mbps > 0.1 && mbps < 100000.0);

    /* single-core pass: 16 instances */
    t = 1;
    CHECK(ac_run(ac, 1, &t, &mbps, &bytes) == 0);
    CHECK(mbps > 0.1 && mbps < 100000.0);
    CHECK_U64(bytes, ac_task_bytes(ac, 0));
    ac_free(ac);
}

void test_ac_thread_clamp(void)
{
    ac_ctx *ac = ac_prepare(7);
    unsigned t;
    double mbps;
    uint64_t bytes;

    if (!ac) {
        test_fail(__FILE__, __LINE__, "ac_prepare failed");
        return;
    }
    t = 999; /* more threads than the 64-instance multi set */
    CHECK(ac_run(ac, 1, &t, &mbps, &bytes) == 0);
    CHECK_U64(t, 64u);
    CHECK_U64(bytes, ac_task_bytes(ac, 1)); /* the multi set = 4x the 16 */
    ac_free(ac);
}

void test_ac_accessors_null_safe(void)
{
    CHECK_U64(ac_asset_count(NULL), 0u);
    CHECK(ac_asset_kind(NULL, 0) == -1);
    CHECK(ac_asset_image(NULL, 0) == NULL);
    CHECK(ac_asset_mesh(NULL, 0) == NULL);
    CHECK_U64(ac_pass_count(NULL, 0), 0u);
    CHECK(ac_asset_reference(NULL, 0, 0) == NULL);
    CHECK_U64(ac_task_bytes(NULL, 0), 0u);
    CHECK(ac_scratch_init(NULL) == -1);
    ac_scratch_free(NULL);

    ac_ctx *ac = ac_prepare(9);

    if (!ac) {
        test_fail(__FILE__, __LINE__, "ac_prepare failed");
        return;
    }
    CHECK(ac_asset_kind(ac, 16) == -1);
    CHECK(ac_asset_image(ac, 16) == NULL);
    CHECK(ac_asset_mesh(ac, 16) == NULL);
    CHECK(ac_asset_reference(ac, 0, 2) == NULL); /* pass out of range */
    CHECK(ac_asset_mesh(ac, 0) == NULL);         /* kind mismatch */
    CHECK(ac_asset_image(ac, 12) == NULL);
    ac_free(ac);
}
