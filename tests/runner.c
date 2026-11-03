/* LotofBench unit-test runner — explicit registry, exit 1 on failure. */

#include "tests.h"

#include <stdio.h>

typedef struct {
    const char *name;
    void (*fn)(void);
} test_entry;

static const test_entry TESTS[] = {
    /* rng */
    { "rng/deterministic", test_rng_deterministic },
    { "rng/golden-v0.1-stream", test_rng_golden_values },
    { "rng/below-bounds", test_rng_below_bounds },
    { "rng/distribution", test_rng_distribution },
    /* sha1 */
    { "sha1/rfc3174-vectors", test_sha1_known_vectors },
    { "sha1/million-a", test_sha1_million_a },
    /* lz4 */
    { "lz4/roundtrip-shapes", test_lz4_roundtrip_shapes },
    { "lz4/golden-blocks", test_lz4_golden_vectors },
    { "lz4/incompressible-overhead", test_lz4_incompressible_overhead },
    { "lz4/rejects-malformed", test_lz4_rejects_malformed },
    { "lz4/small-thread-stack", test_lz4_small_thread_stack },
    /* harness */
    { "harness/median", test_median_odd_even },
    { "harness/score-rounding", test_score_rounding },
    { "harness/counts-and-times", test_harness_counts_and_times },
    { "harness/failure-propagates", test_harness_failure_propagates },
    { "harness/single-thread", test_harness_single_thread },
    /* workload #1 — file compression */
    { "compression/deterministic-corpus", test_fc_deterministic_corpus },
    { "compression/corpus-golden-v0.1", test_fc_corpus_golden },
    { "compression/run-sane", test_fc_run_sane },
    { "compression/null-accessors", test_fc_accessors_null_safe },
    /* workload #2 — navigation */
    { "navigation/graph-deterministic", test_nav_graph_deterministic },
    { "navigation/csr-invariants", test_nav_csr_invariants },
    { "navigation/route-band", test_nav_route_band },
    { "navigation/differential-dijkstra", test_nav_differential_dijkstra },
    { "navigation/edges-thread-independent",
      test_nav_edges_thread_independent },
    { "navigation/thread-clamp", test_nav_thread_clamp },
    { "navigation/route-time-guards", test_nav_route_time_guards },
    { "navigation/run-sane", test_nav_run_sane },
    /* workload #3 — html5 browser */
    { "html5/pages-deterministic", test_h5_pages_deterministic },
    { "html5/input-invariants", test_h5_input_invariants },
    { "html5/parser-units", test_h5_parser_units },
    { "html5/paint-differential", test_h5_paint_differential },
    { "html5/layout-invariants", test_h5_layout_invariants },
    { "html5/pixmap-golden", test_h5_pixmap_golden },
    { "html5/run-sane", test_h5_run_sane },
    { "html5/thread-clamp", test_h5_thread_clamp },
    { "html5/null-accessors", test_h5_accessors_null_safe },
    /* workload #4 — pdf render */
    { "pdf/deterministic-docs", test_pdf_deterministic_docs },
    { "pdf/corpus-golden", test_pdf_corpus_golden },
    { "pdf/doc-shape", test_pdf_doc_shape },
    { "pdf/probes-in-bounds", test_pdf_probes_in_bounds },
    { "pdf/seed-variation", test_pdf_seed_variation },
    { "pdf/run-sane", test_pdf_run_sane },
    { "pdf/thread-clamp", test_pdf_thread_clamp },
    { "pdf/null-accessors", test_pdf_accessors_null_safe },
    /* workload #6 — clang */
    { "clang/units-deterministic", test_cl_units_deterministic },
    { "clang/input-invariants", test_cl_input_invariants },
    { "clang/lexer-units", test_cl_lexer_units },
    { "clang/parser-units", test_cl_parser_units },
    { "clang/codegen-golden", test_cl_codegen_golden },
    { "clang/stack-discipline", test_cl_stack_discipline },
    { "clang/run-sane", test_cl_run_sane },
    { "clang/thread-clamp", test_cl_thread_clamp },
    { "clang/null-accessors", test_cl_accessors_null_safe },
    { "clang/golden", test_cl_golden },
    /* workload #7 — text processing */
    { "textprocessing/corpus-deterministic",
      test_tp_corpus_deterministic },
    { "textprocessing/input-invariants", test_tp_input_invariants },
    { "textprocessing/regex-semantics", test_tp_regex_semantics },
    { "textprocessing/markdown-units", test_tp_markdown_units },
    { "textprocessing/thread-clamp", test_tp_thread_clamp },
    { "textprocessing/run-sane", test_tp_run_sane },
    { "textprocessing/null-accessors", test_tp_accessors_null_safe },
    { "textprocessing/golden", test_tp_golden },
    /* imgops — shared deterministic image kernels */
    { "imgops/levels", test_img_levels },
    { "imgops/color-matrix", test_img_color_matrix },
    { "imgops/saturate", test_img_saturate },
    { "imgops/box-blur", test_img_box_blur },
    { "imgops/crop", test_img_crop },
    { "imgops/scale", test_img_scale },
    /* workload #13 — photo filter */
    { "photofilter/photos-deterministic", test_pf_photos_deterministic },
    { "photofilter/input-invariants", test_pf_input_invariants },
    { "photofilter/reference-reproducible", test_pf_reference_reproducible },
    { "photofilter/corpus-golden", test_pf_corpus_golden },
    { "photofilter/seed-variation", test_pf_seed_variation },
    { "photofilter/run-sane", test_pf_run_sane },
    { "photofilter/thread-clamp", test_pf_thread_clamp },
    { "photofilter/null-accessors", test_pf_accessors_null_safe },
};

int main(void)
{
    const int total = (int)(sizeof TESTS / sizeof TESTS[0]);

    printf("LotofBench unit tests\n");
    for (int i = 0; i < total; i++) {
        int before = test_failure_count();

        printf("[ %2d/%d ] %-38s ", i + 1, total, TESTS[i].name);
        fflush(stdout);
        TESTS[i].fn();
        printf(before == test_failure_count() ? "ok\n" : "FAIL\n");
    }
    printf("\n%d tests, %d failed check(s)\n", total,
           test_failure_count());
    return test_failure_count() ? 1 : 0;
}
