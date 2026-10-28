/* LotofBench tests — SHA-1 (sha1.[ch]) against the published NIST /
 * RFC 3174 test vectors. */

#include "tests.h"

#include "sha1.h"

#include <stdlib.h>
#include <string.h>

void test_sha1_known_vectors(void)
{
    char hex[41];

    sha1_hex((const uint8_t *)"", 0, hex);
    CHECK_STR(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    sha1_hex((const uint8_t *)"abc", 3, hex);
    CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

    /* 56 bytes: exactly one block short of the two-block boundary that
     * exercises the padding rules. */
    sha1_hex((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljkl"
                              "mklmnlmnomnopnopq",
             56, hex);
    CHECK_STR(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    sha1_hex((const uint8_t *)"The quick brown fox jumps over the lazy dog",
             43, hex);
    CHECK_STR(hex, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

void test_sha1_million_a(void)
{
    /* The NIST million-'a' vector: exercises the length encoding far
     * beyond a single block, at exactly 1,000,000 bytes. */
    const size_t n = 1000000;
    uint8_t *buf = malloc(n);
    char hex[41], hex2[41];
    uint8_t digest[20];

    if (!buf) {
        test_fail(__FILE__, __LINE__, "out of memory");
        return;
    }
    memset(buf, 'a', n);

    sha1_hex(buf, n, hex);
    CHECK_STR(hex, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    /* The raw-digest form must agree with the hex form. */
    sha1(buf, n, digest);
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < 20; i++) {
        hex2[2 * i] = digits[digest[i] >> 4];
        hex2[2 * i + 1] = digits[digest[i] & 15];
    }
    hex2[40] = '\0';
    CHECK_STR(hex2, hex);

    free(buf);
}
