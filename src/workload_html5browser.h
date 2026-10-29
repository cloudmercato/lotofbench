#ifndef WORKLOAD_HTML5BROWSER_H
#define WORKLOAD_HTML5BROWSER_H

#include <stddef.h>
#include <stdint.h>

/* Workload #3 — HTML5 Browser.
 *
 * Mirrors the third Geekbench 6 CPU workload: a headless browser that
 * opens, parses, lays out and renders text and images for web pages.
 * Geekbench 6 drives Gumbo + litehtml + FreeType + AGG over snapshots of
 * popular sites; lotofbench reproduces the shape of the pipeline
 * dependency-free — own tokenizer, own block layout, own scanline
 * rasterizer — over deterministic synthetic pages. Everything is
 * documented in docs/benchmarks.md, section "Workload 3". */

typedef struct html5_ctx html5_ctx;
typedef struct html5_layout html5_layout;

/* Fixed input size, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the page set is the page set. */
#define HTML5_PAGES   32u
#define HTML5_VIEW_W  800u
#define HTML5_VIEW_H  600u

/* Build the page set (off the clock; fixed seed => byte-identical HTML
 * on every machine) and the reference render of every page. */
html5_ctx *html5_prepare(unsigned seed);
void       html5_free(html5_ctx *ctx);

/* One workload pass with *threads threads (clamped to the page count).
 * Fills *throughput_pps with the median over the runs, in pages rendered
 * per second, and *pages_rendered with the exact page count of one pass
 * (deterministic, identical for every run). Returns 0, or -1 if a page
 * failed verification. */
int html5_run(html5_ctx *ctx, unsigned runs, unsigned *threads,
              double *throughput_pps, uint64_t *pages_rendered);

/* One display-list record — the output of parse + layout, the input of
 * the rasterizer. Exposed read-only for tests and tooling. */
typedef struct {
    uint8_t kind;      /* HTML5_REC_* */
    uint8_t color;     /* bg/border/text palette index; images: pattern id */
    uint8_t aux;       /* border: thickness; text: bold flag; image: phase */
    uint8_t font;      /* text: font pixel size */
    int32_t x, y, w, h;
    const uint8_t *s;  /* text: word bytes (not NUL-terminated) */
    uint16_t slen;
} html5_rec;

#define HTML5_REC_BG     1u  /* solid fill: palette color `color` */
#define HTML5_REC_BORDER 2u  /* 4 strips of `aux` px, palette color `color` */
#define HTML5_REC_TEXT   3u  /* word blit at (x, y), glyph top-left */
#define HTML5_REC_IMG    4u  /* procedural image, pattern `color` */

/* Image pattern ids (the HTML5_REC_IMG `color` field). */
#define HTML5_PAT_PHOTO  1u
#define HTML5_PAT_CHART  2u
#define HTML5_PAT_AVATAR 3u

/* Parse and lay out one page (no raster). The returned layout lives in
 * the context's scratch and stays valid until the next html5_page_layout
 * or html5_parse_count call on the same context. Returns NULL on parse
 * or layout failure. */
const html5_layout *html5_page_layout(html5_ctx *ctx, unsigned page);

uint32_t html5_record_count(const html5_layout *lay);
int      html5_record_at(const html5_layout *lay, uint32_t i, html5_rec *out);

/* Raster a layout into a caller-supplied HTML5_VIEW_W * HTML5_VIEW_H * 4
 * RGBA8 buffer — the same paint code the timed cycle uses. */
void html5_paint(const html5_layout *lay, uint8_t *rgba);

/* Render one page with the workload's full pipeline (parse, layout,
 * paint) into a caller-supplied viewport buffer. Returns 0, or -1 on
 * failure. */
int html5_render_page(html5_ctx *ctx, unsigned page, uint8_t *rgba);

/* Parse an arbitrary HTML document and count its element nodes — a test
 * and tooling hook for the tokenizer. Returns the element count, or -1
 * on malformed input (unterminated tag or comment). */
long html5_parse_count(html5_ctx *ctx, const char *html, size_t len);

/* Font and palette data for tests and tooling. The glyph table and the
 * palette are shared data; what the differential test re-implements is
 * the compositing. html5_font_glyph_row returns one 5-bit row (bit 4 =
 * leftmost column) of the glyph for `c`; html5_palette returns the 4
 * RGBA bytes of color `idx` (idx & 15). */
int html5_font_rows(void);
int html5_font_scale(unsigned font_px);
int html5_font_advance(unsigned font_px);
int html5_font_glyph_row(uint8_t c, int row);
const uint8_t *html5_palette(uint8_t idx);

/* Read-only access for tests and tooling. */
uint32_t    html5_pages(const html5_ctx *ctx);
uint64_t    html5_elements(const html5_ctx *ctx); /* DOM elements per pass */
uint64_t    html5_page_elements(const html5_ctx *ctx, unsigned page);
uint64_t    html5_page_records(const html5_ctx *ctx, unsigned page);
const char *html5_page_source(const html5_ctx *ctx, unsigned page);
size_t      html5_page_source_len(const html5_ctx *ctx, unsigned page);

#endif /* WORKLOAD_HTML5BROWSER_H */
