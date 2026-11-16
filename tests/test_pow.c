/* LotofBench tests — proof of work (upload/pow.[ch]). */

#include "tests.h"

#include "sha256.h"
#include "upload/pow.h"

#include <stdio.h>
#include <string.h>

void test_pow_leading_zero_bits(void)
{
    uint8_t d[32];

    /* Hand-checked byte patterns — the counter reads bits from the
     * most significant end and stops at the first set bit. */
    memset(d, 0, sizeof(d));
    CHECK_U64(pow_leading_zero_bits(d), 256);

    memset(d, 0xff, sizeof(d));
    CHECK_U64(pow_leading_zero_bits(d), 0);

    memset(d, 0x00, sizeof(d));
    d[0] = 0x01; /* 0000 0001 ... */
    CHECK_U64(pow_leading_zero_bits(d), 7);

    d[0] = 0x20; /* 0010 0000 ... */
    CHECK_U64(pow_leading_zero_bits(d), 2);

    d[0] = 0x40; /* 0100 0000 ... */
    CHECK_U64(pow_leading_zero_bits(d), 1);

    d[0] = 0x80; /* 1000 0000 ... */
    CHECK_U64(pow_leading_zero_bits(d), 0);

    memset(d, 0x00, sizeof(d));
    d[1] = 0x01; /* one zero byte then 0000 0001 ... */
    CHECK_U64(pow_leading_zero_bits(d), 15);
}

void test_pow_solve_small(void)
{
    unsigned long long nonce = 0;

    /* The reference nonce was computed independently with hashlib;
     * the solver must find exactly the smallest one. */
    CHECK_U64(pow_solve("lotofbench-selftest", 8, &nonce, NULL), 0);
    CHECK_U64(nonce, 241);

    /* The found nonce must satisfy the predicate when re-hashed. */
    char msg[64];
    uint8_t digest[32];

    snprintf(msg, sizeof(msg), "lotofbench-selftest:%llu", nonce);
    sha256((const uint8_t *)msg, strlen(msg), digest);
    CHECK(pow_leading_zero_bits(digest) >= 8);

    /* Difficulty 0 accepts the very first candidate: nonces start at 0. */
    nonce = 12345;
    CHECK_U64(pow_solve("any-prefix", 0, &nonce, NULL), 0);
    CHECK_U64(nonce, 0);
}

void test_pow_solve_progress(void)
{
    /* "lb-progress-lvdfqy" has its first difficulty-20 nonce at 1,737,353
     * (verified with hashlib): the search crosses the 2^20 progress step
     * before finishing, so the progress stream must be fed — and the
     * candidate count stays under ~2M hashes (< 1 s). */
    FILE *progress = tmpfile();
    unsigned long long nonce = 0;

    if (!progress) {
        test_fail(__FILE__, __LINE__, "tmpfile() failed");
        return;
    }
    CHECK_U64(pow_solve("lb-progress-lvdfqy", 20, &nonce, progress), 0);
    CHECK_U64(nonce, 1737353);

    rewind(progress);
    char buf[512] = { 0 };
    size_t got = fread(buf, 1, sizeof(buf) - 1, progress);

    CHECK(got > 0);
    CHECK(strstr(buf, "proof of work") != NULL);
    CHECK(strstr(buf, "20 leading zero bits") != NULL);
    fclose(progress);
}

void test_pow_rejects_absurd(void)
{
    unsigned long long nonce = 0;

    /* More than 64 bits of leading zeros is physically out of reach:
     * refuse instead of spinning forever. */
    CHECK_U64(pow_solve("prefix", 65, &nonce, NULL), -1);

    /* Empty or missing prefix: the message must contain "prefix:". */
    CHECK_U64(pow_solve("", 4, &nonce, NULL), -1);
    CHECK_U64(pow_solve(NULL, 4, &nonce, NULL), -1);

    /* Missing out parameter. */
    CHECK_U64(pow_solve("prefix", 4, NULL, NULL), -1);
}
