#ifndef LOTOFBENCH_UPLOAD_POW_H
#define LOTOFBENCH_UPLOAD_POW_H

#include <stdio.h>
#include <stdint.h>

/* Proof of work of the LotofBench ingest portal (plan v2.1 §A7): the
 * session issues a difficulty D and the client must find the smallest
 * decimal nonce N such that SHA-256("<prefix>:<N>") has at least D
 * leading zero *bits*. The prefix is the session id, the nonce is an
 * incrementing decimal string starting at 0 — the exact string the
 * server re-hashes when it verifies the proof. */

/* Number of leading zero bits of a 32-byte digest (256 when all zero). */
int pow_leading_zero_bits(const uint8_t digest[32]);

/* Solve the proof of work. Returns 0 on success and stores the nonce
 * through `nonce_out`; returns -1 when the difficulty is impossible
 * (more than 64 bits) or the prefix is missing/too long. Progress is
 * reported one line per 2^20 candidates on `progress` (may be NULL;
 * the caller owns the stream — the CLI passes stderr, never stdout). */
int pow_solve(const char *prefix, unsigned difficulty,
              unsigned long long *nonce_out, FILE *progress);

#endif /* LOTOFBENCH_UPLOAD_POW_H */
