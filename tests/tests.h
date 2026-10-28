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

#endif /* LOTOFBENCH_TESTS_H */
