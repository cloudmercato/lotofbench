/* LotofBench tests — workload #1, File Compression
 * (workload_compression.[ch]): corpus determinism (the published
 * byte-identical-inputs promise, pinned to the v0.1 stream) and sane
 * workload runs. Round-trip correctness itself is covered by the lz4
 * tests; the workload re-verifies every run internally. */

#include "tests.h"

#include "sha1.h"
#include "workload_compression.h"

#include <stdlib.h>
#include <string.h>

void test_fc_deterministic_corpus(void)
{
    fc_ctx *a = fc_prepare(1, 42);
    fc_ctx *b = fc_prepare(1, 42);
    fc_ctx *c = fc_prepare(1, 43);

    if (!a || !b || !c) {
        test_fail(__FILE__, __LINE__, "fc_prepare failed");
        fc_free(a);
        fc_free(b);
        fc_free(c);
        return;
    }

    CHECK_U64(fc_len(a), 1000000u);
    CHECK_U64(fc_len(b), 1000000u);
    CHECK(memcmp(fc_corpus(a), fc_corpus(b), fc_len(a)) == 0);
    CHECK(memcmp(fc_corpus(a), fc_corpus(c), fc_len(a)) != 0);

    /* First bytes of the seed-42 corpus, pinned from v0.2 sources. */
    static const uint8_t head[8] = { 0x67, 0x20, 0x61, 0x61,
                                     0x70, 0x72, 0x6b, 0x61 };

    CHECK(memcmp(fc_corpus(a), head, sizeof head) == 0);

    fc_free(a);
    fc_free(b);
    fc_free(c);
}

void test_fc_corpus_golden(void)
{
    /* SHA-1 of the full 1 MB seed-42 corpus: pins the whole generator
     * (PRNG stream, letter table, mix) to its v0.1 behaviour. */
    fc_ctx *fc = fc_prepare(1, 42);
    char hex[41];

    if (!fc) {
        test_fail(__FILE__, __LINE__, "fc_prepare failed");
        return;
    }
    sha1_hex(fc_corpus(fc), fc_len(fc), hex);
    CHECK_STR(hex, "473641488db6d5e08b001b529d3ea42e86075f1a");
    fc_free(fc);
}

void test_fc_run_sane(void)
{
    fc_ctx *fc = fc_prepare(2, 7);
    unsigned t;
    double mbps;

    if (!fc) {
        test_fail(__FILE__, __LINE__, "fc_prepare failed");
        return;
    }
    t = 2;
    mbps = 0.0;
    CHECK(fc_run(fc, 1, &t, &mbps) == 0);
    CHECK(mbps > 1.0 && mbps < 100000.0);
    t = 1;
    CHECK(fc_run(fc, 1, &t, &mbps) == 0);
    fc_free(fc);
}

void test_fc_accessors_null_safe(void)
{
    CHECK(fc_corpus(NULL) == NULL);
    CHECK_U64(fc_len(NULL), 0);
}
