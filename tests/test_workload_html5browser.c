/* LotofBench tests — workload #3, HTML5 Browser
 * (workload_html5browser.[ch]).
 *
 * Determinism (byte-identical pages and pixmaps for a seed — the
 * published comparable-scores promise), input invariants (documented
 * element/record bands, per-archetype image presence), tokenizer unit
 * behavior on handcrafted documents, the published golden pins — and
 * the centerpiece: a differential test of the rasterizer against an
 * independent, brute-force compositor (reverse topmost-pixel scan vs
 * the workload's forward sequential overdraw). */

#include "tests.h"

#include "lotofbench.h"
#include "sha1.h"
#include "workload_html5browser.h"

#include <stdlib.h>
#include <string.h>

#define FB_BYTES ((size_t)HTML5_VIEW_W * HTML5_VIEW_H * 4)

/* Bounded substring search — page sources are length-delimited buffers,
 * not NUL-terminated strings. */
static int src_contains(const char *s, size_t n, const char *needle)
{
    size_t m = strlen(needle);

    if (n < m)
        return 0;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(s + i, needle, m) == 0)
            return 1;
    return 0;
}

static html5_ctx *build(unsigned seed)
{
    html5_ctx *c = html5_prepare(seed);

    if (!c)
        test_fail(__FILE__, __LINE__, "html5_prepare(%u) failed", seed);
    return c;
}

/* --- tests --------------------------------------------------------------------*/

void test_h5_pages_deterministic(void)
{
    html5_ctx *a = build(7), *b = build(7), *c = build(8);
    static uint8_t pa[FB_BYTES], pb[FB_BYTES];

    if (!a || !b || !c) {
        html5_free(a);
        html5_free(b);
        html5_free(c);
        return;
    }
    CHECK_U64(html5_pages(a), HTML5_PAGES);
    for (unsigned p = 0; p < HTML5_PAGES; p++) {
        size_t la = html5_page_source_len(a, p);

        if (la != html5_page_source_len(b, p)) {
            test_fail(__FILE__, __LINE__, "page %u length differs", p);
            break;
        }
        if (memcmp(html5_page_source(a, p), html5_page_source(b, p),
                   la) != 0) {
            test_fail(__FILE__, __LINE__, "page %u source differs", p);
            break;
        }
        if (html5_page_elements(a, p) != html5_page_elements(b, p) ||
            html5_page_records(a, p) != html5_page_records(b, p)) {
            test_fail(__FILE__, __LINE__, "page %u counts differ", p);
            break;
        }
    }
    /* A different seed produces a different page set. */
    CHECK(memcmp(html5_page_source(a, 0), html5_page_source(c, 0),
                 html5_page_source_len(a, 0)) != 0);
    /* Full-pipeline determinism: identical rendered pixmaps. */
    CHECK(html5_render_page(a, 0, pa) == 0);
    CHECK(html5_render_page(b, 0, pb) == 0);
    CHECK(memcmp(pa, pb, FB_BYTES) == 0);
    html5_free(a);
    html5_free(b);
    html5_free(c);
}

void test_h5_input_invariants(void)
{
    static const unsigned SEEDS[] = { 1, 7, 42, 12345 };

    for (size_t s = 0; s < sizeof SEEDS / sizeof SEEDS[0]; s++) {
        html5_ctx *h = build(SEEDS[s]);

        if (!h)
            continue;
        for (unsigned p = 0; p < HTML5_PAGES; p++) {
            const char *src = html5_page_source(h, p);
            size_t len = html5_page_source_len(h, p);
            uint64_t elts = html5_page_elements(h, p);
            uint64_t recs = html5_page_records(h, p);

            if (!src || len < 400) {
                test_fail(__FILE__, __LINE__,
                          "seed %u page %u: missing/short source",
                          SEEDS[s], p);
                break;
            }
            if (elts < 25 || elts > 3000) {
                test_fail(__FILE__, __LINE__,
                          "seed %u page %u: %llu elements out of band",
                          SEEDS[s], p, (unsigned long long)elts);
                break;
            }
            if (recs < 20 || recs > 40000) {
                test_fail(__FILE__, __LINE__,
                          "seed %u page %u: %llu records out of band",
                          SEEDS[s], p, (unsigned long long)recs);
                break;
            }
            /* The parser must reproduce the published count. */
            if (html5_parse_count(h, src, len) != (long)elts) {
                test_fail(__FILE__, __LINE__,
                          "seed %u page %u: parse count mismatch",
                          SEEDS[s], p);
                break;
            }
            if (!src_contains(src, len, "<body>") ||
                !src_contains(src, len, "</html>")) {
                test_fail(__FILE__, __LINE__,
                          "seed %u page %u: malformed skeleton",
                          SEEDS[s], p);
                break;
            }
            /* Per-archetype image presence: portal/article/feed/landing
             * carry photos; wiki and search results carry none. */
            int wants_img = (p % 6 == 0 || p % 6 == 1 || p % 6 == 3 ||
                             p % 6 == 5);

            if (wants_img != (src_contains(src, len, "<img") != 0)) {
                test_fail(__FILE__, __LINE__,
                          "seed %u page %u: image presence mismatch",
                          SEEDS[s], p);
                break;
            }
        }
        html5_free(h);
    }
}

void test_h5_parser_units(void)
{
    html5_ctx *h = build(1);

    if (!h)
        return;
    /* well-formed basics */
    CHECK_U64(html5_parse_count(h, "<p>hello</p>", 12), 1);
    CHECK_U64(html5_parse_count(h, "<div><p>a</p><p>b</p></div>", 27), 3);
    CHECK_U64(html5_parse_count(h, "<img src=\"x\">", 13), 1); /* void */
    CHECK_U64(html5_parse_count(h, "<br><hr>", 8), 2);
    CHECK_U64(html5_parse_count(h, "<!-- comment --><p>x</p>", 24), 1);
    CHECK_U64(html5_parse_count(h, "<!doctype html><p>x</p>", 23), 1);
    CHECK_U64(html5_parse_count(h, "<p>a &amp; b &lt; c &gt; d &#65;</p>",
                                 36), 1);
    CHECK_U64(html5_parse_count(h, "<ul><li>a<li>b</ul>", 19), 3);
    CHECK_U64(html5_parse_count(h, "<p>unclosed", 11), 1); /* auto-close */
    CHECK_U64(html5_parse_count(h, "</p><p>x</p>", 12), 1); /* stray end */
    CHECK_U64(html5_parse_count(h, "<foo>bar</foo>", 14), 1); /* unknown */
    CHECK_U64(html5_parse_count(h, "<p class=\"note\" data-x=1>y</p>", 30),
              1);
    CHECK_U64(html5_parse_count(h, "<p><img src=\"a\"><img src=\"b\"></p>",
                                 33), 3);
    /* stray '<' is text, not markup */
    CHECK_U64(html5_parse_count(h, "<3 hearts", 9), 0);
    /* malformed input fails cleanly */
    CHECK_U64(html5_parse_count(h, "<p", 2), (uint64_t)-1);
    CHECK_U64(html5_parse_count(h, "<!-- oops", 9), (uint64_t)-1);
    CHECK_U64(html5_parse_count(h, "<p href=\"x", 10), (uint64_t)-1);
    CHECK_U64(html5_parse_count(h, "<p &amp;", 8), (uint64_t)-1);
    html5_free(h);
}

/* --- independent brute-force compositor --------------------------------------*/

/* Coverage and color of one record at one pixel, re-implemented from
 * the public record contract (the compositing semantics, not the
 * workload's paint loops): returns 1 and fills rgba[4] when the record
 * paints the pixel. */

/* Is the glyph pixel at (x, y) painted by this text record? */
static int text_pixel_on(const html5_rec *r, int32_t x, int32_t y, int k,
                         int adv)
{
    int32_t dx = x - r->x, dy = y - r->y;
    int i, off, gy;

    if (!r->s || dx < 0 || dy < 0 || dy >= 7 * k)
        return 0;
    i = (int)(dx / adv);
    if (i >= r->slen)
        return 0;
    off = (int)(dx % adv);
    if (off >= 5 * k)
        return 0;
    gy = (int)(dy / k);
    return (html5_font_glyph_row(r->s[i], gy) & (1 << (4 - off / k))) != 0;
}

static int rec_covers(const html5_rec *r, int32_t x, int32_t y,
                      uint8_t rgba[4])
{
    int k = html5_font_scale(r->font);
    int adv = html5_font_advance(r->font);

    switch (r->kind) {
    case HTML5_REC_BG:
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            memcpy(rgba, html5_palette(r->color), 4);
            return 1;
        }
        return 0;
    case HTML5_REC_BORDER: {
        int32_t t = r->aux ? r->aux : 1;

        if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h)
            return 0;
        if (x < r->x + t || x >= r->x + r->w - t || y < r->y + t ||
            y >= r->y + r->h - t) {
            memcpy(rgba, html5_palette(r->color), 4);
            return 1;
        }
        return 0;
    }
    case HTML5_REC_TEXT:
        if (text_pixel_on(r, x, y, k, adv) ||
            (r->aux && text_pixel_on(r, x - 1, y, k, adv))) {
            memcpy(rgba, html5_palette(r->color), 4);
            return 1;
        }
        return 0;
    case HTML5_REC_IMG: {
        unsigned idx;
        uint8_t ph = r->aux;

        if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h)
            return 0;
        if (r->color == HTML5_PAT_CHART) {
            if (x == r->x || x == r->x + r->w - 1 || y == r->y ||
                y == r->y + r->h - 1)
                idx = 1; /* dark frame */
            else
                idx = (unsigned)(x / 24 + y / 40) + ph;
        } else if (r->color == HTML5_PAT_AVATAR) {
            int32_t dx = x - (r->x + r->w / 2);
            int32_t dy = y - (r->y + r->h / 2);

            if (dx < 0)
                dx = -dx;
            if (dy < 0)
                dy = -dy;
            idx = (unsigned)((dx > dy ? dx : dy) / 6) + ph;
        } else { /* HTML5_PAT_PHOTO */
            idx = (unsigned)(((x * y) >> 6) + ((x * x + y * y) >> 8)) + ph;
        }
        memcpy(rgba, html5_palette((uint8_t)idx), 4);
        return 1;
    }
    default:
        return 0;
    }
}

/* Reverse topmost-pixel scan: the final state of a forward sequential
 * overdraw with opaque pixels, computed by independent semantics. */
static void brute_paint(const html5_layout *lay, uint8_t *fb)
{
    uint32_t n = html5_record_count(lay);
    html5_rec *recs = n ? malloc(n * sizeof(*recs)) : NULL;
    uint32_t *row = n ? malloc(n * sizeof(*row)) : NULL;

    memset(fb, 0xFF, FB_BYTES);
    if (!recs || !row) {
        free(recs);
        free(row);
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        html5_record_at(lay, i, &recs[i]);
    for (int32_t y = 0; y < (int32_t)HTML5_VIEW_H; y++) {
        uint32_t rn = 0;

        for (uint32_t i = 0; i < n; i++) {
            const html5_rec *r = &recs[i];
            int32_t h = r->h;

            if (r->kind == HTML5_REC_TEXT)
                h = 7 * html5_font_scale(r->font);
            if (y >= r->y && y < r->y + h)
                row[rn++] = i;
        }
        for (int32_t x = 0; x < (int32_t)HTML5_VIEW_W; x++) {
            for (uint32_t j = rn; j-- > 0;) {
                uint8_t rgba[4];

                if (rec_covers(&recs[row[j]], x, y, rgba)) {
                    uint8_t *o = fb +
                        ((size_t)y * HTML5_VIEW_W + (size_t)x) * 4;

                    o[0] = rgba[0];
                    o[1] = rgba[1];
                    o[2] = rgba[2];
                    o[3] = rgba[3];
                    break;
                }
            }
        }
    }
    free(recs);
    free(row);
}

void test_h5_paint_differential(void)
{
    static const unsigned PAGES[] = { 0, 3, 4 }; /* portal, feed, search */
    html5_ctx *h = build(7);
    static uint8_t fast[FB_BYTES], ref[FB_BYTES];

    if (!h)
        return;
    for (size_t pi = 0; pi < sizeof PAGES / sizeof PAGES[0]; pi++) {
        unsigned p = PAGES[pi];
        const html5_layout *lay = html5_page_layout(h, p);

        if (!lay) {
            test_fail(__FILE__, __LINE__, "layout of page %u failed", p);
            break;
        }
        html5_paint(lay, fast);
        brute_paint(lay, ref);
        if (memcmp(fast, ref, FB_BYTES) != 0) {
            /* locate the first divergent pixel for the report */
            for (size_t i = 0; i < FB_BYTES; i++)
                if (fast[i] != ref[i]) {
                    uint32_t px = (uint32_t)(i / 4) % HTML5_VIEW_W;
                    uint32_t py = (uint32_t)(i / 4) / HTML5_VIEW_W;

                    test_fail(__FILE__, __LINE__,
                              "page %u: pixel (%u,%u) byte %zu: "
                              "%u != %u",
                              p, px, py, i % 4, fast[i], ref[i]);
                    break;
                }
            break;
        }
    }
    html5_free(h);
}

void test_h5_layout_invariants(void)
{
    html5_ctx *h = build(7);
    static const unsigned PAGES[] = { 0, 1, 3 };

    if (!h)
        return;
    for (size_t pi = 0; pi < sizeof PAGES / sizeof PAGES[0]; pi++) {
        const html5_layout *lay = html5_page_layout(h, PAGES[pi]);
        uint32_t n;
        uint32_t imgs = 0;

        if (!lay) {
            test_fail(__FILE__, __LINE__, "layout of page %u failed",
                      PAGES[pi]);
            continue;
        }
        n = html5_record_count(lay);
        CHECK(n > 0);
        html5_rec r0;

        CHECK(html5_record_at(lay, 0, &r0) == 0);
        CHECK_U64(r0.kind, HTML5_REC_BG); /* body background first */
        CHECK_U64(r0.x, 0);
        CHECK_U64(r0.y, 0);
        CHECK_U64(r0.w, HTML5_VIEW_W);
        for (uint32_t i = 0; i < n; i++) {
            html5_rec r;

            if (html5_record_at(lay, i, &r) != 0) {
                test_fail(__FILE__, __LINE__, "record %u unreadable", i);
                break;
            }
            if (r.kind == HTML5_REC_IMG)
                imgs++;
            if (r.x < 0 || r.y < 0) {
                test_fail(__FILE__, __LINE__,
                          "page %u record %u at (%d,%d)",
                          PAGES[pi], i, r.x, r.y);
                break;
            }
            if (r.kind != HTML5_REC_TEXT)
                continue;
            if (r.font != 12 && r.font != 16 && r.font != 20 &&
                r.font != 28) {
                test_fail(__FILE__, __LINE__,
                          "page %u record %u: font %u", PAGES[pi], i,
                          r.font);
                break;
            }
            if (!r.s || r.slen == 0 || r.slen > 512 || r.w <= 0) {
                test_fail(__FILE__, __LINE__,
                          "page %u record %u: bad word", PAGES[pi], i);
                break;
            }
            /* generated words always fit: a word placed past the right
             * edge means the line breaker is broken */
            if (r.x < 8 || r.x + r.w > (int32_t)HTML5_VIEW_W) {
                test_fail(__FILE__, __LINE__,
                          "page %u record %u: word [%d,%d) overflows",
                          PAGES[pi], i, r.x, r.x + r.w);
                break;
            }
        }
        CHECK(imgs > 0); /* all three archetypes carry images */
    }
    html5_free(h);
}

void test_h5_pixmap_golden(void)
{
    html5_ctx *h = build(1);
    static uint8_t pix[FB_BYTES];
    char hex[41];

    if (!h)
        return;
    /* sources, counts and rendered pixmaps of pages 0 (portal) and
     * 5 (landing), generated by tools/gen_goldens.c */
    CHECK(html5_page_source(h, 0) != NULL);
    sha1_hex((const uint8_t *)html5_page_source(h, 0),
             html5_page_source_len(h, 0), hex);
    CHECK_STR(hex, "e6825b61b24a379e407d57430ea56fdbfcf82591");
    CHECK_U64(html5_page_source_len(h, 0), 3346);
    CHECK_U64(html5_page_elements(h, 0), 59);
    CHECK_U64(html5_page_records(h, 0), 388);
    CHECK(html5_render_page(h, 0, pix) == 0);
    sha1_hex(pix, FB_BYTES, hex);
    CHECK_STR(hex, "0a9d933ca425f3fed0eee1db461427270b855bb5");

    sha1_hex((const uint8_t *)html5_page_source(h, 5),
             html5_page_source_len(h, 5), hex);
    CHECK_STR(hex, "9c99776807edadeff0250287cd1ade2e371cf141");
    CHECK_U64(html5_page_source_len(h, 5), 1856);
    CHECK_U64(html5_page_elements(h, 5), 40);
    CHECK_U64(html5_page_records(h, 5), 192);
    CHECK(html5_render_page(h, 5, pix) == 0);
    sha1_hex(pix, FB_BYTES, hex);
    CHECK_STR(hex, "84a2c524fdfb2fe5f74eb349f83abded202f5c72");

    CHECK_U64(html5_elements(h), 3544);
    /* the pinned page must keep exercising the entity decoder */
    CHECK(strstr(html5_page_source(h, 0), "&amp;") != NULL);
    html5_free(h);
}

void test_h5_run_sane(void)
{
    html5_ctx *h = build(1);
    unsigned th = 2;
    double tp = 0.0;
    uint64_t pages = 0;

    if (!h)
        return;
    uint64_t elts_before = html5_elements(h);

    CHECK(html5_run(h, 3, &th, &tp, &pages) == 0);
    CHECK_U64(pages, HTML5_PAGES);
    CHECK_U64(th, 2);
    CHECK(tp > 0.0 && tp < 100000.0);
    CHECK_U64(html5_elements(h), elts_before); /* run leaves no trace */
    html5_free(h);
}

void test_h5_thread_clamp(void)
{
    html5_ctx *h = build(1);
    unsigned th = 999;
    double tp = 0.0;
    uint64_t pages = 0;

    if (!h)
        return;
    CHECK(html5_run(h, 2, &th, &tp, &pages) == 0);
    CHECK_U64(th, HTML5_PAGES); /* clamped to the page count */
    CHECK_U64(pages, HTML5_PAGES);
    CHECK(tp > 0.0);
    html5_free(h);
}

void test_h5_accessors_null_safe(void)
{
    html5_ctx *h = build(1);
    static uint8_t pix[FB_BYTES];
    html5_rec r;

    if (!h)
        return;
    CHECK_U64(html5_pages(NULL), 0);
    CHECK_U64(html5_elements(NULL), 0);
    CHECK_U64(html5_page_elements(NULL, 0), 0);
    CHECK(html5_page_source(NULL, 0) == NULL);
    CHECK_U64(html5_page_source_len(NULL, 0), 0);
    CHECK(html5_page_layout(NULL, 0) == NULL);
    CHECK_U64(html5_parse_count(NULL, "<p>x</p>", 8), (uint64_t)-1);
    CHECK(html5_render_page(NULL, 0, pix) != 0);
    CHECK_U64(html5_record_count(NULL), 0);
    CHECK_U64(html5_record_at(NULL, 0, &r), (uint64_t)-1);
    html5_paint(NULL, pix); /* must not crash */

    CHECK_U64(html5_page_elements(h, HTML5_PAGES), 0);
    CHECK_U64(html5_page_records(h, HTML5_PAGES), 0);
    CHECK(html5_page_source(h, HTML5_PAGES) == NULL);
    CHECK_U64(html5_page_source_len(h, HTML5_PAGES), 0);
    CHECK(html5_page_layout(h, HTML5_PAGES) == NULL);
    CHECK(html5_render_page(h, HTML5_PAGES, pix) != 0);

    const html5_layout *lay = html5_page_layout(h, 0);

    if (lay) {
        CHECK_U64(html5_record_at(lay, html5_record_count(lay), &r),
                  (uint64_t)-1);
        CHECK_U64(html5_record_at(lay, 0, NULL), (uint64_t)-1);
    } else {
        test_fail(__FILE__, __LINE__, "layout of page 0 failed");
    }
    html5_free(h);
}
