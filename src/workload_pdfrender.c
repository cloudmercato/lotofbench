/* LotofBench — PDF Render workload (workload #4).
 *
 * Mirrors the fourth Geekbench 6 CPU workload: open complex PDF
 * documents and render them. Geekbench 6 pushes four American National
 * Park Service maps (897 KB to 1.5 MB, large vector art, lines and
 * text) through PDFium — 4 PDFs single-core, 16 PDFs multi-core.
 *
 * LotofBench reproduces the shape dependency-free:
 *
 *   - deterministic corpus — four seeded "park map" PDF documents (a
 *     cover page and a map page each, byte sizes matched to the four
 *     NPS maps) written as real PDF 1.4 files: header, catalog, page
 *     tree, uncompressed content streams, classic xref table, trailer.
 *     Fixed --seed means byte-identical documents on every machine;
 *   - a from-scratch PDF reader parses the documents strictly (anything
 *     outside the emitted subset is a parse error) before rendering;
 *   - a from-scratch scanline renderer rasterizes every page with 4x4
 *     anti-aliasing: bezier flattening, nonzero and even-odd filling,
 *     stroking with joins/caps/dashes, clipping, gray/RGB/CMYK color;
 *   - correctness inside the timed cycle — every content stream is
 *     SHA-1-checked against the generator's digest, every rendered page
 *     must reproduce its prepare-time reference digest, a set of
 *     generator-computed probe pixels must match exactly, and the
 *     operator count must match the generator's count. Any mismatch
 *     aborts the run instead of scoring.
 *
 * The throughput metric is device pixels: the exact pixel count of the
 * pass (4 docs x 2 pages single-core, 4 x that multi-core) divided by
 * the wall-clock time of the whole pass, in millions of pixels per
 * second. The count is exact and machine-independent, like the corpus
 * size of workload #1 and the edge count of workload #2.
 *
 * Divergences from Geekbench 6 (deliberate, see docs/benchmarks.md):
 * synthetic documents instead of NPS scans, uncompressed content
 * streams, no font engine (text is drawn as filled vector outlines),
 * no images/forms/shadings, 144 dpi instead of PDFium's default. */

#include "workload_pdfrender.h"

#include "harness.h"
#include "lotofbench.h"
#include "pdf.h"
#include "pdfrender.h"
#include "rng.h"
#include "sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- geometry constants (all coordinates are quarter-points) -------------*/

#define MB_W 612.0
#define MB_H 792.0
#define Q4PT 4 /* quarter-points per point */

/* Byte-size targets of the four documents, matching the NPS maps
 * Geekbench 6 renders (Crater Lake 897 KB, Golden Gate 1.5 MB,
 * Lewis and Clark 288 KB, Mount Rainier 949 KB). */
static const size_t DOC_TARGET[PDF_DOCS] = { 897000, 1500000, 288000, 949000 };

static const char *const DOC_TITLE[PDF_DOCS] = {
    "CRATER LAKE", "GOLDEN GATE", "LEWIS AND CLARK", "MOUNT RAINIER"
};

/* Reserved verification area on the map page (user points): the legend
 * swatches, the scale bar and the inset frame live here, painted last,
 * and every random element must avoid the rectangle. */
#define RES_X0 0
#define RES_Y0 0
#define RES_X1 190
#define RES_Y1 150

/* The lake's center (points), BELOW the map element box: every random
 * element's geometry stays inside x [200, 590] pt, y [156, 770] pt
 * (out-of-box legs are dropped, forest anchors are reserve-checked), so
 * nothing painted after the lake can come within ~46 pt of the probe
 * pixel at the lake's center. */
#define LAKE_CX 395.0
#define LAKE_CY 110.0

/* rotation tables — literal constants, no libm in generation */
static const char *const ROT[5] = {
    "1 0 0 1",
    "0.8660 0.5 -0.5 0.8660",       /* +30 deg */
    "0.8660 -0.5 0.5 0.8660",       /* -30 deg */
    "0.7071 0.7071 -0.7071 0.7071", /* +45 deg */
    "0.7071 -0.7071 0.7071 0.7071", /* -45 deg */
};

/* 12-direction circle table (30 degree steps) for blob outlines. */
static const double CIRC[12][2] = {
    { 1, 0 },         { 0.8660254037844386, 0.5 },
    { 0.5, 0.8660254037844386 }, { 0, 1 },
    { -0.5, 0.8660254037844386 }, { -0.8660254037844386, 0.5 },
    { -1, 0 },        { -0.8660254037844386, -0.5 },
    { -0.5, -0.8660254037844386 }, { 0, -1 },
    { 0.5, -0.8660254037844386 }, { 0.8660254037844386, -0.5 },
};

/* 4x6 cell font: A-Z, 0-9, space, '-', '.', ','. Each glyph is six
 * 4-bit rows, top row first (bit 3 = leftmost cell). Text is rendered
 * as filled unit-cell rectangles — the vector-outline stand-in for a
 * font engine (documented divergence). */
#define G(a, b, c, d, e, f) \
    ((uint32_t)(a) << 20 | (uint32_t)(b) << 16 | (uint32_t)(c) << 12 | \
     (uint32_t)(d) << 8 | (uint32_t)(e) << 4 | (uint32_t)(f))
static const uint32_t GLYPH[40] = {
    G(6, 9, 15, 9, 9, 9),  G(14, 9, 14, 9, 9, 14), G(7, 8, 8, 8, 8, 7),
    G(14, 9, 9, 9, 9, 14), G(15, 8, 14, 8, 8, 15),  G(15, 8, 14, 8, 8, 8),
    G(7, 8, 11, 9, 9, 7),  G(9, 9, 15, 9, 9, 9),    G(14, 4, 4, 4, 4, 14),
    G(7, 2, 2, 2, 10, 12), G(9, 10, 12, 10, 9, 9),  G(8, 8, 8, 8, 8, 15),
    G(9, 11, 15, 9, 9, 9), G(9, 13, 13, 11, 11, 9), G(6, 9, 9, 9, 9, 6),
    G(14, 9, 9, 14, 8, 8), G(6, 9, 9, 9, 10, 5),    G(14, 9, 9, 14, 10, 9),
    G(7, 8, 6, 1, 1, 14),  G(15, 6, 6, 6, 6, 6),    G(9, 9, 9, 9, 9, 6),
    G(9, 9, 9, 9, 6, 6),   G(9, 9, 9, 15, 15, 6),   G(9, 9, 6, 6, 9, 9),
    G(9, 9, 6, 6, 6, 6),   G(15, 1, 2, 4, 8, 15),
    G(6, 9, 11, 13, 9, 6), G(2, 6, 2, 2, 2, 7),     G(6, 9, 1, 2, 4, 15),
    G(14, 1, 6, 1, 1, 14), G(2, 6, 10, 15, 2, 2),   G(15, 8, 14, 1, 1, 14),
    G(7, 8, 14, 9, 9, 6),  G(15, 1, 2, 2, 4, 4),    G(6, 9, 6, 9, 9, 6),
    G(6, 9, 9, 7, 1, 14),  G(0, 0, 0, 0, 0, 0),     G(0, 0, 15, 0, 0, 0),
    G(0, 0, 0, 0, 0, 6),   G(0, 0, 0, 0, 2, 4),
};

static unsigned glyph_index(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return (unsigned)(ch - 'A');
    if (ch >= '0' && ch <= '9')
        return 26u + (unsigned)(ch - '0');
    if (ch == '-')
        return 37;
    if (ch == '.')
        return 38;
    if (ch == ',')
        return 39;
    return 36; /* space */
}

static const char *const WORDS[] = {
    "PARK", "TRAIL", "LAKE", "SUMMIT", "RIVER", "CAMP", "VIEW", "LOOP",
    "RIDGE", "CREEK", "FALLS", "MEADOW", "GATE", "NORTH", "SOUTH", "EAST",
    "WEST", "ELEV", "FT", "MI", "KM", "TRAILHEAD", "MARKER", "STATION",
    "RANGER", "DISTRICT", "WILDERNESS", "CANYON", "BASIN", "PASS",
    "SPRING", "BRIDGE"
};
#define NWORDS (sizeof(WORDS) / sizeof(*WORDS))

/* --- content buffer --------------------------------------------------------*/

typedef struct {
    uint8_t *b;
    size_t   n, cap;
    uint64_t ops; /* counted path/paint/clip operators emitted */
} cbuf;

static void cput(cbuf *c, const char *s)
{
    size_t len = strlen(s);
    if (c->n + len > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 65536;
        while (cap < c->n + len)
            cap *= 2;
        uint8_t *nb = realloc(c->b, cap);
        if (!nb)
            abort(); /* generation OOM: fatal, not a benchmark result */
        c->b = nb;
        c->cap = cap;
    }
    memcpy(c->b + c->n, s, len);
    c->n += len;
}

/* Length-aware append: stream payloads may contain NUL bytes, so the
 * strlen-based cput above must never be used for them. */
static void cputn(cbuf *c, const void *p, size_t len)
{
    if (c->n + len > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 65536;
        while (cap < c->n + len)
            cap *= 2;
        uint8_t *nb = realloc(c->b, cap);
        if (!nb)
            abort(); /* generation OOM: fatal, not a benchmark result */
        c->b = nb;
        c->cap = cap;
    }
    memcpy(c->b + c->n, p, len);
    c->n += len;
}

/* Emit a number given in quarter-points. Integer arithmetic only, so
 * the document bytes are identical on every machine (no printf float
 * rounding). */
static void cnum(cbuf *c, int32_t q)
{
    char tmp[32];
    int32_t ipart = q / 4;
    int32_t frac = q % 4;

    if (frac < 0) {
        frac += 4;
        ipart -= 1;
    }
    if (frac == 0)
        snprintf(tmp, sizeof(tmp), "%d", (int)ipart);
    else
        snprintf(tmp, sizeof(tmp), "%d.%02d", (int)ipart, (int)(frac * 25));
    cput(c, tmp);
}

static void csp(cbuf *c)
{
    cput(c, " ");
}

static void cnl(cbuf *c)
{
    cput(c, "\n");
}

static void c_re(cbuf *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    cnum(c, x); csp(c); cnum(c, y); csp(c); cnum(c, w); csp(c); cnum(c, h);
    cput(c, " re\n");
    c->ops++;
}

static void c_pt(cbuf *c, int32_t x, int32_t y, const char *op)
{
    cnum(c, x); csp(c); cnum(c, y); cput(c, " "); cput(c, op); cnl(c);
    c->ops++;
}

static void c_c(cbuf *c, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                int32_t x3, int32_t y3)
{
    cnum(c, x1); csp(c); cnum(c, y1); csp(c);
    cnum(c, x2); csp(c); cnum(c, y2); csp(c);
    cnum(c, x3); csp(c); cnum(c, y3);
    cput(c, " c\n");
    c->ops++;
}

static void c_h(cbuf *c)
{
    cput(c, "h\n");
    c->ops++;
}

static void c_paint(cbuf *c, const char *op)
{
    cput(c, op);
    cnl(c);
    c->ops++;
}

/* Line width: `w` takes exactly ONE operand in PDF. */
static void c_w(cbuf *c, int32_t q)
{
    cnum(c, q);
    cput(c, " w\n");
}

static void c_q(cbuf *c)
{
    cput(c, "q\n");
}

static void c_Q(cbuf *c)
{
    cput(c, "Q\n");
}

static void c_cm_trans(cbuf *c, int32_t x, int32_t y)
{
    cput(c, "1 0 0 1 ");
    cnum(c, x); csp(c); cnum(c, y);
    cput(c, " cm\n");
}

static void c_cm_rot_trans(cbuf *c, int rot, int32_t x, int32_t y)
{
    cput(c, ROT[rot]);
    cput(c, " ");
    cnum(c, x); csp(c); cnum(c, y);
    cput(c, " cm\n");
}

/* Draw one label: every glyph cell is a rectangle in one path, filled
 * once. (x, y) is the baseline origin in quarter-points; csq the cell
 * size in quarter-points. */
static void c_text(cbuf *c, const char *s, int32_t x, int32_t y, int32_t csq,
                   int rot)
{
    c_q(c);
    if (rot)
        c_cm_rot_trans(c, rot, x, y);
    else
        c_cm_trans(c, x, y);
    int32_t adv = 0;
    for (const char *p = s; *p; p++) {
        uint32_t gl = GLYPH[glyph_index(*p)];
        for (int row = 0; row < 6; row++) {
            uint32_t bits = (gl >> ((5 - row) * 4)) & 0xF;
            for (int col = 0; col < 4; col++)
                if (bits & (0x8u >> col))
                    c_re(c, adv + col * csq, (5 - row) * csq, csq, csq);
        }
        adv += 5 * csq;
    }
    c_paint(c, "f");
    c_Q(c);
}

/* --- probe recording ---------------------------------------------------------*/

typedef struct {
    uint16_t x, y; /* device pixel */
    uint8_t  r, g, b;
} probe;

/* The pipeline's channel quantization rule (v >= 0, clamped to [0,1],
 * then (uint8_t)(v * 255 + 0.5)) is documented here: the probe colors
 * in this file were derived from it analytically. */

typedef struct {
    rng_t   *rng;
    cbuf     content[PDF_PAGES_PER_DOC];
    probe   *probes[PDF_PAGES_PER_DOC];
    uint32_t probe_n[PDF_PAGES_PER_DOC];
    uint64_t ops;
} gen_ctx;

static void add_probe(gen_ctx *g, unsigned page, uint32_t px, uint32_t py,
                      uint8_t r, uint8_t gc, uint8_t b)
{
    probe *list =
        realloc(g->probes[page], (g->probe_n[page] + 1) * sizeof(probe));
    if (!list)
        abort();
    g->probes[page] = list;
    probe *p = &list[g->probe_n[page]++];
    p->x = (uint16_t)px;
    p->y = (uint16_t)py;
    p->r = r;
    p->g = gc;
    p->b = b;
}

/* Probe a device pixel from a user-space point (points, double). */
static void add_probe_user(gen_ctx *g, unsigned page, double ux, double uy,
                           uint8_t r, uint8_t gc, uint8_t b)
{
    add_probe(g, page, (uint32_t)(ux * 2.0), (uint32_t)(MB_H - uy) * 2u, r, gc,
              b);
}

/* Probe the center of an ON cell of a label (generator-side transform,
 * independent of the renderer). */
static void probe_cell(gen_ctx *g, unsigned page, int32_t tx, int32_t ty,
                       int col, int row, int32_t csq, uint8_t r, uint8_t gc,
                       uint8_t b)
{
    int32_t x0 = tx + col * csq; /* label space, quarter-points */
    int32_t y0 = ty + (5 - row) * csq;

    /* device px = quarter-points / 2 (2 px per pt, 4 q4 per pt); py
     * counts from the top of the device image */
    uint32_t px = (uint32_t)((int64_t)x0 / 2 + (int64_t)csq / 4);
    uint32_t py = (uint32_t)((2 * 1584 - (int64_t)y0) / 2 - (int64_t)csq / 4);

    add_probe(g, page, px, py, r, gc, b);
}

/* --- document generation --------------------------------------------------------*/

static uint32_t rq(gen_ctx *g, int32_t lo, int32_t hi) /* quarter-point pick */
{
    return lo + (int32_t)rng_below(g->rng, (uint32_t)(hi - lo + 1));
}

static void rand_word(gen_ctx *g, char *out, size_t sz)
{
    snprintf(out, sz, "%s", WORDS[rng_below(g->rng, NWORDS)]);
}

/* Does an element's bounding box (quarter-points) hit the protected
 * areas — the verification reserve or the corners probed for paper
 * color? The lake needs no keep-out: it sits below the element box
 * (see LAKE_CX/LAKE_CY). */
static int protected_hit(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    /* verification reserve */
    if (x0 < RES_X1 * Q4PT && y0 < RES_Y1 * Q4PT && x1 > RES_X0 &&
        y1 > RES_Y0)
        return 1;
    /* paper probes: bottom-left and top-left device corners */
    if (x0 < 60 && y0 > 3050) /* user-space top strip: device y < 70 */
        return 1;
    if (x0 < 60 && y1 < 120) /* bottom strip on the map page */
        return 1;
    return 0;
}

static void gen_paper(cbuf *c)
{
    cput(c, "0.965 0.955 0.91 rg\n");
    c_re(c, 0, 0, (int32_t)(MB_W * 4), (int32_t)(MB_H * 4));
    c_paint(c, "f");
}

static void gen_grid(cbuf *c)
{
    cput(c, "0.82 0.82 0.8 RG\n");
    c_w(c, 1);
    for (int32_t x = 200 * Q4PT; x <= 590 * Q4PT; x += 24 * Q4PT) {
        c_pt(c, x, 160 * Q4PT, "m");
        c_pt(c, x, 770 * Q4PT, "l");
        c_paint(c, "S");
    }
    for (int32_t y = 168 * Q4PT; y <= 770 * Q4PT; y += 24 * Q4PT) {
        c_pt(c, 200 * Q4PT, y, "m");
        c_pt(c, 590 * Q4PT, y, "l");
        c_paint(c, "S");
    }
}

/* Closed smooth blob around (cx, cy): Catmull-Rom through 12 anchors,
 * emitted as cubic beziers, all coordinates in quarter-points. The
 * anchor radii wobble deterministically; cx, cy, r, wobble are plain
 * doubles, so -ffp-contract=off keeps the quantized anchors identical
 * on every machine. */
static void blob_emit(cbuf *c, double cx, double cy, double r, rng_t *rng,
                      const char *color_op, double wobble)
{
    int32_t ax[12], ay[12];

    for (int k = 0; k < 12; k++) {
        double rr = r * (1.0 - wobble + 2.0 * wobble *
                           (double)rng_below(rng, 1000) / 1000.0);
        double x = cx + CIRC[k][0] * rr, y = cy + CIRC[k][1] * rr;
        ax[k] = (int32_t)(x * 4.0 + 0.5);
        ay[k] = (int32_t)(y * 4.0 + 0.5);
    }

    cput(c, color_op);
    cnl(c);
    c_pt(c, ax[0], ay[0], "m");
    for (int k = 0; k < 12; k++) {
        /* Catmull-Rom -> bezier: the segment from anchor k to anchor
         * k+1 (indices wrap mod 12; the last one lands back on p0) */
        int32_t p0x = ax[(k + 11) % 12], p0y = ay[(k + 11) % 12];
        int32_t p1x = ax[k], p1y = ay[k];
        int32_t p2x = ax[(k + 1) % 12], p2y = ay[(k + 1) % 12];
        int32_t p3x = ax[(k + 2) % 12], p3y = ay[(k + 2) % 12];
        c_c(c, p1x + (p2x - p0x) / 6, p1y + (p2y - p0y) / 6,
            p2x - (p3x - p1x) / 6, p2y - (p3y - p1y) / 6, p2x, p2y);
    }
    c_h(c);
    c_paint(c, "f");
}

static void cover_page(gen_ctx *g, unsigned doc)
{
    cbuf *c = &g->content[0];
    char word[32];

    gen_paper(c);

    /* double border */
    cput(c, "0.2 0.2 0.22 RG\n");
    c_w(c, 8);
    c_re(c, 18 * Q4PT, 18 * Q4PT, (int32_t)(576 * Q4PT), (int32_t)(756 * Q4PT));
    c_paint(c, "S");
    c_w(c, 2);
    c_re(c, 26 * Q4PT, 26 * Q4PT, (int32_t)(560 * Q4PT), (int32_t)(740 * Q4PT));
    c_paint(c, "S");

    /* title */
    const char *title = DOC_TITLE[doc];
    size_t tlen = strlen(title);
    int32_t csq = 24; /* 6 pt cells */
    int32_t twidth = (int32_t)tlen * 5 * csq;
    int32_t tx = (int32_t)(MB_W * Q4PT) - twidth / 2;
    int32_t ty = 2800; /* 700 pt */
    cput(c, "0.15 0.15 0.17 rg\n");
    c_text(c, title, tx, ty, csq, 0);

    /* subtitle */
    snprintf(word, sizeof(word), "TOPOGRAPHIC MAP SHEET %u", doc + 1);
    csq = 12;
    int32_t swidth = (int32_t)strlen(word) * 5 * csq;
    cput(c, "0.25 0.25 0.28 rg\n");
    c_text(c, word, (int32_t)(MB_W * Q4PT) - swidth / 2, 2600, csq, 0);

    /* rule under the subtitle */
    cput(c, "0.4 0.35 0.3 RG\n");
    c_w(c, 3);
    c_pt(c, 170 * Q4PT, 2500, "m");
    c_pt(c, 442 * Q4PT, 625 * Q4PT, "l");
    c_paint(c, "S");

    /* mountain silhouette */
    blob_emit(c, 306, 400, 110, g->rng, "0.45 0.48 0.42 rg", 0.45);

    /* compass rose */
    blob_emit(c, 520, 620, 26, g->rng, "0.9 0.88 0.8 rg", 0.12);
    cput(c, "0.15 0.15 0.17 RG\n");
    c_w(c, 2);
    c_pt(c, 2080, 2480, "m"); /* 520,620 */
    c_pt(c, 2080, 2624, "l"); /* 520,656 */
    c_paint(c, "S");
    c_pt(c, 2080, 2624, "m");
    c_pt(c, 2064, 2592, "l");
    c_paint(c, "S");
    c_pt(c, 2080, 2624, "m");
    c_pt(c, 2096, 2592, "l");
    c_paint(c, "S");

    /* paragraph filler until the cover budget is met: two columns of
     * small cell text, words drawn from the fixed list */
    size_t budget = DOC_TARGET[doc] / 4;
    int32_t col_x[2] = { 60 * Q4PT, 320 * Q4PT };
    int col = 0;
    int32_t y = 2240; /* 560 pt */
    char line[49];

    cput(c, "0.2 0.2 0.24 rg\n");
    while (c->n < budget && y >= 600) {
        size_t len = 0;
        line[0] = '\0';
        while (len < 43) {
            rand_word(g, word, sizeof(word));
            if (len + strlen(word) + 1 > 44)
                break;
            if (len)
                line[len++] = ' ';
            strcpy(line + len, word);
            len += strlen(word);
        }
        line[len] = '\0';
        c_text(c, line, col_x[col], y, 8, 0);
        y -= 56;
        if (y < 600 && col == 0) {
            col = 1;
            y = 2240;
        }
    }

    /* revision block */
    cput(c, "0.3 0.3 0.32 RG\n");
    c_w(c, 2);
    c_re(c, 60 * Q4PT, 140 * Q4PT, 180 * Q4PT, 60 * Q4PT);
    c_paint(c, "S");
    for (int i = 1; i < 3; i++) {
        c_pt(c, 60 * Q4PT, (140 + i * 20) * Q4PT, "m");
        c_pt(c, 240 * Q4PT, (140 + i * 20) * Q4PT, "l");
        c_paint(c, "S");
    }

    /* cover probes: paper corner + the first ON cell of the title's
     * first glyph (cell found by table lookup, so any title works) */
    add_probe(g, 0, 16, 16, 246, 244, 232);
    uint32_t gl = GLYPH[glyph_index(title[0])];
    int pc = 0, pr = 0, found = 0;
    for (int row = 0; row < 6 && !found; row++)
        for (int col = 0; col < 4 && !found; col++)
            if ((gl >> ((5 - row) * 4)) & (0x8u >> col)) {
                pc = col;
                pr = row;
                found = 1;
            }
    probe_cell(g, 0, tx, ty, pc, pr, 24, 38, 38, 43);
}

/* One random map element. Returns bytes appended (0 when nothing fit). */
static size_t map_element(gen_ctx *g, cbuf *c)
{
    char word[32];
    uint32_t kind = rng_below(g->rng, 100);

    if (kind < 30) { /* rotated feature label */
        rand_word(g, word, sizeof(word));
        int32_t csq = 10;
        int32_t wq = (int32_t)strlen(word) * 5 * csq;
        int rot = (int)rng_below(g->rng, 5);
        int32_t span = rot ? wq / 2 + wq / 4 : wq;
        for (int tries = 0; tries < 24; tries++) {
            int32_t x = rq(g, 200 * Q4PT, (int32_t)(590 * Q4PT) - span);
            int32_t y = rq(g, 165 * Q4PT, 745 * Q4PT);
            if (!protected_hit(x, y, x + span, y + 6 * csq)) {
                cput(c, "0.12 0.1 0.1 rg\n");
                c_text(c, word, x, y, csq, rot);
                return c->n;
            }
        }
        return 0;
    }
    if (kind < 55) { /* contour line: stroked beziers */
        int32_t x = rq(g, 210 * Q4PT, 560 * Q4PT);
        int32_t y = rq(g, 170 * Q4PT, 740 * Q4PT);
        if (protected_hit(x - 20 * Q4PT, y - 20 * Q4PT, x + 120 * Q4PT,
                          y + 120 * Q4PT))
            return 0;
        cput(c, "0.45 0.3 0.15 RG\n");
        c_w(c, 3); /* 0.75 pt */
        c_pt(c, x, y, "m");
        for (int sgm = 0; sgm < 3; sgm++) {
            c_c(c, x + 20 * Q4PT, y + 30 * Q4PT, x + 50 * Q4PT,
                y - 10 * Q4PT, x + 80 * Q4PT, y + 20 * Q4PT);
            x += 80 * Q4PT;
            y += 20 * Q4PT;
        }
        c_paint(c, "S");
        return c->n;
    }
    if (kind < 70) { /* stream: stroked bezier, blue */
        int32_t x = rq(g, 210 * Q4PT, 560 * Q4PT);
        int32_t y = rq(g, 170 * Q4PT, 700 * Q4PT);
        if (protected_hit(x - 20 * Q4PT, y - 20 * Q4PT, x + 150 * Q4PT,
                          y + 150 * Q4PT))
            return 0;
        cput(c, "0.2 0.4 0.7 RG\n");
        c_w(c, 5);
        c_pt(c, x, y, "m");
        for (int sgm = 0; sgm < 2; sgm++) {
            c_c(c, x + 30 * Q4PT, y + 60 * Q4PT, x - 20 * Q4PT,
                y + 120 * Q4PT, x + 40 * Q4PT, y + 160 * Q4PT);
            x += 40 * Q4PT;
            y += 160 * Q4PT;
        }
        c_paint(c, "S");
        return c->n;
    }
    if (kind < 85) { /* road: axis-aligned legs, miter corners */
        int32_t x = rq(g, 210 * Q4PT, 480 * Q4PT);
        int32_t y = rq(g, 175 * Q4PT, 700 * Q4PT);
        if (protected_hit(x - 10 * Q4PT, y - 10 * Q4PT, x + 200 * Q4PT,
                          y + 200 * Q4PT))
            return 0;
        cput(c, "0.2 0.2 0.22 RG\n");
        c_w(c, 12); /* 3 pt */
        c_pt(c, x, y, "m");
        for (int leg = 0; leg < 4; leg++) {
            int32_t dx = (int32_t)rng_below(g->rng, 90) + 20;
            int32_t dy = (int32_t)rng_below(g->rng, 90) + 20;
            if (rng_below(g->rng, 2))
                dx = -dx;
            if (rng_below(g->rng, 2))
                dy = -dy;
            x += dx * Q4PT;
            y += dy * Q4PT;
            if (x < 200 * Q4PT || x > 590 * Q4PT || y < 165 * Q4PT ||
                y > 770 * Q4PT)
                break;
            c_pt(c, x, y, "l");
        }
        c_paint(c, "S");
        return c->n;
    }
    if (kind < 95) { /* dashed trail */
        int32_t x = rq(g, 210 * Q4PT, 500 * Q4PT);
        int32_t y = rq(g, 175 * Q4PT, 740 * Q4PT);
        if (protected_hit(x - 10 * Q4PT, y - 10 * Q4PT, x + 150 * Q4PT,
                          y + 100 * Q4PT))
            return 0;
        cput(c, "0.45 0.25 0.05 RG\n");
        c_w(c, 4);
        cput(c, "[6 4] 0 d\n");
        c_pt(c, x, y, "m");
        for (int leg = 0; leg < 3; leg++) {
            x += (int32_t)(rng_below(g->rng, 60) + 20) * Q4PT;
            y += (int32_t)(rng_below(g->rng, 40) - 20) * Q4PT;
            if (x < 200 * Q4PT || x > 590 * Q4PT || y < 165 * Q4PT ||
                y > 770 * Q4PT)
                break;
            c_pt(c, x, y, "l");
        }
        c_paint(c, "S");
        cput(c, "[] 0 d\n");
        return c->n;
    }
    /* forest patch: green blob */
    int32_t x = rq(g, 240 * Q4PT, 520 * Q4PT);
    int32_t y = rq(g, 200 * Q4PT, 700 * Q4PT);
    if (protected_hit(x - 60 * Q4PT, y - 60 * Q4PT, x + 60 * Q4PT,
                      y + 60 * Q4PT))
        return 0;
    char color[64];
    uint32_t shade = rng_below(g->rng, 40);
    snprintf(color, sizeof(color), "0.3%d 0.5%d 0.2%d rg\n", 0 + shade / 10,
             0 + shade % 10, 0 + shade / 20);
    blob_emit(c, (double)x / Q4PT, (double)y / Q4PT, 40.0, g->rng, color,
              0.35);
    return c->n;
}

/* The map page's verification cluster: legend swatches (one per color
 * model), an even-odd donut, a mitered scale bar, a dashed trail
 * sample and a clipped inset frame — all painted last inside the
 * reserved rectangle, each with exact expected probe colors. */
static void gen_verify_cluster(gen_ctx *g, cbuf *c)
{
    /* legend frame */
    cput(c, "0.25 0.25 0.28 RG\n");
    c_w(c, 2);
    c_re(c, 8 * Q4PT, 8 * Q4PT, (int32_t)(176 * Q4PT), (int32_t)(136 * Q4PT));
    c_paint(c, "S");

    /* legend title */
    cput(c, "0.15 0.15 0.17 rg\n");
    c_text(c, "LEGEND", 16 * Q4PT, 118 * Q4PT, 12, 0);

    /* swatch 1: DeviceRGB */
    cput(c, "0.85 0.3 0.1 rg\n");
    c_re(c, 16 * Q4PT, 92 * Q4PT, 34 * Q4PT, 18 * Q4PT);
    c_paint(c, "f");
    add_probe_user(g, 1, 33, 101, 217, 77, 26);

    /* swatch 2: DeviceGray */
    cput(c, "0.35 g\n");
    c_re(c, 16 * Q4PT, 68 * Q4PT, 34 * Q4PT, 18 * Q4PT);
    c_paint(c, "f");
    add_probe_user(g, 1, 33, 77, 89, 89, 89);

    /* swatch 3: DeviceCMYK */
    cput(c, "0.1 0.6 0 0.05 k\n");
    c_re(c, 16 * Q4PT, 44 * Q4PT, 34 * Q4PT, 18 * Q4PT);
    c_paint(c, "f");
    add_probe_user(g, 1, 33, 53, 218, 97, 242);

    /* even-odd donut */
    cput(c, "0.55 0.45 0.65 rg\n");
    c_re(c, 70 * Q4PT, 16 * Q4PT, 50 * Q4PT, 24 * Q4PT);
    c_re(c, 80 * Q4PT, 22 * Q4PT, 30 * Q4PT, 12 * Q4PT);
    c_paint(c, "f*");
    add_probe_user(g, 1, 73, 28, 140, 115, 166);   /* ring: painted */
    add_probe_user(g, 1, 95, 28, 246, 244, 232);   /* hole: paper */

    /* scale bar: mitered 90-degree corner, w = 3 pt */
    cput(c, "0.1 G\n");
    c_w(c, 12);
    c_pt(c, 140 * Q4PT, 24 * Q4PT, "m");
    c_pt(c, 178 * Q4PT, 24 * Q4PT, "l");
    c_pt(c, 178 * Q4PT, 64 * Q4PT, "l");
    c_paint(c, "S");
    add_probe_user(g, 1, 179, 25, 26, 26, 26);

    /* dashed trail sample, w = 2 pt, dash [8 8] phase 0 */
    cput(c, "0.45 0.25 0.05 RG\n");
    c_w(c, 8);
    cput(c, "[8 8] 0 d\n");
    c_pt(c, 140 * Q4PT, 84 * Q4PT, "m");
    c_pt(c, 186 * Q4PT, 84 * Q4PT, "l");
    c_paint(c, "S");
    cput(c, "[] 0 d\n");
    add_probe_user(g, 1, 144, 84, 115, 64, 13);

    /* clipped inset frame: a big fill seen through a small clip */
    c_q(c);
    c_re(c, 130 * Q4PT, 100 * Q4PT, 56 * Q4PT, 24 * Q4PT);
    cput(c, "W n\n"); /* W and n are both counted operators */
    c->ops += 2;
    cput(c, "0.2 0.5 0.3 rg\n");
    c_re(c, 100 * Q4PT, 80 * Q4PT, 120 * Q4PT, 60 * Q4PT);
    c_paint(c, "f");
    c_Q(c);
    add_probe_user(g, 1, 160, 110, 51, 128, 77);
}

static void map_page(gen_ctx *g, size_t budget)
{
    cbuf *c = &g->content[1];

    gen_paper(c);
    gen_grid(c);

    /* the lake: large CMYK vector art below the element box */
    blob_emit(c, LAKE_CX, LAKE_CY, 62.0, g->rng, "0.88 0.42 0 0.15 k", 0.28);
    add_probe_user(g, 1, LAKE_CX, LAKE_CY, 26, 126, 217);

    unsigned guard = 0;
    while (c->n < budget && guard++ < 200000)
        map_element(g, c);

    gen_verify_cluster(g, c);

    /* map paper probes: top-left device corner, above every element */
    add_probe(g, 1, 16, 16, 246, 244, 232);
}

/* --- PDF file assembly --------------------------------------------------------------*/

static void put_obj_header(cbuf *c, unsigned num, size_t *ofs)
{
    char tmp[48];

    *ofs = c->n;
    snprintf(tmp, sizeof(tmp), "%u 0 obj\n", num);
    cput(c, tmp);
}

static void put_xref_entry(cbuf *c, size_t ofs)
{
    char tmp[32];

    snprintf(tmp, sizeof(tmp), "%010zu 00000 n \n", ofs);
    cput(c, tmp);
}

/* Assemble the two content streams into a full PDF file. */
static uint8_t *build_pdf(const cbuf *p0, const cbuf *p1, size_t *out_len)
{
    cbuf f;
    size_t ofs[7];
    char tmp[128];

    memset(&f, 0, sizeof(f));
    cput(&f, "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n");

    put_obj_header(&f, 1, &ofs[1]);
    cput(&f, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    put_obj_header(&f, 2, &ofs[2]);
    cput(&f, "<< /Type /Pages /Count 2 /MediaBox [0 0 612 792] "
             "/Kids [3 0 R 5 0 R] >>\nendobj\n");

    for (unsigned p = 0; p < 2; p++) {
        unsigned page_obj = 3 + p * 2, cont_obj = 4 + p * 2;
        put_obj_header(&f, page_obj, &ofs[page_obj]);
        snprintf(tmp, sizeof(tmp),
                 "<< /Type /Page /Parent 2 0 R /Contents %u 0 R "
                 "/Resources << >> >>\nendobj\n",
                 cont_obj);
        cput(&f, tmp);
        put_obj_header(&f, cont_obj, &ofs[cont_obj]);
        snprintf(tmp, sizeof(tmp), "<< /Length %zu >>\nstream\n",
                 p == 0 ? p0->n : p1->n);
        cput(&f, tmp);
        cputn(&f, p == 0 ? p0->b : p1->b, p == 0 ? p0->n : p1->n);
        cput(&f, "endstream\nendobj\n");
    }

    size_t xref_ofs = f.n;
    cput(&f, "xref\n0 7\n0000000000 65535 f \n");
    for (unsigned i = 1; i <= 6; i++)
        put_xref_entry(&f, ofs[i]);
    cput(&f, "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n");
    snprintf(tmp, sizeof(tmp), "%zu\n", xref_ofs);
    cput(&f, tmp);
    /* literal cput: %%EOF inside an snprintf format would collapse */
    cput(&f, "%%EOF\n");

    *out_len = f.n;
    return f.b;
}

/* --- the workload context --------------------------------------------------------------*/

struct pdf_ctx {
    uint8_t *doc[PDF_DOCS];
    size_t   doc_len[PDF_DOCS];
    uint32_t pages[PDF_DOCS];
    uint64_t ops[PDF_DOCS];      /* counted ops per document */
    size_t   content_max;        /* largest per-doc content total */

    uint8_t (*stream_sha[PDF_DOCS])[20]; /* one per content stream */
    uint32_t stream_n[PDF_DOCS];
    uint8_t  fb_sha[PDF_DOCS][PDF_PAGES_PER_DOC][20];

    probe  *probes[PDF_DOCS][PDF_PAGES_PER_DOC];
    uint32_t probe_n[PDF_DOCS][PDF_PAGES_PER_DOC];
};

/* Read the page tree's /MediaBox — the cycle verifies the file, not the
 * generator's intentions. Returns -1 when absent or malformed. */
static int media_box_of(pdf_obj *pages, double mb[4])
{
    pdf_obj *box = pdf_dict_get(pages, "MediaBox");

    if (!box || box->kind != PDF_ARRAY || box->u.arr.n != 4)
        return -1;
    for (int i = 0; i < 4; i++) {
        pdf_obj *v = pdf_array_get(box, i);
        if (!v || (v->kind != PDF_INT && v->kind != PDF_REAL))
            return -1;
        mb[i] = v->kind == PDF_INT ? (double)v->u.i : v->u.d;
    }
    return 0;
}

/* Parse a generated document and render every page once, checking the
 * probes; stores the stream digests and the reference framebuffer
 * digests (checked again inside every timed cycle). */
static int self_check(pdf_ctx *ctx, unsigned d, uint8_t *fb, uint8_t *scratch,
                      char *err, size_t errsz)
{
    pdf_file *f = pdf_parse(ctx->doc[d], ctx->doc_len[d], err, errsz);
    if (!f)
        return -1;
    int rc = -1;
    uint32_t stream_idx = 0;
    uint64_t ops_total = 0;
    size_t scratch_n = 0;

    pdf_obj *catalog = f->root_num < f->n_objs ? f->objs[f->root_num].obj
                                               : NULL;
    pdf_obj *pages =
        catalog ? pdf_resolve(f, pdf_dict_get(catalog, "Pages")) : NULL;
    pdf_obj *kids = pages ? pdf_dict_get(pages, "Kids") : NULL;
    if (!kids || kids->kind != PDF_ARRAY || kids->u.arr.n == 0) {
        snprintf(err, errsz, "doc %u: page tree broken", d);
        goto out;
    }
    ctx->pages[d] = kids->u.arr.n;
    ctx->stream_n[d] = 0;

    double mb[4];
    if (media_box_of(pages, mb) != 0) {
        snprintf(err, errsz, "doc %u: missing /MediaBox", d);
        goto out;
    }

    for (uint32_t p = 0; p < kids->u.arr.n; p++) {
        pdf_obj *page = pdf_resolve(f, pdf_array_get(kids, p));
        pdf_obj *contents = page ? pdf_dict_get(page, "Contents") : NULL;
        if (!page || page->kind != PDF_DICT || !contents) {
            snprintf(err, errsz, "doc %u page %u: broken page object", d, p);
            goto out;
        }
        /* collect the content streams (single ref or array of refs) */
        scratch_n = 0;
        if (contents->kind == PDF_REF) {
            pdf_obj *so = pdf_resolve(f, contents);
            pdf_stream *st = so && f->objs[(uint32_t)contents->u.ref.num].stream
                                 ? f->objs[(uint32_t)contents->u.ref.num].stream
                                 : NULL;
            if (!st) {
                snprintf(err, errsz, "doc %u page %u: missing stream", d, p);
                goto out;
            }
            uint8_t *dig = ctx->stream_sha[d][stream_idx];
            sha1(st->data, st->len, dig);
            stream_idx++;
            memcpy(scratch, st->data, st->len);
            scratch_n = st->len;
        } else if (contents->kind == PDF_ARRAY) {
            for (uint32_t i = 0; i < contents->u.arr.n; i++) {
                pdf_obj *item = pdf_resolve(f, contents->u.arr.items[i]);
                if (!item || item->kind != PDF_REF) {
                    snprintf(err, errsz, "doc %u page %u: bad stream ref", d,
                             p);
                    goto out;
                }
                pdf_stream *st = f->objs[(uint32_t)item->u.ref.num].stream;
                if (!st) {
                    snprintf(err, errsz, "doc %u page %u: missing stream", d,
                             p);
                    goto out;
                }
                if (i)
                    scratch[scratch_n++] = '\n';
                uint8_t *dig = ctx->stream_sha[d][stream_idx];
                sha1(st->data, st->len, dig);
                stream_idx++;
                memcpy(scratch + scratch_n, st->data, st->len);
                scratch_n += st->len;
            }
        } else {
            snprintf(err, errsz, "doc %u page %u: bad /Contents", d, p);
            goto out;
        }
        if (scratch_n + 2 > ctx->content_max) {
            snprintf(err, errsz, "doc %u: content exceeds scratch", d);
            goto out;
        }

        uint64_t ops = 0;
        pdf_bitmap fbv = { PDF_PAGE_W, PDF_PAGE_H, fb };
        char rerr[128]; /* renderer's own message, no aliasing with err */
        if (pdf_render_contents(scratch, scratch_n, mb, &fbv, &ops, rerr,
                                sizeof(rerr)) != 0) {
            snprintf(err, errsz, "doc %u page %u: %s", d, p,
                     rerr[0] ? rerr : "render error");
            goto out;
        }
        ops_total += ops;
        ctx->stream_n[d] = stream_idx;

        /* reference digest of the rendered page */
        sha1(fb, (size_t)PDF_PAGE_W * PDF_PAGE_H * 3, ctx->fb_sha[d][p]);

        /* probes */
        for (uint32_t i = 0; i < ctx->probe_n[d][p]; i++) {
            const probe *pr = &ctx->probes[d][p][i];
            const uint8_t *px = fb + ((size_t)pr->y * PDF_PAGE_W + pr->x) * 3;
            if (px[0] != pr->r || px[1] != pr->g || px[2] != pr->b) {
                snprintf(err, errsz,
                         "doc %u page %u probe %u at (%u,%u): got "
                         "%02x%02x%02x, expected %02x%02x%02x",
                         d, p, i, pr->x, pr->y, px[0], px[1], px[2], pr->r,
                         pr->g, pr->b);
                goto out;
            }
        }
    }
    if (ops_total != ctx->ops[d]) {
        snprintf(err, errsz,
                 "doc %u: rendered %llu ops, generator emitted %llu", d,
                 (unsigned long long)ops_total,
                 (unsigned long long)ctx->ops[d]);
        goto out;
    }
    rc = 0;
out:
    pdf_file_free(f);
    return rc;
}

pdf_ctx *pdf_prepare(unsigned seed)
{
    pdf_ctx *ctx = calloc(1, sizeof(*ctx));
    gen_ctx g;
    char err[256];

    if (!ctx)
        return NULL;
    memset(&g, 0, sizeof(g));
    rng_t rng;
    /* independent PRNG stream, uncorrelated with workloads #1 and #2 */
    rng_init(&rng, (uint64_t)seed * 0x9E3779B97F4A7C15ull +
                       0xBF58476D1CE4E5B9ull);
    g.rng = &rng;

    for (unsigned d = 0; d < PDF_DOCS; d++) {
        memset(g.content, 0, sizeof(g.content));
        for (unsigned p = 0; p < PDF_PAGES_PER_DOC; p++) {
            g.probes[p] = NULL;
            g.probe_n[p] = 0;
        }
        g.ops = 0;

        cover_page(&g, d);
        /* the cover filler stops when its budget is met or its columns
         * run out; the map page absorbs whatever remains of the target */
        size_t map_budget = DOC_TARGET[d] > g.content[0].n + 65536
                                ? DOC_TARGET[d] - g.content[0].n
                                : 65536;
        map_page(&g, map_budget);
        ctx->ops[d] = g.content[0].ops + g.content[1].ops;

        ctx->doc[d] = build_pdf(&g.content[0], &g.content[1],
                                &ctx->doc_len[d]);
        if (!ctx->doc[d])
            goto fail;
        ctx->stream_n[d] = PDF_PAGES_PER_DOC;
        ctx->stream_sha[d] = calloc(PDF_PAGES_PER_DOC, sizeof(*ctx->stream_sha[d]));
        if (!ctx->stream_sha[d])
            goto fail;
        size_t content_total = g.content[0].n + g.content[1].n + 16;
        if (content_total > ctx->content_max)
            ctx->content_max = content_total;
        for (unsigned p = 0; p < PDF_PAGES_PER_DOC; p++) {
            ctx->probes[d][p] = g.probes[p];
            ctx->probe_n[d][p] = g.probe_n[p];
            g.probes[p] = NULL; /* ownership moved */
        }

        /* self-check: parse and render the document once, off the clock */
        uint8_t *fb = malloc((size_t)PDF_PAGE_W * PDF_PAGE_H * 3);
        uint8_t *scratch = malloc(ctx->content_max);
        if (!fb || !scratch) {
            free(fb);
            free(scratch);
            goto fail;
        }
        if (self_check(ctx, d, fb, scratch, err, sizeof(err)) != 0) {
            free(fb);
            free(scratch);
            goto fail;
        }
        free(fb);
        free(scratch);

        for (unsigned p = 0; p < PDF_PAGES_PER_DOC; p++) {
            free(g.content[p].b);
            g.content[p].b = NULL;
            g.content[p].n = g.content[p].cap = 0;
            g.content[p].ops = 0;
        }
    }
    return ctx;

fail:
    for (unsigned p = 0; p < PDF_PAGES_PER_DOC; p++) {
        free(g.probes[p]);
        free(g.content[p].b);
    }
    pdf_free(ctx);
    return NULL;
}

void pdf_free(pdf_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned d = 0; d < PDF_DOCS; d++) {
        free(ctx->doc[d]);
        free(ctx->stream_sha[d]);
        for (unsigned p = 0; p < PDF_PAGES_PER_DOC; p++)
            free(ctx->probes[d][p]);
    }
    free(ctx);
}

/* --- the timed cycle ------------------------------------------------------------------------*/

typedef struct {
    pdf_ctx *ctx;
    size_t   doc_begin, doc_end; /* slice of the 16-doc multi set */
    uint8_t *fb;
    uint8_t *scratch;
} pdf_slice;

static int verify_doc(pdf_slice *s, unsigned d, char *err, size_t errsz)
{
    pdf_ctx *ctx = s->ctx;
    pdf_file *f = pdf_parse(ctx->doc[d], ctx->doc_len[d], err, errsz);

    if (!f)
        return -1;
    int rc = -1;
    uint32_t stream_idx = 0;
    uint64_t ops_total = 0;

    pdf_obj *catalog = f->root_num < f->n_objs ? f->objs[f->root_num].obj
                                               : NULL;
    pdf_obj *pages =
        catalog ? pdf_resolve(f, pdf_dict_get(catalog, "Pages")) : NULL;
    pdf_obj *kids = pages ? pdf_dict_get(pages, "Kids") : NULL;
    if (!kids || kids->kind != PDF_ARRAY ||
        kids->u.arr.n != ctx->pages[d]) {
        snprintf(err, errsz, "doc %u: page tree mismatch", d);
        goto out;
    }
    pdf_obj *count = pdf_dict_get(pages, "Count");
    if (!count || count->kind != PDF_INT ||
        (uint32_t)count->u.i != ctx->pages[d]) {
        snprintf(err, errsz, "doc %u: /Count mismatch", d);
        goto out;
    }
    double mb[4];
    if (media_box_of(pages, mb) != 0) {
        snprintf(err, errsz, "doc %u: missing /MediaBox", d);
        goto out;
    }

    for (uint32_t p = 0; p < kids->u.arr.n; p++) {
        pdf_obj *page = pdf_resolve(f, pdf_array_get(kids, p));
        pdf_obj *contents = page ? pdf_dict_get(page, "Contents") : NULL;

        if (!page || page->kind != PDF_DICT || !contents ||
            contents->kind != PDF_REF) {
            snprintf(err, errsz, "doc %u page %u: broken page", d, p);
            goto out;
        }
        pdf_stream *st =
            f->objs[(uint32_t)contents->u.ref.num].stream;
        if (!st) {
            snprintf(err, errsz, "doc %u page %u: missing stream", d, p);
            goto out;
        }
        uint8_t dig[20];
        sha1(st->data, st->len, dig);
        if (memcmp(dig, ctx->stream_sha[d][stream_idx], 20) != 0) {
            snprintf(err, errsz, "doc %u page %u: stream digest mismatch", d,
                     p);
            goto out;
        }
        stream_idx++;
        memcpy(s->scratch, st->data, st->len);

        uint64_t ops = 0;
        pdf_bitmap fbv = { PDF_PAGE_W, PDF_PAGE_H, s->fb };
        char rerr[128]; /* renderer's own message, no aliasing with err */
        if (pdf_render_contents(s->scratch, st->len, mb, &fbv, &ops, rerr,
                                sizeof(rerr)) != 0) {
            snprintf(err, errsz, "doc %u page %u: %s", d, p,
                     rerr[0] ? rerr : "render error");
            goto out;
        }
        ops_total += ops;

        sha1(s->fb, (size_t)PDF_PAGE_W * PDF_PAGE_H * 3, dig);
        if (memcmp(dig, ctx->fb_sha[d][p], 20) != 0) {
            snprintf(err, errsz, "doc %u page %u: framebuffer digest mismatch",
                     d, p);
            goto out;
        }
        for (uint32_t i = 0; i < ctx->probe_n[d][p]; i++) {
            const probe *pr = &ctx->probes[d][p][i];
            const uint8_t *px = s->fb + ((size_t)pr->y * PDF_PAGE_W + pr->x) * 3;
            if (px[0] != pr->r || px[1] != pr->g || px[2] != pr->b) {
                snprintf(err, errsz,
                         "doc %u page %u probe %u: pixel mismatch", d, p, i);
                goto out;
            }
        }
    }
    if (stream_idx != ctx->stream_n[d] || ops_total != ctx->ops[d]) {
        snprintf(err, errsz, "doc %u: op/stream count mismatch", d);
        goto out;
    }
    rc = 0;
out:
    pdf_file_free(f);
    return rc;
}

static int pdf_cycle(void *arg)
{
    pdf_slice *s = arg;
    char err[256];

    for (size_t i = s->doc_begin; i < s->doc_end; i++)
        if (verify_doc(s, (unsigned)(i % PDF_DOCS), err, sizeof(err)) != 0) {
            fprintf(stderr, "lotofbench: pdf-render: %s\n", err);
            return -1;
        }
    return 0;
}

int pdf_run(pdf_ctx *ctx, unsigned runs, unsigned *threads,
            double *throughput_mpixels, uint64_t *pixels)
{
    unsigned t_count = *threads;
    unsigned doc_set = t_count > 1 ? PDF_DOCS_MULTI : PDF_DOCS;

    if (t_count > doc_set)
        t_count = doc_set;

    pdf_slice *slices = calloc(t_count, sizeof(*slices));
    void **ctxs = malloc(t_count * sizeof(*ctxs));
    double elapsed[MAX_RUNS], times[MAX_RUNS];
    int rc = -1;

    if (!slices || !ctxs)
        goto out;

    size_t per_doc = doc_set / t_count;
    unsigned extra = doc_set % t_count;
    size_t begin = 0;

    for (unsigned t = 0; t < t_count; t++) {
        size_t count = per_doc + (t < extra ? 1u : 0u);
        slices[t].ctx = ctx;
        slices[t].doc_begin = begin;
        slices[t].doc_end = begin + count;
        begin += count;
        slices[t].fb = malloc((size_t)PDF_PAGE_W * PDF_PAGE_H * 3);
        slices[t].scratch = malloc(ctx->content_max);
        if (!slices[t].fb || !slices[t].scratch)
            goto out;
        ctxs[t] = &slices[t];
    }

    rc = harness_run(t_count, runs, pdf_cycle, ctxs, elapsed);
    if (rc == 0) {
        uint64_t px =
            (uint64_t)doc_set * PDF_PAGES_PER_DOC * PDF_PAGE_W * PDF_PAGE_H;
        for (unsigned r = 0; r < runs; r++)
            times[r] = elapsed[r] > 0.0 ? (double)px / elapsed[r] : -1.0;
        *throughput_mpixels = lb_median(times, runs) / 1e6;
        *threads = t_count;
        *pixels = px;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++) {
            if (slices[t].fb)
                free(slices[t].fb);
            if (slices[t].scratch)
                free(slices[t].scratch);
        }
    free(slices);
    free(ctxs);
    return rc;
}

/* --- read-only access for tests and tooling ---------------------------------------------*/

const uint8_t *pdf_doc(const pdf_ctx *ctx, unsigned doc)
{
    return ctx && doc < PDF_DOCS ? ctx->doc[doc] : NULL;
}

size_t pdf_doc_len(const pdf_ctx *ctx, unsigned doc)
{
    return ctx && doc < PDF_DOCS ? ctx->doc_len[doc] : 0;
}

unsigned pdf_doc_pages(const pdf_ctx *ctx, unsigned doc)
{
    return ctx && doc < PDF_DOCS ? ctx->pages[doc] : 0;
}

uint64_t pdf_doc_ops(const pdf_ctx *ctx, unsigned doc)
{
    return ctx && doc < PDF_DOCS ? ctx->ops[doc] : 0;
}

unsigned pdf_probe_count(const pdf_ctx *ctx, unsigned doc, unsigned page)
{
    if (!ctx || doc >= PDF_DOCS || page >= PDF_PAGES_PER_DOC)
        return 0;
    return ctx->probe_n[doc][page];
}

int pdf_probe(const pdf_ctx *ctx, unsigned doc, unsigned page, unsigned index,
              uint32_t *x, uint32_t *y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!ctx || doc >= PDF_DOCS || page >= PDF_PAGES_PER_DOC ||
        index >= ctx->probe_n[doc][page])
        return -1;
    const probe *p = &ctx->probes[doc][page][index];
    if (x)
        *x = p->x;
    if (y)
        *y = p->y;
    if (r)
        *r = p->r;
    if (g)
        *g = p->g;
    if (b)
        *b = p->b;
    return 0;
}
