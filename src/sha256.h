#ifndef LOTOFBENCH_SHA256_H
#define LOTOFBENCH_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* FIPS 180-4 SHA-256, single-buffer convenience forms, zero dependencies.
 * The upload path uses it for the portal proof of work — the digest of
 * "<session_id>:<nonce>" must have a server-set number of leading zero
 * bits (plan v2.1 §A7) — and the test suite cross-checks the compression
 * function through RFC 2104 / RFC 4231 HMAC-SHA-256 vectors. */

void sha256(const uint8_t *data, size_t len, uint8_t digest[32]);

/* Lowercase hexadecimal rendering, NUL-terminated (65 bytes). */
void sha256_hex(const uint8_t *data, size_t len, char hex[65]);

/* RFC 2104 HMAC over SHA-256. Keys longer than the 64-byte block are
 * hashed first (RFC 4231 §2), exactly like every conformant client.
 * `out` receives the 32 raw bytes. */
void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                 size_t data_len, uint8_t out[32]);

#endif /* LOTOFBENCH_SHA256_H */
