/* LotofBench tests — the LZ4 block codec (lz4.[ch]).
 *
 * Correctness: round-trips across the codec's edge paths (empty input,
 * single byte, no-match tails, extended literal/match lengths, the
 * 64 KiB window), plus the format-level guarantees (bound respected,
 * malformed offsets rejected, truncation and capacity overflow caught).
 * Stability: the exact output of the encoder on fixed inputs is pinned
 * as golden SHA-1s, so a silent format change cannot slip in. */

#include "tests.h"

#include "lz4.h"
#include "rng.h"
#include "sha1.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Round-trip one buffer at both levels: compress must fit the bound,
 * decompress must restore the source byte for byte. Returns 0 on
 * success, -1 on failure (failure details are reported by the caller). */
static int roundtrip_ok(const uint8_t *src, size_t len)
{
    size_t bound = lz4_compress_bound(len);
    uint8_t *cbuf = malloc(bound ? bound : 1);
    uint8_t *dbuf = malloc(len ? len : 1);
    int ok = 1;

    if (!cbuf || !dbuf) {
        free(cbuf);
        free(dbuf);
        return -1;
    }
    for (int level = LZ4_LEVEL_FAST; level <= LZ4_LEVEL_HIGH; level++) {
        size_t clen = lz4_compress(src, len, cbuf, level);
        size_t dlen;

        if (clen == 0 || clen > bound) {
            ok = 0;
            break;
        }
        dlen = lz4_decompress(cbuf, clen, dbuf, len);
        if (dlen != len || (len && memcmp(dbuf, src, len) != 0)) {
            ok = 0;
            break;
        }
    }
    free(cbuf);
    free(dbuf);
    return ok ? 0 : -1;
}

void test_lz4_roundtrip_shapes(void)
{
    /* Sizes sweep the format's edge paths: pure literals (short inputs),
     * extended length encodings, offsets near the 64 KiB window limit,
     * and inputs longer than the window. Two fills: maximal matches
     * (one repeated byte) and no matches (PRNG bytes). */
    static const size_t sizes[] = { 0,   1,     2,     4,     5,
                                    12,  13,    16,    63,    64,
                                    4095, 4096, 65535, 65536, 100000 };
    uint8_t *buf = malloc(sizes[sizeof sizes / sizeof sizes[0] - 1]);
    rng_t r;

    if (!buf) {
        test_fail(__FILE__, __LINE__, "out of memory");
        return;
    }
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        size_t n = sizes[k];

        memset(buf, 'a', n);
        if (roundtrip_ok(buf, n) != 0) {
            test_fail(__FILE__, __LINE__, "round-trip failed: %zu x 'a'",
                      n);
            break;
        }
        rng_init(&r, (uint64_t)n * 7919u + 13u);
        for (size_t i = 0; i < n; i++)
            buf[i] = (uint8_t)rng_next(&r);
        if (roundtrip_ok(buf, n) != 0) {
            test_fail(__FILE__, __LINE__, "round-trip failed: %zu random",
                      n);
            break;
        }
    }
    free(buf);
}

void test_lz4_golden_vectors(void)
{
    /* Golden encoder outputs, generated from v0.2 sources: exact length
     * plus SHA-1 of the compressed block (compactly pins the format). */
    uint8_t in[4096], cbuf[16384];
    char hex[41];

    memset(in, 'a', sizeof in);
    CHECK_U64(lz4_compress(in, sizeof in, cbuf, LZ4_LEVEL_FAST), 26);
    sha1_hex(cbuf, 26, hex);
    CHECK_STR(hex, "c86650eb08c8abe6c3491ac6b14f8b3f06a9d8b1");
    CHECK_U64(lz4_compress(in, sizeof in, cbuf, LZ4_LEVEL_HIGH), 26);
    sha1_hex(cbuf, 26, hex);
    CHECK_STR(hex, "c86650eb08c8abe6c3491ac6b14f8b3f06a9d8b1");

    rng_t r;

    rng_init(&r, 1234);
    for (size_t i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)rng_next(&r);
    CHECK_U64(lz4_compress(in, sizeof in, cbuf, LZ4_LEVEL_FAST), 4114);
    sha1_hex(cbuf, 4114, hex);
    CHECK_STR(hex, "92770d3fdfc6facf2fb07a8673b36e171828db49");
    CHECK_U64(lz4_compress(in, sizeof in, cbuf, LZ4_LEVEL_HIGH), 4114);
    sha1_hex(cbuf, 4114, hex);
    CHECK_STR(hex, "92770d3fdfc6facf2fb07a8673b36e171828db49");
}

void test_lz4_incompressible_overhead(void)
{
    /* Random bytes carry no matches: the output must be at least the
     * input (per-sequence overhead) and within the advertised bound. */
    rng_t r;
    uint8_t src[4096], cbuf[8192];

    rng_init(&r, 31337);
    for (size_t i = 0; i < sizeof src; i++)
        src[i] = (uint8_t)rng_next(&r);
    size_t clen = lz4_compress(src, sizeof src, cbuf, LZ4_LEVEL_FAST);

    CHECK(clen >= sizeof src);
    CHECK(clen <= lz4_compress_bound(sizeof src));
}

void test_lz4_rejects_malformed(void)
{
    uint8_t back[16384], src[8192], comp[16384];
    size_t clen;

    /* token 0x40 = 4 literals then a 4-byte match; LZ4 offsets are
     * backward and 1-based, so offset 0 and offsets reaching before the
     * output start are invalid. */
    uint8_t off_zero[] = { 0x40, 'a', 'b', 'c', 'd', 0x00, 0x00 };
    uint8_t off_before_start[] = { 0x40, 'a', 'b', 'c', 'd', 0x05, 0x00 };

    CHECK_U64(lz4_decompress(off_zero, sizeof off_zero, back, sizeof back),
              0);
    CHECK_U64(lz4_decompress(off_before_start, sizeof off_before_start,
                             back, sizeof back),
              0);

    /* Truncated real stream. */
    memset(src, 'a', sizeof src);
    clen = lz4_compress(src, sizeof src, comp, LZ4_LEVEL_FAST);
    CHECK(clen > 0);
    CHECK_U64(lz4_decompress(comp, clen - 1, back, sizeof back), 0);

    /* Output capacity smaller than the content. */
    CHECK_U64(lz4_decompress(comp, clen, back, sizeof src - 1), 0);

    /* Unknown compression level. */
    CHECK_U64(lz4_compress(src, sizeof src, comp, 3), 0);
}

/* --- regression: the codec must survive a 128 KiB thread stack ---------- */

/* lz4_compress() used to place its 256 KiB hash table and the 128 KiB
 * high-level chain on the caller's stack. glibc threads (8 MiB default)
 * never noticed, but musl gives every pthread a 128 KiB stack — the
 * default in alpine/scratch containers — so the first multi-core run in
 * such a container segfaulted before doing any work. The tables are
 * _Thread_local now; this test pins the contract: the codec works in a
 * worker thread with a 128 KiB stack, the smallest stack we run on. */

#define TINY_STACK_BYTES (128u * 1024u)

static void *tiny_stack_worker(void *arg)
{
    int *status = arg;
    static const size_t n = 65536; /* past MF_LIMIT: the match loop and
                                    * the high-level chain really run */
    uint8_t *src = malloc(n);
    rng_t r;

    if (!src) {
        *status = -1;
        return NULL;
    }
    /* Half repeated bytes (long matches), half PRNG (literals): both
     * encoder paths in one round-trip, at both levels. */
    memset(src, 'a', n / 2);
    rng_init(&r, 20261029);
    for (size_t i = n / 2; i < n; i++)
        src[i] = (uint8_t)rng_next(&r);
    *status = roundtrip_ok(src, n);
    free(src);
    return NULL;
}

void test_lz4_small_thread_stack(void)
{
    pthread_attr_t attr;
    pthread_t t;
    int status = -1;

    CHECK(pthread_attr_init(&attr) == 0);
    CHECK(pthread_attr_setstacksize(&attr, TINY_STACK_BYTES) == 0);
    /* pthread functions report errors via the return value, not errno. */
    int rc = pthread_create(&t, &attr, tiny_stack_worker, &status);
    if (rc != 0) {
        test_fail(__FILE__, __LINE__,
                  "pthread_create(%u KiB stack): %s",
                  TINY_STACK_BYTES / 1024u, strerror(rc));
        return;
    }
    pthread_join(t, NULL);
    if (status != 0)
        test_fail(__FILE__, __LINE__,
                  "round-trip failed in a 128 KiB-stack thread (musl "
                  "worker stacks are this small)");
}
