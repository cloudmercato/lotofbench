/* LotofBench — Object Remover workload (workload #11).
 *
 * Mirrors the eleventh Geekbench 6 CPU workload: remove a masked object
 * from a photo with content-aware fill (inpainting). LotofBench
 * reproduces the shape dependency-free, with the same honesty rules as
 * the other workloads:
 *
 *   - deterministic synthetic input — a seeded set of four photos
 *     (1600x1200) in four archetype families (beach, forest, city,
 *     studio), each an analytic integer scene — color-band gradients,
 *     a sun disc with a quadratic falloff, canopy discs, lit-window
 *     buildings, a vignette — plus one +-6 per-pixel noise draw shared
 *     by the three channels (the correlated sensor-noise stand-in).
 *     Every photo plants the mask to fill: the union of one disc
 *     (radius 64..96) and one overlapping rectangle (160-256 x 64-112),
 *     both at least 48 px inside the frame, the rectangle always
 *     containing the disc's centre row and column, so the two shapes
 *     always overlap. The bytes under the mask stand in for the object
 *     being removed — a seeded flat 16 px checker; they are never read
 *     by the fill (masked pixels are excluded at every level) and only
 *     mark the object in the corpus. Fixed --seed means byte-identical
 *     photos on every machine; generation is off the clock;
 *   - a deterministic fill pass — push-pull pyramid inpainting in 100%
 *     integer arithmetic: a 6-level pyramid whose coarse cells are the
 *     half-up mean of their distinct known children (the clamped corner
 *     taps deduplicated; masked bytes never read, at any level); the
 *     coarsest plane's unknown cells take the per-channel half-up mean
 *     of its known cells (counted in coarse_fill; an all-unknown plane
 *     is a contract failure); then coarse to fine, every level's
 *     unknown cells initialized with the shared img_scale_bilinear of
 *     the level below's filled plane and refined by OR_ITERS in-place
 *     Gauss-Seidel sweeps of the discrete 4-neighbour Laplace equation
 *     (raster order, edge-clamped neighbours, half-up
 *     (l + r + u + d + 2) >> 2) over the unknown cells only, known
 *     cells staying fixed — the Dirichlet boundary. The coarse levels
 *     carry the low-frequency fill, the per-level sweeps blend in the
 *     local boundary detail;
 *   - correctness inside the timed cycle — a pass only counts if every
 *     photo reproduces its prepare-time reference fill: SHA-1 of the
 *     mask and of the filled frame, plus the exact unknown/coarse_fill/
 *     iters counters, all bit-identical, with the degeneracy guards
 *     unknown >= 1 and coarse_fill >= 1 (every disc covers a fully
 *     masked 32x32 block at the coarsest level). Any drift aborts the
 *     run instead of scoring;
 *   - shared-task multi-core through the common harness — the
 *     multi-core pass fills 16 photo instances (4x the set) split into
 *     equal contiguous slices, one per thread.
 *
 * The throughput metric is filled output pixels: every photo
 * contributes exactly one full 1600x1200 frame, exact and
 * machine-independent, the image-workload analogue of the corpus size.
 *
 * Deliberate divergences from Geekbench 6 (all documented in
 * docs/benchmarks.md): four 1600x1200 synthetic photos instead of real
 * captures (the pipeline shape and the Mpixels/s metric are the
 * workload's), one fixed two-shape lasso mask per photo instead of
 * interactive user strokes, a documented mean-fill coarsest level plus
 * Gauss-Seidel Laplace sweeps instead of an unspecified production
 * fill stack, and synthetic seeded scenes instead of real photos.
 */

#include "workload_objectremover.h"

#include "harness.h"
#include "imgops.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <stdlib.h>
#include <string.h>

#define OR_PHOTO_PX    ((size_t)OR_PHOTO_W * OR_PHOTO_H)
#define OR_PHOTO_BYTES (OR_PHOTO_PX * 3)
#define OR_ARCHETYPES  4u

struct or_ctx {
    uint8_t  *photo[OR_PHOTOS];
    uint8_t  *mask[OR_PHOTOS];
    or_result ref[OR_PHOTOS];
    unsigned  arch[OR_PHOTOS];
    or_disc   disc[OR_PHOTOS];
    or_rect   rect[OR_PHOTOS];
    or_box    box[OR_PHOTOS];
    uint32_t  mpx[OR_PHOTOS];
};

/* --- corpus generation ------------------------------------------------------*/

/* Half-up two-stop vertical gradient over rows [0, rows), the same
 * expression as the other corpora (deterministic; the falling case
 * truncates toward zero, which is fine — the value is pinned by the
 * seed, not by a rounding claim). */
static int32_t grad_v(int32_t top, int32_t bot, int32_t y, int32_t rows)
{
    return top + ((bot - top) * y * 2 + rows) / (rows * 2);
}

/* Truncating lerp by a 0..255 weight — corpus generation only (the
 * pipeline itself uses exact half-up ops); pinned by the seed. */
static int32_t lerp8(int32_t a, int32_t b, int32_t w)
{
    return a + (b - a) * w / 255;
}

static uint8_t clamp8(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

/* Archetype 0 — beach: sky/sea/sand bands, a sun disc with a quadratic
 * falloff and two lighter wave stripes in the sea. */
static void paint_beach(uint8_t *rgb, rng_t *r)
{
    uint32_t horizon = 480 + rng_below(r, 193);
    uint32_t sea_end = horizon + 224 + rng_below(r, 129);
    uint32_t sun_cx = 1000 + rng_below(r, 401);
    uint32_t sun_cy = 96 + rng_below(r, 161);
    uint32_t rad = 48 + rng_below(r, 33);
    uint32_t rad2 = rad * rad;
    uint32_t w1y = horizon + 32 + rng_below(r, 65);
    uint32_t w2y = w1y + 24 + rng_below(r, 49);

    for (uint32_t y = 0; y < OR_PHOTO_H; y++) {
        int32_t c0, c1, c2;

        if (y < horizon) {
            c0 = grad_v(168, 210, (int32_t)y, (int32_t)horizon);
            c1 = grad_v(208, 228, (int32_t)y, (int32_t)horizon);
            c2 = grad_v(242, 246, (int32_t)y, (int32_t)horizon);
        } else if (y < sea_end) {
            int32_t rows = (int32_t)(sea_end - horizon);
            int32_t yy = (int32_t)(y - horizon);

            c0 = grad_v(32, 64, yy, rows);
            c1 = grad_v(96, 144, yy, rows);
            c2 = grad_v(128, 168, yy, rows);
        } else {
            int32_t rows = (int32_t)(OR_PHOTO_H - sea_end);
            int32_t yy = (int32_t)(y - sea_end);

            c0 = grad_v(216, 232, yy, rows);
            c1 = grad_v(196, 214, yy, rows);
            c2 = grad_v(152, 170, yy, rows);
        }
        for (uint32_t x = 0; x < OR_PHOTO_W; x++) {
            size_t i = ((size_t)y * OR_PHOTO_W + x) * 3;

            rgb[i] = (uint8_t)c0;
            rgb[i + 1] = (uint8_t)c1;
            rgb[i + 2] = (uint8_t)c2;
        }
    }
    for (int32_t dy = -(int32_t)rad; dy <= (int32_t)rad; dy++) {
        int32_t y = (int32_t)sun_cy + dy;

        if (y < 0 || y >= (int32_t)OR_PHOTO_H)
            continue;
        for (int32_t dx = -(int32_t)rad; dx <= (int32_t)rad; dx++) {
            int32_t x = (int32_t)sun_cx + dx;
            int32_t d2 = dx * dx + dy * dy;
            int32_t w;
            static const int32_t SUN[3] = { 255, 250, 220 };

            if (x < 0 || x >= (int32_t)OR_PHOTO_W || d2 > (int32_t)rad2)
                continue;
            w = ((int32_t)rad2 - d2) * 255 / (int32_t)rad2;
            for (int c = 0; c < 3; c++) {
                size_t i = ((size_t)y * OR_PHOTO_W + (size_t)x) * 3 + c;

                rgb[i] = (uint8_t)clamp8(lerp8(rgb[i], SUN[c], w));
            }
        }
    }
    for (int c = 0; c < 2; c++) {
        uint32_t wy = c ? w2y : w1y;

        for (uint32_t y = wy; y < wy + 6 && y < OR_PHOTO_H; y++)
            for (uint32_t x = 0; x < OR_PHOTO_W; x++) {
                size_t i = ((size_t)y * OR_PHOTO_W + x) * 3;

                for (int ch = 0; ch < 3; ch++)
                    rgb[i + ch] = clamp8((int32_t)rgb[i + ch] + 24);
            }
    }
}

/* Archetype 1 — forest: a green gradient, five canopy discs with a
 * quadratic falloff (alternating dark and light), a sloped dirt path
 * band. */
static void paint_forest(uint8_t *rgb, rng_t *r)
{
    static const int32_t DARK[3] = { 28, 60, 28 };
    static const int32_t LIGHT[3] = { 120, 170, 90 };

    for (uint32_t y = 0; y < OR_PHOTO_H; y++) {
        int32_t c0 = grad_v(44, 88, (int32_t)y, (int32_t)OR_PHOTO_H);
        int32_t c1 = grad_v(92, 140, (int32_t)y, (int32_t)OR_PHOTO_H);
        int32_t c2 = grad_v(40, 72, (int32_t)y, (int32_t)OR_PHOTO_H);

        for (uint32_t x = 0; x < OR_PHOTO_W; x++) {
            size_t i = ((size_t)y * OR_PHOTO_W + x) * 3;

            rgb[i] = (uint8_t)c0;
            rgb[i + 1] = (uint8_t)c1;
            rgb[i + 2] = (uint8_t)c2;
        }
    }
    for (unsigned k = 0; k < 5; k++) {
        int32_t cx = (int32_t)rng_below(r, OR_PHOTO_W);
        int32_t cy = (int32_t)rng_below(r, 897);
        int32_t rad = 96 + (int32_t)rng_below(r, 97);
        int32_t rad2 = rad * rad;
        const int32_t *t = (k & 1) ? LIGHT : DARK;
        int32_t y0 = cy - rad < 0 ? 0 : cy - rad;
        int32_t y1 = cy + rad >= (int32_t)OR_PHOTO_H
                         ? (int32_t)OR_PHOTO_H - 1 : cy + rad;

        for (int32_t y = y0; y <= y1; y++)
            for (int32_t x = cx - rad; x <= cx + rad; x++) {
                int32_t d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                int32_t w;
                size_t i;

                if (x < 0 || x >= (int32_t)OR_PHOTO_W || d2 > rad2)
                    continue;
                w = (rad2 - d2) * 255 / rad2;
                i = ((size_t)y * OR_PHOTO_W + (size_t)x) * 3;
                for (int c = 0; c < 3; c++)
                    rgb[i + c] = (uint8_t)clamp8(lerp8(rgb[i + c], t[c],
                                                       w));
            }
    }
    {
        int32_t band_y = 880 + (int32_t)rng_below(r, 161);
        int32_t slope = (int32_t)rng_below(r, 129) - 64;

        for (uint32_t x = 0; x < OR_PHOTO_W; x++) {
            int32_t ry = band_y +
                         ((((int32_t)x - (int32_t)OR_PHOTO_W / 2) * slope) >>
                          8);

            for (int32_t y = ry; y < ry + 48; y++) {
                size_t i;

                if (y < 0 || y >= (int32_t)OR_PHOTO_H)
                    continue;
                i = ((size_t)y * OR_PHOTO_W + x) * 3;
                rgb[i] = 150;
                rgb[i + 1] = 120;
                rgb[i + 2] = 80;
            }
        }
    }
}

/* Archetype 2 — city: a sky gradient, eight seeded buildings standing
 * on a ground line with lit-window lattices, an asphalt road with lane
 * dashes at the bottom. */
static void paint_city(uint8_t *rgb, rng_t *r)
{
    uint32_t gl = OR_PHOTO_H - 224; /* the ground line */

    for (uint32_t y = 0; y < OR_PHOTO_H; y++) {
        int32_t c0, c1, c2;

        if (y < gl) {
            c0 = grad_v(188, 218, (int32_t)y, (int32_t)gl);
            c1 = grad_v(196, 224, (int32_t)y, (int32_t)gl);
            c2 = grad_v(210, 232, (int32_t)y, (int32_t)gl);
        } else {
            c0 = c1 = 58;
            c2 = 62;
        }
        for (uint32_t x = 0; x < OR_PHOTO_W; x++) {
            size_t i = ((size_t)y * OR_PHOTO_W + x) * 3;

            rgb[i] = (uint8_t)c0;
            rgb[i + 1] = (uint8_t)c1;
            rgb[i + 2] = (uint8_t)c2;
        }
    }
    for (uint32_t x = 0; x < OR_PHOTO_W; x += 96) {
        uint32_t xe = x + 48 < OR_PHOTO_W ? x + 48 : OR_PHOTO_W;

        for (uint32_t y = gl + 108; y < gl + 112 && y < OR_PHOTO_H; y++)
            for (uint32_t xx = x; xx < xe; xx++) {
                size_t i = ((size_t)y * OR_PHOTO_W + xx) * 3;

                rgb[i] = 210;
                rgb[i + 1] = 210;
                rgb[i + 2] = 200;
            }
    }
    for (unsigned col = 0; col < 8; col++) {
        int32_t bw = 120 + (int32_t)rng_below(r, 81);
        int32_t bh = 160 + (int32_t)rng_below(r, 257);
        int32_t bx = (int32_t)(col * 200) + (int32_t)rng_below(r, 41);
        int32_t by = (int32_t)gl - bh;
        int32_t g = 64 + (int32_t)rng_below(r, 57);
        int32_t tint[3] = { g, clamp8(g + (int32_t)rng_below(r, 9)),
                            clamp8(g + (int32_t)rng_below(r, 13)) };

        for (int32_t y = by < 0 ? 0 : by; y < (int32_t)gl; y++)
            for (int32_t x = bx;
                 x < bx + bw && x < (int32_t)OR_PHOTO_W; x++) {
                size_t i;

                if (x < 0)
                    continue;
                i = ((size_t)y * OR_PHOTO_W + (size_t)x) * 3;
                rgb[i] = (uint8_t)tint[0];
                rgb[i + 1] = (uint8_t)tint[1];
                rgb[i + 2] = (uint8_t)tint[2];
            }
        for (int32_t wy = by + 16; wy + 12 <= (int32_t)gl - 8; wy += 28)
            for (int32_t wx = bx + 12; wx + 8 <= bx + bw - 12; wx += 24) {
                if (rng_below(r, 5) == 0)
                    continue;
                for (int32_t y = wy; y < wy + 12; y++)
                    for (int32_t x = wx; x < wx + 8; x++) {
                        size_t i;

                        if (x < 0 || x >= (int32_t)OR_PHOTO_W)
                            continue;
                        i = ((size_t)y * OR_PHOTO_W + (size_t)x) * 3;
                        rgb[i] = 232;
                        rgb[i + 1] = 214;
                        rgb[i + 2] = 140;
                    }
            }
    }
}

/* Archetype 3 — studio: a quadratic vignette around a seeded centre,
 * three warm backdrop stripes blended over it, a darkened floor band. */
static void paint_studio(uint8_t *rgb, rng_t *r)
{
    static const int32_t EDGE[3] = { 148, 144, 140 };
    static const int32_t CEN[3] = { 224, 220, 212 };
    static const int32_t STRIPE[3] = { 210, 198, 184 };
    int32_t vcx = 640 + (int32_t)rng_below(r, 321);
    int32_t vcy = 480 + (int32_t)rng_below(r, 241);
    int32_t vr = 700 + (int32_t)rng_below(r, 201);
    int32_t vr2 = vr * vr;

    for (uint32_t y = 0; y < OR_PHOTO_H; y++)
        for (uint32_t x = 0; x < OR_PHOTO_W; x++) {
            int32_t dx = (int32_t)x - vcx, dy = (int32_t)y - vcy;
            int32_t d2 = dx * dx + dy * dy;
            int32_t w = d2 < vr2 ? (vr2 - d2) * 255 / vr2 : 0;
            size_t i = ((size_t)y * OR_PHOTO_W + x) * 3;

            for (int c = 0; c < 3; c++)
                rgb[i + c] = (uint8_t)clamp8(lerp8(EDGE[c], CEN[c], w));
        }
    for (unsigned k = 0; k < 3; k++) {
        int32_t sx = 160 + (int32_t)rng_below(r, 1281);
        int32_t sw = 128 + (int32_t)rng_below(r, 97);
        int32_t xe = sx + sw >= (int32_t)OR_PHOTO_W ? (int32_t)OR_PHOTO_W
                                                    : sx + sw;

        for (uint32_t y = 0; y < OR_PHOTO_H; y++)
            for (int32_t x = sx < 0 ? 0 : sx; x < xe; x++) {
                size_t i = ((size_t)y * OR_PHOTO_W + (size_t)x) * 3;

                for (int c = 0; c < 3; c++)
                    rgb[i + c] = (uint8_t)clamp8(lerp8(rgb[i + c],
                                                       STRIPE[c], 160));
            }
    }
    for (uint32_t y = OR_PHOTO_H - 200; y < OR_PHOTO_H; y++)
        for (uint32_t x = 0; x < OR_PHOTO_W; x++) {
            size_t i = ((size_t)y * OR_PHOTO_W + x) * 3;

            for (int c = 0; c < 3; c++)
                rgb[i + c] = (uint8_t)clamp8((int32_t)rgb[i + c] - 28);
        }
}

static void (*const PAINT[OR_ARCHETYPES])(uint8_t *, rng_t *) = {
    paint_beach, paint_forest, paint_city, paint_studio
};

static void gen_photo(or_ctx *ctx, unsigned s, unsigned arch, rng_t *rng)
{
    uint8_t *rgb = ctx->photo[s];
    uint8_t *mask = ctx->mask[s];
    or_disc *d = &ctx->disc[s];
    or_rect *rc = &ctx->rect[s];
    or_box *box = &ctx->box[s];
    uint8_t obj[2][3];
    uint32_t mpx = 0;

    /* the mask shapes: a disc and a rectangle whose centre is jittered
     * around the disc's centre within a quarter of its own side, so it
     * always contains the disc's centre row and column (the two shapes
     * overlap); both shapes stay 48 px inside the frame */
    d->r = (int32_t)(OR_DISC_MIN + rng_below(rng, OR_DISC_RNG));
    d->cx = 320 + (int32_t)rng_below(rng, 961);
    d->cy = 240 + (int32_t)rng_below(rng, 721);
    {
        int32_t w = 160 + (int32_t)rng_below(rng, 97);
        int32_t h = 64 + (int32_t)rng_below(rng, 49);
        int32_t dx = (int32_t)rng_below(rng, w / 2 + 1) - w / 4;
        int32_t dy = (int32_t)rng_below(rng, h / 2 + 1) - h / 4;

        rc->x0 = d->cx + dx - w / 2;
        rc->y0 = d->cy + dy - h / 2;
        rc->x1 = rc->x0 + w;
        rc->y1 = rc->y0 + h;
    }
    /* the object's checker colors (erased by the fill, never read) */
    for (int c = 0; c < 3; c++) {
        obj[0][c] = (uint8_t)rng_below(rng, 256);
        obj[1][c] = clamp8((int32_t)obj[0][c] + 80);
    }

    PAINT[arch](rgb, rng);
    /* per-pixel scene noise: +-6, one draw shared by the three
     * channels (the correlated sensor-noise stand-in) */
    for (size_t p = 0; p < OR_PHOTO_PX; p++) {
        int32_t nz = (int32_t)rng_below(rng, 13) - 6;

        for (int c = 0; c < 3; c++)
            rgb[p * 3 + c] = clamp8((int32_t)rgb[p * 3 + c] + nz);
    }

    /* the mask: 1 = the object to remove, 0 = keep (the fill's
     * Dirichlet data). The disc first, then the rectangle — counted
     * and bounded while painted (exclusive x1/y1) */
    box->x0 = box->y0 = INT32_MAX;
    box->x1 = box->y1 = INT32_MIN;
    for (int32_t y = d->cy - d->r; y <= d->cy + d->r; y++) {
        int32_t dy = y - d->cy;

        if (y < 0 || y >= (int32_t)OR_PHOTO_H)
            continue;
        for (int32_t x = d->cx - d->r; x <= d->cx + d->r; x++) {
            int32_t dx = x - d->cx;
            size_t p;

            if (x < 0 || x >= (int32_t)OR_PHOTO_W)
                continue;
            if (dx * dx + dy * dy > d->r * d->r)
                continue;
            p = (size_t)y * OR_PHOTO_W + (size_t)x;
            if (mask[p])
                continue;
            mask[p] = 1;
            mpx++;
            if (x < box->x0)
                box->x0 = x;
            if (x + 1 > box->x1)
                box->x1 = x + 1;
            if (y < box->y0)
                box->y0 = y;
            if (y + 1 > box->y1)
                box->y1 = y + 1;
        }
    }
    for (int32_t y = rc->y0; y < rc->y1; y++)
        for (int32_t x = rc->x0; x < rc->x1; x++) {
            size_t p = (size_t)y * OR_PHOTO_W + (size_t)x;

            if (mask[p])
                continue;
            mask[p] = 1;
            mpx++;
            if (x < box->x0)
                box->x0 = x;
            if (x + 1 > box->x1)
                box->x1 = x + 1;
            if (y < box->y0)
                box->y0 = y;
            if (y + 1 > box->y1)
                box->y1 = y + 1;
        }
    ctx->mpx[s] = mpx;

    /* the object: a seeded flat 16 px checker over the mask — the fill
     * never reads these bytes (masked pixels are excluded at every
     * level); they only mark the object in the corpus */
    for (int32_t y = box->y0; y < box->y1; y++)
        for (int32_t x = box->x0; x < box->x1; x++) {
            size_t p = (size_t)y * OR_PHOTO_W + (size_t)x;
            const uint8_t *col;
            int cell;

            if (!mask[p])
                continue;
            cell = (int)(((x >> 4) + (y >> 4)) & 1);
            col = obj[cell];
            for (int c = 0; c < 3; c++)
                rgb[p * 3 + c] = col[c];
        }
}

/* --- pyramid geometry --------------------------------------------------------*/

/* Level sizes and per-plane offsets, derived once from the fixed photo
 * size (level l + 1 halves level l, half-up). pyr holds OR_LEVELS + 1
 * RGB planes (byte offsets), maskp holds OR_LEVELS + 1 mask planes. */
static struct {
    size_t w[OR_LEVELS + 1], h[OR_LEVELS + 1];
    size_t goff[OR_LEVELS + 2];
    size_t moff[OR_LEVELS + 2];
    size_t gtotal, mtotal;
} GEO;

static void geo_init(void)
{
    static int done;

    if (done)
        return;
    GEO.w[0] = OR_PHOTO_W;
    GEO.h[0] = OR_PHOTO_H;
    GEO.goff[0] = 0;
    GEO.moff[0] = 0;
    for (unsigned l = 0; l <= OR_LEVELS; l++) {
        size_t px = GEO.w[l] * GEO.h[l];

        if (l < OR_LEVELS) {
            GEO.w[l + 1] = (GEO.w[l] + 1) / 2;
            GEO.h[l + 1] = (GEO.h[l] + 1) / 2;
        }
        GEO.goff[l + 1] = GEO.goff[l] + px * 3;
        GEO.moff[l + 1] = GEO.moff[l] + px;
    }
    GEO.gtotal = GEO.goff[OR_LEVELS + 1];
    GEO.mtotal = GEO.moff[OR_LEVELS + 1];
    done = 1;
}

/* --- the integer kernels ---------------------------------------------------- */

void or_push_rgb(uint8_t *dst, uint8_t *dmask, size_t dw, size_t dh,
                 const uint8_t *src, const uint8_t *smask,
                 size_t sw, size_t sh)
{
    for (size_t y = 0; y < dh; y++) {
        size_t y0 = y * 2, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;

        for (size_t x = 0; x < dw; x++) {
            size_t x0 = x * 2, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            /* the distinct children: 2x2 inside, 2 on a clamped
             * edge, 1 in a clamped corner */
            size_t cx[4], cy[4];
            int n = 0;
            int32_t sum[3] = { 0, 0, 0 };
            int known = 0;

            cx[n] = x0;
            cy[n] = y0;
            n++;
            if (x1 != x0) {
                cx[n] = x1;
                cy[n] = y0;
                n++;
            }
            if (y1 != y0) {
                cx[n] = x0;
                cy[n] = y1;
                n++;
            }
            if (x1 != x0 && y1 != y0) {
                cx[n] = x1;
                cy[n] = y1;
                n++;
            }
            for (int k = 0; k < n; k++) {
                size_t p = cy[k] * sw + cx[k];

                if (smask[p]) /* the object: never read, at any level */
                    continue;
                known++;
                for (int c = 0; c < 3; c++)
                    sum[c] += src[p * 3 + c];
            }
            dmask[y * dw + x] = known ? 0 : 1;
            if (known)
                for (int c = 0; c < 3; c++)
                    dst[(y * dw + x) * 3 + c] =
                        (uint8_t)((2 * sum[c] + known) / (2 * known));
            else
                for (int c = 0; c < 3; c++)
                    dst[(y * dw + x) * 3 + c] = 0;
        }
    }
}

int or_coarse_fill(uint8_t *plane, const uint8_t *mask, size_t w,
                   size_t h, uint32_t *filled)
{
    int32_t sum[3] = { 0, 0, 0 };
    size_t n = w * h;
    int32_t known = 0;
    uint8_t mean[3];

    for (size_t p = 0; p < n; p++) {
        if (mask[p]) /* the unknown cells: the fill's work set */
            continue;
        known++;
        for (int c = 0; c < 3; c++)
            sum[c] += plane[p * 3 + c];
    }
    if (!known)
        return -1;
    for (int c = 0; c < 3; c++)
        mean[c] = (uint8_t)((2 * sum[c] + known) / (2 * known));
    for (size_t p = 0; p < n; p++) {
        if (!mask[p])
            continue;
        for (int c = 0; c < 3; c++)
            plane[p * 3 + c] = mean[c];
        if (filled)
            (*filled)++;
    }
    return 0;
}

void or_pull(uint8_t *plane, uint8_t *up, const uint8_t *mask,
             size_t w, size_t h, const uint8_t *coarse,
             size_t cw, size_t ch)
{
    img_scale_bilinear(up, w, h, coarse, cw, ch);
    for (size_t p = 0; p < w * h; p++) {
        if (!mask[p]) /* known: the fixed Dirichlet boundary */
            continue;
        plane[p * 3] = up[p * 3];
        plane[p * 3 + 1] = up[p * 3 + 1];
        plane[p * 3 + 2] = up[p * 3 + 2];
    }
}

void or_sweep(uint8_t *plane, const uint8_t *mask, size_t w, size_t h,
              size_t x0, size_t y0, size_t x1, size_t y1)
{
    for (size_t y = y0; y < y1; y++) {
        size_t iu = (y ? y - 1 : 0) * w;
        size_t id = (y + 1 < h ? y + 1 : h - 1) * w;
        size_t iy = y * w;

        for (size_t x = x0; x < x1; x++) {
            size_t i = iy + x;
            size_t il = iy + (x ? x - 1 : 0);
            size_t ir = iy + (x + 1 < w ? x + 1 : w - 1);
            size_t p3 = i * 3;

            if (!mask[i]) /* known: the fixed Dirichlet boundary */
                continue;
            for (int c = 0; c < 3; c++)
                plane[p3 + c] =
                    (uint8_t)((plane[iu * 3 + x * 3 + c] +
                               plane[id * 3 + x * 3 + c] +
                               plane[il * 3 + c] +
                               plane[ir * 3 + c] + 2) >> 2);
        }
    }
}

/* --- the pipeline ------------------------------------------------------------*/

/* Buffer roles: the photo and the mask (read-only inputs) are copied
 * into the pyramid's level 0; the push fills levels 1..OR_LEVELS of
 * both pyramids; the coarsest fill completes level OR_LEVELS; the pull
 * (img_scale_bilinear into s->up, then the unknown cells) and the
 * Gauss-Seidel sweeps walk levels OR_LEVELS-1..0 in place, leaving the
 * filled frame in the pyramid's level 0. The sweeps run over the
 * unknown cells' bounding box only — the only pixels they can write. */

int or_process(const uint8_t *photo, const uint8_t *mask, or_scratch *s,
               or_result *out)
{
    size_t n = OR_PHOTO_PX;
    uint32_t unknown = 0, coarse_filled = 0, iters = 0;

    if (!photo || !mask || !s || !out || !s->pyr || !s->maskp || !s->up)
        return -1;
    geo_init();
    memcpy(s->pyr, photo, OR_PHOTO_BYTES);
    memcpy(s->maskp, mask, n);
    for (size_t p = 0; p < n; p++)
        if (mask[p])
            unknown++;
    sha1(s->maskp, n, out->mask_digest);
    if (unknown == 0) /* degenerate: nothing to fill */
        return -1;

    for (unsigned l = 0; l < OR_LEVELS; l++)
        or_push_rgb(s->pyr + GEO.goff[l + 1], s->maskp + GEO.moff[l + 1],
                    GEO.w[l + 1], GEO.h[l + 1], s->pyr + GEO.goff[l],
                    s->maskp + GEO.moff[l], GEO.w[l], GEO.h[l]);

    if (or_coarse_fill(s->pyr + GEO.goff[OR_LEVELS],
                       s->maskp + GEO.moff[OR_LEVELS], GEO.w[OR_LEVELS],
                       GEO.h[OR_LEVELS], &coarse_filled) != 0)
        return -1; /* degenerate: an all-unknown coarsest plane */

    for (unsigned l = OR_LEVELS; l-- > 0;) {
        uint8_t *pl = s->pyr + GEO.goff[l];
        const uint8_t *mk = s->maskp + GEO.moff[l];
        size_t bx0 = GEO.w[l], by0 = GEO.h[l], bx1 = 0, by1 = 0;
        int hole = 0;

        for (size_t y = 0; y < GEO.h[l]; y++)
            for (size_t x = 0; x < GEO.w[l]; x++) {
                if (!mk[y * GEO.w[l] + x])
                    continue;
                hole = 1;
                if (x < bx0)
                    bx0 = x;
                if (x + 1 > bx1)
                    bx1 = x + 1;
                if (y < by0)
                    by0 = y;
                if (y + 1 > by1)
                    by1 = y + 1;
            }
        if (!hole) /* nothing reached this level */
            continue;
        or_pull(pl, s->up, mk, GEO.w[l], GEO.h[l],
                s->pyr + GEO.goff[l + 1], GEO.w[l + 1], GEO.h[l + 1]);
        for (unsigned it = 0; it < OR_ITERS; it++)
            or_sweep(pl, mk, GEO.w[l], GEO.h[l], bx0, by0, bx1, by1);
        iters += OR_ITERS;
    }

    sha1(s->pyr, OR_PHOTO_BYTES, out->filled_digest);
    out->unknown = unknown;
    out->coarse_fill = coarse_filled;
    out->iters = iters;
    s->res = *out; /* the scratch keeps the result of the last pipeline */
    return 0;
}

/* --- scratch / prepare / run -------------------------------------------------*/

int or_scratch_init(or_scratch *s)
{
    if (!s)
        return -1;
    geo_init();
    memset(s, 0, sizeof(*s));
    s->pyr = malloc(GEO.gtotal);
    s->maskp = malloc(GEO.mtotal);
    s->up = malloc(OR_PHOTO_BYTES);
    if (!s->pyr || !s->maskp || !s->up) {
        or_scratch_free(s);
        return -1;
    }
    return 0;
}

void or_scratch_free(or_scratch *s)
{
    if (!s)
        return;
    free(s->pyr);
    free(s->maskp);
    free(s->up);
    memset(s, 0, sizeof(*s));
}

or_ctx *or_prepare(unsigned seed)
{
    or_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t prng;

    if (!ctx)
        return NULL;
    geo_init();
    rng_init(&prng, seed);
    for (unsigned s = 0; s < OR_PHOTOS; s++) {
        unsigned arch = s % OR_ARCHETYPES;
        or_scratch sc;

        ctx->photo[s] = malloc(OR_PHOTO_BYTES);
        ctx->mask[s] = malloc(OR_PHOTO_PX);
        if (!ctx->photo[s] || !ctx->mask[s]) {
            or_free(ctx);
            return NULL;
        }
        memset(ctx->mask[s], 0, OR_PHOTO_PX);
        ctx->arch[s] = arch;
        gen_photo(ctx, s, arch, &prng);

        /* reference pipeline, off the clock */
        if (or_scratch_init(&sc) != 0) {
            or_free(ctx);
            return NULL;
        }
        if (or_process(ctx->photo[s], ctx->mask[s], &sc, &ctx->ref[s]) !=
                0 ||
            ctx->ref[s].unknown != ctx->mpx[s] ||
            ctx->ref[s].unknown == 0 || ctx->ref[s].coarse_fill == 0) {
            or_scratch_free(&sc);
            or_free(ctx);
            return NULL;
        }
        or_scratch_free(&sc);
    }
    return ctx;
}

void or_free(or_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned s = 0; s < OR_PHOTOS; s++) {
        free(ctx->photo[s]);
        free(ctx->mask[s]);
    }
    free(ctx);
}

/* One thread's slice: contiguous photo instances, own scratch —
 * threads never share writable state (shared-task harness contract). */
typedef struct {
    or_ctx *ctx;
    or_scratch sc;
    size_t begin, end; /* instance range [begin, end) of the pass */
} or_slice;

static int or_cycle(void *arg)
{
    or_slice *sl = arg;
    or_ctx *ctx = sl->ctx;

    for (size_t i = sl->begin; i < sl->end; i++) {
        unsigned s = (unsigned)(i % OR_PHOTOS);
        or_result r;
        const or_result *ref = &ctx->ref[s];

        if (or_process(ctx->photo[s], ctx->mask[s], &sl->sc, &r) != 0)
            return -1;
        if (memcmp(r.mask_digest, ref->mask_digest, 20) != 0 ||
            memcmp(r.filled_digest, ref->filled_digest, 20) != 0 ||
            r.unknown != ref->unknown ||
            r.coarse_fill != ref->coarse_fill ||
            r.iters != ref->iters ||
            r.unknown == 0 || /* the mask is never empty */
            r.coarse_fill == 0) /* the disc reaches the coarsest level */
            return -1;
    }
    return 0;
}

uint64_t or_task_pixels(unsigned multi)
{
    return (multi ? OR_PHOTOS_MULTI : OR_PHOTOS) * (uint64_t)OR_PHOTO_PX;
}

int or_run(or_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mpx, uint64_t *pixels)
{
    unsigned t_count;
    unsigned photo_set;
    or_slice *slices;
    void **ctxs;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    int rc = -1;

    if (!ctx || !threads || !throughput_mpx || !pixels)
        return -1;
    t_count = *threads;
    photo_set = t_count > 1 ? OR_PHOTOS_MULTI : OR_PHOTOS;

    if (t_count > photo_set) /* one photo instance per thread at least */
        t_count = photo_set;
    if (t_count < 1)
        t_count = 1;

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;

    {
        size_t per = photo_set / t_count;
        unsigned extra = photo_set % t_count;
        size_t begin = 0;

        for (unsigned t = 0; t < t_count; t++) {
            size_t count = per + (t < extra ? 1u : 0u);

            slices[t].ctx = ctx;
            slices[t].begin = begin;
            slices[t].end = begin + count;
            begin += count;
            if (or_scratch_init(&slices[t].sc) != 0)
                goto out;
            ctxs[t] = &slices[t];
        }
    }

    rc = harness_run(t_count, runs, or_cycle, ctxs, elapsed);
    if (rc == 0) {
        uint64_t npx = or_task_pixels(t_count > 1 ? 1u : 0u);

        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0
                           ? (double)npx / elapsed[r] / 1e6
                           : -1.0;
        *throughput_mpx = lb_median(rates, runs);
        *threads = t_count;
        *pixels = npx;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++)
            or_scratch_free(&slices[t].sc);
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors ---------------------------------------------------------------*/

const uint8_t *or_photo(const or_ctx *ctx, unsigned scene)
{
    return ctx && scene < OR_PHOTOS ? ctx->photo[scene] : NULL;
}

size_t or_photo_len(const or_ctx *ctx, unsigned scene)
{
    (void)scene;
    return ctx ? OR_PHOTO_BYTES : 0;
}

const uint8_t *or_mask(const or_ctx *ctx, unsigned scene)
{
    return ctx && scene < OR_PHOTOS ? ctx->mask[scene] : NULL;
}

const or_result *or_reference(const or_ctx *ctx, unsigned scene)
{
    return ctx && scene < OR_PHOTOS ? &ctx->ref[scene] : NULL;
}

unsigned or_archetype(const or_ctx *ctx, unsigned scene)
{
    return ctx && scene < OR_PHOTOS ? ctx->arch[scene] : 0;
}

or_disc or_mask_disc(const or_ctx *ctx, unsigned scene)
{
    or_disc zero = { 0, 0, 0 };

    return ctx && scene < OR_PHOTOS ? ctx->disc[scene] : zero;
}

or_rect or_mask_rect(const or_ctx *ctx, unsigned scene)
{
    or_rect zero = { 0, 0, 0, 0 };

    return ctx && scene < OR_PHOTOS ? ctx->rect[scene] : zero;
}

or_box or_mask_box(const or_ctx *ctx, unsigned scene)
{
    or_box zero = { 0, 0, 0, 0 };

    return ctx && scene < OR_PHOTOS ? ctx->box[scene] : zero;
}

uint32_t or_mask_pixels(const or_ctx *ctx, unsigned scene)
{
    return ctx && scene < OR_PHOTOS ? ctx->mpx[scene] : 0;
}
