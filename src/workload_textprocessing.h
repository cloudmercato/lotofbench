#ifndef WORKLOAD_TEXTPROCESSING_H
#define WORKLOAD_TEXTPROCESSING_H

#include <stddef.h>
#include <stdint.h>

/* Workload #7 — Text Processing.
 *
 * Mirrors the seventh Geekbench 6 CPU workload: regex matching and
 * Markdown/HTML processing over a text corpus (Geekbench 6 runs it in
 * Python 3). A scripted runtime cannot be part of a reproducible
 * benchmark — interpreter versions and library drift change the
 * numbers — so lotofbench reproduces the shape of that pipeline
 * dependency-free, with the same honesty rules as the other workloads:
 * a hand-rolled backtracking regex engine over a documented safe
 * subset, a from-scratch Markdown-to-HTML converter, and seeded
 * synthetic documents in six archetype families. Everything is
 * documented in docs/benchmarks.md, section "Workload 7". */

typedef struct tp_ctx tp_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the documents are the
 * documents. */
#define TP_DOCS     32u
#define TP_PATTERNS 8u

/* Build the document corpus and its per-document pattern set (off the
 * clock; fixed seed => byte-identical sources on every machine), the
 * reference match count of every pattern and the reference conversion
 * of every document. */
tp_ctx *tp_prepare(unsigned seed);
void    tp_free(tp_ctx *ctx);

/* One workload pass with *threads threads (clamped to the doc count).
 * Fills *throughput_mbps with the median over the runs, in decimal
 * megabytes of text processed per second, and *bytes with the exact
 * task size of one pass in bytes (deterministic, identical for every
 * run): every document counted once for the Markdown stage and once
 * per pattern for the regex stage. Returns 0, or -1 if any document
 * failed verification. */
int tp_run(tp_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mbps, uint64_t *bytes);

/* Read-only access for tests and tooling. */
uint32_t    tp_docs(const tp_ctx *ctx);
uint64_t    tp_bytes(const tp_ctx *ctx); /* task bytes per pass */
uint64_t    tp_doc_len(const tp_ctx *ctx, unsigned doc);
const char *tp_doc_source(const tp_ctx *ctx, unsigned doc);
const char *tp_doc_pattern(const tp_ctx *ctx, unsigned doc, unsigned pat);
uint32_t    tp_doc_matches(const tp_ctx *ctx, unsigned doc, unsigned pat);
size_t      tp_doc_html_len(const tp_ctx *ctx, unsigned doc);
const char *tp_doc_html(const tp_ctx *ctx, unsigned doc);
uint64_t    tp_doc_digest(const tp_ctx *ctx, unsigned doc); /* SHA-1[0..7] */

/* Test hooks on arbitrary input. tp_regex_count compiles `pattern`
 * (the documented subset) and counts its non-overlapping leftmost
 * matches over text[0..len), or -1 on a pattern error. tp_markdown
 * converts markdown[0..len) into out (capacity cap), returning the
 * HTML length, or -1 on any error or short capacity. Both use the
 * context's tool scratch; like the cl hooks they are not safe to call
 * from threads. */
long tp_regex_count(tp_ctx *ctx, const char *pattern,
                    const char *text, size_t len);
long tp_markdown(tp_ctx *ctx, const char *markdown, size_t len,
                 uint8_t *out, size_t cap);

#endif /* WORKLOAD_TEXTPROCESSING_H */
