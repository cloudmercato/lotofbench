#ifndef LOTOFBENCH_TESTS_H
#define LOTOFBENCH_TESTS_H

/* Minimal dependency-free test framework for LotofBench.
 *
 * Every test is a `void fn(void)` registered in tests/runner.c; failures
 * are reported through the CHECK_* macros (file, line, what broke) and
 * counted globally. A nonzero count at the end of the run fails the
 * suite with exit status 1. */

#include <stdint.h>
#include <string.h>

/* Report a failure with a printf-style message. */
void test_fail(const char *file, int line, const char *fmt, ...);

/* Number of failed checks since the run started. */
int test_failure_count(void);

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond))                                                        \
            test_fail(__FILE__, __LINE__, "check failed: %s", #cond);       \
    } while (0)

/* Integer equality (any integer types, compared as uint64_t). */
#define CHECK_U64(got, want)                                                \
    do {                                                                    \
        uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want);               \
        if (g_ != w_)                                                       \
            test_fail(__FILE__, __LINE__, "%s = %llu, expected %llu",       \
                      #got, (unsigned long long)g_,                         \
                      (unsigned long long)w_);                              \
    } while (0)

/* Exact double equality — only for values produced by identical code
 * paths (e.g. the median helper on known arrays). */
#define CHECK_DBL(got, want)                                                \
    do {                                                                    \
        double g_ = (double)(got), w_ = (double)(want);                     \
        if (!(g_ == w_))                                                    \
            test_fail(__FILE__, __LINE__, "%s = %.17g, expected %.17g",     \
                      #got, g_, w_);                                        \
    } while (0)

#define CHECK_STR(got, want)                                                \
    do {                                                                    \
        const char *g_ = (got), *w_ = (want);                               \
        if (!g_ || !w_ || strcmp(g_, w_) != 0)                              \
            test_fail(__FILE__, __LINE__, "%s = %s, expected %s",           \
                      #got, g_ ? g_ : "(null)", w_ ? w_ : "(null)");        \
    } while (0)

/* --- rng ---------------------------------------------------------------------*/
void test_rng_deterministic(void);
void test_rng_golden_values(void);
void test_rng_below_bounds(void);
void test_rng_distribution(void);

/* --- sha1 ----------------------------------------------------------------------*/
void test_sha1_known_vectors(void);
void test_sha1_million_a(void);

/* --- lz4 -----------------------------------------------------------------------*/
void test_lz4_roundtrip_shapes(void);
void test_lz4_golden_vectors(void);
void test_lz4_incompressible_overhead(void);
void test_lz4_rejects_malformed(void);
void test_lz4_small_thread_stack(void);

/* --- harness ---------------------------------------------------------------------*/
void test_median_odd_even(void);
void test_score_rounding(void);
void test_harness_counts_and_times(void);
void test_harness_failure_propagates(void);
void test_harness_single_thread(void);

/* --- workload #1: file compression --------------------------------------------------*/
void test_fc_deterministic_corpus(void);
void test_fc_corpus_golden(void);
void test_fc_run_sane(void);
void test_fc_accessors_null_safe(void);

/* --- workload #2: navigation ----------------------------------------------------------*/
void test_nav_graph_deterministic(void);
void test_nav_csr_invariants(void);
void test_nav_route_band(void);
void test_nav_differential_dijkstra(void);
void test_nav_edges_thread_independent(void);
void test_nav_thread_clamp(void);
void test_nav_route_time_guards(void);
void test_nav_run_sane(void);

/* --- workload #3: html5 browser ---------------------------------------------------------*/
void test_h5_pages_deterministic(void);
void test_h5_input_invariants(void);
void test_h5_parser_units(void);
void test_h5_paint_differential(void);
void test_h5_layout_invariants(void);
void test_h5_pixmap_golden(void);
void test_h5_run_sane(void);
void test_h5_thread_clamp(void);
void test_h5_accessors_null_safe(void);

/* --- workload #4: pdf render ------------------------------------------------------------*/
void test_pdf_deterministic_docs(void);
void test_pdf_corpus_golden(void);
void test_pdf_doc_shape(void);
void test_pdf_probes_in_bounds(void);
void test_pdf_seed_variation(void);
void test_pdf_run_sane(void);
void test_pdf_thread_clamp(void);
void test_pdf_accessors_null_safe(void);
/* --- workload #5: photo library ------------------------------------------------------------*/
void test_pl_corpus_deterministic(void);
void test_pl_input_invariants(void);
void test_pl_classify_planted(void);
void test_pl_pipeline_units(void);
void test_pl_seed_variation(void);
void test_pl_run_sane(void);
void test_pl_thread_clamp(void);
void test_pl_accessors_null_safe(void);
void test_pl_golden(void);
/* --- workload #6: clang -----------------------------------------------------------------*/
void test_cl_units_deterministic(void);
void test_cl_input_invariants(void);
void test_cl_lexer_units(void);
void test_cl_parser_units(void);
void test_cl_codegen_golden(void);
void test_cl_stack_discipline(void);
void test_cl_run_sane(void);
void test_cl_thread_clamp(void);
void test_cl_accessors_null_safe(void);
void test_cl_golden(void);

/* --- workload #7: text processing --------------------------------------------------------*/
void test_tp_corpus_deterministic(void);
void test_tp_input_invariants(void);
void test_tp_regex_semantics(void);
void test_tp_markdown_units(void);
void test_tp_thread_clamp(void);
void test_tp_run_sane(void);
void test_tp_accessors_null_safe(void);
void test_tp_golden(void);

/* --- workload #8: asset compression -------------------------------------------------*/
void test_ac_textures_deterministic(void);
void test_ac_input_invariants(void);
void test_ac_bc1_units(void);
void test_ac_bc3_units(void);
void test_ac_mesh_units(void);
void test_ac_mesh_foreign_orders(void);
void test_ac_corpus_golden(void);
void test_ac_seed_variation(void);
void test_ac_run_sane(void);
void test_ac_thread_clamp(void);
void test_ac_accessors_null_safe(void);

/* --- imgops: shared deterministic image kernels ------------------------------------------*/
void test_img_levels(void);
void test_img_color_matrix(void);
void test_img_saturate(void);
void test_img_box_blur(void);
void test_img_grayscale(void);
void test_img_gray_gaussian_blur5(void);
void test_img_crop(void);
void test_img_scale(void);
void test_img_box_downscale(void);

/* --- workload #12: horizon detection ---------------------------------------------------------*/
void test_hd_photos_deterministic(void);
void test_hd_input_invariants(void);
void test_hd_ground_truth(void);
void test_hd_detect_units(void);
void test_hd_corpus_golden(void);
void test_hd_seed_variation(void);
void test_hd_run_sane(void);
void test_hd_thread_clamp(void);
void test_hd_accessors_null_safe(void);

/* --- workload #15: ray tracer ------------------------------------------------*/
void test_rt_scene_deterministic(void);
void test_rt_input_invariants(void);
void test_rt_render_differential(void);
void test_rt_unit_vectors(void);
void test_rt_corpus_golden(void);
void test_rt_seed_variation(void);
void test_rt_run_sane(void);
void test_rt_thread_clamp(void);
void test_rt_accessors_null_safe(void);

/* --- workload #13: photo filter ------------------------------------------------------------*/
void test_pf_photos_deterministic(void);
void test_pf_input_invariants(void);
void test_pf_reference_reproducible(void);
void test_pf_corpus_golden(void);
void test_pf_seed_variation(void);
void test_pf_run_sane(void);
void test_pf_thread_clamp(void);
void test_pf_accessors_null_safe(void);

/* --- workload #16: structure from motion ---------------------------------------------------*/
void test_sfm_scene_deterministic(void);
void test_sfm_input_invariants(void);
void test_sfm_reference_golden(void);
void test_sfm_essential_estimator(void);
void test_sfm_fast9_synthetic(void);
void test_sfm_jacobi3_eigen(void);
void test_sfm_jacobi9_nullvec(void);
void test_sfm_sampson_known(void);
void test_sfm_decompose_pose(void);
void test_sfm_triangulate_known(void);
void test_sfm_seed_variation(void);
void test_sfm_run_sane(void);
void test_sfm_thread_clamp(void);
void test_sfm_accessors_null_safe(void);

/* --- workload #9: object detection ---------------------------------------------------*/
void test_od_photos_deterministic(void);
void test_od_input_invariants(void);
void test_od_seed_variation(void);
void test_od_net_seed_independent(void);
void test_od_quantize_units(void);
void test_od_conv_units(void);
void test_od_anchor_units(void);
void test_od_decode_units(void);
void test_od_differential_naive(void);
void test_od_corpus_golden(void);
void test_od_run_sane(void);
void test_od_thread_clamp(void);
void test_od_accessors_null_safe(void);

#endif /* LOTOFBENCH_TESTS_H */
