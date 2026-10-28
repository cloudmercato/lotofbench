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
