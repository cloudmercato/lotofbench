/* LotofBench tests — the shared run harness (harness.[ch]):
 * medians, scoring, the start-gated multi-core runner. */

#include "tests.h"

#include "harness.h"

#include <math.h>
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

void test_section_geomean(void)
{
    long a[] = { 1000, 1000 };
    long b[] = { 2000, 500 };          /* product 1e6 -> mean 1000 */
    long c[] = { 8, 2 };               /* sqrt(16) = 4 */
    long d[] = { 1000, 1000, 1000 };   /* constant scores stay constant */
    long z[] = { 0, 1000 };            /* a dead workload kills its section */

    /* exp/log round-trips carry ~1 ULP of error, so the transcendental
     * results are compared with a tolerance, not exactly. */
    CHECK(fabs(lb_geomean(a, 2) - 1000.0) < 1e-9);
    CHECK(fabs(lb_geomean(b, 2) - 1000.0) < 1e-9);
    CHECK(fabs(lb_geomean(c, 2) - 4.0) < 1e-9);
    CHECK(fabs(lb_geomean(d, 3) - 1000.0) < 1e-9);
    CHECK_DBL(lb_geomean(z, 2), 0.0);
    CHECK_DBL(lb_geomean(a, 0), 0.0);
}

void test_section_rounding(void)
{
    long a[] = { 2000, 500 };
    long b[] = { 1000, 1001 };         /* mean 1000.4999... rounds down */
    long c[] = { 1000, 1002 };         /* mean 1000.9995... rounds up */
    long one[] = { 3 };

    CHECK_U64(lb_section(a, 2), 1000);
    CHECK_U64(lb_section(b, 2), 1000);
    CHECK_U64(lb_section(c, 2), 1001);
    CHECK_U64(lb_section(one, 1), 3);
}

void test_composite_weighting(void)
{
    /* The 65/35 weighted arithmetic mean of the two section scores. */
    CHECK_U64(lb_composite(1000, 1000), 1000);
    CHECK_U64(lb_composite(1000, 2000), 1350); /* 650 + 700 */
    CHECK_U64(lb_composite(2000, 1000), 1650); /* 1300 + 350 */
    CHECK_U64(lb_composite(10, 0), 7);         /* 6.5 rounds half up */
    CHECK_U64(lb_composite(0, 10), 4);         /* 3.5 rounds half up */
    CHECK_U64(lb_composite(0, 0), 0);
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
