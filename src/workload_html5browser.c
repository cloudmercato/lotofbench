/* LotofBench — HTML5 Browser workload (workload #3).
 *
 * Mirrors the third Geekbench 6 CPU workload: a headless browser that
 * opens, parses, lays out and renders text and images for web pages.
 * Geekbench 6 drives Gumbo (HTML parser), litehtml (CSS layout),
 * FreeType (fonts), AGG (2D rasterizer) and libjpeg/libpng (image
 * codecs) over snapshots of popular sites. LotofBench reproduces the
 * shape of that pipeline dependency-free, with the same honesty rules
 * as the other workloads:
 *
 *   - deterministic synthetic input — a seeded set of HTML pages in six
 *     site archetypes (portal, article, wiki, feed, search results,
 *     landing page), full of paragraphs, inline elements, images,
 *     attributes, comments and entities. Fixed --seed means
 *     byte-identical pages on every machine, so scores are comparable
 *     across runs. The pages are generated off the clock — the stand-in
 *     for the network fetch;
 *   - the full pipeline inside the timed cycle — every page is
 *     tokenized into a DOM, laid out into a display list of boxes, text
 *     runs and image boxes, and rasterized into an 800x600 RGBA
 *     framebuffer. All arithmetic is integer, so the rendered pixels
 *     are identical on every machine;
 *   - correctness inside the timed cycle — a pass only counts if every
 *     page reproduces its reference element count, display-list record
 *     count and rendered-pixmap digest (FNV-1a over every pixel write);
 *     any divergence aborts the run instead of scoring;
 *   - shared-task multi-core through the common harness — the page set
 *     is split into equal contiguous slices, one per thread.
 *
 * The throughput metric is pages: the score measures how many pages per
 * second the machine opens, parses, lays out and renders over the whole
 * 32-page set. */
/* Workload #3 — HTML5 Browser. See workload_html5browser.h. */

#include "workload_html5browser.h"

#include "harness.h"
#include "lotofbench.h"
#include "rng.h"

#include <stdlib.h>
#include <string.h>

#define NONE32 UINT32_MAX

/* --- tags, classes, styles ---------------------------------------------------*/

enum {
    T_HTML, T_HEAD, T_TITLE, T_META, T_LINK, T_BODY, T_HEADER, T_NAV,
    T_MAIN, T_SECTION, T_ARTICLE, T_ASIDE, T_FOOTER, T_H1, T_H2, T_H3,
    T_P, T_UL, T_LI, T_A, T_SPAN, T_STRONG, T_EM, T_DIV, T_IMG, T_BR,
    T_HR, T_TAGCOUNT,
    T_TEXT = 255 /* text node */
};

static const char *const TAG_NAMES[T_TAGCOUNT] = {
    "html", "head", "title", "meta", "link", "body", "header", "nav",
    "main", "section", "article", "aside", "footer", "h1", "h2", "h3",
    "p", "ul", "li", "a", "span", "strong", "em", "div", "img", "br",
    "hr"
};

/* Void elements never take children; hidden ones parse but do not lay
 * out (the whole <head> subtree). */
static const uint8_t TAG_VOID[T_TAGCOUNT] = {
    0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 1, 1
};
static const uint8_t TAG_HIDDEN[T_TAGCOUNT] = {
    0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0
};
static const uint8_t TAG_INLINE[T_TAGCOUNT] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
    1, 0, 0, 0, 0
};

enum {
    C_NONE, C_HERO, C_LEAD, C_NOTE, C_SIDE, C_AD, C_CAPTION, C_CHIP,
    C_TAGGY, C_META, C_CARD, C_BUTTON, C_PHOTO, C_CHART, C_AVATAR,
    C_CLASSCOUNT
};

static const char *const CLASS_NAMES[C_CLASSCOUNT] = {
    "", "hero", "lead", "note", "side", "ad", "caption", "chip", "tag",
    "meta", "card", "button", "photo", "chart", "avatar"
};

/* Image pattern ids (REC_IMG color field). */
#define PAT_PHOTO  HTML5_PAT_PHOTO
#define PAT_CHART  HTML5_PAT_CHART
#define PAT_AVATAR HTML5_PAT_AVATAR

/* Resolved style of one box. Integer-only: every field feeds the layout
 * arithmetic directly. font 0 = "inherit from the enclosing block"
 * (inline tags only). */
typedef struct {
    uint8_t bg, fg;    /* palette indices */
    uint8_t font;      /* text pixel size */
    uint8_t bold;
    uint8_t width_pct; /* of the parent content width */
    uint8_t pad;       /* padding, px per side */
    uint8_t margin_v;  /* vertical margin, px top and bottom */
    uint8_t border;    /* border thickness, px */
    uint8_t pattern;   /* image pattern id */
    uint8_t hidden;
} style;

#define ST(bgc, fgc, f, bo, wp, pd, mg, br, pa, hi) \
    { bgc, fgc, f, bo, wp, pd, mg, br, pa, hi }

static const style TAG_STYLE[T_TAGCOUNT] = {
    ST(0, 1, 16, 0, 100, 0, 0, 0, 0, 0),  /* html */
    ST(0, 1, 16, 0, 100, 0, 0, 0, 0, 1),  /* head */
    ST(0, 1, 16, 0, 100, 0, 0, 0, 0, 1),  /* title */
    ST(0, 1, 16, 0, 100, 0, 0, 0, 0, 1),  /* meta */
    ST(0, 1, 16, 0, 100, 0, 0, 0, 0, 1),  /* link */
    ST(0, 1, 16, 0, 100, 8, 0, 0, 0, 0),  /* body */
    ST(14, 0, 16, 0, 100, 10, 0, 0, 0, 0),/* header */
    ST(3, 1, 12, 0, 100, 4, 4, 0, 0, 0),  /* nav */
    ST(0, 1, 16, 0, 100, 4, 0, 0, 0, 0),  /* main */
    ST(15, 1, 16, 0, 100, 6, 6, 1, 0, 0), /* section */
    ST(0, 1, 16, 0, 100, 6, 6, 1, 0, 0),  /* article */
    ST(3, 1, 12, 0, 30, 6, 6, 1, 0, 0),   /* aside */
    ST(2, 0, 12, 0, 100, 8, 8, 0, 0, 0),  /* footer */
    ST(0, 1, 28, 1, 100, 0, 6, 0, 0, 0),  /* h1 */
    ST(0, 1, 20, 1, 100, 0, 5, 0, 0, 0),  /* h2 */
    ST(0, 1, 16, 1, 100, 0, 4, 0, 0, 0),  /* h3 */
    ST(0, 1, 16, 0, 100, 0, 3, 0, 0, 0),  /* p */
    ST(0, 1, 16, 0, 100, 0, 3, 0, 0, 0),  /* ul */
    ST(0, 1, 16, 0, 100, 0, 1, 0, 0, 0),  /* li */
    ST(0, 4, 0, 0, 100, 0, 0, 0, 0, 0),   /* a */
    ST(0, 0, 0, 0, 100, 0, 0, 0, 0, 0),   /* span */
    ST(0, 0, 0, 1, 100, 0, 0, 0, 0, 0),   /* strong */
    ST(0, 2, 0, 0, 100, 0, 0, 0, 0, 0),   /* em */
    ST(0, 1, 16, 0, 100, 0, 0, 0, 0, 0),  /* div */
    ST(0, 1, 16, 0, 100, 0, 4, 0, PAT_PHOTO, 0), /* img */
    ST(0, 1, 16, 0, 100, 0, 0, 0, 0, 0),  /* br */
    ST(0, 1, 16, 0, 100, 0, 0, 0, 0, 0),  /* hr */
};

/* A class replaces the tag's whole style (except on img, where it only
 * picks the pattern) — the generator pairs each class with tags it was
 * written for, so full replacement is predictable. */
static const style CLASS_STYLE[C_CLASSCOUNT] = {
    ST(0, 1, 16, 0, 100, 0, 0, 0, 0, 0),   /* (none) */
    ST(14, 0, 0, 0, 100, 12, 0, 0, 0, 0),  /* hero */
    ST(0, 2, 20, 0, 100, 0, 4, 0, 0, 0),   /* lead */
    ST(0, 2, 12, 0, 100, 0, 2, 0, 0, 0),   /* note */
    ST(3, 1, 12, 0, 30, 8, 6, 0, 0, 0),    /* side */
    ST(8, 1, 16, 0, 100, 8, 8, 0, 0, 0),   /* ad */
    ST(0, 2, 12, 0, 100, 0, 2, 0, 0, 0),   /* caption */
    ST(0, 4, 12, 0, 100, 0, 0, 0, 0, 0),   /* chip */
    ST(0, 4, 12, 0, 100, 0, 0, 0, 0, 0),   /* tag */
    ST(0, 2, 12, 0, 100, 0, 2, 0, 0, 0),   /* meta */
    ST(15, 1, 16, 0, 100, 8, 8, 1, 0, 0),  /* card */
    ST(4, 0, 16, 0, 100, 6, 4, 0, 0, 0),   /* button */
    ST(0, 1, 16, 0, 100, 0, 4, 0, PAT_PHOTO, 0),  /* photo */
    ST(0, 1, 16, 0, 100, 0, 4, 0, PAT_CHART, 0),  /* chart */
    ST(0, 1, 16, 0, 100, 0, 4, 0, PAT_AVATAR, 0), /* avatar */
};

/* Fixed 16-color palette (RGBA bytes). Part of the rendered-pixmap
 * contract: changing any entry moves every pixmap digest. */
static const uint8_t PALETTE[16][4] = {
    { 255, 255, 255, 255 }, { 26, 26, 26, 255 },
    { 90, 90, 90, 255 },    { 216, 216, 216, 255 },
    { 32, 64, 192, 255 },   { 64, 160, 255, 255 },
    { 192, 64, 64, 255 },   { 64, 160, 96, 255 },
    { 224, 192, 64, 255 },  { 128, 64, 160, 255 },
    { 224, 128, 64, 255 },  { 64, 192, 192, 255 },
    { 240, 224, 208, 255 }, { 128, 96, 64, 255 },
    { 48, 72, 96, 255 },    { 248, 248, 240, 255 }
};

#define BORDER_COLOR 2u /* gray borders everywhere */

/* --- the synthetic 5x7 font --------------------------------------------------
 *
 * A fixed bitmap font, 7 rows of 5 bits per glyph (bit 4 = leftmost
 * column). Part of the pixmap contract. Uppercase maps to the lowercase
 * glyph; anything outside the table renders as a filled box, so no byte
 * can break determinism. Glyph shapes are deliberately simple — the
 * workload measures layout and blitting, not font rasterization. */

#define FONT_ROWS     7
#define GLYPH_FALLBACK (36 + 13) /* after 26 letters, 10 digits, 13 punct */

static const char PUNCT_CHARS[] = ".,:;!?-()/'\"&";

static const uint8_t FONT[][FONT_ROWS] = {
    { 0x00, 0x00, 0x0E, 0x11, 0x1F, 0x11, 0x0F }, /* a */
    { 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E }, /* b */
    { 0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E }, /* c */
    { 0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0E }, /* d */
    { 0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E }, /* e */
    { 0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08 }, /* f */
    { 0x00, 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x0E }, /* g */
    { 0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11 }, /* h */
    { 0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E }, /* i */
    { 0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C }, /* j */
    { 0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12 }, /* k */
    { 0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* l */
    { 0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15 }, /* m */
    { 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11 }, /* n */
    { 0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E }, /* o */
    { 0x00, 0x16, 0x19, 0x11, 0x16, 0x10, 0x10 }, /* p */
    { 0x00, 0x0B, 0x15, 0x11, 0x0F, 0x01, 0x01 }, /* q */
    { 0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10 }, /* r */
    { 0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E }, /* s */
    { 0x04, 0x04, 0x1E, 0x04, 0x04, 0x04, 0x02 }, /* t */
    { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D }, /* u */
    { 0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04 }, /* v */
    { 0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A }, /* w */
    { 0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11 }, /* x */
    { 0x00, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, /* y */
    { 0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F }, /* z */
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, /* 9 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C }, /* . */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04 }, /* , */
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 }, /* : */
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08 }, /* ; */
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 }, /* ! */
    { 0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04 }, /* ? */
    { 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00 }, /* - */
    { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 }, /* ( */
    { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 }, /* ) */
    { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 }, /* / */
    { 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* ' */
    { 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* " */
    { 0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D }, /* & */
    { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F }, /* fallback box */
};

/* Glyph scale: fonts below 20 px blit 5x7 cells, larger ones 10x14.
 * Shared by the layout (measurement) and the rasterizer (blitting). */
static int font_scale(unsigned font_px)
{
    return font_px >= 20 ? 2 : 1;
}

static int font_advance(unsigned font_px)
{
    return (int)(font_px * 3 / 5); /* 12->7, 16->9, 20->12, 28->16 */
}

static int glyph_index(uint8_t c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a';
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= '0' && c <= '9')
        return 26 + (c - '0');
    if (c) {
        const char *p = strchr(PUNCT_CHARS, c);

        if (p)
            return 36 + (int)(p - PUNCT_CHARS);
    }
    return GLYPH_FALLBACK;
}

/* --- scratch arenas -----------------------------------------------------------*/

typedef struct {
    uint32_t first, last, next; /* child list + next sibling */
    uint16_t tag;
    uint16_t cls;
    uint16_t img_w, img_h;
    uint32_t txt_off, txt_len;  /* text nodes: bytes in the scratch txt arena */
} h5_node;

typedef struct {
    h5_node *dom;
    size_t dom_n, dom_cap;
    uint32_t *stk; /* open-element stack */
    size_t stk_n, stk_cap;
    html5_rec *recs;
    size_t rec_n, rec_cap;
    uint8_t *txt; /* entity-decoded text bytes */
    size_t txt_n, txt_cap;
    uint8_t *fb;
    int oom;
} h5_scratch;

static void scratch_reset(h5_scratch *sc)
{
    sc->dom_n = 0;
    sc->stk_n = 0;
    sc->rec_n = 0;
    sc->txt_n = 0;
    sc->oom = 0;
}

static void scratch_free(h5_scratch *sc)
{
    free(sc->dom);
    free(sc->stk);
    free(sc->recs);
    free(sc->txt);
    free(sc->fb);
    memset(sc, 0, sizeof(*sc));
}

static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
    size_t ncap = *cap ? *cap : 64;
    void *np;

    while (ncap < need)
        ncap *= 2;
    np = realloc(p, ncap * elem);
    if (np)
        *cap = ncap;
    return np;
}

static uint32_t node_new(h5_scratch *sc, uint16_t tag, uint32_t parent)
{
    h5_node *d;

    if (sc->dom_n >= sc->dom_cap) {
        d = grow(sc->dom, &sc->dom_cap, sc->dom_n + 1, sizeof(*d));
        if (!d) {
            sc->oom = 1;
            return NONE32;
        }
        sc->dom = d;
    }
    d = &sc->dom[sc->dom_n];
    d->first = NONE32;
    d->last = NONE32;
    d->next = NONE32;
    d->tag = tag;
    d->cls = 0;
    d->img_w = 0;
    d->img_h = 0;
    d->txt_off = 0;
    d->txt_len = 0;
    if (parent != NONE32) {
        h5_node *p = &sc->dom[parent];

        if (p->last != NONE32)
            sc->dom[p->last].next = sc->dom_n;
        else
            p->first = sc->dom_n;
        p->last = sc->dom_n;
    }
    return sc->dom_n++;
}

static int txt_push(h5_scratch *sc, uint8_t c)
{
    if (sc->txt_n >= sc->txt_cap) {
        uint8_t *t = grow(sc->txt, &sc->txt_cap, sc->txt_n + 1, 1);

        if (!t) {
            sc->oom = 1;
            return -1;
        }
        sc->txt = t;
    }
    sc->txt[sc->txt_n++] = c;
    return 0;
}

static size_t rec_push(h5_scratch *sc, const html5_rec *r)
{
    if (sc->rec_n >= sc->rec_cap) {
        html5_rec *a = grow(sc->recs, &sc->rec_cap, sc->rec_n + 1,
                            sizeof(*a));

        if (!a) {
            sc->oom = 1;
            return 0;
        }
        sc->recs = a;
    }
    sc->recs[sc->rec_n] = *r;
    return sc->rec_n++;
}

/* A laid-out page: the display list the rasterizer consumes. */
struct html5_layout {
    html5_rec *recs;
    uint32_t n;
};

/* --- entity decoding ----------------------------------------------------------*/

static int is_space(uint8_t c)
{
    return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

/* Decode one entity at `s` (which points at '&'). Writes at most 4
 * bytes to `out`, stores the count in *outn, returns the number of
 * input bytes consumed — 0 when it is not a supported entity. */
static size_t decode_entity(const uint8_t *s, size_t avail, uint8_t *out,
                            size_t *outn)
{
    static const struct { const char *name; uint8_t ch; } NAMED[] = {
        { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' },
        { "quot;", '"' }, { "apos;", '\'' }
    };

    *outn = 0;
    if (avail < 3 || s[0] != '&')
        return 0;
    for (size_t i = 0; i < 5; i++) {
        size_t n = strlen(NAMED[i].name);

        if (avail >= n + 1 && memcmp(s + 1, NAMED[i].name, n) == 0) {
            out[0] = NAMED[i].ch;
            *outn = 1;
            return n + 1;
        }
    }
    if (s[1] == '#') { /* numeric: &#NN; */
        size_t k = 2;
        unsigned v = 0;

        while (k < avail && s[k] >= '0' && s[k] <= '9' && k < 6) {
            v = v * 10 + (unsigned)(s[k] - '0');
            k++;
        }
        if (k > 2 && k < avail && s[k] == ';') {
            out[0] = v == 0 ? '?' : (v > 255 ? '?' : (uint8_t)v);
            *outn = 1;
            return k + 1;
        }
    }
    return 0;
}

/* --- the tokenizer ------------------------------------------------------------*/

static uint16_t tag_lookup(const uint8_t *name, size_t n)
{
    for (uint16_t t = 0; t < T_TAGCOUNT; t++)
        if (strlen(TAG_NAMES[t]) == n &&
            memcmp(TAG_NAMES[t], name, n) == 0)
            return t;
    return T_SPAN; /* unknown tags parse as generic inline containers */
}

static uint16_t class_lookup(const uint8_t *v, size_t n)
{
    if (n == 0 || n > 16)
        return C_NONE;
    for (uint16_t c = 1; c < C_CLASSCOUNT; c++)
        if (strlen(CLASS_NAMES[c]) == n &&
            memcmp(CLASS_NAMES[c], v, n) == 0)
            return c;
    return C_NONE;
}

static unsigned attr_number(const uint8_t *v, size_t n)
{
    unsigned val = 0;

    if (n == 0 || n > 4)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9')
            return 0;
        val = val * 10 + (unsigned)(v[i] - '0');
    }
    return val > 2000 ? 2000 : val;
}

/* Parse one document into the scratch DOM. Returns 0, or -1 on
 * malformed input (unterminated tag/comment/quoted attribute). */
static int parse_doc(h5_scratch *sc, const uint8_t *src, size_t len,
                     uint64_t *elts_out)
{
    uint64_t elts = 0;
    size_t i = 0;

    scratch_reset(sc);
    while (i < len) {
        if (src[i] != '<') { /* text run: decode entities, keep words */
            size_t b = i, off0 = sc->txt_n;
            int nonspace = 0;

            while (i < len && src[i] != '<')
                i++;
            for (size_t k = b; k < i;) {
                if (src[k] == '&') {
                    uint8_t dec[4];
                    size_t dn = 0, used = decode_entity(src + k, i - k,
                                                        dec, &dn);

                    if (used) {
                        for (size_t q = 0; q < dn; q++) {
                            if (!is_space(dec[q]))
                                nonspace = 1;
                            if (txt_push(sc, dec[q]) != 0)
                                return -1;
                        }
                        k += used;
                        continue;
                    }
                }
                if (!is_space(src[k]))
                    nonspace = 1;
                if (txt_push(sc, src[k]) != 0)
                    return -1;
                k++;
            }
            if (nonspace) {
                uint32_t parent = sc->stk_n ? sc->stk[sc->stk_n - 1]
                                            : NONE32;
                uint32_t id = node_new(sc, T_TEXT, parent);

                if (id == NONE32)
                    return -1;
                sc->dom[id].txt_off = (uint32_t)off0;
                sc->dom[id].txt_len = (uint32_t)(sc->txt_n - off0);
            }
            continue;
        }

        /* markup */
        if (i + 1 >= len)
            return -1;
        if (src[i + 1] == '!') { /* comment or doctype */
            if (i + 3 < len && src[i + 2] == '-' && src[i + 3] == '-') {
                size_t k = i + 4;

                while (k + 2 < len &&
                       !(src[k] == '-' && src[k + 1] == '-' &&
                         src[k + 2] == '>'))
                    k++;
                if (k + 2 >= len)
                    return -1;
                i = k + 3;
            } else {
                size_t k = i + 2;

                while (k < len && src[k] != '>')
                    k++;
                if (k >= len)
                    return -1;
                i = k + 1;
            }
            continue;
        }
        if (src[i + 1] == '/') { /* closing tag */
            size_t k = i + 2;
            uint16_t tag;
            size_t name_len;

            while (k < len && src[k] != '>')
                k++;
            if (k >= len)
                return -1;
            name_len = 0;
            for (size_t m = i + 2; m < k; m++) {
                uint8_t c = src[m];

                if (is_space(c) || c == '/')
                    break;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-')
                    name_len++;
                else
                    break;
            }
            tag = tag_lookup(src + i + 2, name_len);
            while (sc->stk_n) { /* pop to the nearest matching open tag */
                if (sc->dom[sc->stk[sc->stk_n - 1]].tag == tag) {
                    sc->stk_n--;
                    break;
                }
                sc->stk_n--;
            }
            i = k + 1;
            continue;
        }
        if (!((src[i + 1] >= 'a' && src[i + 1] <= 'z') ||
              (src[i + 1] >= 'A' && src[i + 1] <= 'Z'))) {
            /* stray '<' — honest text, not markup */
            uint32_t parent = sc->stk_n ? sc->stk[sc->stk_n - 1] : NONE32;
            uint32_t id = node_new(sc, T_TEXT, parent);
            size_t off0 = sc->txt_n;

            if (id == NONE32)
                return -1;
            if (txt_push(sc, '<') != 0)
                return -1;
            sc->dom[id].txt_off = (uint32_t)off0;
            sc->dom[id].txt_len = 1;
            i++;
            continue;
        }

        /* opening tag */
        {
            size_t k = i + 1, name_start = k;
            uint16_t tag;
            uint16_t cls = C_NONE;
            unsigned img_w = 0, img_h = 0;
            int selfclose = 0;
            uint32_t id;

            while (k < len &&
                   ((src[k] >= 'a' && src[k] <= 'z') ||
                    (src[k] >= 'A' && src[k] <= 'Z') ||
                    (src[k] >= '0' && src[k] <= '9') || src[k] == '-'))
                k++;
            tag = tag_lookup(src + name_start, k - name_start);
            i = k;

            for (;;) { /* attributes */
                while (i < len && is_space(src[i]))
                    i++;
                if (i >= len)
                    return -1;
                if (src[i] == '>') {
                    i++;
                    break;
                }
                if (src[i] == '/') {
                    i++;
                    if (i < len && src[i] == '>') {
                        i++;
                        selfclose = 1;
                        break;
                    }
                    continue;
                }
                size_t an = i;

                while (i < len && !is_space(src[i]) && src[i] != '=' &&
                       src[i] != '>' && src[i] != '/')
                    i++;
                if (i == an)
                    return -1;
                size_t name_len = i - an;
                size_t vn = 0;
                const uint8_t *vp = NULL;

                while (i < len && is_space(src[i]))
                    i++;
                if (i < len && src[i] == '=') {
                    i++;
                    while (i < len && is_space(src[i]))
                        i++;
                    if (i >= len)
                        return -1;
                    if (src[i] == '"' || src[i] == '\'') {
                        uint8_t q = src[i++];

                        vp = src + i;
                        while (i < len && src[i] != q)
                            i++;
                        if (i >= len)
                            return -1;
                        vn = (size_t)(src + i - vp);
                        i++;
                    } else {
                        vp = src + i;
                        while (i < len && !is_space(src[i]) &&
                               src[i] != '>')
                            i++;
                        vn = (size_t)(src + i - vp);
                    }
                }
                /* the three attributes the engine actually consumes */
                if (vp && vn) {
                    if (name_len == 5 &&
                        memcmp(src + an, "class", 5) == 0)
                        cls = class_lookup(vp, vn);
                    else if (tag == T_IMG && name_len == 5 &&
                             memcmp(src + an, "width", 5) == 0)
                        img_w = (uint16_t)attr_number(vp, vn);
                    else if (tag == T_IMG && name_len == 6 &&
                             memcmp(src + an, "height", 6) == 0)
                        img_h = (uint16_t)attr_number(vp, vn);
                }
            }

            id = node_new(sc, tag, sc->stk_n ? sc->stk[sc->stk_n - 1]
                                             : NONE32);
            if (id == NONE32)
                return -1;
            sc->dom[id].cls = cls;
            sc->dom[id].img_w = (uint16_t)img_w;
            sc->dom[id].img_h = (uint16_t)img_h;
            elts++;
            if (!TAG_VOID[tag] && !selfclose) {
                if (sc->stk_n >= sc->stk_cap) {
                    uint32_t *st = grow(sc->stk, &sc->stk_cap,
                                        sc->stk_n + 1, sizeof(*st));

                    if (!st)
                        return -1;
                    sc->stk = st;
                }
                sc->stk[sc->stk_n++] = id;
            }
        }
    }
    *elts_out = elts;
    return 0;
}

/* --- layout --------------------------------------------------------------------
 *
 * Two record kinds come out of layout directly (backgrounds, borders)
 * and two are accumulated along the way (text runs, image boxes).
 * Records are appended in paint order: a box's background lands before
 * its children, so overdraw mimics a real browser's painting. Box
 * heights are patched after the children are laid out (the record index
 * is kept, the array only ever grows). All arithmetic is integer. */

typedef struct {
    int32_t ix0, ix1;   /* content bounds of the current flow */
    int32_t pen_x;      /* inline pen */
    int32_t line_top;   /* top of the current line */
    int32_t line_h;     /* height accrued on the current line */
    int open;
} flow;

typedef struct {
    h5_scratch *sc;
    const h5_node *nodes;
    uint8_t page;      /* image phase seed */
    uint16_t img_n;    /* images so far on the page */
    uint8_t font, fg, bold; /* current inline style */
    int oom;
} L;

static style resolve(uint16_t tag, uint16_t cls)
{
    style s = TAG_STYLE[tag];

    if (cls != C_NONE) {
        const style *c = &CLASS_STYLE[cls];

        if (tag == T_IMG)
            s.pattern = c->pattern;
        else
            s = *c;
    }
    return s;
}

static void place_word(L *s, flow *f, const uint8_t *w_, uint32_t len)
{
    html5_rec r;
    int adv = font_advance(s->font);
    int k = font_scale(s->font);
    int32_t wlh = (int32_t)s->font + 6;

    if (len > 512) /* keep words bounded whatever the input says */
        len = 512;
    if (f->pen_x > f->ix0 && f->pen_x + (int32_t)len * adv > f->ix1) {
        f->line_top += f->line_h;
        f->line_h = 0;
        f->pen_x = f->ix0;
    }
    memset(&r, 0, sizeof(r));
    r.kind = HTML5_REC_TEXT;
    r.color = s->fg;
    r.aux = s->bold;
    r.font = s->font;
    r.x = f->pen_x;
    r.y = f->line_top + (wlh - 7 * k) / 2;
    r.w = (int32_t)len * adv;
    r.h = 7 * k;
    r.s = w_;
    r.slen = (uint16_t)len;
    rec_push(s->sc, &r);
    f->pen_x += (int32_t)len * adv + adv; /* the word plus one space */
    if (wlh > f->line_h)
        f->line_h = wlh;
}

static void emit_words(L *s, flow *f, const uint8_t *p, uint32_t n)
{
    uint32_t i = 0;

    while (i < n) {
        uint32_t b;

        while (i < n && is_space(p[i]))
            i++;
        b = i;
        while (i < n && !is_space(p[i]))
            i++;
        if (i > b)
            place_word(s, f, p + b, i - b);
    }
}

/* Inline subtrees (a, span, strong, em, and anything nested inside
 * them) contribute words to the enclosing flow with their own color and
 * weight; unexpected block descendants pass through transparently. */
static void lay_inline(L *s, flow *f, uint32_t id)
{
    uint8_t of = s->font, og = s->fg, ob = s->bold;
    const style st = resolve(s->nodes[id].tag, s->nodes[id].cls);

    if (st.fg)
        s->fg = st.fg;
    if (st.bold)
        s->bold = 1;
    if (st.font)
        s->font = st.font;
    for (uint32_t c = s->nodes[id].first; c != NONE32;
         c = s->nodes[c].next) {
        const h5_node *n = &s->nodes[c];

        if (n->tag == T_TEXT)
            emit_words(s, f, s->sc->txt + n->txt_off, n->txt_len);
        else
            lay_inline(s, f, c);
    }
    s->font = of;
    s->fg = og;
    s->bold = ob;
}

static int32_t lay_block(L *s, uint32_t id, int32_t cx0, int32_t cx1,
                         int32_t ytop);

/* Lay out the children of `id` between the content bounds; returns the
 * bottom of the content. */
static int32_t lay_children(L *s, uint32_t id, int32_t ix0, int32_t ix1,
                            int32_t y)
{
    flow f = { ix0, ix1, ix0, y, 0, 0 };
    int32_t cur = y, bottom = y;
    const h5_node *nodes = s->nodes;

    for (uint32_t c = nodes[id].first; c != NONE32; c = nodes[c].next) {
        const h5_node *n = &nodes[c];

        if (n->tag < T_TAGCOUNT && TAG_HIDDEN[n->tag])
            continue;
        if (n->tag == T_TEXT ||
            (n->tag < T_TAGCOUNT && TAG_INLINE[n->tag])) {
            if (!f.open) {
                f.open = 1;
                f.pen_x = ix0;
                f.line_top = cur;
                f.line_h = 0;
            }
            if (n->tag == T_TEXT)
                emit_words(s, &f, s->sc->txt + n->txt_off, n->txt_len);
            else
                lay_inline(s, &f, c);
            int32_t lb = f.line_top + f.line_h;

            if (lb > bottom)
                bottom = lb;
            continue;
        }
        if (n->tag == T_BR) { /* forced line break */
            if (f.open) {
                cur = f.line_top + f.line_h;
                if (cur > bottom)
                    bottom = cur;
                f.open = 0;
            }
            continue;
        }
        /* block child: close any open line first (anonymous-box rule) */
        if (f.open) {
            cur = f.line_top + f.line_h;
            f.open = 0;
        }
        int32_t b = lay_block(s, c, ix0, ix1, cur);

        cur = b;
        if (b > bottom)
            bottom = b;
    }
    if (f.open) {
        int32_t lb = f.line_top + f.line_h;

        if (lb > bottom)
            bottom = lb;
    }
    return bottom;
}

static int32_t lay_block(L *s, uint32_t id, int32_t cx0, int32_t cx1,
                         int32_t ytop)
{
    const h5_node *n = &s->nodes[id];
    const style st = resolve(n->tag, n->cls);
    html5_rec r;
    int32_t m = st.margin_v;
    int32_t y = ytop + m;

    if (st.hidden)
        return ytop;
    memset(&r, 0, sizeof(r));

    if (n->tag == T_IMG) {
        int32_t w = n->img_w ? (int32_t)n->img_w : 240;
        int32_t h = n->img_h ? (int32_t)n->img_h : 160;

        if (w > cx1 - cx0)
            w = cx1 - cx0;
        if (w < 8)
            w = 8;
        if (h < 8)
            h = 8;
        if (h > 480)
            h = 480;
        r.kind = HTML5_REC_IMG;
        r.color = st.pattern ? st.pattern : PAT_PHOTO;
        r.aux = (uint8_t)((s->page * 11u + s->img_n * 37u) & 0xFFu);
        s->img_n++;
        r.x = cx0;
        r.y = y;
        r.w = w;
        r.h = h;
        rec_push(s->sc, &r);
        return y + h + m;
    }
    if (n->tag == T_HR) { /* a thin rule on its own line */
        r.kind = HTML5_REC_BG;
        r.color = 3;
        r.x = cx0;
        r.y = y + 6;
        r.w = cx1 - cx0 > 0 ? cx1 - cx0 : 1;
        r.h = 2;
        rec_push(s->sc, &r);
        return y + 14;
    }

    int32_t w = (cx1 - cx0) * st.width_pct / 100;

    if (w < 16)
        w = 16;
    if (w > cx1 - cx0)
        w = cx1 - cx0;
    int32_t x1 = cx0 + w;

    r.kind = HTML5_REC_BG;
    r.color = st.bg;
    r.x = cx0;
    r.y = y;
    r.w = w;
    r.h = 0; /* patched once the children are placed */
    uint32_t bg_i = rec_push(s->sc, &r);
    uint32_t b_i = 0;

    if (st.border) {
        r.kind = HTML5_REC_BORDER;
        r.color = BORDER_COLOR;
        r.aux = st.border;
        r.h = 0;
        b_i = rec_push(s->sc, &r);
    }
    int32_t b = st.border, p = st.pad;
    int32_t ix0 = cx0 + b + p, ix1 = x1 - b - p;

    if (ix1 < ix0)
        ix1 = ix0;
    int32_t bottom;

    if (n->tag == T_LI && ix0 + 16 <= ix1)
        bottom = lay_children(s, id, ix0 + 16, ix1, y + b + p);
    else
        bottom = lay_children(s, id, ix0, ix1, y + b + p);
    int32_t outer_bottom = bottom + p + b;

    if (!s->sc->oom) {
        s->sc->recs[bg_i].h = outer_bottom - y;
        if (st.border)
            s->sc->recs[b_i].h = outer_bottom - y;
    }
    return outer_bottom + m;
}

/* --- painting ------------------------------------------------------------------
 *
 * Scanline rasterizer into an RGBA8 framebuffer. Paint order = record
 * order; records clip against the viewport. `digest`, when not NULL,
 * folds every pixel write into an FNV-1a-64 state: the timed cycle
 * verifies the whole rendering (values, order and coverage), not just
 * the final bytes, at almost no extra cost. */

#define FB_W ((int32_t)HTML5_VIEW_W)
#define FB_H ((int32_t)HTML5_VIEW_H)

typedef struct {
    uint8_t *fb;
    uint64_t *dig; /* FNV-1a state, may be NULL */
} paint_ctx;

static void px_fold(paint_ctx *p, int32_t x, int32_t y, const uint8_t *col)
{
    uint8_t *o = p->fb + ((size_t)y * FB_W + (size_t)x) * 4;

    o[0] = col[0];
    o[1] = col[1];
    o[2] = col[2];
    o[3] = 255;
    if (p->dig) {
        uint32_t v = (uint32_t)col[0] | ((uint32_t)col[1] << 8) |
                     ((uint32_t)col[2] << 16) | (1u << 24);

        *p->dig = (*p->dig ^ v) * 0x100000001B3ull;
    }
}

static void fill_rect(paint_ctx *p, int32_t x, int32_t y, int32_t w,
                      int32_t h, const uint8_t *col)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > FB_W)
        w = FB_W - x;
    if (y + h > FB_H)
        h = FB_H - y;
    if (w <= 0 || h <= 0)
        return;
    for (int32_t r = 0; r < h; r++)
        for (int32_t c = 0; c < w; c++)
            px_fold(p, x + c, y + r, col);
}

static void paint_text(paint_ctx *p, const html5_rec *r)
{
    int k = font_scale(r->font);
    int adv = font_advance(r->font);
    const uint8_t *col = PALETTE[r->color & 15];
    int32_t x = r->x, y = r->y;

    for (uint16_t i = 0; i < r->slen; i++) {
        int g = glyph_index(r->s[i]);

        for (int gy = 0; gy < FONT_ROWS; gy++) {
            uint8_t bits = FONT[g][gy];

            for (int gx = 0; gx < 5; gx++) {
                if (!(bits & (1u << (4 - gx))))
                    continue;
                for (int dy = 0; dy < k; dy++)
                    for (int dx = 0; dx < k; dx++) {
                        int32_t px = x + gx * k + dx;
                        int32_t py = y + gy * k + dy;

                        if (px >= 0 && px < FB_W && py >= 0 && py < FB_H)
                            px_fold(p, px, py, col);
                        if (r->aux && px + 1 >= 0 && px + 1 < FB_W &&
                            py >= 0 && py < FB_H)
                            px_fold(p, px + 1, py, col); /* fake bold */
                    }
            }
        }
        x += adv;
    }
}

static void paint_img(paint_ctx *p, const html5_rec *r)
{
    int32_t x0 = r->x, y0 = r->y, x1 = r->x + r->w, y1 = r->y + r->h;
    uint8_t ph = r->aux;

    for (int32_t y = y0 < 0 ? 0 : y0; y < y1 && y < FB_H; y++) {
        for (int32_t x = x0 < 0 ? 0 : x0; x < x1 && x < FB_W; x++) {
            unsigned idx;

            if (r->color == PAT_CHART) {
                if (x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1)
                    idx = 1; /* dark frame */
                else
                    idx = (unsigned)(x / 24 + y / 40) + ph;
            } else if (r->color == PAT_AVATAR) {
                int32_t dx = x - (r->x + r->w / 2);
                int32_t dy = y - (r->y + r->h / 2);

                if (dx < 0)
                    dx = -dx;
                if (dy < 0)
                    dy = -dy;
                idx = (unsigned)((dx > dy ? dx : dy) / 6) + ph;
            } else { /* photo */
                idx = (unsigned)(((x * y) >> 6) +
                                 ((x * x + y * y) >> 8)) + ph;
            }
            px_fold(p, x, y, PALETTE[idx & 15]);
        }
    }
}

/* Paint a record list; `dig` optional (see above). */
static void paint_list(const html5_rec *recs, size_t n, uint8_t *fb,
                       uint64_t *dig)
{
    paint_ctx p = { fb, dig };

    memset(fb, 0xFF, (size_t)FB_W * FB_H * 4); /* white page */
    for (size_t i = 0; i < n; i++) {
        const html5_rec *r = &recs[i];

        switch (r->kind) {
        case HTML5_REC_BG:
            fill_rect(&p, r->x, r->y, r->w, r->h, PALETTE[r->color & 15]);
            break;
        case HTML5_REC_BORDER: {
            int32_t t = r->aux ? (int32_t)r->aux : 1;
            const uint8_t *col = PALETTE[r->color & 15];

            fill_rect(&p, r->x, r->y, r->w, t, col);
            fill_rect(&p, r->x, r->y + r->h - t, r->w, t, col);
            fill_rect(&p, r->x, r->y, t, r->h, col);
            fill_rect(&p, r->x + r->w - t, r->y, t, r->h, col);
            break;
        }
        case HTML5_REC_TEXT:
            paint_text(&p, r);
            break;
        case HTML5_REC_IMG:
            paint_img(&p, r);
            break;
        default:
            break;
        }
    }
}

void html5_paint(const html5_layout *lay, uint8_t *rgba)
{
    if (!lay || !rgba)
        return;
    paint_list(lay->recs, lay->n, rgba, NULL);
}

/* --- synthetic page generator ---------------------------------------------------
 *
 * Six site archetypes — Geekbench 6 opens pages "based on popular
 * websites": a portal, a long article, a wiki page, a photo feed, a
 * search-results page, a landing page. Everything is seeded: same
 * --seed, byte-identical HTML on every machine. The pages exercise the
 * tokenizer for real — attributes, comments, entities, void elements,
 * nested inline elements — and the layout with mixed inline content,
 * images and per-class styling. */

typedef struct {
    uint8_t *p;
    size_t n, cap;
    int oom;
} buf;

static void buf_grow(buf *b, size_t need)
{
    size_t cap = b->cap ? b->cap : 1024;
    uint8_t *np;

    if (b->oom || b->n + need <= b->cap)
        return;
    while (cap < b->n + need)
        cap *= 2;
    np = realloc(b->p, cap);
    if (!np) {
        b->oom = 1;
        return;
    }
    b->p = np;
    b->cap = cap;
}

static void buf_putn(buf *b, const char *s, size_t n)
{
    buf_grow(b, n);
    if (b->oom)
        return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void buf_puts(buf *b, const char *s)
{
    buf_putn(b, s, strlen(s));
}

static void buf_putc(buf *b, char c)
{
    buf_putn(b, &c, 1);
}

static void buf_putu(buf *b, unsigned v)
{
    char t[12];
    int k = 0;

    do {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        buf_putc(b, t[--k]);
}

static void gen_word(buf *b, rng_t *r, int cap)
{
    static const char CON[] = "bcdfgklmnprstvz";
    static const char VO[] = "aeiou";
    unsigned n = 2 + rng_below(r, 6);

    for (unsigned i = 0; i < n; i++) {
        char c = (i & 1) ? VO[rng_below(r, 5)] : CON[rng_below(r, 15)];

        if (cap && i == 0)
            c = (char)(c - 'a' + 'A');
        buf_putc(b, c);
    }
}

/* A run of words with punctuation, entity words and digit words — the
 * entity words exercise the decoder inside the timed parse. */
static void gen_words(buf *b, rng_t *r, unsigned lo, unsigned hi)
{
    unsigned n = lo + rng_below(r, hi - lo + 1);

    for (unsigned i = 0; i < n; i++) {
        if (i)
            buf_putc(b, ' ');
        unsigned m = rng_below(r, 20);

        if (m == 0) {
            buf_puts(b, "&amp;");
            continue;
        }
        if (m == 1) {
            buf_puts(b, "&lt;");
            continue;
        }
        if (m == 2) {
            buf_puts(b, "&gt;");
            continue;
        }
        if (m == 3) {
            buf_putu(b, 100 + rng_below(r, 9000));
            continue;
        }
        gen_word(b, r, rng_below(r, 6) == 0);
        switch (rng_below(r, 18)) {
        case 0: buf_puts(b, "."); break;
        case 1: buf_puts(b, ","); break;
        case 2: buf_puts(b, ";"); break;
        case 3: buf_puts(b, ":"); break;
        case 4: buf_puts(b, "!"); break;
        case 5: buf_puts(b, "?"); break;
        default: break;
        }
    }
}

/* A run mixing plain words with inline elements — the layout's
 * anonymous-flow and style-inheritance paths. */
static void gen_textmix(buf *b, rng_t *r, unsigned lo, unsigned hi)
{
    unsigned n = lo + rng_below(r, hi - lo + 1);

    for (unsigned i = 0; i < n; i++) {
        if (i)
            buf_putc(b, ' ');
        unsigned m = rng_below(r, 100);

        if (m < 62) {
            gen_word(b, r, 0);
            continue;
        }
        if (m < 75) {
            buf_puts(b, "<a href=\"#r");
            buf_putu(b, rng_below(r, 64));
            buf_puts(b, "\">");
            gen_words(b, r, 1, 2);
            buf_puts(b, "</a>");
            continue;
        }
        if (m < 85) {
            buf_puts(b, "<strong>");
            gen_words(b, r, 1, 3);
            buf_puts(b, "</strong>");
            continue;
        }
        if (m < 93) {
            buf_puts(b, "<em>");
            gen_words(b, r, 1, 2);
            buf_puts(b, "</em>");
            continue;
        }
        buf_puts(b, "<span class=\"note\">");
        gen_words(b, r, 1, 3);
        buf_puts(b, "</span>");
    }
}

static void gen_img(buf *b, rng_t *r, const char *cls, unsigned wlo,
                    unsigned whi, unsigned hlo, unsigned hhi)
{
    buf_puts(b, "<img class=\"");
    buf_puts(b, cls);
    buf_puts(b, "\" width=\"");
    buf_putu(b, wlo + rng_below(r, whi - wlo + 1));
    buf_puts(b, "\" height=\"");
    buf_putu(b, hlo + rng_below(r, hhi - hlo + 1));
    buf_puts(b, "\" alt=\"");
    gen_word(b, r, 0);
    buf_putc(b, ' ');
    gen_word(b, r, 0);
    buf_puts(b, "\">");
}

static void gen_par(buf *b, rng_t *r, const char *cls, unsigned lo,
                    unsigned hi)
{
    if (cls) {
        buf_puts(b, "<p class=\"");
        buf_puts(b, cls);
        buf_puts(b, "\">");
    } else {
        buf_puts(b, "<p>");
    }
    gen_words(b, r, lo, hi);
    buf_puts(b, "</p>\n");
}

static void gen_page(buf *b, rng_t *r, unsigned page)
{
    unsigned n;

    buf_puts(b, "<!doctype html>\n<html>\n<head>\n"
                "<meta charset=\"utf-8\">\n");
    buf_puts(b, "<meta name=\"description\" content=\"");
    gen_words(b, r, 3, 7);
    buf_puts(b, "\">\n<title>");
    gen_words(b, r, 2, 5);
    buf_puts(b, "</title>\n<link rel=\"stylesheet\" href=\"/assets/site");
    buf_putu(b, page % 7);
    buf_puts(b, ".css\">\n<!-- lotofbench generated page ");
    buf_putu(b, page);
    buf_puts(b, " -->\n</head>\n<body>\n");

    switch (page % 6) {
    case 0: { /* portal: hero header, nav chips, image sections */
        buf_puts(b, "<header class=\"hero\">\n<h1>");
        gen_words(b, r, 3, 6);
        buf_puts(b, "</h1>\n");
        gen_par(b, r, "lead", 10, 20);
        buf_puts(b, "</header>\n<nav><ul>\n");
        n = 5 + rng_below(r, 5);
        for (unsigned i = 0; i < n; i++) {
            buf_puts(b, "<li><a href=\"#nav");
            buf_putu(b, i);
            buf_puts(b, "\">");
            gen_words(b, r, 1, 3);
            buf_puts(b, "</a></li>\n");
        }
        buf_puts(b, "</ul></nav>\n<main>\n");
        n = 6 + rng_below(r, 5);
        for (unsigned i = 0; i < n; i++) {
            buf_puts(b, "<section id=\"sec");
            buf_putu(b, i);
            buf_puts(b, "\">\n<h2>");
            gen_words(b, r, 2, 5);
            buf_puts(b, "</h2>\n");
            gen_par(b, r, NULL, 25, 60);
            gen_img(b, r, "photo", 280, 460, 160, 260);
            buf_putc(b, '\n');
            gen_par(b, r, "caption", 4, 10);
            buf_puts(b, "</section>\n");
        }
        buf_puts(b, "</main>\n<footer>");
        gen_par(b, r, "note", 6, 14);
        buf_puts(b, "</footer>\n");
        break;
    }
    case 1: { /* article: long text with inline elements, photos, aside */
        buf_puts(b, "<header class=\"hero\">\n<h1>");
        gen_words(b, r, 4, 9);
        buf_puts(b, "</h1>\n");
        gen_par(b, r, "meta", 5, 12);
        buf_puts(b, "</header>\n<main>\n<article>\n");
        gen_par(b, r, "lead", 20, 40);
        n = 7 + rng_below(r, 7);
        for (unsigned i = 0; i < n; i++) {
            buf_puts(b, "<p>");
            gen_textmix(b, r, 10, 25);
            buf_puts(b, "</p>\n");
            if (i % 4 == 2) {
                gen_img(b, r, "photo", 300, 480, 180, 280);
                buf_putc(b, '\n');
            }
        }
        buf_puts(b, "<aside class=\"side\">\n<h3>");
        gen_words(b, r, 2, 4);
        buf_puts(b, "</h3>\n");
        gen_par(b, r, NULL, 15, 30);
        gen_par(b, r, NULL, 10, 20);
        buf_puts(b, "</aside>\n");
        gen_par(b, r, NULL, 20, 45);
        buf_puts(b, "</article>\n</main>\n<footer>");
        gen_par(b, r, "note", 5, 12);
        buf_puts(b, "</footer>\n");
        break;
    }
    case 2: { /* wiki: toc list, linked sections */
        buf_puts(b, "<h1>");
        gen_words(b, r, 2, 6);
        buf_puts(b, "</h1>\n");
        gen_par(b, r, NULL, 15, 30);
        gen_par(b, r, "meta", 3, 6);
        buf_puts(b, "<ul>\n");
        n = 4 + rng_below(r, 5);
        for (unsigned i = 0; i < n; i++) {
            buf_puts(b, "<li><a href=\"#sec");
            buf_putu(b, i);
            buf_puts(b, "\">");
            gen_words(b, r, 1, 4);
            buf_puts(b, "</a></li>\n");
        }
        buf_puts(b, "</ul>\n");
        n = 4 + rng_below(r, 4);
        for (unsigned i = 0; i < n; i++) {
            buf_puts(b, "<section id=\"sec");
            buf_putu(b, i);
            buf_puts(b, "\">\n<h2>");
            gen_words(b, r, 1, 4);
            buf_puts(b, "</h2>\n");
            unsigned m = 2 + rng_below(r, 3);

            for (unsigned j = 0; j < m; j++) {
                buf_puts(b, "<p>");
                gen_textmix(b, r, 15, 40);
                buf_puts(b, "</p>\n");
            }
            buf_puts(b, "</section>\n");
        }
        buf_puts(b, "<footer>");
        gen_par(b, r, "tag", 4, 8);
        buf_puts(b, "</footer>\n");
        break;
    }
    case 3: { /* feed: avatar + text + big photo cards */
        buf_puts(b, "<header class=\"hero\"><h1>");
        gen_words(b, r, 2, 4);
        buf_puts(b, "</h1></header>\n<main>\n");
        n = 6 + rng_below(r, 5);
        for (unsigned i = 0; i < n; i++) {
            buf_puts(b, "<div class=\"card\">\n"
                        "<img class=\"avatar\" width=\"64\" "
                        "height=\"64\" alt=\"");
            gen_word(b, r, 0);
            buf_puts(b, "\">\n<h3>");
            gen_words(b, r, 2, 4);
            buf_puts(b, "</h3>\n<p>");
            gen_textmix(b, r, 6, 16);
            buf_puts(b, "</p>\n");
            gen_img(b, r, "photo", 280, 440, 170, 260);
            buf_putc(b, '\n');
            gen_par(b, r, "caption", 3, 8);
            buf_puts(b, "<p>");
            unsigned m = 2 + rng_below(r, 3);

            for (unsigned j = 0; j < m; j++) {
                if (j)
                    buf_putc(b, ' ');
                buf_puts(b, "<a class=\"chip\" href=\"#tag");
                buf_putu(b, j);
                buf_puts(b, "\">");
                gen_words(b, r, 1, 2);
                buf_puts(b, "</a>");
            }
            buf_puts(b, "</p>\n</div>\n");
        }
        buf_puts(b, "</main>\n<footer>");
        gen_par(b, r, "note", 4, 9);
        buf_puts(b, "</footer>\n");
        break;
    }
    case 4: { /* search results: link headline, url line, snippet */
        buf_puts(b, "<header class=\"hero\"><h1>");
        gen_words(b, r, 1, 3);
        buf_puts(b, "</h1>");
        gen_par(b, r, "lead", 6, 14);
        buf_puts(b, "</header>\n<main>\n");
        n = 8 + rng_below(r, 7);
        for (unsigned i = 0; i < n; i++) {
            buf_puts(b, "<div class=\"card\">\n<h3><a href=\"#r");
            buf_putu(b, i);
            buf_puts(b, "\">");
            gen_words(b, r, 2, 6);
            buf_puts(b, "</a></h3>\n<p class=\"meta\">");
            gen_word(b, r, 0);
            buf_putc(b, '.');
            gen_word(b, r, 0);
            buf_putc(b, '/');
            gen_word(b, r, 0);
            buf_putc(b, '/');
            gen_word(b, r, 0);
            buf_puts(b, "</p>\n<p>");
            gen_textmix(b, r, 12, 28);
            buf_puts(b, "</p>\n</div>\n");
        }
        buf_puts(b, "</main>\n<footer>");
        gen_par(b, r, "note", 3, 7);
        buf_puts(b, "</footer>\n");
        break;
    }
    default: { /* landing page: hero image, feature sections */
        buf_puts(b, "<div class=\"hero\">\n");
        gen_img(b, r, "photo", 600, 640, 200, 240);
        buf_putc(b, '\n');
        buf_puts(b, "<h1>");
        gen_words(b, r, 2, 5);
        buf_puts(b, "</h1>\n");
        gen_par(b, r, "lead", 12, 24);
        buf_puts(b, "<p><a class=\"button\" href=\"#signup\">");
        gen_words(b, r, 1, 3);
        buf_puts(b, "</a></p>\n</div>\n<main>\n");
        n = 2 + rng_below(r, 3);
        for (unsigned i = 0; i < n; i++) {
            buf_puts(b, "<section>\n<h2>");
            gen_words(b, r, 2, 4);
            buf_puts(b, "</h2>\n<div class=\"card\">\n");
            gen_img(b, r, "chart", 240, 360, 140, 220);
            buf_putc(b, '\n');
            buf_puts(b, "<h3>");
            gen_words(b, r, 2, 4);
            buf_puts(b, "</h3>\n");
            gen_par(b, r, NULL, 15, 35);
            buf_puts(b, "</div>\n</section>\n");
        }
        buf_puts(b, "</main>\n<footer>");
        gen_par(b, r, "note", 5, 10);
        buf_puts(b, "</footer>\n");
        break;
    }
    }
    buf_puts(b, "</body>\n</html>\n");
}

/* --- context, reference render, run --------------------------------------------*/

struct html5_ctx {
    unsigned seed;
    struct {
        uint8_t *src;
        size_t len;
        uint64_t elts, recs, digest;
    } page[HTML5_PAGES];
    uint64_t elts_total;
    h5_scratch tool;     /* html5_page_layout / parse_count / render_page */
    html5_layout tool_lay;
};

static int ensure_fb(h5_scratch *sc)
{
    if (sc->fb)
        return 0;
    sc->fb = malloc((size_t)FB_W * FB_H * 4);
    return sc->fb ? 0 : -1;
}

static uint32_t find_body(const h5_scratch *sc)
{
    for (size_t i = 0; i < sc->dom_n; i++)
        if (sc->dom[i].tag == T_BODY)
            return (uint32_t)i;
    return 0; /* no body: lay from the root */
}

/* One page through the full pipeline (parse -> layout -> paint). Never
 * called with threads; all state lives in the caller's scratch. */
static int page_render(h5_scratch *sc, const html5_ctx *ctx, unsigned page,
                       uint8_t *fb, uint64_t *elts, uint64_t *recn,
                       uint64_t *dig)
{
    uint64_t el = 0;
    L l;

    if (ensure_fb(sc) != 0)
        return -1;
    scratch_reset(sc);
    if (parse_doc(sc, ctx->page[page].src, ctx->page[page].len, &el) != 0)
        return -1;
    memset(&l, 0, sizeof(l));
    l.sc = sc;
    l.nodes = sc->dom;
    l.page = (uint8_t)(page & 0xFF);
    l.font = 16;
    l.fg = 1;
    lay_block(&l, find_body(sc), 0, FB_W, 0);
    if (sc->oom || l.oom)
        return -1;
    paint_list(sc->recs, sc->rec_n, fb ? fb : sc->fb, dig);
    *elts = el;
    *recn = sc->rec_n;
    return 0;
}

html5_ctx *html5_prepare(unsigned seed)
{
    html5_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t rng;

    if (!ctx)
        return NULL;
    ctx->seed = seed;
    rng_init(&rng, (uint64_t)seed * 0x9E3779B97F4A7C15ull +
                       0xC2B2AE3D27D4EB4Full);

    /* --- generate the 32 synthetic pages (off the clock) --- */
    for (unsigned p = 0; p < HTML5_PAGES; p++) {
        buf b = { NULL, 0, 0, 0 };

        gen_page(&b, &rng, p);
        if (b.oom || !b.p) {
            free(b.p);
            html5_free(ctx);
            return NULL;
        }
        ctx->page[p].src = b.p;
        ctx->page[p].len = b.n;
    }

    /* --- reference render of every page (off the clock) --- */
    for (unsigned p = 0; p < HTML5_PAGES; p++) {
        uint64_t elts = 0, recn = 0, dig = 0;

        if (page_render(&ctx->tool, ctx, p, NULL, &elts, &recn, &dig) != 0) {
            html5_free(ctx);
            return NULL;
        }
        ctx->page[p].elts = elts;
        ctx->page[p].recs = recn;
        ctx->page[p].digest = dig;
        ctx->elts_total += elts;
    }
    scratch_reset(&ctx->tool); /* free the parse state, keep the fb */
    return ctx;
}

void html5_free(html5_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned p = 0; p < HTML5_PAGES; p++)
        free(ctx->page[p].src);
    scratch_free(&ctx->tool);
    free(ctx);
}

/* One thread's slice: contiguous pages, own framebuffer and arenas —
 * threads never share writable state (shared-task harness contract). */
typedef struct {
    html5_ctx *ctx;
    h5_scratch sc;
    uint32_t begin, end;
} h5_slice;

static int h5_cycle(void *arg)
{
    h5_slice *sl = arg;

    for (uint32_t p = sl->begin; p < sl->end; p++) {
        uint64_t elts = 0, recn = 0, dig = 0;

        if (page_render(&sl->sc, sl->ctx, p, NULL, &elts, &recn, &dig) != 0)
            return -1;
        if (elts != sl->ctx->page[p].elts ||
            recn != sl->ctx->page[p].recs ||
            dig != sl->ctx->page[p].digest)
            return -1;
    }
    return 0;
}

int html5_run(html5_ctx *ctx, unsigned runs, unsigned *threads,
              double *throughput_pps, uint64_t *pages_rendered)
{
    unsigned t_count = *threads;
    h5_slice *slices;
    void **ctxs;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    int rc = -1;

    if (t_count > HTML5_PAGES) /* one page per thread at least */
        t_count = HTML5_PAGES;
    if (t_count < 1)
        t_count = 1;

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;

    {
        uint32_t per = HTML5_PAGES / t_count;
        unsigned extra = HTML5_PAGES % t_count;
        uint32_t begin = 0;

        for (unsigned t = 0; t < t_count; t++) {
            uint32_t count = per + (t < extra ? 1u : 0u);

            slices[t].ctx = ctx;
            slices[t].begin = begin;
            slices[t].end = begin + count;
            begin += count;
            ctxs[t] = &slices[t];
        }
    }

    rc = harness_run(t_count, runs, h5_cycle, ctxs, elapsed);
    if (rc == 0) {
        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0
                           ? (double)HTML5_PAGES / elapsed[r]
                           : -1.0;
        *throughput_pps = lb_median(rates, runs);
        *threads = t_count;
        *pages_rendered = HTML5_PAGES;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++)
            scratch_free(&slices[t].sc);
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors for tests and tooling -------------------------------------------*/

uint32_t html5_pages(const html5_ctx *ctx)
{
    return ctx ? HTML5_PAGES : 0;
}

uint64_t html5_elements(const html5_ctx *ctx)
{
    return ctx ? ctx->elts_total : 0;
}

uint64_t html5_page_elements(const html5_ctx *ctx, unsigned page)
{
    if (!ctx || page >= HTML5_PAGES)
        return 0;
    return ctx->page[page].elts;
}

uint64_t html5_page_records(const html5_ctx *ctx, unsigned page)
{
    if (!ctx || page >= HTML5_PAGES)
        return 0;
    return ctx->page[page].recs;
}

const char *html5_page_source(const html5_ctx *ctx, unsigned page)
{
    if (!ctx || page >= HTML5_PAGES)
        return NULL;
    return (const char *)ctx->page[page].src;
}

size_t html5_page_source_len(const html5_ctx *ctx, unsigned page)
{
    if (!ctx || page >= HTML5_PAGES)
        return 0;
    return ctx->page[page].len;
}

const html5_layout *html5_page_layout(html5_ctx *ctx, unsigned page)
{
    uint64_t elts = 0, recn = 0, dig = 0;

    if (!ctx || page >= HTML5_PAGES)
        return NULL;
    if (page_render(&ctx->tool, ctx, page, NULL, &elts, &recn, &dig) != 0)
        return NULL;
    ctx->tool_lay.recs = ctx->tool.recs;
    ctx->tool_lay.n = (uint32_t)ctx->tool.rec_n;
    return &ctx->tool_lay;
}

uint32_t html5_record_count(const html5_layout *lay)
{
    return lay ? lay->n : 0;
}

int html5_record_at(const html5_layout *lay, uint32_t i, html5_rec *out)
{
    if (!lay || i >= lay->n || !out)
        return -1;
    *out = lay->recs[i];
    return 0;
}

int html5_render_page(html5_ctx *ctx, unsigned page, uint8_t *rgba)
{
    uint64_t elts = 0, recn = 0, dig = 0;

    if (!ctx || !rgba || page >= HTML5_PAGES)
        return -1;
    return page_render(&ctx->tool, ctx, page, rgba, &elts, &recn, &dig);
}

long html5_parse_count(html5_ctx *ctx, const char *html, size_t len)
{
    uint64_t elts = 0;

    if (!ctx || !html)
        return -1;
    scratch_reset(&ctx->tool);
    if (parse_doc(&ctx->tool, (const uint8_t *)html, len, &elts) != 0)
        return -1;
    return (long)elts;
}

int html5_font_rows(void)
{
    return FONT_ROWS;
}

int html5_font_scale(unsigned font_px)
{
    return font_scale(font_px);
}

int html5_font_advance(unsigned font_px)
{
    return font_advance(font_px);
}

int html5_font_glyph_row(uint8_t c, int row)
{
    if (row < 0 || row >= FONT_ROWS)
        return 0;
    return FONT[glyph_index(c)][row];
}

const uint8_t *html5_palette(uint8_t idx)
{
    return PALETTE[idx & 15];
}
