 /* Unit tests — workload #11: Object Remover (pyramid inpainting). */

#include "tests.h"

#include "rng.h"
#include "sha1.h"
#include "workload_objectremover.h"

#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.16.0, seed 1). After a
 * DELIBERATE change to the corpus, the mask shapes or the fill
 * pipeline, re-run the tool and port the printed values here in the
 * same commit. */
static const char *const OR_GOLDEN_PHOTO_HEX[4] = {
    "fb1ca2ec5477962a48bcb586e2dba86c311c14df",
    "59b1c14130d0a5a3cbeec1f94354c929b97c2036",
    "9c524804101c2aaf62d1bf8e3a8f45689c797c28",
    "0798e57bd7e0a26b5b397b67e95afd469a296db3",
};

static const uint8_t OR_GOLDEN_MASK[4][20] = {
{
    0x52, 0x28, 0xf3, 0x7d, 0xdb,
    0x62, 0xa4, 0xd8, 0x36, 0x27,
    0x4f, 0x2e, 0xcf, 0x57, 0x4e,
    0x59, 0xd0, 0xe8, 0x73, 0x6c
},
{
    0x1e, 0x10, 0x1b, 0x01, 0xd6,
    0xc6, 0x52, 0x90, 0x36, 0x66,
    0xa2, 0xbf, 0xa1, 0x59, 0x5a,
    0x8f, 0xda, 0x4e, 0xf1, 0x20
},
{
    0xf8, 0xf1, 0x13, 0x1e, 0xc2,
    0xcf, 0x03, 0x3e, 0x56, 0x49,
    0x48, 0x65, 0x26, 0x70, 0x3f,
    0x44, 0xc8, 0x7c, 0xa5, 0x02
},
{
    0xaf, 0x6f, 0xdc, 0x8b, 0x9a,
    0x9d, 0x59, 0x7e, 0x21, 0xd7,
    0xed, 0x7e, 0x25, 0xa3, 0x7f,
    0x84, 0x22, 0xde, 0xf7, 0x02
}
};

static const uint8_t OR_GOLDEN_FILLED[4][20] = {
{
    0xaa, 0x10, 0x44, 0x98, 0x6e,
    0xef, 0x5c, 0x29, 0x3a, 0x09,
    0x47, 0xf9, 0x23, 0x0e, 0xd1,
    0x6a, 0x88, 0x4e, 0x04, 0x7e
},
{
    0xf9, 0x16, 0xd7, 0x0b, 0x70,
    0xb1, 0x19, 0x3c, 0xe5, 0x5c,
    0x60, 0x7f, 0xd8, 0xff, 0xad,
    0x27, 0xff, 0x49, 0x96, 0x02
},
{
    0xf0, 0x8a, 0x9f, 0xc5, 0x20,
    0x8d, 0x68, 0xc8, 0xe1, 0xf2,
    0xe9, 0x31, 0x42, 0x76, 0x3a,
    0x5e, 0xe5, 0x08, 0x40, 0x37
},
{
    0x05, 0xc4, 0x44, 0xad, 0x07,
    0x2b, 0x8c, 0x64, 0xdd, 0x2d,
    0x48, 0x5a, 0xa9, 0xc5, 0xbe,
    0x10, 0x20, 0x88, 0xed, 0x80
}
};

static const int32_t OR_GOLDEN_DISC[4][3] = {
    { 1147, 584, 72 }, { 705, 584, 80 },
    { 524, 948, 81 }, { 1202, 591, 94 },
};
static const int32_t OR_GOLDEN_RECT[4][4] = {
    { 986, 563, 1221, 644 }, { 631, 544, 795, 645 },
    { 459, 918, 682, 1007 }, { 1044, 566, 1290, 663 },
};
static const int32_t OR_GOLDEN_BOX[4][4] = {
    { 986, 512, 1221, 657 }, { 625, 504, 795, 665 },
    { 443, 867, 682, 1030 }, { 1044, 497, 1297, 686 },
};
static const int32_t OR_GOLDEN_ARCH[4] = { 0, 1, 2, 3 };
static const uint32_t OR_GOLDEN_MASK_PX[4] = {
    24791u, 22020u, 27966u, 35154u,
};
static const uint32_t OR_GOLDEN_COARSE_FILL[4] = { 3u, 2u, 1u, 4u };
#define OR_GOLDEN_ITERS            144u
#define OR_GOLDEN_TASK_PIXELS       7680000ull
#define OR_GOLDEN_TASK_PIXELS_MULTI 30720000ull

/* One shared prepare for the read-only tests (the corpus is immutable;
 * the process exits right after the suite anyway). */
static or_ctx *shared_or(void)
{
    static or_ctx *ctx;

    if (!ctx)
        ctx = or_prepare(1);
    return ctx;
}

/* Byte diff, the differential's reporter: returns the number of
 * diverging bytes and stores the first divergence's index. */
static size_t nv_diff_count(const uint8_t *a, const uint8_t *b, size_t n,
                            size_t *first)
{
    size_t bad = 0;

    *first = (size_t)-1;
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) {
            if (*first == (size_t)-1)
                *first = i;
            bad++;
        }
    return bad;
}

/* --- determinism and corpus --------------------------------------------- */

void test_or_photos_deterministic(void)
{
    or_ctx *a = or_prepare(1);
    or_ctx *b = or_prepare(1);

    CHECK(a && b);
    if (!a || !b) {
        or_free(a);
        or_free(b);
        return;
    }
    for (unsigned s = 0; s < OR_PHOTOS; s++) {
        const or_result *ra = or_reference(a, s);
        const or_result *rb = or_reference(b, s);
        or_disc da = or_mask_disc(a, s), db = or_mask_disc(b, s);
        or_rect rca = or_mask_rect(a, s), rcb = or_mask_rect(b, s);

        CHECK(ra && rb);
        CHECK(memcmp(or_photo(a, s), or_photo(b, s),
                     or_photo_len(a, s)) == 0);
        CHECK(memcmp(or_mask(a, s), or_mask(b, s),
                     (size_t)OR_PHOTO_W * OR_PHOTO_H) == 0);
        if (ra && rb) {
            CHECK(memcmp(ra->mask_digest, rb->mask_digest, 20) == 0);
            CHECK(memcmp(ra->filled_digest, rb->filled_digest, 20) == 0);
            CHECK_U64(ra->unknown, rb->unknown);
            CHECK_U64(ra->coarse_fill, rb->coarse_fill);
            CHECK_U64(ra->iters, rb->iters);
        }
        CHECK_U64(or_archetype(a, s), or_archetype(b, s));
        CHECK(da.cx == db.cx && da.cy == db.cy && da.r == db.r);
        CHECK(rca.x0 == rcb.x0 && rca.y0 == rcb.y0 &&
              rca.x1 == rcb.x1 && rca.y1 == rcb.y1);
    }
    or_free(a);
    or_free(b);
}

/* The mask, rebuilt from the exposed shapes by THIS file's own point
 * predicates (not the generator's loops). */
static int nv_in_disc(const or_disc *d, int32_t x, int32_t y)
{
    int32_t dx = x - d->cx, dy = y - d->cy;

    return dx * dx + dy * dy <= d->r * d->r;
}

static int nv_in_rect(const or_rect *rc, int32_t x, int32_t y)
{
    return x >= rc->x0 && x < rc->x1 && y >= rc->y0 && y < rc->y1;
}

void test_or_input_invariants(void)
{
    or_ctx *ctx = shared_or();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned s = 0; s < OR_PHOTOS; s++) {
        or_disc d = or_mask_disc(ctx, s);
        or_rect rc = or_mask_rect(ctx, s);
        or_box box = or_mask_box(ctx, s);
        const uint8_t *mask = or_mask(ctx, s);
        const uint8_t *photo = or_photo(ctx, s);
        uint32_t count = 0;
        int32_t bx0 = (int32_t)OR_PHOTO_W, by0 = (int32_t)OR_PHOTO_H;
        int32_t bx1 = -1, by1 = -1;
        uint8_t color[2][3];
        int have[2] = { 0, 0 };

        CHECK_U64(or_archetype(ctx, s), s);
        /* the shapes: radius band, margins, containment, overlap */
        CHECK(d.r >= (int32_t)OR_DISC_MIN && d.r <= (int32_t)OR_DISC_MAX);
        CHECK(d.cx - d.r >= (int32_t)OR_MASK_MARGIN);
        CHECK(d.cx + d.r < (int32_t)OR_PHOTO_W - (int32_t)OR_MASK_MARGIN);
        CHECK(d.cy - d.r >= (int32_t)OR_MASK_MARGIN);
        CHECK(d.cy + d.r < (int32_t)OR_PHOTO_H - (int32_t)OR_MASK_MARGIN);
        CHECK(rc.x0 >= (int32_t)OR_MASK_MARGIN);
        CHECK(rc.y0 >= (int32_t)OR_MASK_MARGIN);
        CHECK(rc.x1 <= (int32_t)OR_PHOTO_W - (int32_t)OR_MASK_MARGIN);
        CHECK(rc.y1 <= (int32_t)OR_PHOTO_H - (int32_t)OR_MASK_MARGIN);
        CHECK(rc.x0 <= d.cx && d.cx < rc.x1);
        CHECK(rc.y0 <= d.cy && d.cy < rc.y1);
        CHECK(nv_in_disc(&d, d.cx, d.cy) && nv_in_rect(&rc, d.cx, d.cy));

        /* the mask plane: rebuilt from the shapes byte for byte; the
         * box is the mask's exact bounding box */
        for (int32_t y = 0; y < (int32_t)OR_PHOTO_H; y++)
            for (int32_t x = 0; x < (int32_t)OR_PHOTO_W; x++) {
                uint8_t want = nv_in_disc(&d, x, y) ||
                               nv_in_rect(&rc, x, y);
                uint8_t got = mask[(size_t)y * OR_PHOTO_W + (size_t)x];

                CHECK_U64(got, want);
                if (want) {
                    count++;
                    if (x < bx0)
                        bx0 = x;
                    if (x + 1 > bx1)
                        bx1 = x + 1;
                    if (y < by0)
                        by0 = y;
                    if (y + 1 > by1)
                        by1 = y + 1;
                }
            }
        CHECK_U64(count, or_mask_pixels(ctx, s));
        CHECK(box.x0 == bx0 && box.y0 == by0 && box.x1 == bx1 &&
              box.y1 == by1);
        CHECK(box.x1 > box.x0 && box.y1 > box.y0);

        /* the object's checker: every masked pixel carries one flat
         * color per 16 px parity class, and the two colors differ —
         * the planted object the fill has to replace */
        for (int32_t y = box.y0; y < box.y1; y++)
            for (int32_t x = box.x0; x < box.x1; x++) {
                size_t p = (size_t)y * OR_PHOTO_W + (size_t)x;
                int cell = (int)(((x >> 4) + (y >> 4)) & 1);

                if (!mask[p])
                    continue;
                if (!have[cell]) {
                    for (int c = 0; c < 3; c++)
                        color[cell][c] = photo[p * 3 + c];
                    have[cell] = 1;
                } else {
                    for (int c = 0; c < 3; c++)
                        CHECK_U64(photo[p * 3 + c], color[cell][c]);
                }
            }
        CHECK(have[0] && have[1]);
        CHECK(color[0][0] != color[1][0] || color[0][1] != color[1][1] ||
              color[0][2] != color[1][2]);

        /* the four photos are four distinct corpora */
        if (s) {
            CHECK(memcmp(or_photo(ctx, s), or_photo(ctx, 0),
                         or_photo_len(ctx, s)) != 0);
        }
    }
}

/* --- kernel units -------------------------------------------------------- */

void test_or_push_units(void)
{
    /* 3x3 -> 2x2, mixed mask (1 = unknown): cell (0,0) means its two
     * known children {10, 40} (half-up, exact mean 25), cell (0,1)
     * keeps its single known child (the clamped second tap is
     * deduplicated), cells (1,*) have only unknown children -> masked
     * coarse cells carrying 0 */
    static const uint8_t V[9] = { 10, 20, 30, 40, 50, 60, 70, 80, 90 };
    static const uint8_t M[9] = { 0, 1, 0, 0, 1, 1, 1, 1, 1 };
    uint8_t src[27], sm[9], dst[12], dm[4];
    static const uint8_t WANT_DST[12] = {
        25, 25, 25,  30, 30, 30,
        0, 0, 0,      0, 0, 0,
    };
    static const uint8_t WANT_DM[4] = { 0, 0, 1, 1 };

    for (int i = 0; i < 9; i++) {
        src[i * 3] = src[i * 3 + 1] = src[i * 3 + 2] = V[i];
        sm[i] = M[i];
    }
    or_push_rgb(dst, dm, 2, 2, src, sm, 3, 3);
    for (int i = 0; i < 12; i++)
        CHECK_U64(dst[i], WANT_DST[i]);
    for (int i = 0; i < 4; i++)
        CHECK_U64(dm[i], WANT_DM[i]);

    /* the half-up rounding: two known children summing to an odd
     * multiple of 255 rounds up ({25, 40} -> 32.5 -> 33) */
    {
        uint8_t s2[2 * 3], m2[2] = { 0, 0 }, d2[3], dm2[1];

        s2[0] = 25; s2[1] = 25; s2[2] = 25;
        s2[3] = 40; s2[4] = 40; s2[5] = 40;
        or_push_rgb(d2, dm2, 1, 1, s2, m2, 2, 1);
        for (int c = 0; c < 3; c++)
            CHECK_U64(d2[c], 33u);
        CHECK_U64(dm2[0], 0u);
    }

    /* distinct channels: the mean is per channel (a 1x2 row, both
     * known: channels {10,11} -> 10.5 -> 11, {20,22} -> 21, {40,44}
     * -> 42) */
    {
        uint8_t s3[6] = { 10, 20, 40, 11, 22, 44 };
        uint8_t m3[2] = { 0, 0 };
        uint8_t d3[3], dm3[1];

        or_push_rgb(d3, dm3, 1, 1, s3, m3, 2, 1);
        CHECK_U64(d3[0], 11u);
        CHECK_U64(d3[1], 21u);
        CHECK_U64(d3[2], 42u);
    }

    /* the degenerate corner: a 1x1 source pushes to itself (or to a
     * masked 0 cell) */
    {
        uint8_t s4[3] = { 7, 7, 7 };
        uint8_t m4[1] = { 0 };
        uint8_t d4[3], dm4[1];

        or_push_rgb(d4, dm4, 1, 1, s4, m4, 1, 1);
        CHECK_U64(d4[0], 7u);
        CHECK_U64(dm4[0], 0u);
        m4[0] = 1;
        or_push_rgb(d4, dm4, 1, 1, s4, m4, 1, 1);
        CHECK_U64(d4[0], 0u);
        CHECK_U64(dm4[0], 1u);
    }
}

void test_or_coarse_fill_units(void)
{
    /* 3x2, mask (1 = unknown): the known cells {10, 30, 60} average
     * 100/3 = 33.33 -> 33, the three unknown cells take it, the known
     * cells keep their bytes, and *filled counts exactly 3 */
    uint8_t plane[18] = {
        10, 10, 10,  20, 20, 20,  30, 30, 30,
        40, 40, 40,  50, 50, 50,  60, 60, 60,
    };
    static const uint8_t M[6] = { 0, 1, 0, 1, 1, 0 };
    uint8_t want[18];
    uint32_t filled = 100;

    memcpy(want, plane, sizeof plane);
    for (int p = 0; p < 6; p++)
        if (M[p])
            for (int c = 0; c < 3; c++)
                want[p * 3 + c] = 33;
    CHECK(or_coarse_fill(plane, M, 3, 2, &filled) == 0);
    CHECK_U64(filled, 103u); /* the counter accumulates */
    CHECK(memcmp(plane, want, sizeof plane) == 0);

    /* a NULL counter is accepted */
    CHECK(or_coarse_fill(plane, M, 3, 2, NULL) == 0);

    /* the contract failure: an all-unknown plane refuses (and leaves
     * the bytes alone) */
    {
        uint8_t all_unknown[6] = { 1, 1, 1, 1, 1, 1 };
        uint8_t save[18];

        memcpy(save, plane, sizeof plane);
        CHECK(or_coarse_fill(plane, all_unknown, 3, 2, &filled) == -1);
        CHECK(memcmp(plane, save, sizeof plane) == 0);
    }
}

void test_or_pull_units(void)
{
    /* 2x2 -> 4x4: the up scratch receives the shared img_scale_bilinear
     * of the coarse plane (the pull must not reinvent it), the unknown
     * cells (mask != 0) take the upsampled bytes and the known cells
     * keep their planted values */
    uint8_t coarse[12], plane[48], planted[48], up[48];
    static const uint8_t M[16] = {
        1, 0, 1, 0,
        0, 1, 0, 1,
        1, 0, 1, 0,
        0, 1, 0, 1,
    };
    static const uint8_t PLANT = 200;

    for (int p = 0; p < 4; p++)
        for (int c = 0; c < 3; c++)
            coarse[p * 3 + c] = (uint8_t)(10 + 10 * p);
    for (int p = 0; p < 16; p++)
        for (int c = 0; c < 3; c++)
            planted[p * 3 + c] = PLANT;
    memcpy(plane, planted, sizeof plane);
    or_pull(plane, up, M, 4, 4, coarse, 2, 2);
    /* the convention anchor: dst(3,3) samples the clamped corner
     * (1,1) = 40 (10 + 10 * 3) */
    for (int c = 0; c < 3; c++)
        CHECK_U64(up[(3 * 4 + 3) * 3 + c], 40u);
    for (int p = 0; p < 16; p++)
        for (int c = 0; c < 3; c++) {
            if (M[p])
                CHECK_U64(plane[p * 3 + c], up[p * 3 + c]);
            else
                CHECK_U64(plane[p * 3 + c], PLANT);
        }

    /* an all-known plane is left byte-identical (the up scratch is
     * still filled — the caller owns it) */
    {
        uint8_t m2[16] = { 0 };

        memcpy(plane, planted, sizeof plane);
        or_pull(plane, up, m2, 4, 4, coarse, 2, 2);
        CHECK(memcmp(plane, planted, sizeof plane) == 0);
    }
}

void test_or_sweep_units(void)
{
    /* 3x3, two adjacent unknown cells (1 = unknown): one sweep of the
     * in-place Gauss-Seidel, hand-computed in raster order -- (1,1)
     * blends its OLD right neighbour (60), then (1,2) blends the NEW
     * (1,1) = 21 (and its clamped self as the right tap); the known
     * cells are untouched */
    uint8_t plane[27], mask[9];
    static const uint8_t V0[9] = { 10, 1, 2, 20, 50, 60, 30, 4, 40 };
    static const uint8_t M[9] = { 0, 0, 0, 0, 1, 1, 0, 0, 0 };

    for (int i = 0; i < 9; i++) {
        plane[i * 3] = plane[i * 3 + 1] = plane[i * 3 + 2] = V0[i];
        mask[i] = M[i];
    }
    or_sweep(plane, mask, 3, 3, 0, 0, 3, 3);
    for (int i = 0; i < 9; i++) {
        uint8_t want = V0[i];

        if (i == 4)
            want = 21; /* (20 + 60 + 1 + 4 + 2) >> 2 */
        if (i == 5)
            want = 31; /* (21 + 60 + 2 + 40 + 2) >> 2, the fresh (1,1) */
        for (int c = 0; c < 3; c++)
            CHECK_U64(plane[i * 3 + c], want);
    }

    /* a frame-corner hole: the clamped self-neighbours are part of the
     * average -- sweep 1: (0+5+0+7+2)>>2 = 3, sweep 2 reads its own
     * fresh 3 through both clamped taps: (3+5+3+7+2)>>2 = 5 */
    {
        uint8_t p2[12] = { 0, 0, 0, 5, 5, 5, 7, 7, 7, 9, 9, 9 };
        uint8_t m2[4] = { 1, 0, 0, 0 };

        or_sweep(p2, m2, 2, 2, 0, 0, 2, 2);
        for (int c = 0; c < 3; c++)
            CHECK_U64(p2[0 + c], 3u);
        or_sweep(p2, m2, 2, 2, 0, 0, 2, 2);
        for (int c = 0; c < 3; c++)
            CHECK_U64(p2[0 + c], 5u);
        CHECK_U64(p2[9], 9u); /* the known diagonal stays */
    }

    /* the region: cells outside [x0, x1) x [y0, y1) are not touched,
     * even unknown ones */
    {
        uint8_t p3[27], m3[9];

        for (int i = 0; i < 9; i++) {
            p3[i * 3] = p3[i * 3 + 1] = p3[i * 3 + 2] = V0[i];
            m3[i] = M[i];
        }
        or_sweep(p3, m3, 3, 3, 0, 0, 2, 2); /* column 2 excluded */
        for (int c = 0; c < 3; c++) {
            CHECK_U64(p3[4 * 3 + c], 21u); /* (1,1) inside */
            CHECK_U64(p3[5 * 3 + c], 60u); /* (1,2) outside: original */
        }
    }

    /* an all-known plane: a sweep changes nothing */
    {
        uint8_t p4[27], m4[9] = { 0 };

        for (int i = 0; i < 9; i++)
            for (int c = 0; c < 3; c++)
                p4[i * 3 + c] = V0[i];
        or_sweep(p4, m4, 3, 3, 0, 0, 3, 3);
        for (int i = 0; i < 9; i++)
            for (int c = 0; c < 3; c++)
                CHECK_U64(p4[i * 3 + c], V0[i]);
    }
}

/* --- the flat closed-form fill --------------------------------------------- */

/* A constant photo under a disc + rectangle mask: the push means, the
 * coarsest fill and the convex bilinear pull all reproduce constants
 * exactly, and the Laplace sweeps keep them -- the filled frame is the
 * photo, byte for byte, on the unknown cells too. */
void test_or_flat_fill_exact(void)
{
    or_scratch s;
    uint8_t *photo, *mask;
    or_result r;
    static const uint8_t C[3] = { 137, 92, 201 };
    or_disc d = { 800, 600, 80 };
    or_rect rc = { 720, 560, 900, 640 };
    size_t n = (size_t)OR_PHOTO_W * OR_PHOTO_H;
    uint32_t mpx = 0;

    CHECK(or_scratch_init(&s) == 0);
    photo = malloc((size_t)OR_PHOTO_W * OR_PHOTO_H * 3);
    mask = malloc(n);
    CHECK(photo && mask);
    if (!photo || !mask)
        goto out;
    for (size_t p = 0; p < n; p++) {
        photo[p * 3] = C[0];
        photo[p * 3 + 1] = C[1];
        photo[p * 3 + 2] = C[2];
        mask[p] = 0;
    }
    for (int32_t y = d.cy - d.r; y <= d.cy + d.r; y++)
        for (int32_t x = d.cx - d.r; x <= d.cx + d.r; x++)
            if ((x - d.cx) * (x - d.cx) + (y - d.cy) * (y - d.cy) <=
                d.r * d.r)
                mask[(size_t)y * OR_PHOTO_W + (size_t)x] = 1;
    for (int32_t y = rc.y0; y < rc.y1; y++)
        for (int32_t x = rc.x0; x < rc.x1; x++)
            mask[(size_t)y * OR_PHOTO_W + (size_t)x] = 1;
    for (size_t p = 0; p < n; p++)
        if (mask[p])
            mpx++;

    CHECK(or_process(photo, mask, &s, &r) == 0);
    /* the input is untouched and the filled frame is the constant
     * everywhere -- the harmonic fill of a constant boundary is the
     * constant, to the byte */
    for (size_t p = 0; p < n; p++)
        for (int c = 0; c < 3; c++) {
            CHECK_U64(photo[p * 3 + c], C[c]);
            CHECK_U64(s.pyr[p * 3 + c], C[c]);
        }
    CHECK_U64(r.unknown, mpx);
    CHECK_U64(r.iters, (unsigned)OR_ITERS * OR_LEVELS);
    CHECK(r.coarse_fill >= 1);

    /* the scratch kept the result; a second scratch reproduces the
     * same digests byte for byte */
    {
        or_scratch s2;
        or_result r2;

        CHECK(or_scratch_init(&s2) == 0);
        CHECK(or_process(photo, mask, &s2, &r2) == 0);
        CHECK(memcmp(r2.mask_digest, r.mask_digest, 20) == 0);
        CHECK(memcmp(r2.filled_digest, r.filled_digest, 20) == 0);
        or_scratch_free(&s2);
    }
out:
    free(photo);
    free(mask);
    or_scratch_free(&s);
}

/* --- the naive differential -------------------------------------------------
 * An independently written fill: own pyramid buffers (one malloc per
 * level), own push/coarse/pull/sweep loops, no shared code with the
 * module beyond the documented contracts. Every photo's filled frame
 * must match byte for byte. */

static void nv_push(uint8_t *dst, uint8_t *dm, size_t dw, size_t dh,
                    const uint8_t *src, const uint8_t *sm,
                    size_t sw, size_t sh)
{
    for (size_t y = 0; y < dh; y++)
        for (size_t x = 0; x < dw; x++) {
            size_t xs[2] = { x * 2, x * 2 + 1 < sw ? x * 2 + 1 : sw - 1 };
            size_t ys[2] = { y * 2, y * 2 + 1 < sh ? y * 2 + 1 : sh - 1 };
            size_t kx[4], ky[4], kn = 0;
            int32_t sum[3] = { 0, 0, 0 };
            size_t known = 0;

            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int dup = 0;

                    for (size_t q = 0; q < kn && !dup; q++)
                        dup = kx[q] == xs[i] && ky[q] == ys[j];
                    if (!dup) {
                        kx[kn] = xs[i];
                        ky[kn] = ys[j];
                        kn++;
                    }
                }
            for (size_t q = 0; q < kn; q++) {
                size_t p = ky[q] * sw + kx[q];

                if (sm[p])
                    continue;
                known++;
                for (int c = 0; c < 3; c++)
                    sum[c] += src[p * 3 + c];
            }
            dm[y * dw + x] = known ? 0 : 1;
            for (int c = 0; c < 3; c++)
                dst[(y * dw + x) * 3 + c] =
                    known ? (uint8_t)((2 * sum[c] + (int32_t)known) /
                                     (int32_t)(2 * known))
                          : 0;
        }
}

static int nv_coarse_fill(uint8_t *plane, const uint8_t *mask, size_t w,
                          size_t h, uint32_t *filled)
{
    size_t n = w * h;
    int32_t sum[3] = { 0, 0, 0 };
    size_t known = 0;

    for (size_t p = 0; p < n; p++)
        if (!mask[p]) {
            known++;
            for (int c = 0; c < 3; c++)
                sum[c] += plane[p * 3 + c];
        }
    if (!known)
        return -1;
    for (size_t p = 0; p < n; p++)
        if (mask[p]) {
            for (int c = 0; c < 3; c++)
                plane[p * 3 + c] =
                    (uint8_t)((2 * sum[c] + (int32_t)known) /
                              (int32_t)(2 * known));
            (*filled)++;
        }
    return 0;
}

/* The documented sampling convention of img_scale_bilinear, restated:
 * source coordinate of dst x is (x + 0.5) * sw / dw - 0.5, stepped in
 * 16.16 (an integer step of sw << 16 / dw from the half-start), the
 * neighbours edge-clamped, the blend rounded half-up at the 32.32
 * product. */
static void nv_bilinear(uint8_t *dst, size_t dw, size_t dh,
                        const uint8_t *src, size_t sw, size_t sh)
{
    int64_t stepx = (int64_t)((uint64_t)sw * 65536u / dw);
    int64_t stepy = (int64_t)((uint64_t)sh * 65536u / dh);
    int64_t sx0 = (int64_t)((uint64_t)sw * 32768u / dw) - 32768;
    int64_t sy0 = (int64_t)((uint64_t)sh * 32768u / dh) - 32768;

    for (size_t y = 0; y < dh; y++) {
        int64_t sy = sy0 + stepy * (int64_t)y;
        int64_t iy = sy >> 16;
        int64_t fy = sy - iy * 65536;
        size_t y0 = (size_t)(iy < 0 ? 0 : iy);
        size_t y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;

        for (size_t x = 0; x < dw; x++) {
            int64_t sx = sx0 + stepx * (int64_t)x;
            int64_t ix = sx >> 16;
            int64_t fx = sx - ix * 65536;
            size_t x0 = (size_t)(ix < 0 ? 0 : ix);
            size_t x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            int64_t wx0 = 65536 - fx, wx1 = fx;
            int64_t wy0 = 65536 - fy, wy1 = fy;

            for (int c = 0; c < 3; c++) {
                int64_t t = (int64_t)src[(y0 * sw + x0) * 3 + c] * wx0 +
                            (int64_t)src[(y0 * sw + x1) * 3 + c] * wx1;
                int64_t b = (int64_t)src[(y1 * sw + x0) * 3 + c] * wx0 +
                            (int64_t)src[(y1 * sw + x1) * 3 + c] * wx1;
                int64_t v = (t * wy0 + b * wy1 + ((int64_t)1 << 31)) >> 32;

                dst[(y * dw + x) * 3 + c] = (uint8_t)v;
            }
        }
    }
}

static void nv_sweep(uint8_t *plane, const uint8_t *mask, size_t w,
                     size_t h, size_t x0, size_t y0, size_t x1,
                     size_t y1)
{
    for (size_t y = y0; y < y1; y++) {
        size_t yu = y ? y - 1 : 0;
        size_t yd = y + 1 < h ? y + 1 : h - 1;

        for (size_t x = x0; x < x1; x++) {
            size_t xl = x ? x - 1 : 0;
            size_t xr = x + 1 < w ? x + 1 : w - 1;
            size_t pc = (y * w + x) * 3;

            if (!mask[y * w + x]) /* known: fixed boundary */
                continue;
            for (int c = 0; c < 3; c++)
                plane[pc + c] =
                    (uint8_t)((plane[(yu * w + x) * 3 + c] +
                               plane[(yd * w + x) * 3 + c] +
                               plane[(y * w + xl) * 3 + c] +
                               plane[(y * w + xr) * 3 + c] + 2) >> 2);
        }
    }
}

/* The whole fill, naive: returns 0 and fills *out (the counters; the
 * filled frame lands in out_filled), or -1. */
static int nv_fill(const uint8_t *photo, const uint8_t *mask,
                   uint8_t *out_filled, uint8_t *up, or_result *out)
{
    static const size_t LW[OR_LEVELS + 1] = { 1600, 800, 400, 200, 100,
                                              50, 25 };
    static const size_t LH[OR_LEVELS + 1] = { 1200, 600, 300, 150, 75,
                                              38, 19 };
    uint8_t *rgb[OR_LEVELS + 1] = { 0 };
    uint8_t *mk[OR_LEVELS + 1] = { 0 };
    int rc = -1;

    for (unsigned l = 0; l <= OR_LEVELS; l++) {
        rgb[l] = malloc(LW[l] * LH[l] * 3);
        mk[l] = malloc(LW[l] * LH[l]);
        if (!rgb[l] || !mk[l])
            goto out;
    }
    memcpy(rgb[0], photo, LW[0] * LH[0] * 3);
    memcpy(mk[0], mask, LW[0] * LH[0]);
    for (unsigned l = 0; l < OR_LEVELS; l++)
        nv_push(rgb[l + 1], mk[l + 1], LW[l + 1], LH[l + 1], rgb[l],
                mk[l], LW[l], LH[l]);
    if (nv_coarse_fill(rgb[OR_LEVELS], mk[OR_LEVELS], LW[OR_LEVELS],
                       LH[OR_LEVELS], &out->coarse_fill) != 0)
        goto out;
    for (unsigned li = OR_LEVELS; li-- > 0;) {
        size_t l = li;
        size_t bx0 = LW[l], by0 = LH[l], bx1 = 0, by1 = 0;

        for (size_t y = 0; y < LH[l]; y++)
            for (size_t x = 0; x < LW[l]; x++) {
                if (!mk[l][y * LW[l] + x])
                    continue;
                if (x < bx0)
                    bx0 = x;
                if (x + 1 > bx1)
                    bx1 = x + 1;
                if (y < by0)
                    by0 = y;
                if (y + 1 > by1)
                    by1 = y + 1;
            }
        if (bx0 == LW[l])
            continue; /* nothing reached this level */
        nv_bilinear(up, LW[l], LH[l], rgb[l + 1], LW[l + 1], LH[l + 1]);
        for (size_t p = 0; p < LW[l] * LH[l]; p++)
            if (mk[l][p])
                for (int c = 0; c < 3; c++)
                    rgb[l][p * 3 + c] = up[p * 3 + c];
        for (unsigned it = 0; it < OR_ITERS; it++)
            nv_sweep(rgb[l], mk[l], LW[l], LH[l], bx0, by0, bx1, by1);
        out->iters += OR_ITERS;
    }
    memcpy(out_filled, rgb[0], LW[0] * LH[0] * 3);
    rc = 0;
out:
    for (unsigned l = 0; l <= OR_LEVELS; l++) {
        free(rgb[l]);
        free(mk[l]);
    }
    return rc;
}

void test_or_differential_naive(void)
{
    or_ctx *ctx = shared_or();
    or_scratch s;
    uint8_t *nv_out, *up;

    CHECK(ctx);
    if (!ctx)
        return;
    nv_out = malloc((size_t)OR_PHOTO_W * OR_PHOTO_H * 3);
    up = malloc((size_t)OR_PHOTO_W * OR_PHOTO_H * 3);
    CHECK(nv_out && up);
    CHECK(or_scratch_init(&s) == 0);
    if (!nv_out || !up)
        goto out;
    for (unsigned p = 0; p < OR_PHOTOS; p++) {
        const uint8_t *photo = or_photo(ctx, p);
        const uint8_t *mask = or_mask(ctx, p);
        const or_result *ref = or_reference(ctx, p);
        or_result rmod, rnv = { { 0 }, { 0 }, 0, 0, 0 };
        size_t first;

        CHECK(ref != NULL);
        CHECK(or_process(photo, mask, &s, &rmod) == 0);
        for (size_t q = 0; q < (size_t)OR_PHOTO_W * OR_PHOTO_H; q++)
            if (mask[q])
                rnv.unknown++;
        CHECK(nv_fill(photo, mask, nv_out, up, &rnv) == 0);
        CHECK_U64(rnv.unknown, ref->unknown);
        CHECK_U64(rnv.coarse_fill, ref->coarse_fill);
        CHECK_U64(rnv.iters, ref->iters);
        {
            size_t bad = nv_diff_count(nv_out, s.pyr,
                                       (size_t)OR_PHOTO_W * OR_PHOTO_H * 3,
                                       &first);

            CHECK_U64(bad, 0);
            if (bad)
                test_fail(__FILE__, __LINE__,
                          "filled[%zu] diverges on photo %u", first, p);
        }
        CHECK_U64(rmod.unknown, ref->unknown);
        CHECK_U64(rmod.coarse_fill, ref->coarse_fill);
        CHECK_U64(rmod.iters, ref->iters);
    }
out:
    free(nv_out);
    free(up);
    or_scratch_free(&s);
}

/* --- corpus goldens and reproducibility ------------------------------------ */

void test_or_process_reproducible(void)
{
    or_ctx *ctx = shared_or();
    or_scratch sc;
    int ok;

    CHECK(ctx);
    if (!ctx)
        return;
    ok = or_scratch_init(&sc) == 0;
    CHECK(ok);
    if (!ok)
        return;
    for (unsigned s = 0; s < OR_PHOTOS; s++) {
        or_result r;
        const or_result *ref = or_reference(ctx, s);

        CHECK(ref != NULL);
        if (!ref)
            break;
        CHECK(or_process(or_photo(ctx, s), or_mask(ctx, s), &sc, &r)
              == 0);
        CHECK(memcmp(r.mask_digest, ref->mask_digest, 20) == 0);
        CHECK(memcmp(r.filled_digest, ref->filled_digest, 20) == 0);
        CHECK_U64(r.unknown, ref->unknown);
        CHECK_U64(r.coarse_fill, ref->coarse_fill);
        CHECK_U64(r.iters, ref->iters);
        {
            or_result r2;

            CHECK(or_process(or_photo(ctx, s), or_mask(ctx, s), &sc, &r2)
                  == 0);
            CHECK(memcmp(r2.filled_digest, r.filled_digest, 20) == 0);
            CHECK_U64(r2.unknown, r.unknown);
        }
    }
    or_scratch_free(&sc);
}

void test_or_seed_variation(void)
{
    or_ctx *ctx = shared_or();
    or_ctx *alt = or_prepare(2);

    CHECK(ctx && alt);
    if (!ctx || !alt) {
        or_free(alt);
        return;
    }
    for (unsigned s = 0; s < OR_PHOTOS; s++) {
        const or_result *r1 = or_reference(ctx, s);
        const or_result *r2 = or_reference(alt, s);
        or_disc d = or_mask_disc(alt, s);
        or_rect rc = or_mask_rect(alt, s);

        CHECK(r1 && r2);
        if (!r1 || !r2)
            continue;
        /* same invariants under a new seed... */
        CHECK_U64(or_archetype(alt, s), s);
        CHECK(d.r >= (int32_t)OR_DISC_MIN && d.r <= (int32_t)OR_DISC_MAX);
        CHECK(d.cx - d.r >= (int32_t)OR_MASK_MARGIN);
        CHECK(d.cx + d.r < (int32_t)OR_PHOTO_W - (int32_t)OR_MASK_MARGIN);
        CHECK(rc.x0 >= (int32_t)OR_MASK_MARGIN);
        CHECK(rc.x1 <= (int32_t)OR_PHOTO_W - (int32_t)OR_MASK_MARGIN);
        CHECK_U64(r2->unknown, or_mask_pixels(alt, s));
        CHECK(r2->unknown >= 1);
        CHECK_U64(r2->iters, OR_GOLDEN_ITERS);
        CHECK(r2->coarse_fill >= 1);
        /* ...but a different corpus: every digest moves */
        CHECK(memcmp(r1->mask_digest, r2->mask_digest, 20) != 0);
        CHECK(memcmp(r1->filled_digest, r2->filled_digest, 20) != 0);
        CHECK(memcmp(or_photo(ctx, s), or_photo(alt, s),
                     or_photo_len(ctx, s)) != 0);
        CHECK(memcmp(or_mask(ctx, s), or_mask(alt, s),
                     (size_t)OR_PHOTO_W * OR_PHOTO_H) != 0);
    }
    or_free(alt);
}

void test_or_corpus_golden(void)
{
    /* the golden pins proper: per-photo shapes, exact mask pixel
     * counts, photo/mask/filled digests, exact counters and the task
     * pixel counts */
    or_ctx *ctx = shared_or();
    char hex[41];

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned s = 0; s < OR_PHOTOS; s++) {
        const or_result *r = or_reference(ctx, s);
        or_disc d = or_mask_disc(ctx, s);
        or_rect rc = or_mask_rect(ctx, s);
        or_box b = or_mask_box(ctx, s);

        CHECK(r != NULL);
        if (!r)
            return;
        CHECK_U64(or_archetype(ctx, s), (unsigned)OR_GOLDEN_ARCH[s]);
        CHECK(d.cx == OR_GOLDEN_DISC[s][0] && d.cy == OR_GOLDEN_DISC[s][1] &&
              d.r == OR_GOLDEN_DISC[s][2]);
        CHECK(rc.x0 == OR_GOLDEN_RECT[s][0] && rc.y0 == OR_GOLDEN_RECT[s][1] &&
              rc.x1 == OR_GOLDEN_RECT[s][2] && rc.y1 == OR_GOLDEN_RECT[s][3]);
        CHECK(b.x0 == OR_GOLDEN_BOX[s][0] && b.y0 == OR_GOLDEN_BOX[s][1] &&
              b.x1 == OR_GOLDEN_BOX[s][2] && b.y1 == OR_GOLDEN_BOX[s][3]);
        CHECK_U64(or_mask_pixels(ctx, s), OR_GOLDEN_MASK_PX[s]);
        sha1_hex(or_photo(ctx, s), or_photo_len(ctx, s), hex);
        CHECK_STR(hex, OR_GOLDEN_PHOTO_HEX[s]);
        CHECK(memcmp(r->mask_digest, OR_GOLDEN_MASK[s], 20) == 0);
        CHECK(memcmp(r->filled_digest, OR_GOLDEN_FILLED[s], 20) == 0);
        CHECK_U64(r->unknown, OR_GOLDEN_MASK_PX[s]);
        CHECK_U64(r->coarse_fill, OR_GOLDEN_COARSE_FILL[s]);
        CHECK_U64(r->iters, OR_GOLDEN_ITERS);
    }
    CHECK_U64(or_task_pixels(0), OR_GOLDEN_TASK_PIXELS);
    CHECK_U64(or_task_pixels(1), OR_GOLDEN_TASK_PIXELS_MULTI);
}

/* --- run cycles ------------------------------------------------------------ */

void test_or_run_sane(void)
{
    or_ctx *ctx = shared_or();
    unsigned threads = 1;
    double mpx = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(or_run(ctx, 1, &threads, &mpx, &pixels) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(pixels, OR_GOLDEN_TASK_PIXELS);
    CHECK(mpx > 0.0);
    threads = 2;
    CHECK(or_run(ctx, 1, &threads, &mpx, &pixels) == 0);
    CHECK_U64(threads, 2);
    CHECK_U64(pixels, OR_GOLDEN_TASK_PIXELS_MULTI);
    CHECK(mpx > 0.0);
}

void test_or_thread_clamp(void)
{
    or_ctx *ctx = shared_or();
    unsigned threads = 0;
    double mpx = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* zero threads: clamped to one, the single-photo-set pass */
    CHECK(or_run(ctx, 1, &threads, &mpx, &pixels) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(pixels, OR_GOLDEN_TASK_PIXELS);
    /* more threads than photo instances: clamped to the 16-photo
     * multi set */
    threads = 999;
    CHECK(or_run(ctx, 1, &threads, &mpx, &pixels) == 0);
    CHECK_U64(threads, OR_PHOTOS_MULTI);
    CHECK_U64(pixels, OR_GOLDEN_TASK_PIXELS_MULTI);
}

void test_or_accessors_null_safe(void)
{
    or_ctx *ctx = shared_or();
    or_scratch sc;
    or_result r;
    or_disc d;
    or_rect rc;
    or_box b;

    CHECK(ctx);
    CHECK(or_photo(NULL, 0) == NULL);
    CHECK(or_photo(ctx, OR_PHOTOS) == NULL);
    CHECK(or_mask(NULL, 0) == NULL);
    CHECK(or_mask(ctx, OR_PHOTOS) == NULL);
    CHECK(or_reference(NULL, 0) == NULL);
    CHECK(or_reference(ctx, OR_PHOTOS) == NULL);
    CHECK_U64(or_photo_len(NULL, 0), 0);
    CHECK_U64(or_photo_len(ctx, 0), (size_t)OR_PHOTO_W * OR_PHOTO_H * 3);
    CHECK_U64(or_archetype(NULL, 0), 0);
    CHECK_U64(or_archetype(ctx, OR_PHOTOS), 0);
    d = or_mask_disc(NULL, 0);
    CHECK(d.cx == 0 && d.cy == 0 && d.r == 0);
    d = or_mask_disc(ctx, OR_PHOTOS);
    CHECK(d.cx == 0 && d.cy == 0 && d.r == 0);
    rc = or_mask_rect(NULL, 0);
    CHECK(rc.x0 == 0 && rc.y0 == 0 && rc.x1 == 0 && rc.y1 == 0);
    rc = or_mask_rect(ctx, OR_PHOTOS);
    CHECK(rc.x0 == 0 && rc.y0 == 0 && rc.x1 == 0 && rc.y1 == 0);
    b = or_mask_box(NULL, 0);
    CHECK(b.x0 == 0 && b.y0 == 0 && b.x1 == 0 && b.y1 == 0);
    b = or_mask_box(ctx, OR_PHOTOS);
    CHECK(b.x0 == 0 && b.y0 == 0 && b.x1 == 0 && b.y1 == 0);
    CHECK_U64(or_mask_pixels(NULL, 0), 0);
    CHECK_U64(or_mask_pixels(ctx, OR_PHOTOS), 0);
    CHECK(or_scratch_init(NULL) == -1);
    or_scratch_free(NULL);
    CHECK(or_process(NULL, or_mask(ctx, 0), &sc, &r) == -1);
    CHECK(or_process(or_photo(ctx, 0), NULL, &sc, &r) == -1);
    CHECK(or_process(or_photo(ctx, 0), or_mask(ctx, 0), NULL, &r) == -1);
    CHECK(or_process(or_photo(ctx, 0), or_mask(ctx, 0), &sc, NULL) == -1);
    CHECK(or_scratch_init(&sc) == 0);
    {
        or_scratch broken;

        memset(&broken, 0, sizeof broken);
        CHECK(or_process(or_photo(ctx, 0), or_mask(ctx, 0), &broken, &r)
              == -1);
    }
    /* a fully-known mask: nothing to fill */
    {
        uint8_t *m0 = calloc((size_t)OR_PHOTO_W * OR_PHOTO_H, 1);

        CHECK(m0);
        CHECK(or_process(or_photo(ctx, 0), m0, &sc, &r) == -1);
        free(m0);
    }
    /* a fully-unknown mask: the coarsest plane has no known cell */
    {
        uint8_t *m1 = malloc((size_t)OR_PHOTO_W * OR_PHOTO_H);

        CHECK(m1);
        memset(m1, 1, (size_t)OR_PHOTO_W * OR_PHOTO_H);
        CHECK(or_process(or_photo(ctx, 0), m1, &sc, &r) == -1);
        free(m1);
    }
    or_scratch_free(&sc);
    {
        unsigned t = 1;
        double mpx = 0.0;
        uint64_t px = 0;

        CHECK(or_run(NULL, 1, &t, &mpx, &px) == -1);
    }
}
