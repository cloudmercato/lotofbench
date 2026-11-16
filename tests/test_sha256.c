/* LotofBench tests — SHA-256 (sha256.[ch]) against the published FIPS
 * 180-4 vectors, plus RFC 2104 / RFC 4231 HMAC-SHA-256 cross-checks. */

#include "tests.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

void test_sha256_nist_vectors(void)
{
    char hex[65];
    uint8_t digest[32];

    /* FIPS 180-4 example vectors. */
    sha256_hex((const uint8_t *)"", 0, hex);
    CHECK_STR(hex,
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855");

    sha256_hex((const uint8_t *)"abc", 3, hex);
    CHECK_STR(hex,
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");

    /* 56 bytes (448 bits): exactly one byte past the 55-byte single-tail
     * limit, exercising the two-block padding path. */
    sha256_hex((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljkl"
                                "mklmnlmnomnopnopq",
               56, hex);
    CHECK_STR(hex,
              "248d6a61d20638b8e5c026930c3e6039"
              "a33ce45964ff2167f6ecedd419db06c1");

    /* 1000 'a' (8000 bits): the NIST multi-block example. */
    uint8_t *buf = malloc(1000);

    if (!buf) {
        test_fail(__FILE__, __LINE__, "out of memory");
        return;
    }
    memset(buf, 'a', 1000);
    sha256_hex(buf, 1000, hex);
    CHECK_STR(hex,
              "41edece42d63e8d9bf515a9ba6932e1c"
              "20cbc9f5a5d134645adb5db1b9737ea3");

    /* The raw-digest form must agree with the hex form. */
    static const char digits[] = "0123456789abcdef";
    char hex2[65];

    sha256(buf, 1000, digest);
    for (int i = 0; i < 32; i++) {
        hex2[2 * i] = digits[digest[i] >> 4];
        hex2[2 * i + 1] = digits[digest[i] & 15];
    }
    hex2[64] = '\0';
    CHECK_STR(hex2, hex);

    free(buf);
}

void test_sha256_hex(void)
{
    char hex[65];

    sha256_hex((const uint8_t *)"abc", 3, hex);
    CHECK_U64(strlen(hex), 64);
    /* lowercase only */
    for (const char *p = hex; *p; p++)
        CHECK((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));
    /* every byte position rendered */
    CHECK_STR(hex,
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");
}

static void check_hmac(const char *key_hex, const uint8_t *key,
                       size_t key_len, const char *data, const char *want)
{
    uint8_t mac[32];
    static const char digits[] = "0123456789abcdef";
    char hex[65];

    hmac_sha256(key, key_len, (const uint8_t *)data, strlen(data), mac);
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = digits[mac[i] >> 4];
        hex[2 * i + 1] = digits[mac[i] & 15];
    }
    hex[64] = '\0';
    (void)key_hex;
    CHECK_STR(hex, want);
}

void test_sha256_hmac_rfc4231(void)
{
    /* RFC 4231 test case 1: 20 bytes of 0x0b, data "Hi There". */
    uint8_t key1[20];

    memset(key1, 0x0b, sizeof(key1));
    check_hmac("tc1", key1, sizeof(key1), "Hi There",
               "b0344c61d8db38535ca8afceaf0bf12b"
               "881dc200c9833da726e9376c2e32cff7");

    /* RFC 4231 test case 2: key "Jefe". */
    check_hmac("tc2", (const uint8_t *)"Jefe", 4,
               "what do ya want for nothing?",
               "5bdcc146bf60754e6a042426089575c7"
               "5a003f089d2739839dec58b964ec3843");

    /* RFC 4231 test case 3: 20 bytes of 0xaa, 50 bytes of 0xdd. */
    uint8_t key3[20], data3[50];

    memset(key3, 0xaa, sizeof(key3));
    memset(data3, 0xdd, sizeof(data3));
    {
        uint8_t mac[32];
        static const char digits[] = "0123456789abcdef";
        char hex[65];

        hmac_sha256(key3, sizeof(key3), data3, sizeof(data3), mac);
        for (int i = 0; i < 32; i++) {
            hex[2 * i] = digits[mac[i] >> 4];
            hex[2 * i + 1] = digits[mac[i] & 15];
        }
        hex[64] = '\0';
        CHECK_STR(hex,
                  "773ea91e36800e46854db8ebd09181a7"
                  "2959098b3ef8c122d9635514ced565fe");
    }

    /* RFC 4231 test case 6: a 131-byte key (larger than the 64-byte
     * block) must be hashed first — exercises the long-key branch. */
    uint8_t key6[131];

    memset(key6, 0xaa, sizeof(key6));
    check_hmac("tc6", key6, sizeof(key6),
               "Test Using Larger Than Block-Size Key - Hash Key First",
               "60e431591ee0b67f0d8a26aacbf5b77f"
               "8e0bc6213728c5140546040f0ee37f54");
}
