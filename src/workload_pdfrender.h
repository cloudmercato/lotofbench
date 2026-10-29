#ifndef WORKLOAD_PDFRENDER_H
#define WORKLOAD_PDFRENDER_H

#include <stddef.h>
#include <stdint.h>

/* Workload #4 — PDF Render.
 *
 * Mirrors the fourth Geekbench 6 CPU workload: open complex PDF
 * documents and render them. Geekbench 6 pushes four American National
 * Park Service maps (897 KB to 1.5 MB, large vector art, lines and
 * text) through PDFium — 4 PDFs single-core, 16 PDFs multi-core.
 * LotofBench reproduces the shape dependency-free: four seeded "park
 * map" PDF documents (a cover page and a map page each) are parsed by
 * a from-scratch PDF reader and rasterized by a from-scratch scanline
 * renderer with 4x4 anti-aliasing. See docs/benchmarks.md, section
 * "Workload 4". */

typedef struct pdf_ctx pdf_ctx;

/* Fixed input geometry, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the map is the map. */
#define PDF_DOCS           4u  /* the single-core pass renders these four */
#define PDF_DOCS_MULTI    16u  /* multi-core pass = 4 x the document set */
#define PDF_PAGES_PER_DOC  2u  /* cover page + map page */
#define PDF_PAGE_W       1224u /* device pixels: US Letter at 2x (144 dpi) */
#define PDF_PAGE_H       1584u

/* Build the four PDF documents plus everything the timed cycle verifies
 * against (content-stream digests, op counts, reference page renders,
 * probe points). Off the clock; fixed seed => byte-identical documents
 * on every machine. */
pdf_ctx *pdf_prepare(unsigned seed);
void     pdf_free(pdf_ctx *ctx);

/* One workload pass with *threads threads (clamped to the multi-core
 * document count). Fills *throughput_mpixels with the median over the
 * runs, in millions of device pixels rendered per second, and *pixels
 * with the exact device-pixel count of one pass. Returns 0, or -1 if
 * any document failed to parse, render or verify. */
int pdf_run(pdf_ctx *ctx, unsigned runs, unsigned *threads,
            double *throughput_mpixels, uint64_t *pixels);

/* Read-only access for tests and tooling. */
const uint8_t *pdf_doc(const pdf_ctx *ctx, unsigned doc); /* raw bytes */
size_t         pdf_doc_len(const pdf_ctx *ctx, unsigned doc);
unsigned       pdf_doc_pages(const pdf_ctx *ctx, unsigned doc);
uint64_t       pdf_doc_ops(const pdf_ctx *ctx, unsigned doc);

/* Probe points: device-space coordinates with the exact expected RGB.
 * The generator knows the geometry it emitted, so these expectations
 * are computed analytically — independent of the renderer. */
unsigned pdf_probe_count(const pdf_ctx *ctx, unsigned doc, unsigned page);
int      pdf_probe(const pdf_ctx *ctx, unsigned doc, unsigned page,
                   unsigned index, uint32_t *x, uint32_t *y,
                   uint8_t *r, uint8_t *g, uint8_t *b);

#endif /* WORKLOAD_PDFRENDER_H */
