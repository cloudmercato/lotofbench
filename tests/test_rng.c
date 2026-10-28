/* LotofBench tests — the deterministic PRNG stream (rng.[ch]).
 *
 * The corpus and the road network both promise byte-identical inputs for
 * a given seed on every machine: the stream itself is part of the
 * published contract, so its exact values are pinned as golden numbers
 * alongside the usual property checks. */

#include "tests.h"

#include "rng.h"

void test_rng_deterministic(void)
{
    rng_t a, b, c;

    rng_init(&a, 42);
    rng_init(&b, 42);
    rng_init(&c, 43);
    for (int i = 0; i < 512; i++)
        CHECK_U64(rng_next(&a), rng_next(&b));

    /* A different seed must actually produce a different stream. */
    rng_init(&a, 42);
    int differ = 0;

    for (int i = 0; i < 64 && !differ; i++)
        if (rng_next(&a) != rng_next(&c))
            differ = 1;
    CHECK(differ);
}

void test_rng_golden_values(void)
{
    /* Regression pin for the v0.1 corpus stream (seed 42). */
    rng_t r;

    rng_init(&r, 42);
    CHECK_U64(rng_next(&r), 13630318207717698898ull);
    CHECK_U64(rng_next(&r), 14453732259950985564ull);
    CHECK_U64(rng_next(&r), 7138533898591876820ull);
    rng_init(&r, 42);
    CHECK_U64(rng_below(&r, 100), 98);
    CHECK_U64(rng_below(&r, 100), 64);
    CHECK_U64(rng_below(&r, 100), 20);
    CHECK_U64(rng_below(&r, 100), 14);
}

void test_rng_below_bounds(void)
{
    rng_t r;

    rng_init(&r, 7);
    for (int i = 0; i < 1000; i++)
        CHECK_U64(rng_below(&r, 1), 0);

    static const uint32_t ns[] = { 2, 3, 7, 100, 1000 };
    int bad = 0;

    for (unsigned k = 0; k < sizeof ns / sizeof ns[0] && !bad; k++)
        for (int i = 0; i < 20000; i++)
            if (rng_below(&r, ns[k]) >= ns[k])
                bad = 1;
    CHECK(!bad);
}

void test_rng_distribution(void)
{
    /* Loose bucket check: every value of rng_below(10) appears often,
     * none dominates. Catches a dropped or saturated bucket. */
    rng_t r;
    unsigned buckets[10] = { 0 };

    rng_init(&r, 99);
    for (int i = 0; i < 100000; i++)
        buckets[rng_below(&r, 10)]++;
    for (int i = 0; i < 10; i++)
        if (buckets[i] < 5000 || buckets[i] > 15000) {
            test_fail(__FILE__, __LINE__,
                      "bucket %d out of range: %u draws", i, buckets[i]);
            return;
        }
    CHECK(1);
}
