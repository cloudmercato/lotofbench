/* FIPS 180-1 SHA-1 for the LotofBench workload verification step.
 *
 * Straightforward single-shot implementation: the workload hashes whole
 * buffers once per cycle, so streaming state would only add complexity.
 * SHA-1 is cryptographically broken but perfectly adequate as a
 * round-trip data check, which is exactly how Geekbench 6 uses it. */

#include "sha1.h"

#include <string.h>

static uint32_t rol32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

static void sha1_block(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1(const uint8_t *data, size_t len, uint8_t digest[20])
{
    uint32_t state[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u,
    };
    uint64_t total_bits = (uint64_t)len * 8;
    uint8_t tail[128]; /* remainder + padding + length, worst case 120 */
    size_t full = len & ~(size_t)63;

    for (size_t i = 0; i < full; i += 64)
        sha1_block(state, data + i);

    /* Copy the remainder, append the 0x80 marker and the 64-bit big-endian
     * bit length. One block fits when the padded remainder ends at or
     * before byte 56; otherwise the length spills into a second block. */
    size_t rem = len - full;
    size_t padded = rem + 1;
    size_t zeros = (padded <= 56) ? 56 - padded : 120 - padded;

    memcpy(tail, data + full, rem);
    tail[rem] = 0x80;
    memset(tail + padded, 0, zeros);
    for (int i = 0; i < 8; i++)
        tail[padded + zeros + i] = (uint8_t)(total_bits >> (56 - 8 * i));

    sha1_block(state, tail);
    if (padded > 56)
        sha1_block(state, tail + 64);

    for (int i = 0; i < 5; i++) {
        digest[i * 4] = (uint8_t)(state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)state[i];
    }
}

void sha1_hex(const uint8_t *data, size_t len, char hex[41])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[20];

    sha1(data, len, digest);
    for (int i = 0; i < 20; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0xF];
    }
    hex[40] = '\0';
}
