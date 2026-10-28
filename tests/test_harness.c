/* LotofBench tests — the shared run harness (harness.[ch]):
 * medians, scoring, the start-gated multi-core runner. */

#include "tests.h"

#include "harness.h"

#include <stdlib.h>

void test_median_odd_even(void)
{
    double a[] = { 3, 1, 2 };
    double b[] = { 4, 1, 3, 2 };
    double c[] = { 7 };
    double d[] = { 5, 5, 1 };

    CHECK_DBL(lb_median(a, 3), 2.0);
    CHECK_DBL(lb_median(b, 4), 2.5);
    CHECK_DBL(lb_median(c, 1), 7.0);
    CHECK_DBL(lb_median(d, 3), 5.0);
}

void test_score_rounding(void)
{
    /* score = round(throughput / baseline * 1000), half up. */
    CHECK_U64(lb_score(250.0, 250.0), 1000);
    CHECK_U64(lb_score(125.0, 250.0), 500);
    CHECK_U64(lb_score(500.0, 250.0), 2000);
    CHECK_U64(lb_score(0.0, 250.0), 0);
    CHECK_U64(lb_score(1000.4, 1000.0), 1000);
    CHECK_U64(lb_score(1000.9, 1000.0), 1001);
}

/* Counting cycle: each invocation increments the counter in its ctx,
 * unless the ctx is set up to fail. */
typedef struct {
    unsigned count;
    int fail;
} count_ctx;

static int counting_cycle(void *arg)
{
    count_ctx *c = arg;

    if (c->fail)
        return -1;
    c->count++;
    return 0;
}

void test_harness_counts_and_times(void)
{
    enum { T = 3, R = 4 };
    count_ctx ctx_data[T] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
    void *ctxs[T] = { &ctx_data[0], &ctx_data[1], &ctx_data[2] };
    double elapsed[R];
    unsigned total = 0;

    CHECK(harness_run(T, R, counting_cycle, ctxs, elapsed) == 0);
    for (int t = 0; t < T; t++)
        total += ctx_data[t].count;
    CHECK_U64(total, T * R); /* every thread ran every run exactly once */
    for (int r = 0; r < R; r++)
        CHECK(elapsed[r] >= 0.0);
}

void test_harness_failure_propagates(void)
{
    count_ctx c = { 0, 1 };
    void *ctxs[1] = { &c };
    double elapsed[2];

    CHECK(harness_run(1, 2, counting_cycle, ctxs, elapsed) == -1);
}

void test_harness_single_thread(void)
{
    count_ctx c = { 0, 0 };
    void *ctxs[1] = { &c };
    double elapsed[2];

    CHECK(harness_run(1, 2, counting_cycle, ctxs, elapsed) == 0);
    CHECK_U64(c.count, 2);
}
