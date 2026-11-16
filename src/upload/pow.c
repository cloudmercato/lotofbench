/* Proof-of-work solver for the LotofBench ingest portal.
 *
 * The search is embarrassingly simple and deliberately deterministic:
 * start at nonce 0, hash "<prefix>:<nonce>", count the leading zero
 * bits, increment. Each candidate hashes a short string with the
 * zero-dependency SHA-256 of sha256.c, so the solver allocates nothing
 * on the hot path and a difficulty-22 session (the portal default)
 * resolves in a few million hashes — seconds on any modern CPU. */

#include "pow.h"

#include "../sha256.h"

#include <stdio.h>
#include <string.h>

#define POW_PREFIX_MAX 64
#define POW_PROGRESS_EVERY (1ULL << 20)

int pow_leading_zero_bits(const uint8_t digest[32])
{
    int zeros = 0;

    for (int i = 0; i < 32; i++) {
        uint8_t byte = digest[i];

        if (byte == 0) {
            zeros += 8;
            continue;
        }
        for (unsigned mask = 0x80u; mask && !(byte & mask); mask >>= 1)
            zeros++;
        break;
    }
    return zeros;
}

int pow_solve(const char *prefix, unsigned difficulty,
              unsigned long long *nonce_out, FILE *progress)
{
    size_t prefix_len;

    if (!prefix || !nonce_out)
        return -1;
    prefix_len = strlen(prefix);
    if (prefix_len == 0 || prefix_len > POW_PREFIX_MAX)
        return -1;
    if (difficulty > 64)
        return -1; /* expected search size past 2^64: refuse, do not spin */

    char msg[POW_PREFIX_MAX + 1 + 20 + 1]; /* prefix ':' nonce NUL */
    memcpy(msg, prefix, prefix_len);
    msg[prefix_len] = ':';
    char *nonce_str = msg + prefix_len + 1;

    for (unsigned long long nonce = 0;; nonce++) {
        snprintf(nonce_str, 21, "%llu", nonce);
        size_t msg_len = prefix_len + 1 + strlen(nonce_str);

        uint8_t digest[32];
        sha256((const uint8_t *)msg, msg_len, digest);

        if (pow_leading_zero_bits(digest) >= (int)difficulty) {
            *nonce_out = nonce;
            return 0;
        }
        if (progress && (nonce + 1) % POW_PROGRESS_EVERY == 0) {
            fprintf(progress,
                    "lotofbench: proof of work: %llu candidates hashed, "
                    "target %u leading zero bits\n",
                    nonce + 1, difficulty);
            fflush(progress);
        }
    }
}
