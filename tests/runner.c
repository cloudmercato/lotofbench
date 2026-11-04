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
    /* workload #5 — photo library */
    { "photo-library/corpus-deterministic", test_pl_corpus_deterministic },
    { "photo-library/input-invariants", test_pl_input_invariants },
    { "photo-library/classify-planted", test_pl_classify_planted },
    { "photo-library/pipeline-units", test_pl_pipeline_units },
    { "photo-library/seed-variation", test_pl_seed_variation },
    { "photo-library/run-sane", test_pl_run_sane },
    { "photo-library/thread-clamp", test_pl_thread_clamp },
    { "photo-library/null-accessors", test_pl_accessors_null_safe },
    { "photo-library/golden-v0.10", test_pl_golden },
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
    /* workload #8 — asset compression */
    { "assetcompression/textures-deterministic",
      test_ac_textures_deterministic },
    { "assetcompression/input-invariants", test_ac_input_invariants },
    { "assetcompression/bc1-units", test_ac_bc1_units },
    { "assetcompression/bc3-units", test_ac_bc3_units },
    { "assetcompression/mesh-units", test_ac_mesh_units },
    { "assetcompression/mesh-foreign-orders", test_ac_mesh_foreign_orders },
    { "assetcompression/corpus-golden", test_ac_corpus_golden },
    { "assetcompression/seed-variation", test_ac_seed_variation },
    { "assetcompression/run-sane", test_ac_run_sane },
    { "assetcompression/thread-clamp", test_ac_thread_clamp },
    { "assetcompression/null-accessors", test_ac_accessors_null_safe },
    /* imgops — shared deterministic image kernels */
    { "imgops/levels", test_img_levels },
    { "imgops/color-matrix", test_img_color_matrix },
    { "imgops/saturate", test_img_saturate },
    { "imgops/box-blur", test_img_box_blur },
    { "imgops/grayscale", test_img_grayscale },
    { "imgops/gray-gaussian-blur5", test_img_gray_gaussian_blur5 },
    { "imgops/crop", test_img_crop },
    { "imgops/scale", test_img_scale },
    { "imgops/box-downscale", test_img_box_downscale },
    /* workload #12 — horizon detection */
    { "horizon/photos-deterministic", test_hd_photos_deterministic },
    { "horizon/input-invariants", test_hd_input_invariants },
    { "horizon/ground-truth", test_hd_ground_truth },
    { "horizon/detect-units", test_hd_detect_units },
    { "horizon/corpus-golden", test_hd_corpus_golden },
    { "horizon/seed-variation", test_hd_seed_variation },
    { "horizon/run-sane", test_hd_run_sane },
    { "horizon/thread-clamp", test_hd_thread_clamp },
    { "horizon/null-accessors", test_hd_accessors_null_safe },
    /* workload #15 — ray tracer */
    { "raytracer/scene-deterministic", test_rt_scene_deterministic },
    { "raytracer/input-invariants", test_rt_input_invariants },
    { "raytracer/render-differential", test_rt_render_differential },
    { "raytracer/unit-vectors", test_rt_unit_vectors },
    { "raytracer/corpus-golden", test_rt_corpus_golden },
    { "raytracer/seed-variation", test_rt_seed_variation },
    { "raytracer/run-sane", test_rt_run_sane },
    { "raytracer/thread-clamp", test_rt_thread_clamp },
    { "raytracer/null-accessors", test_rt_accessors_null_safe },
    /* workload #13 — photo filter */
    { "photofilter/photos-deterministic", test_pf_photos_deterministic },
    { "photofilter/input-invariants", test_pf_input_invariants },
    { "photofilter/reference-reproducible", test_pf_reference_reproducible },
    { "photofilter/corpus-golden", test_pf_corpus_golden },
    { "photofilter/seed-variation", test_pf_seed_variation },
    { "photofilter/run-sane", test_pf_run_sane },
    { "photofilter/thread-clamp", test_pf_thread_clamp },
    { "photofilter/null-accessors", test_pf_accessors_null_safe },

    { "sfm/scene-deterministic", test_sfm_scene_deterministic },
    { "sfm/input-invariants", test_sfm_input_invariants },
    { "sfm/reference-golden", test_sfm_reference_golden },
    { "sfm/essential-estimator", test_sfm_essential_estimator },
    { "sfm/fast9-synthetic", test_sfm_fast9_synthetic },
    { "sfm/jacobi3-eigen", test_sfm_jacobi3_eigen },
    { "sfm/jacobi9-nullvec", test_sfm_jacobi9_nullvec },
    { "sfm/sampson-known", test_sfm_sampson_known },
    { "sfm/decompose-pose", test_sfm_decompose_pose },
    { "sfm/triangulate-known", test_sfm_triangulate_known },
    { "sfm/seed-variation", test_sfm_seed_variation },
    { "sfm/run-sane", test_sfm_run_sane },
    { "sfm/thread-clamp", test_sfm_thread_clamp },
    { "sfm/null-accessors", test_sfm_accessors_null_safe },
    /* workload #9 — object detection */
    { "objectdetection/photos-deterministic",
      test_od_photos_deterministic },
    { "objectdetection/input-invariants", test_od_input_invariants },
    { "objectdetection/seed-variation", test_od_seed_variation },
    { "objectdetection/net-seed-independent",
      test_od_net_seed_independent },
    { "objectdetection/quantize-units", test_od_quantize_units },
    { "objectdetection/conv-units", test_od_conv_units },
    { "objectdetection/anchor-units", test_od_anchor_units },
    { "objectdetection/decode-units", test_od_decode_units },
    { "objectdetection/differential-naive", test_od_differential_naive },
    { "objectdetection/corpus-golden", test_od_corpus_golden },
    { "objectdetection/run-sane", test_od_run_sane },
    { "objectdetection/thread-clamp", test_od_thread_clamp },
    { "objectdetection/null-accessors", test_od_accessors_null_safe },
    /* workload #10 — background blur */
    { "backgroundblur/frames-deterministic",
      test_bb_frames_deterministic },
    { "backgroundblur/input-invariants", test_bb_input_invariants },
    { "backgroundblur/seed-variation", test_bb_seed_variation },
    { "backgroundblur/segment-units", test_bb_segment_units },
    { "backgroundblur/majority-units", test_bb_majority_units },
    { "backgroundblur/mask-upscale-units", test_bb_mask_upscale_units },
    { "backgroundblur/composite-units", test_bb_composite_units },
    { "backgroundblur/differential-naive", test_bb_differential_naive },
    { "backgroundblur/mask-ground-truth", test_bb_mask_ground_truth },
    { "backgroundblur/corpus-golden", test_bb_corpus_golden },
    { "backgroundblur/run-sane", test_bb_run_sane },
    { "backgroundblur/thread-clamp", test_bb_thread_clamp },
    { "backgroundblur/null-accessors", test_bb_accessors_null_safe },

    /* workload #11 — object remover */
    { "objectremover/photos-deterministic",
      test_or_photos_deterministic },
    { "objectremover/input-invariants", test_or_input_invariants },
    { "objectremover/push-units", test_or_push_units },
    { "objectremover/coarse-fill-units", test_or_coarse_fill_units },
    { "objectremover/pull-units", test_or_pull_units },
    { "objectremover/sweep-units", test_or_sweep_units },
    { "objectremover/flat-fill-exact", test_or_flat_fill_exact },
    { "objectremover/differential-naive", test_or_differential_naive },
    { "objectremover/process-reproducible", test_or_process_reproducible },
    { "objectremover/corpus-golden", test_or_corpus_golden },
    { "objectremover/seed-variation", test_or_seed_variation },
    { "objectremover/run-sane", test_or_run_sane },
    { "objectremover/thread-clamp", test_or_thread_clamp },
    { "objectremover/null-accessors", test_or_accessors_null_safe },

    { "hdr/scenes-deterministic", test_hdr_scenes_deterministic },
    { "hdr/input-invariants", test_hdr_input_invariants },
    { "hdr/weight-units", test_hdr_weight_units },
    { "hdr/normalize-units", test_hdr_normalize_units },
    { "hdr/down-units", test_hdr_down_units },
    { "hdr/expand-units", test_hdr_expand_units },
    { "hdr/flat-fusion-exact", test_hdr_flat_fusion_exact },
    { "hdr/process-reproducible", test_hdr_process_reproducible },
    { "hdr/corpus-golden", test_hdr_corpus_golden },
    { "hdr/seed-variation", test_hdr_seed_variation },
    { "hdr/run-sane", test_hdr_run_sane },
    { "hdr/thread-clamp", test_hdr_thread_clamp },
    { "hdr/null-accessors", test_hdr_accessors_null_safe },
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
