/* LotofBench — PDF content interpreter and rasterizer (workload #4,
 * internal).
 *
 * Renders one page: walks the content stream, maintains the graphics
 * state (CTM stack, colors, dash, clip), builds device-space flattened
 * paths, and scan-converts them into an RGB framebuffer with 4x4
 * supersampled anti-aliasing.
 *
 * Determinism contract (what makes a render bit-identical everywhere):
 *   - every scan-conversion quantity is integer fixed point (24.8);
 *   - the CTM algebra is the only floating point, and the build uses
 *     -ffp-contract=off, so transforms reproduce exactly across
 *     compilers and machines (IEEE doubles, no FMA contraction);
 *   - sqrt() appears only where IEEE 754 defines it as correctly
 *     rounded (the conformal line-width scale, the join half-angle);
 *     every other square root is the integer routine below;
 *   - curve flattening is adaptive de Casteljau with an integer
 *     flatness test (1/2 px) and a depth cap.
 *
 * Supported operator set (strict — anything else is a render error):
 *   q Q cm w J j M d ri i g G rg RG k K cs CS sc scn
 *   m l c v y h re S s f F f* B B* b b* n W W*
 * Text, XObjects, inline images and shadings are NOT supported: the
 * corpus renders text as filled vector outlines (see docs/benchmarks.md,
 * "Workload 4", divergences). */

#include "pdfrender.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- fixed point helpers -----------------------------------------------------*/

static pdf_fix to_fix(double v)
{
    double r = v * (double)PDF_ONE + (v >= 0 ? 0.5 : -0.5);

    if (r > 2147483520.0)
        r = 2147483520.0;
    if (r < -2147483520.0)
        r = -2147483520.0;
    return (pdf_fix)r;
}

static int64_t abs64(int64_t v)
{
    return v < 0 ? -v : v;
}

/* Integer floor(sqrt(x)), x >= 0. */
static uint64_t isqrt64(uint64_t x)
{
    uint64_t lo = 0, hi = (uint64_t)1 << 32;

    while (lo + 1 < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (mid * mid <= x)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

/* --- device paths -------------------------------------------------------------*/

typedef struct {
    pdf_pt  *pts;
    uint32_t n, cap;
    int      closed;
} rcontour;

typedef struct {
    rcontour *c;
    uint32_t  n, cap;
} rpath;

static void rpath_clear(rpath *p)
{
    for (uint32_t i = 0; i < p->n; i++)
        free(p->c[i].pts);
    p->n = 0;
}

static void rpath_free(rpath *p)
{
    rpath_clear(p);
    free(p->c);
    memset(p, 0, sizeof(*p));
}

static rcontour *rpath_contour(rpath *p)
{
    if (p->n == p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 8;
        rcontour *nc = realloc(p->c, cap * sizeof(*nc));
        if (!nc)
            return NULL;
        p->c = nc;
        p->cap = cap;
    }
    rcontour *c = &p->c[p->n];
    memset(c, 0, sizeof(*c));
    p->n++;
    return c;
}

static int contour_point(rcontour *c, pdf_fix x, pdf_fix y)
{
    if (c->n == c->cap) {
        uint32_t cap = c->cap ? c->cap * 2 : 16;
        pdf_pt *np = realloc(c->pts, cap * sizeof(*np));
        if (!np)
            return -1;
        c->pts = np;
        c->cap = cap;
    }
    c->pts[c->n].x = x;
    c->pts[c->n].y = y;
    c->n++;
    return 0;
}

/* --- render context ------------------------------------------------------------*/

#define PDF_GSTATE_MAX  28u /* q nesting depth cap */
#define PDF_DASH_MAX     8u
#define PDF_OPER_MAX    33u /* PDF operand stack minimum */

/* color spaces */
enum { CS_GRAY = 1, CS_RGB = 3, CS_CMYK = 4 };

typedef struct {
    double   m[6];       /* CTM: a b c d e f */
    double   lw;         /* line width, user space */
    int      cap, join;  /* 0 butt, 1 round, 2 square / 0 miter, 1 round, 2 bevel */
    double   miter;      /* miter limit (ratio) */
    double   dash[PDF_DASH_MAX];
    uint32_t dash_n;
    double   dash_phase;
    double   fill_rgb[3], stroke_rgb[3];
    int      fill_cs, stroke_cs;
    uint8_t *clip_saved; /* per-stack-slot copy of the clip mask */
} gstate;

typedef struct {
    uint32_t   w, h, ss;
    uint8_t   *cov;    /* per-op coverage, w*h, 0..ss*ss */
    uint8_t   *clip;   /* clip coverage, w*h, 0..ss*ss */
    uint8_t   *ctmp;   /* scratch for W/W* */
    pdf_bitmap fb_view;
    rpath      path;
    gstate     gs[PDF_GSTATE_MAX];
    uint32_t   gs_n;
    uint64_t   ops;
    pdf_dbg   *dbg;
    char       err[128];
    int        failed;
} rctx;

static void fail(rctx *r, const char *msg)
{
    if (!r->failed) {
        r->failed = 1;
        snprintf(r->err, sizeof(r->err), "%s", msg);
    }
}

/* --- transforms ------------------------------------------------------------------*/

static void gs_ctm_concat(gstate *g, const double cm[6])
{
    double a = cm[0], b = cm[1], c = cm[2], d = cm[3], e = cm[4], f = cm[5];
    double a0 = g->m[0], b0 = g->m[1], c0 = g->m[2], d0 = g->m[3];
    double e0 = g->m[4], f0 = g->m[5];

    g->m[0] = a * a0 + b * c0;
    g->m[1] = a * b0 + b * d0;
    g->m[2] = c * a0 + d * c0;
    g->m[3] = c * b0 + d * d0;
    g->m[4] = e * a0 + f * c0 + e0;
    g->m[5] = e * b0 + f * d0 + f0;
}

static pdf_fix gs_x(const gstate *g, double ux, double uy)
{
    return to_fix(g->m[0] * ux + g->m[2] * uy + g->m[4]);
}

static pdf_fix gs_y(const gstate *g, double ux, double uy)
{
    return to_fix(g->m[1] * ux + g->m[3] * uy + g->m[5]);
}

/* Areal scale of the CTM: user-space distances map through sqrt(|det|)
 * for conformal transforms (rotation + uniform scale), the only kind
 * the corpus emits. Documented approximation otherwise. */
static double gs_scale(const gstate *g)
{
    double det = g->m[0] * g->m[3] - g->m[1] * g->m[2];

    if (det < 0)
        det = -det;
    return det > 1e-12 ? det : 1e-12;
}

/* --- curve flattening ---------------------------------------------------------------*/

static int bez_flat(pdf_fix x0, pdf_fix y0, pdf_fix x1, pdf_fix y1,
                    pdf_fix x2, pdf_fix y2, pdf_fix x3, pdf_fix y3)
{
    int64_t dx = (int64_t)x3 - x0, dy = (int64_t)y3 - y0;
    uint64_t len = isqrt64((uint64_t)(dx * dx + dy * dy));
    int64_t c1 = abs64(dx * ((int64_t)y1 - y0) - dy * ((int64_t)x1 - x0));
    int64_t c2 = abs64(dx * ((int64_t)y2 - y0) - dy * ((int64_t)x2 - x0));

    /* distance of the control points from the chord <= 1/2 px in 24.8:
     * cross / len <= 128  <=>  2 * cross <= len */
    return 2 * c1 <= (int64_t)len && 2 * c2 <= (int64_t)len;
}

static int bez_emit(rctx *r, rcontour *c, pdf_fix x0, pdf_fix y0,
                    pdf_fix x1, pdf_fix y1, pdf_fix x2, pdf_fix y2,
                    pdf_fix x3, pdf_fix y3, int depth)
{
    if (depth >= 18 || bez_flat(x0, y0, x1, y1, x2, y2, x3, y3))
        return contour_point(c, x3, y3);

    /* de Casteljau split at t = 1/2, fixed point */
    int64_t m01x = ((int64_t)x0 + x1) >> 1, m01y = ((int64_t)y0 + y1) >> 1;
    int64_t m12x = ((int64_t)x1 + x2) >> 1, m12y = ((int64_t)y1 + y2) >> 1;
    int64_t m23x = ((int64_t)x2 + x3) >> 1, m23y = ((int64_t)y2 + y3) >> 1;
    int64_t mx = (m01x + m12x) >> 1, my = (m01y + m12y) >> 1;
    int64_t nx = (m12x + m23x) >> 1, ny = (m12y + m23y) >> 1;
    int64_t cx = (mx + nx) >> 1, cy = (my + ny) >> 1;

    if (bez_emit(r, c, x0, y0, (pdf_fix)m01x, (pdf_fix)m01y,
                 (pdf_fix)mx, (pdf_fix)my, (pdf_fix)cx, (pdf_fix)cy,
                 depth + 1) != 0)
        return -1;
    return bez_emit(r, c, (pdf_fix)cx, (pdf_fix)cy, (pdf_fix)nx, (pdf_fix)ny,
                    (pdf_fix)m23x, (pdf_fix)m23y, x3, y3, depth + 1);
}

/* --- scan conversion --------------------------------------------------------------------*/

typedef struct {
    pdf_fix x;
    int32_t wind;
} xcross;

typedef struct {
    uint32_t y0, y1; /* pixel rows covered, [y0, y1) */
} band;

/* Scan-convert `path` into `dest` (w*h bytes; the band is zeroed here).
 * Coverage counts subsample hits, 0..ss*ss. `implicit_close` treats
 * every contour as closed (fill semantics); stroked outlines pass 1
 * too, since they are emitted as closed polygons. */
static int scan_convert(rctx *r, const rpath *p, int even_odd,
                        int implicit_close, uint8_t *dest, band *bd)
{
    const uint32_t ss = r->ss, w = r->w;

    bd->y0 = bd->y1 = 0;
    int64_t ymin = INT64_MAX, ymax = INT64_MIN;
    for (uint32_t ci = 0; ci < p->n; ci++)
        for (uint32_t i = 0; i < p->c[ci].n; i++) {
            int64_t y = p->c[ci].pts[i].y;
            if (y < ymin)
                ymin = y;
            if (y > ymax)
                ymax = y;
        }
    if (ymin > ymax)
        return 0;
    int64_t y0s = ymin >> PDF_FIX;
    int64_t y1s = (ymax >> PDF_FIX) + 1;
    if (y0s < 0)
        y0s = 0;
    if (y1s > (int64_t)r->h)
        y1s = r->h;
    if (y0s >= y1s)
        return 0;
    bd->y0 = (uint32_t)y0s;
    bd->y1 = (uint32_t)y1s;
    memset(dest + (size_t)bd->y0 * w, 0, (size_t)(bd->y1 - bd->y0) * w);

    uint32_t cr_cap = 4096;
    xcross *cr = malloc((size_t)cr_cap * sizeof(*cr));
    if (!cr) {
        fail(r, "out of memory");
        return -1;
    }

    for (uint32_t py = bd->y0; py < bd->y1; py++) {
        uint8_t *row = dest + (size_t)py * w;
        for (uint32_t s = 0; s < ss; s++) {
            pdf_fix ys = (pdf_fix)(py * PDF_ONE +
                                   (2 * (int)s + 1) * PDF_ONE / (int)(2 * ss));
            uint32_t n = 0;
            for (uint32_t ci = 0; ci < p->n; ci++) {
                const rcontour *c = &p->c[ci];
                uint32_t cnt = (implicit_close || c->closed) && c->n >= 2
                                   ? c->n
                                   : (c->n >= 2 ? c->n - 1 : 0);
                for (uint32_t i = 0; i < cnt; i++) {
                    const pdf_pt *a = &c->pts[i];
                    const pdf_pt *b = &c->pts[(i + 1) % c->n];
                    if ((a->y <= ys && b->y > ys) || (b->y <= ys && a->y > ys)) {
                        if (n == cr_cap) {
                            cr_cap *= 2;
                            xcross *nc = realloc(cr, cr_cap * sizeof(*nc));
                            if (!nc) {
                                free(cr);
                                fail(r, "out of memory");
                                return -1;
                            }
                            cr = nc;
                        }
                        int64_t num =
                            (int64_t)(ys - a->y) * ((int64_t)b->x - a->x);
                        int64_t den = (int64_t)b->y - a->y;
                        cr[n].x = a->x + (pdf_fix)(num / den);
                        cr[n].wind = b->y > a->y ? 1 : -1;
                        n++;
                    }
                }
            }
            /* insertion sort by x (crossing counts per line are small) */
            for (uint32_t i = 1; i < n; i++) {
                xcross key = cr[i];
                uint32_t j = i;
                while (j > 0 && cr[j - 1].x > key.x) {
                    cr[j] = cr[j - 1];
                    j--;
                }
                cr[j] = key;
            }
            /* walk the spans: a span runs from a crossing that turns the
             * winding on to the crossing that turns it off again */
            uint32_t i = 0;
            int32_t wind = 0;
            while (i < n) {
                wind += cr[i].wind;
                i++;
                int inside = even_odd ? (wind & 1) : wind != 0;
                if (!inside)
                    continue;
                pdf_fix xa = cr[i - 1].x;
                pdf_fix xb = xa;
                while (i < n) {
                    pdf_fix xi = cr[i].x;
                    wind += cr[i].wind;
                    i++;
                    int still = even_odd ? (wind & 1) : wind != 0;
                    if (!still) {
                        xb = xi;
                        break;
                    }
                }
                if (xb <= xa)
                    continue;
                int64_t px0 = xa >> PDF_FIX;
                int64_t px1 = ((int64_t)xb - 1) >> PDF_FIX;
                if (px1 < 0 || px0 >= (int64_t)w)
                    continue;
                if (px0 < 0)
                    px0 = 0;
                if (px1 >= (int64_t)w)
                    px1 = (int64_t)w - 1;
                for (int64_t px = px0; px <= px1; px++) {
                    pdf_fix pxa = (pdf_fix)(px * PDF_ONE);
                    pdf_fix pxb = (pdf_fix)((px + 1) * PDF_ONE);
                    uint32_t hits;
                    if (xa <= pxa && pxb <= xb) {
                        hits = ss; /* pixel fully inside the span */
                    } else {
                        hits = 0;
                        for (uint32_t k = 0; k < ss; k++) {
                            pdf_fix xs =
                                (pdf_fix)(px * PDF_ONE +
                                          (2 * (int)k + 1) * PDF_ONE /
                                              (int)(2 * ss));
                            if (xa <= xs && xs < xb)
                                hits++;
                        }
                    }
                    row[px] = (uint8_t)(row[px] + hits);
                }
            }
        }
    }
    free(cr);
    return 0;
}

/* Blend the coverage buffer over the band with an opaque color,
 * modulated by the clip mask. */
static void composite(rctx *r, const band *bd, const uint8_t rgb[3])
{
    const uint32_t ss2 = r->ss * r->ss;

    for (uint32_t py = bd->y0; py < bd->y1; py++) {
        const uint8_t *cov = r->cov + (size_t)py * r->w;
        const uint8_t *clip = r->clip + (size_t)py * r->w;
        uint8_t *px = r->fb_view.px + (size_t)py * r->w * 3;
        for (uint32_t x = 0; x < r->w; x++) {
            if (!cov[x])
                continue;
            uint32_t eff = ((uint32_t)cov[x] * clip[x] + ss2 / 2) / ss2;
            if (eff > ss2)
                eff = ss2;
            uint32_t inv = ss2 - eff;
            for (int ch = 0; ch < 3; ch++) {
                uint32_t v = (uint32_t)rgb[ch] * eff +
                             (uint32_t)px[x * 3 + ch] * inv;
                px[x * 3 + ch] = (uint8_t)((v + ss2 / 2) / ss2);
            }
        }
    }
}

static int paint_fill(rctx *r, int even_odd, const uint8_t rgb[3],
                      const rpath *path)
{
    band bd;

    if (scan_convert(r, path, even_odd, 1, r->cov, &bd) != 0)
        return -1;
    composite(r, &bd, rgb);
    return 0;
}

/* Clip: intersect the current clip mask with the path's coverage. */
static int apply_clip(rctx *r, int even_odd, const rpath *path)
{
    band bd;
    const uint32_t ss2 = r->ss * r->ss;

    if (scan_convert(r, path, even_odd, 1, r->ctmp, &bd) != 0)
        return -1;
    /* The clip intersects the current mask with the clip path. Rows outside
     * the clip path's scan band have zero coverage there, so intersecting
     * with them clears the mask; only rows inside the band multiply. */
    for (uint32_t py = 0; py < r->h; py++) {
        if (py >= bd.y0 && py < bd.y1)
            continue;
        memset(r->clip + (size_t)py * r->w, 0, r->w);
    }
    for (uint32_t py = bd.y0; py < bd.y1; py++) {
        uint8_t *clip = r->clip + (size_t)py * r->w;
        const uint8_t *tmp = r->ctmp + (size_t)py * r->w;
        for (uint32_t x = 0; x < r->w; x++)
            clip[x] = (uint8_t)(((uint32_t)clip[x] * tmp[x] + ss2 / 2) / ss2);
    }
    return 0;
}

/* --- stroking ------------------------------------------------------------------------*/

static int stroke_poly(rctx *r, rpath *out, const pdf_pt *pts, uint32_t n)
{
    rcontour *c = rpath_contour(out);

    if (!c) {
        fail(r, "out of memory");
        return -1;
    }
    for (uint32_t i = 0; i < n; i++)
        if (contour_point(c, pts[i].x, pts[i].y) != 0) {
            fail(r, "out of memory");
            return -1;
        }
    return 0;
}

/* Outward unit normal of segment a->b, scaled by whalf. Zero for a
 * degenerate segment. */
static void seg_normal(double ax, double ay, double bx, double by,
                       double whalf, double *nx, double *ny)
{
    double dx = bx - ax, dy = by - ay;
    double len = (double)isqrt64((uint64_t)(dx * dx + dy * dy));

    if (len < 0.5) {
        *nx = 0;
        *ny = 0;
        return;
    }
    *nx = dy / len * whalf;
    *ny = -dx / len * whalf;
}

/* Stroke one polyline (or closed ring) into `out` as filled polygons:
 * a quad per segment, joins at interior vertices, caps at open ends. */
static int stroke_run(rctx *r, rpath *out, const double *xs,
                      const double *ys, uint32_t n, int closed,
                      double whalf, const gstate *g)
{
    if (n < 2)
        return 0;
    uint32_t segs = closed ? n : n - 1;

    for (uint32_t s = 0; s < segs; s++) {
        uint32_t a = s, b = (s + 1) % n;
        double nx, ny;

        seg_normal(xs[a], ys[a], xs[b], ys[b], whalf, &nx, &ny);
        if (nx == 0 && ny == 0)
            continue;
        pdf_pt quad[4] = {
            { to_fix((double)xs[a] + nx), to_fix((double)ys[a] + ny) },
            { to_fix((double)xs[b] + nx), to_fix((double)ys[b] + ny) },
            { to_fix((double)xs[b] - nx), to_fix((double)ys[b] - ny) },
            { to_fix((double)xs[a] - nx), to_fix((double)ys[a] - ny) },
        };
        if (stroke_poly(r, out, quad, 4) != 0)
            return -1;
    }

    /* joins at vertices with a segment on both sides */
    uint32_t joins = closed ? n : (n >= 3 ? n - 2 : 0);
    for (uint32_t ji = 0; ji < joins; ji++) {
        uint32_t j = closed ? ji : ji + 1;
        uint32_t pa = (j + n - 1) % n, pb = (j + 1) % n;
        double n1x, n1y, n2x, n2y;

        seg_normal(xs[pa], ys[pa], xs[j], ys[j], whalf, &n1x, &n1y);
        seg_normal(xs[j], ys[j], xs[pb], ys[pb], whalf, &n2x, &n2y);
        if ((n1x == 0 && n1y == 0) || (n2x == 0 && n2y == 0))
            continue;
        double bxn = n1x + n2x, byn = n1y + n2y;
        double blen = sqrt(bxn * bxn + byn * byn);
        pdf_pt poly[4];
        uint32_t pn = 0;

        if (g->join == 1) { /* round: bulge point on the bisector */
            poly[pn].x = to_fix((double)xs[j] + n1x);
            poly[pn].y = to_fix((double)ys[j] + n1y);
            pn++;
            poly[pn].x = to_fix((double)xs[j] + bxn * (whalf / blen));
            poly[pn].y = to_fix((double)ys[j] + byn * (whalf / blen));
            pn++;
            poly[pn].x = to_fix((double)xs[j] + n2x);
            poly[pn].y = to_fix((double)ys[j] + n2y);
            pn++;
        } else {
            double u1x = n1x / whalf, u1y = n1y / whalf;
            double u2x = n2x / whalf, u2y = n2y / whalf;
            double dot = u1x * u2x + u1y * u2y;
            if (dot > 1)
                dot = 1;
            if (dot < -1)
                dot = -1;
            double cosh = sqrt((1.0 + dot) / 2.0); /* cos of the half angle */
            double mlen = whalf / (cosh > 1e-3 ? cosh : 1e-3);
            if (g->join == 0 && mlen <= whalf * g->miter) { /* miter */
                poly[pn].x = to_fix((double)xs[j] + n1x);
                poly[pn].y = to_fix((double)ys[j] + n1y);
                pn++;
                poly[pn].x = to_fix((double)xs[j] + bxn * (mlen / blen));
                poly[pn].y = to_fix((double)ys[j] + byn * (mlen / blen));
                pn++;
                poly[pn].x = to_fix((double)xs[j] + n2x);
                poly[pn].y = to_fix((double)ys[j] + n2y);
                pn++;
            } else { /* bevel (or miter over the limit) */
                poly[pn].x = to_fix((double)xs[j] + n1x);
                poly[pn].y = to_fix((double)ys[j] + n1y);
                pn++;
                poly[pn].x = to_fix((double)xs[j] + n2x);
                poly[pn].y = to_fix((double)ys[j] + n2y);
                pn++;
            }
        }
        poly[pn].x = to_fix((double)xs[j]);
        poly[pn].y = to_fix((double)ys[j]);
        pn++;
        if (stroke_poly(r, out, poly, pn) != 0)
            return -1;
    }

    /* caps for open polylines */
    if (!closed && g->cap != 0) {
        for (int end = 0; end < 2; end++) {
            uint32_t i, j;
            if (end == 0) {
                i = 0;
                j = 1;
            } else {
                i = n - 1;
                j = n - 2;
            }
            double dx = (double)xs[j] - xs[i], dy = (double)ys[j] - ys[i];
            double len = (double)isqrt64((uint64_t)(dx * dx + dy * dy));
            if (len < 0.5)
                continue;
            double tx = dx / len, ty = dy / len; /* outward tangent */
            double nx = ty * whalf, ny = -tx * whalf;
            pdf_pt poly[6];
            uint32_t pn = 0;

            if (g->cap == 1) { /* round: half-disc fan */
                const double r45 = 0.70710678118654752;
                poly[pn].x = to_fix((double)xs[i] + nx);
                poly[pn].y = to_fix((double)ys[i] + ny);
                pn++;
                poly[pn].x = to_fix((double)xs[i] + (nx + tx * whalf) * r45);
                poly[pn].y = to_fix((double)ys[i] + (ny + ty * whalf) * r45);
                pn++;
                poly[pn].x = to_fix((double)xs[i] + tx * whalf);
                poly[pn].y = to_fix((double)ys[i] + ty * whalf);
                pn++;
                poly[pn].x = to_fix((double)xs[i] + (-nx + tx * whalf) * r45);
                poly[pn].y = to_fix((double)ys[i] + (-ny + ty * whalf) * r45);
                pn++;
                poly[pn].x = to_fix((double)xs[i] - nx);
                poly[pn].y = to_fix((double)ys[i] - ny);
                pn++;
            } else { /* square: extend by whalf */
                poly[pn].x = to_fix((double)xs[i] + nx);
                poly[pn].y = to_fix((double)ys[i] + ny);
                pn++;
                poly[pn].x = to_fix((double)xs[i] + nx + tx * whalf);
                poly[pn].y = to_fix((double)ys[i] + ny + ty * whalf);
                pn++;
                poly[pn].x = to_fix((double)xs[i] - nx + tx * whalf);
                poly[pn].y = to_fix((double)ys[i] - ny + ty * whalf);
                pn++;
                poly[pn].x = to_fix((double)xs[i] - nx);
                poly[pn].y = to_fix((double)ys[i] - ny);
                pn++;
            }
            if (stroke_poly(r, out, poly, pn) != 0)
                return -1;
        }
    }
    return 0;
}

/* Walk a polyline splitting it on the dash pattern (device units:
 * user-space dash lengths times `scale`), stroking each on-run with
 * butt caps. */
static int stroke_dashed(rctx *r, rpath *out, const gstate *g,
                         const double *xs, const double *ys, uint32_t n,
                         double whalf, double scale)
{
    double pat[PDF_DASH_MAX];
    double per = 0;

    for (uint32_t i = 0; i < g->dash_n; i++) {
        pat[i] = g->dash[i] * scale;
        if (pat[i] < 0.01)
            pat[i] = 0.01;
        per += pat[i];
    }
    if (per <= 0)
        return stroke_run(r, out, xs, ys, n, 0, whalf, g);

    double pos = g->dash_phase * scale;
    pos -= floor(pos / per) * per;
    uint32_t seg = 0;
    double acc = 0;

    while (seg + 1 < g->dash_n && acc + pat[seg] <= pos) {
        acc += pat[seg];
        seg++;
    }
    double into = pos - acc;
    int on = seg % 2 == 0;

    double *rx = malloc((size_t)n * sizeof(double));
    double *ry = malloc((size_t)n * sizeof(double));
    if (!rx || !ry) {
        free(rx);
        free(ry);
        fail(r, "out of memory");
        return -1;
    }
    uint32_t rn = 0;
    int rc = 0;

    for (uint32_t i = 0; i + 1 < n && rc == 0; i++) {
        double dx = (double)xs[i + 1] - xs[i];
        double dy = (double)ys[i + 1] - ys[i];
        double len = (double)isqrt64((uint64_t)(dx * dx + dy * dy));
        if (len < 0.5)
            continue;
        double t = 0;

        if (on && rn == 0) {
            rx[rn] = xs[i];
            ry[rn] = ys[i];
            rn++;
        }
        while (t < len - 1e-9 && rc == 0) {
            double step = pat[seg] - into;
            if (step > len - t)
                step = len - t;
            t += step;
            double f = t / len;
            if (on) {
                rx[rn] = xs[i] + dx * f;
                ry[rn] = ys[i] + dy * f;
                rn++;
            } else if (rn >= 2) {
                rc = stroke_run(r, out, rx, ry, rn, 0, whalf, g);
                rn = 0;
            }
            into += step;
            while (into >= pat[seg]) {
                into -= pat[seg];
                seg = (seg + 1) % g->dash_n;
                on = seg % 2 == 0;
                if (!on) {
                    if (rn >= 2) {
                        rc = stroke_run(r, out, rx, ry, rn, 0, whalf, g);
                    }
                    rn = 0;
                } else if (rn == 0) {
                    rx[rn] = xs[i] + dx * f;
                    ry[rn] = ys[i] + dy * f;
                    rn++;
                }
            }
        }
    }
    if (rc == 0 && on && rn >= 2)
        rc = stroke_run(r, out, rx, ry, rn, 0, whalf, g);
    free(rx);
    free(ry);
    return rc;
}

static void color_bytes(const double rgb[3], uint8_t out[3]);

static int paint_stroke(rctx *r, const gstate *g, const rpath *path)
{
    double scale = sqrt(gs_scale(g));
    double wdev = g->lw * scale;

    if (wdev < 1.0)
        wdev = 1.0; /* spec: zero-width lines render at 1 device pixel */
    double whalf = wdev / 2.0;
    rpath out;
    int rc = 0;

    memset(&out, 0, sizeof(out));
    for (uint32_t ci = 0; ci < path->n && rc == 0; ci++) {
        const rcontour *c = &path->c[ci];
        if (c->n < 2)
            continue;
        /* Path points are device-space fixed point; the stroking pipeline
         * works in device pixels (double) and converts back to fixed point
         * only when emitting polygons. */
        double *xs = malloc((size_t)c->n * sizeof(double));
        double *ys = malloc((size_t)c->n * sizeof(double));
        if (!xs || !ys) {
            free(xs);
            free(ys);
            fail(r, "out of memory");
            rc = -1;
            break;
        }
        for (uint32_t i = 0; i < c->n; i++) {
            xs[i] = (double)c->pts[i].x / (double)PDF_ONE;
            ys[i] = (double)c->pts[i].y / (double)PDF_ONE;
        }
        if (g->dash_n)
            rc = stroke_dashed(r, &out, g, xs, ys, c->n, whalf, scale);
        else
            rc = stroke_run(r, &out, xs, ys, c->n, c->closed, whalf, g);
        free(xs);
        free(ys);
    }
    if (rc == 0) {
        band bd;
        uint8_t rgb[3];

        rc = scan_convert(r, &out, 0, 1, r->cov, &bd);
        if (rc == 0) {
            color_bytes(g->stroke_rgb, rgb);
            composite(r, &bd, rgb);
        }
    }
    rpath_free(&out);
    return rc;
}

/* --- colors ------------------------------------------------------------------------------*/

static uint8_t chan(double v)
{
    if (v < 0)
        v = 0;
    if (v > 1)
        v = 1;
    return (uint8_t)(v * 255.0 + 0.5);
}

static void set_color(double rgb[3], const double *comp, int cs)
{
    if (cs == CS_GRAY) {
        rgb[0] = rgb[1] = rgb[2] = chan(comp[0]) / 255.0;
    } else if (cs == CS_RGB) {
        rgb[0] = chan(comp[0]) / 255.0;
        rgb[1] = chan(comp[1]) / 255.0;
        rgb[2] = chan(comp[2]) / 255.0;
    } else {
        double c = comp[0], m = comp[1], y = comp[2], k = comp[3];

        if (c < 0) c = 0;
        if (c > 1) c = 1;
        if (m < 0) m = 0;
        if (m > 1) m = 1;
        if (y < 0) y = 0;
        if (y > 1) y = 1;
        if (k < 0) k = 0;
        if (k > 1) k = 1;
        rgb[0] = chan((1 - c) * (1 - k)) / 255.0;
        rgb[1] = chan((1 - m) * (1 - k)) / 255.0;
        rgb[2] = chan((1 - y) * (1 - k)) / 255.0;
    }
}

static void color_bytes(const double rgb[3], uint8_t out[3])
{
    out[0] = chan(rgb[0]);
    out[1] = chan(rgb[1]);
    out[2] = chan(rgb[2]);
}

/* --- debug capture --------------------------------------------------------------------------*/

static void dbg_push(rctx *r, int kind, const uint8_t rgb[3],
                     const rpath *path)
{
    pdf_dbg *d = r->dbg;

    if (!d)
        return;
    if (d->n == d->cap) {
        uint32_t cap = d->cap ? d->cap * 2 : 32;
        pdf_cmd *nc = realloc(d->cmds, cap * sizeof(*nc));
        if (!nc) {
            fail(r, "out of memory");
            return;
        }
        d->cmds = nc;
        d->cap = cap;
    }
    pdf_cmd *cmd = &d->cmds[d->n];
    memset(cmd, 0, sizeof(*cmd));
    cmd->kind = kind;
    cmd->r = rgb[0];
    cmd->g = rgb[1];
    cmd->b = rgb[2];
    cmd->path.c = malloc(path->n ? (size_t)path->n * sizeof(pdf_contour) : 1);
    if (!cmd->path.c) {
        fail(r, "out of memory");
        return;
    }
    for (uint32_t ci = 0; ci < path->n; ci++) {
        const rcontour *c = &path->c[ci];
        pdf_contour *nc = &cmd->path.c[cmd->path.n];

        memset(nc, 0, sizeof(*nc));
        nc->pts = malloc(c->n ? (size_t)c->n * sizeof(pdf_pt) : 1);
        if (!nc->pts) {
            fail(r, "out of memory");
            return;
        }
        memcpy(nc->pts, c->pts, (size_t)c->n * sizeof(pdf_pt));
        nc->n = nc->cap = c->n;
        nc->closed = c->closed;
        cmd->path.n++;
    }
    d->n++;
}

/* --- graphics state stack ----------------------------------------------------------------------*/

static int gs_push(rctx *r)
{
    if (r->gs_n >= PDF_GSTATE_MAX) {
        fail(r, "graphics state stack overflow");
        return -1;
    }
    gstate *dst = &r->gs[r->gs_n];
    const gstate *src = &r->gs[r->gs_n - 1];

    *dst = *src;
    dst->clip_saved = malloc((size_t)r->w * r->h);
    if (!dst->clip_saved) {
        fail(r, "out of memory");
        return -1;
    }
    memcpy(dst->clip_saved, r->clip, (size_t)r->w * r->h);
    r->gs_n++;
    return 0;
}

static int gs_pop(rctx *r)
{
    if (r->gs_n <= 1) {
        fail(r, "unbalanced Q");
        return -1;
    }
    gstate *top = &r->gs[r->gs_n - 1];

    memcpy(r->clip, top->clip_saved, (size_t)r->w * r->h);
    free(top->clip_saved);
    top->clip_saved = NULL;
    r->gs_n--;
    return 0;
}

/* --- interpreter ----------------------------------------------------------------------------*/

typedef struct {
    double   num;
    int      kind; /* 0 number, 1 name, 2 array */
    char     name[64];
    double   arr[PDF_DASH_MAX];
    uint32_t arr_n;
} oper;

/* Operators whose executions the benchmark counts (path construction,
 * painting, clipping — the rasterizer's work units, matched by the
 * corpus generator). */
static const char *const OP_COUNTED[] = {
    "m", "l", "c", "v", "y", "h", "re", "S", "s", "f", "F", "f*",
    "B", "B*", "b", "b*", "n", "W", "W*"
};

static int interpret(rctx *r, const uint8_t *buf, size_t len)
{
    oper ops[PDF_OPER_MAX];
    uint32_t nops = 0;
    size_t pos = 0;

#define NEED_NUMS(k_)                                                       \
    do {                                                                    \
        if (nops < (k_)) {                                                  \
            fail(r, "missing operands");                                    \
            return -1;                                                      \
        }                                                                   \
        for (uint32_t q_ = 0; q_ < (k_); q_++)                              \
            if (ops[nops - (k_) + q_].kind != 0) {                          \
                fail(r, "operand type mismatch");                           \
                return -1;                                                  \
            }                                                               \
    } while (0)
#define POPD() (ops[--nops].num)

    for (;;) {
        /* skip whitespace and comments */
        for (;;) {
            while (pos < len &&
                   (buf[pos] == '\0' || buf[pos] == '\t' || buf[pos] == '\n' ||
                    buf[pos] == '\f' || buf[pos] == '\r' || buf[pos] == ' '))
                pos++;
            if (pos < len && buf[pos] == '%') {
                while (pos < len && buf[pos] != '\n' && buf[pos] != '\r')
                    pos++;
                continue;
            }
            break;
        }
        if (pos >= len)
            break;

        int c = buf[pos];

        if (nops < PDF_OPER_MAX &&
            (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9'))) {
            int neg = 0;
            double v = 0;
            int digits = 0;

            if (buf[pos] == '+' || buf[pos] == '-')
                neg = buf[pos++] == '-';
            while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
                v = v * 10 + (buf[pos] - '0');
                digits++;
                pos++;
            }
            if (pos < len && buf[pos] == '.') {
                double sc = 0.1;
                pos++;
                while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
                    v += (buf[pos] - '0') * sc;
                    sc *= 0.1;
                    pos++;
                }
            }
            if (!digits) {
                fail(r, "malformed number in content stream");
                return -1;
            }
            ops[nops].kind = 0;
            ops[nops].num = neg ? -v : v;
            ops[nops].arr_n = 0;
            nops++;
            continue;
        }
        if (c == '/') {
            pos++;
            char nm[64];
            uint32_t nn = 0;

            while (pos < len && nn < sizeof(nm) - 1) {
                int nc = buf[pos];
                if (nc == ' ' || nc == '\n' || nc == '\r' || nc == '\t' ||
                    nc == '\f' || nc == '\0' || nc == '/' || nc == '[' ||
                    nc == ']' || nc == '<' || nc == '>' || nc == '(' ||
                    nc == ')' || nc == '%')
                    break;
                nm[nn++] = (char)nc;
                pos++;
            }
            nm[nn] = '\0';
            if (nn == 0) {
                fail(r, "empty name");
                return -1;
            }
            ops[nops].kind = 1;
            ops[nops].arr_n = 0;
            memcpy(ops[nops].name, nm, sizeof(nm));
            nops++;
            continue;
        }
        if (c == '[') {
            pos++;
            oper arr;
            arr.kind = 2;
            arr.arr_n = 0;
            for (;;) {
                while (pos < len &&
                       (buf[pos] == ' ' || buf[pos] == '\n' || buf[pos] == '\r' ||
                        buf[pos] == '\t' || buf[pos] == '\f' || buf[pos] == '\0'))
                    pos++;
                if (pos < len && buf[pos] == ']') {
                    pos++;
                    break;
                }
                if (pos >= len || arr.arr_n >= PDF_DASH_MAX) {
                    fail(r, "bad array operand");
                    return -1;
                }
                int aneg = 0;
                double av = 0;
                int adig = 0;

                if (buf[pos] == '+' || buf[pos] == '-')
                    aneg = buf[pos++] == '-';
                while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
                    av = av * 10 + (buf[pos] - '0');
                    adig++;
                    pos++;
                }
                if (pos < len && buf[pos] == '.') {
                    double asc = 0.1;
                    pos++;
                    while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
                        av += (buf[pos] - '0') * asc;
                        asc *= 0.1;
                        pos++;
                    }
                }
                if (!adig) {
                    fail(r, "bad array operand");
                    return -1;
                }
                arr.arr[arr.arr_n++] = aneg ? -av : av;
            }
            if (nops >= PDF_OPER_MAX) {
                fail(r, "operand stack overflow");
                return -1;
            }
            ops[nops] = arr;
            nops++;
            continue;
        }
        if (c == '(' || c == '<') {
            /* strings are never interpreted; skip them */
            if (c == '<' && pos + 1 < len && buf[pos + 1] == '<') {
                fail(r, "dictionaries in content streams unsupported");
                return -1;
            }
            if (c == '(') {
                pos++;
                int depth = 1;
                while (pos < len && depth > 0) {
                    if (buf[pos] == '\\') {
                        pos += 2;
                        continue;
                    }
                    if (buf[pos] == '(')
                        depth++;
                    if (buf[pos] == ')')
                        depth--;
                    pos++;
                }
                if (depth != 0) {
                    fail(r, "unterminated string");
                    return -1;
                }
            } else {
                pos++;
                while (pos < len && buf[pos] != '>')
                    pos++;
                if (pos >= len) {
                    fail(r, "unterminated hex string");
                    return -1;
                }
                pos++;
            }
            continue;
        }

        /* operator word */
        char word[8];
        uint32_t wn = 0;

        while (pos < len && wn < sizeof(word) - 1) {
            int wc = buf[pos];
            if (wc == ' ' || wc == '\n' || wc == '\r' || wc == '\t' ||
                wc == '\f' || wc == '\0' || wc == '/' || wc == '[' ||
                wc == ']' || wc == '<' || wc == '>' || wc == '(' ||
                wc == ')' || wc == '%')
                break;
            word[wn++] = (char)wc;
            pos++;
        }
        word[wn] = '\0';
        if (wn == 0) {
            fail(r, "unexpected byte in content stream");
            return -1;
        }

        int counted = 0;
        for (size_t k = 0; k < sizeof(OP_COUNTED) / sizeof(*OP_COUNTED); k++)
            if (!strcmp(word, OP_COUNTED[k])) {
                counted = 1;
                break;
            }
        r->ops += (uint64_t)counted;

        gstate *g = &r->gs[r->gs_n - 1];

        if (!strcmp(word, "q")) {
            if (gs_push(r) != 0)
                return -1;
        } else if (!strcmp(word, "Q")) {
            if (gs_pop(r) != 0)
                return -1;
        } else if (!strcmp(word, "cm")) {
            NEED_NUMS(6);
            double cm[6];
            for (int i = 0; i < 6; i++)
                cm[i] = ops[nops - 6 + i].num;
            nops -= 6;
            gs_ctm_concat(g, cm);
        } else if (!strcmp(word, "w")) {
            NEED_NUMS(1);
            g->lw = POPD();
            if (g->lw < 0)
                g->lw = 0;
        } else if (!strcmp(word, "J")) {
            NEED_NUMS(1);
            g->cap = (int)POPD();
        } else if (!strcmp(word, "j")) {
            NEED_NUMS(1);
            g->join = (int)POPD();
        } else if (!strcmp(word, "M")) {
            NEED_NUMS(1);
            g->miter = POPD();
            if (g->miter < 1)
                g->miter = 1;
        } else if (!strcmp(word, "d")) {
            if (nops < 2 || ops[nops - 2].kind != 2 || ops[nops - 1].kind != 0) {
                fail(r, "bad d operands");
                return -1;
            }
            if (ops[nops - 2].arr_n > PDF_DASH_MAX) {
                fail(r, "dash array too long");
                return -1;
            }
            g->dash_n = ops[nops - 2].arr_n;
            for (uint32_t i = 0; i < g->dash_n; i++)
                g->dash[i] = ops[nops - 2].arr[i];
            g->dash_phase = ops[nops - 1].num;
            nops -= 2;
        } else if (!strcmp(word, "ri") || !strcmp(word, "i")) {
            NEED_NUMS(1);
            (void)POPD(); /* rendering intent / flatness: ignored */
        } else if (!strcmp(word, "g")) {
            NEED_NUMS(1);
            double v = POPD();
            double comp[4] = { v, 0, 0, 0 };
            g->fill_cs = CS_GRAY;
            set_color(g->fill_rgb, comp, CS_GRAY);
        } else if (!strcmp(word, "G")) {
            NEED_NUMS(1);
            double v = POPD();
            double comp[4] = { v, 0, 0, 0 };
            g->stroke_cs = CS_GRAY;
            set_color(g->stroke_rgb, comp, CS_GRAY);
        } else if (!strcmp(word, "rg")) {
            NEED_NUMS(3);
            double comp[4] = { ops[nops - 3].num, ops[nops - 2].num,
                               ops[nops - 1].num, 0 };
            g->fill_cs = CS_RGB;
            nops -= 3;
            set_color(g->fill_rgb, comp, CS_RGB);
        } else if (!strcmp(word, "RG")) {
            NEED_NUMS(3);
            double comp[4] = { ops[nops - 3].num, ops[nops - 2].num,
                               ops[nops - 1].num, 0 };
            g->stroke_cs = CS_RGB;
            nops -= 3;
            set_color(g->stroke_rgb, comp, CS_RGB);
        } else if (!strcmp(word, "k")) {
            NEED_NUMS(4);
            double comp[4];
            for (int i = 0; i < 4; i++)
                comp[i] = ops[nops - 4 + i].num;
            g->fill_cs = CS_CMYK;
            nops -= 4;
            set_color(g->fill_rgb, comp, CS_CMYK);
        } else if (!strcmp(word, "K")) {
            NEED_NUMS(4);
            double comp[4];
            for (int i = 0; i < 4; i++)
                comp[i] = ops[nops - 4 + i].num;
            g->stroke_cs = CS_CMYK;
            nops -= 4;
            set_color(g->stroke_rgb, comp, CS_CMYK);
        } else if (!strcmp(word, "cs") || !strcmp(word, "CS")) {
            if (nops < 1 || ops[nops - 1].kind != 1) {
                fail(r, "cs/CS needs a name");
                return -1;
            }
            const char *nm = ops[nops - 1].name;
            int cs = !strcmp(nm, "DeviceGray")   ? CS_GRAY
                     : !strcmp(nm, "DeviceRGB")  ? CS_RGB
                     : !strcmp(nm, "DeviceCMYK") ? CS_CMYK
                                                 : 0;
            if (!cs) {
                fail(r, "unsupported color space");
                return -1;
            }
            nops--;
            if (word[0] == 'c')
                g->fill_cs = cs;
            else
                g->stroke_cs = cs;
        } else if (!strcmp(word, "sc") || !strcmp(word, "scn")) {
            int cs = word[1] == 'c' ? g->fill_cs : g->stroke_cs;
            NEED_NUMS((uint32_t)cs);
            double comp[4];
            for (int i = 0; i < cs; i++)
                comp[i] = ops[nops - (uint32_t)cs + i].num;
            nops -= (uint32_t)cs;
            if (word[1] == 'c')
                set_color(g->fill_rgb, comp, cs);
            else
                set_color(g->stroke_rgb, comp, cs);
        } else if (!strcmp(word, "m")) {
            NEED_NUMS(2);
            double ux = ops[nops - 2].num, uy = ops[nops - 1].num;
            nops -= 2;
            rcontour *c = rpath_contour(&r->path);
            if (!c || contour_point(c, gs_x(g, ux, uy), gs_y(g, ux, uy)) != 0) {
                fail(r, "out of memory");
                return -1;
            }
        } else if (!strcmp(word, "l")) {
            NEED_NUMS(2);
            if (r->path.n == 0 || r->path.c[r->path.n - 1].n == 0) {
                fail(r, "lineto without moveto");
                return -1;
            }
            double ux = ops[nops - 2].num, uy = ops[nops - 1].num;
            nops -= 2;
            rcontour *c = &r->path.c[r->path.n - 1];
            if (contour_point(c, gs_x(g, ux, uy), gs_y(g, ux, uy)) != 0) {
                fail(r, "out of memory");
                return -1;
            }
        } else if (!strcmp(word, "c") || !strcmp(word, "v") ||
                   !strcmp(word, "y")) {
            int kind = word[0];

            NEED_NUMS((uint32_t)(kind == 'c' ? 6 : 4));
            if (r->path.n == 0 || r->path.c[r->path.n - 1].n == 0) {
                fail(r, "curveto without moveto");
                return -1;
            }
            rcontour *c = &r->path.c[r->path.n - 1];
            pdf_fix x0 = c->pts[c->n - 1].x, y0 = c->pts[c->n - 1].y;
            pdf_fix x1, y1, x2, y2, x3, y3;

            if (kind == 'c') {
                x1 = gs_x(g, ops[nops - 6].num, ops[nops - 5].num);
                y1 = gs_y(g, ops[nops - 6].num, ops[nops - 5].num);
                x2 = gs_x(g, ops[nops - 4].num, ops[nops - 3].num);
                y2 = gs_y(g, ops[nops - 4].num, ops[nops - 3].num);
                x3 = gs_x(g, ops[nops - 2].num, ops[nops - 1].num);
                y3 = gs_y(g, ops[nops - 2].num, ops[nops - 1].num);
                nops -= 6;
            } else if (kind == 'v') { /* first control = current point */
                x1 = x0;
                y1 = y0;
                x2 = gs_x(g, ops[nops - 4].num, ops[nops - 3].num);
                y2 = gs_y(g, ops[nops - 4].num, ops[nops - 3].num);
                x3 = gs_x(g, ops[nops - 2].num, ops[nops - 1].num);
                y3 = gs_y(g, ops[nops - 2].num, ops[nops - 1].num);
                nops -= 4;
            } else { /* 'y': second control = final point */
                x1 = gs_x(g, ops[nops - 4].num, ops[nops - 3].num);
                y1 = gs_y(g, ops[nops - 4].num, ops[nops - 3].num);
                x2 = gs_x(g, ops[nops - 2].num, ops[nops - 1].num);
                y2 = gs_y(g, ops[nops - 2].num, ops[nops - 1].num);
                x3 = x2;
                y3 = y2;
                nops -= 4;
            }
            if (bez_emit(r, c, x0, y0, x1, y1, x2, y2, x3, y3, 0) != 0) {
                fail(r, "out of memory");
                return -1;
            }
        } else if (!strcmp(word, "h")) {
            /* closepath: mark the contour closed; filling auto-closes
             * through the flag and stroking wraps the ring, so no
             * duplicate endpoint is materialized */
            if (r->path.n > 0)
                r->path.c[r->path.n - 1].closed = 1;
        } else if (!strcmp(word, "re")) {
            NEED_NUMS(4);
            double h = ops[nops - 1].num, w = ops[nops - 2].num;
            double y = ops[nops - 3].num, x = ops[nops - 4].num;
            nops -= 4;
            if (w == 0 || h == 0)
                continue; /* degenerate rectangle: appends nothing */
            rcontour *c = rpath_contour(&r->path);
            if (!c) {
                fail(r, "out of memory");
                return -1;
            }
            static const double corner[4][2] = {
                { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }
            };
            for (int i = 0; i < 4; i++) {
                double ux = x + corner[i][0] * w, uy = y + corner[i][1] * h;
                if (contour_point(c, gs_x(g, ux, uy), gs_y(g, ux, uy)) != 0) {
                    fail(r, "out of memory");
                    return -1;
                }
            }
            c->closed = 1;
        } else if (!strcmp(word, "W") || !strcmp(word, "W*")) {
            static const uint8_t black[3] = { 0, 0, 0 };
            if (r->dbg)
                dbg_push(r, PDF_CMD_CLIP, black, &r->path);
            if (apply_clip(r, word[1] == '*', &r->path) != 0)
                return -1;
        } else if (!strcmp(word, "n")) {
            rpath_clear(&r->path);
        } else if (!strcmp(word, "f") || !strcmp(word, "F")) {
            uint8_t rgb[3];

            color_bytes(g->fill_rgb, rgb);
            if (r->dbg)
                dbg_push(r, PDF_CMD_FILL_NZ, rgb, &r->path);
            if (paint_fill(r, 0, rgb, &r->path) != 0)
                return -1;
            rpath_clear(&r->path);
        } else if (!strcmp(word, "f*")) {
            uint8_t rgb[3];

            color_bytes(g->fill_rgb, rgb);
            if (r->dbg)
                dbg_push(r, PDF_CMD_FILL_EO, rgb, &r->path);
            if (paint_fill(r, 1, rgb, &r->path) != 0)
                return -1;
            rpath_clear(&r->path);
        } else if (!strcmp(word, "S") || !strcmp(word, "s")) {
            if (word[0] == 's' && r->path.n > 0)
                r->path.c[r->path.n - 1].closed = 1;
            uint8_t rgb[3];

            color_bytes(g->stroke_rgb, rgb);
            if (r->dbg)
                dbg_push(r, PDF_CMD_STROKE, rgb, &r->path);
            if (paint_stroke(r, g, &r->path) != 0)
                return -1;
            rpath_clear(&r->path);
        } else if (!strcmp(word, "B") || !strcmp(word, "b")) {
            if (word[0] == 'b' && r->path.n > 0)
                r->path.c[r->path.n - 1].closed = 1;
            uint8_t rgb[3];

            color_bytes(g->fill_rgb, rgb);
            if (r->dbg)
                dbg_push(r, PDF_CMD_FILL_NZ, rgb, &r->path);
            if (paint_fill(r, 0, rgb, &r->path) != 0)
                return -1;
            color_bytes(g->stroke_rgb, rgb);
            if (r->dbg)
                dbg_push(r, PDF_CMD_STROKE, rgb, &r->path);
            if (paint_stroke(r, g, &r->path) != 0)
                return -1;
            rpath_clear(&r->path);
        } else if (!strcmp(word, "B*") || !strcmp(word, "b*")) {
            if (word[0] == 'b' && r->path.n > 0)
                r->path.c[r->path.n - 1].closed = 1;
            uint8_t rgb[3];

            color_bytes(g->fill_rgb, rgb);
            if (r->dbg)
                dbg_push(r, PDF_CMD_FILL_EO, rgb, &r->path);
            if (paint_fill(r, 1, rgb, &r->path) != 0)
                return -1;
            color_bytes(g->stroke_rgb, rgb);
            if (r->dbg)
                dbg_push(r, PDF_CMD_STROKE, rgb, &r->path);
            if (paint_stroke(r, g, &r->path) != 0)
                return -1;
            rpath_clear(&r->path);
        } else {
            fail(r, "unsupported content stream operator");
            return -1;
        }
        if (r->failed)
            return -1;
    }
#undef NEED_NUMS
#undef POPD
    return 0;
}

/* --- public entry points ------------------------------------------------------------------------*/

int pdf_bitmap_alloc(pdf_bitmap *fb, uint32_t w, uint32_t h)
{
    fb->px = malloc((size_t)w * h * 3);
    if (!fb->px)
        return -1;
    fb->w = w;
    fb->h = h;
    return 0;
}

void pdf_bitmap_free(pdf_bitmap *fb)
{
    free(fb->px);
    memset(fb, 0, sizeof(*fb));
}

void pdf_dbg_free(pdf_dbg *dbg)
{
    if (!dbg)
        return;
    for (uint32_t i = 0; i < dbg->n; i++) {
        for (uint32_t j = 0; j < dbg->cmds[i].path.n; j++)
            free(dbg->cmds[i].path.c[j].pts);
        free(dbg->cmds[i].path.c);
    }
    free(dbg->cmds);
    memset(dbg, 0, sizeof(*dbg));
}

static int render_common(const uint8_t *contents, size_t clen,
                         const double media_box[4], pdf_bitmap *fb,
                         uint64_t *ops, pdf_dbg *dbg, char *err,
                         size_t errsz)
{
    rctx r;
    int rc;

    memset(&r, 0, sizeof(r));
    r.dbg = dbg;
    r.ss = 4;
    r.w = fb->w;
    r.h = fb->h;
    r.fb_view = *fb;

    if (media_box[2] <= media_box[0] || media_box[3] <= media_box[1]) {
        snprintf(err, errsz, "empty media box");
        return -1;
    }
    memset(fb->px, 0xFF, (size_t)fb->w * fb->h * 3); /* paper white */

    r.cov = malloc((size_t)r.w * r.h);
    r.clip = malloc((size_t)r.w * r.h);
    r.ctmp = malloc((size_t)r.w * r.h);
    if (!r.cov || !r.clip || !r.ctmp) {
        snprintf(err, errsz, "out of memory");
        rc = -1;
        goto out;
    }
    memset(r.clip, 0x10, (size_t)r.w * r.h); /* fully visible */

    /* base graphics state: user space -> device, y down */
    gstate *g = &r.gs[0];
    double s = PDF_RENDER_SCALE;

    g->m[0] = s;
    g->m[1] = 0;
    g->m[2] = 0;
    g->m[3] = -s;
    g->m[4] = -media_box[0] * s;
    g->m[5] = media_box[3] * s;
    g->lw = 1.0;
    g->cap = 0;
    g->join = 0;
    g->miter = 10.0;
    g->dash_n = 0;
    g->dash_phase = 0;
    g->fill_cs = CS_GRAY;
    g->stroke_cs = CS_GRAY;
    double black[4] = { 0, 0, 0, 0 };

    set_color(g->fill_rgb, black, CS_GRAY);
    set_color(g->stroke_rgb, black, CS_GRAY);
    g->clip_saved = NULL;
    r.gs_n = 1;

    rc = interpret(&r, contents, clen);
    if (rc == 0 && r.gs_n != 1) {
        snprintf(err, errsz, "unbalanced q/Q at end of page");
        rc = -1;
    }
    if (rc != 0)
        snprintf(err, errsz, "%s", r.err[0] ? r.err : "render failed");
    else if (ops)
        *ops = r.ops;

out:
    rpath_clear(&r.path);
    for (uint32_t i = 0; i < PDF_GSTATE_MAX; i++) {
        free(r.gs[i].clip_saved);
        r.gs[i].clip_saved = NULL;
    }
    free(r.cov);
    free(r.clip);
    free(r.ctmp);
    return rc;
}

int pdf_render_contents(const uint8_t *contents, size_t clen,
                        const double media_box[4], pdf_bitmap *fb,
                        uint64_t *ops, char *err, size_t errsz)
{
    return render_common(contents, clen, media_box, fb, ops, NULL, err, errsz);
}

int pdf_render_contents_dbg(const uint8_t *contents, size_t clen,
                            const double media_box[4], pdf_bitmap *fb,
                            uint64_t *ops, pdf_dbg *dbg, char *err,
                            size_t errsz)
{
    memset(dbg, 0, sizeof(*dbg));
    return render_common(contents, clen, media_box, fb, ops, dbg, err, errsz);
}
