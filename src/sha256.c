/* FIPS 180-4 SHA-256 for the LotofBench upload path.
 *
 * Straightforward zero-dependency implementation in the style of
 * sha1.c: a streaming core (init/update/final) kept private, with the
 * public single-shot forms on top — the proof of work hashes millions
 * of short messages so every call allocates nothing, and HMAC-SHA-256
 * reuses the same streaming core instead of concatenating buffers. */

#include "sha256.h"

#include <string.h>

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

typedef struct {
    uint32_t state[8];
    uint64_t total_bits;
    uint8_t buf[64];
    size_t fill;
} sha256_ctx;

static void sha256_init(sha256_ctx *c)
{
    c->state[0] = 0x6a09e667u;
    c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u;
    c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu;
    c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu;
    c->state[7] = 0x5be0cd19u;
    c->total_bits = 0;
    c->fill = 0;
}

static void sha256_block(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) | ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) | (a & c) | (b & c);
        uint32_t t2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len)
{
    c->total_bits += (uint64_t)len * 8;

    if (c->fill) {
        size_t take = 64 - c->fill;

        if (take > len)
            take = len;
        memcpy(c->buf + c->fill, data, take);
        c->fill += take;
        data += take;
        len -= take;
        if (c->fill == 64) {
            sha256_block(c->state, c->buf);
            c->fill = 0;
        }
    }
    for (; len >= 64; len -= 64, data += 64)
        sha256_block(c->state, data);
    if (len) {
        memcpy(c->buf, data, len);
        c->fill = len;
    }
}

static void sha256_final(sha256_ctx *c, uint8_t digest[32])
{
    uint64_t total_bits = c->total_bits;
    uint8_t tail[128]; /* remainder + padding + length, worst case 128 */
    size_t padded = c->fill + 1;
    size_t zeros = (padded <= 56) ? 56 - padded : 120 - padded;

    /* Copy the remainder, append the 0x80 marker and the 64-bit
     * big-endian bit length — the tail is hashed directly as full
     * blocks, never fed back through update (it already contains the
     * buffered remainder). */
    memcpy(tail, c->buf, c->fill);
    tail[c->fill] = 0x80;
    memset(tail + padded, 0, zeros);
    for (int i = 0; i < 8; i++)
        tail[padded + zeros + i] = (uint8_t)(total_bits >> (56 - 8 * i));

    sha256_block(c->state, tail);
    if (padded > 56)
        sha256_block(c->state, tail + 64);

    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(c->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)c->state[i];
    }
}

void sha256(const uint8_t *data, size_t len, uint8_t digest[32])
{
    sha256_ctx c;

    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, digest);
}

void sha256_hex(const uint8_t *data, size_t len, char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[32];

    sha256(data, len, digest);
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0xF];
    }
    hex[64] = '\0';
}

void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                 size_t data_len, uint8_t out[32])
{
    uint8_t k0[64], ipad[64], opad[64];
    sha256_ctx inner, outer;

    /* Keys longer than the block are replaced by their digest (RFC 2104). */
    if (key_len > 64) {
        sha256(key, key_len, k0);
        memset(k0 + 32, 0, 32);
    } else {
        memcpy(k0, key, key_len);
        memset(k0 + key_len, 0, 64 - key_len);
    }
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36);
        opad[i] = (uint8_t)(k0[i] ^ 0x5c);
    }

    sha256_init(&inner);
    sha256_update(&inner, ipad, 64);
    sha256_update(&inner, data, data_len);

    uint8_t inner_digest[32];
    sha256_final(&inner, inner_digest);

    sha256_init(&outer);
    sha256_update(&outer, opad, 64);
    sha256_update(&outer, inner_digest, 32);
    sha256_final(&outer, out);
}
