/* Unit tests — workload #4: PDF Render. */

#include "tests.h"

#include "sha1.h"
#include "workload_pdfrender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.3.0, seed 1). After a
 * DELIBERATE change to the corpus or the renderer, re-run the tool and
 * port the printed values here in the same commit. */
#define PDF_GOLDEN_DOC0_SHA1 "05a73b7c1e823ebb31a55097c506ad72e42c3d0f"
#define PDF_GOLDEN_DOC0_LEN  899593u
#define PDF_GOLDEN_TOTAL_OPS 190946ull

/* One shared prepare for the read-only tests (the corpus is immutable;
 * the process exits right after the suite anyway). */
static pdf_ctx *shared_pdf(void)
{
    static pdf_ctx *ctx;

    if (!ctx)
        ctx = pdf_prepare(1);
    return ctx;
}

static void doc_sha1(const pdf_ctx *ctx, unsigned d, char hex[41])
{
    sha1_hex(pdf_doc(ctx, d), pdf_doc_len(ctx, d), hex);
}

void test_pdf_deterministic_docs(void)
{
    pdf_ctx *a = pdf_prepare(1), *b = pdf_prepare(1);

    CHECK(a && b);
    if (!a || !b) {
        pdf_free(a);
        pdf_free(b);
        return;
    }
    for (unsigned d = 0; d < PDF_DOCS; d++) {
        CHECK_U64(pdf_doc_len(a, d), pdf_doc_len(b, d));
        char ha[41], hb[41];

        doc_sha1(a, d, ha);
        doc_sha1(b, d, hb);
        CHECK_STR(ha, hb);
    }
    pdf_free(a);
    pdf_free(b);
}

void test_pdf_corpus_golden(void)
{
    pdf_ctx *ctx = shared_pdf();
    char hex[41];
    uint64_t ops = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    doc_sha1(ctx, 0, hex);
    CHECK_STR(hex, PDF_GOLDEN_DOC0_SHA1);
    CHECK_U64(pdf_doc_len(ctx, 0), PDF_GOLDEN_DOC0_LEN);
    for (unsigned d = 0; d < PDF_DOCS; d++)
        ops += pdf_doc_ops(ctx, d);
    CHECK_U64(ops, PDF_GOLDEN_TOTAL_OPS);
}

/* First index of needle in haystack, or -1 (portable memmem stand-in). */
static ptrdiff_t find_bytes(const uint8_t *hay, size_t hay_len,
                            const char *needle, size_t needle_len)
{
    if (needle_len == 0 || hay_len < needle_len)
        return -1;
    for (size_t i = 0; i + needle_len <= hay_len; i++)
        if (memcmp(hay + i, needle, needle_len) == 0)
            return (ptrdiff_t)i;
    return -1;
}

void test_pdf_doc_shape(void)
{
    pdf_ctx *ctx = shared_pdf();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned d = 0; d < PDF_DOCS; d++) {
        const uint8_t *doc = pdf_doc(ctx, d);
        size_t len = pdf_doc_len(ctx, d);

        CHECK(doc && len > 1000);
        if (!doc || len < 1000)
            continue;
        CHECK(memcmp(doc, "%PDF-1.4\n", 9) == 0);
        CHECK(len >= 6 && memcmp(doc + len - 6, "%%EOF\n", 6) == 0);
        CHECK(find_bytes(doc, len, "/Type /Catalog", 14) >= 0);
        CHECK_U64(pdf_doc_pages(ctx, d), PDF_PAGES_PER_DOC);
        for (unsigned p = 0; p < PDF_PAGES_PER_DOC; p++)
            CHECK(pdf_probe_count(ctx, d, p) >= 1);
    }
}

void test_pdf_probes_in_bounds(void)
{
    pdf_ctx *ctx = shared_pdf();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned d = 0; d < PDF_DOCS; d++)
        for (unsigned p = 0; p < PDF_PAGES_PER_DOC; p++) {
            unsigned n = pdf_probe_count(ctx, d, p);

            for (unsigned i = 0; i < n; i++) {
                uint32_t x, y;
                uint8_t r, g, b;

                CHECK(pdf_probe(ctx, d, p, i, &x, &y, &r, &g, &b) == 0);
                CHECK(x < PDF_PAGE_W && y < PDF_PAGE_H);
            }
        }
}

void test_pdf_seed_variation(void)
{
    pdf_ctx *a = pdf_prepare(1), *b = pdf_prepare(2);
    char ha[41], hb[41];

    CHECK(a && b);
    if (!a || !b) {
        pdf_free(a);
        pdf_free(b);
        return;
    }
    doc_sha1(a, 0, ha);
    doc_sha1(b, 0, hb);
    CHECK(strcmp(ha, hb) != 0); /* different seed => different corpus */
    pdf_free(a);
    pdf_free(b);
}

void test_pdf_run_sane(void)
{
    pdf_ctx *ctx = shared_pdf();
    unsigned threads = 1;
    double mpixels = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(pdf_run(ctx, 1, &threads, &mpixels, &pixels) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(pixels,
              (uint64_t)PDF_DOCS * PDF_PAGES_PER_DOC * PDF_PAGE_W *
                  PDF_PAGE_H);
    CHECK(mpixels > 0.0);
}

void test_pdf_thread_clamp(void)
{
    pdf_ctx *ctx = shared_pdf();
    unsigned threads = 999;
    double mpixels = 0.0;
    uint64_t pixels = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* more threads than documents: clamped to the 16-doc multi-core set */
    CHECK(pdf_run(ctx, 1, &threads, &mpixels, &pixels) == 0);
    CHECK_U64(threads, PDF_DOCS_MULTI);
    CHECK_U64(pixels,
              (uint64_t)PDF_DOCS_MULTI * PDF_PAGES_PER_DOC * PDF_PAGE_W *
                  PDF_PAGE_H);
}

void test_pdf_accessors_null_safe(void)
{
    uint32_t x, y;
    uint8_t r, g, b;

    CHECK(pdf_doc(NULL, 0) == NULL);
    CHECK_U64(pdf_doc_len(NULL, 0), 0);
    CHECK_U64(pdf_doc_pages(NULL, 0), 0);
    CHECK_U64(pdf_doc_ops(NULL, 0), 0);
    CHECK_U64(pdf_probe_count(NULL, 0, 0), 0);
    CHECK(pdf_probe(NULL, 0, 0, 0, &x, &y, &r, &g, &b) == -1);

    pdf_ctx *ctx = shared_pdf();

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(pdf_doc(ctx, PDF_DOCS) == NULL);
    CHECK_U64(pdf_doc_len(ctx, PDF_DOCS), 0);
    CHECK_U64(pdf_probe_count(ctx, 0, PDF_PAGES_PER_DOC), 0);
    CHECK(pdf_probe(ctx, 0, 0, 99999, &x, &y, &r, &g, &b) == -1);
}
