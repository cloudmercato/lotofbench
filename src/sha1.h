#ifndef LOTOFBENCH_SHA1_H
#define LOTOFBENCH_SHA1_H

#include <stddef.h>
#include <stdint.h>

/* FIPS 180-1 SHA-1, single-buffer convenience forms. The File Compression
 * workload uses them the way Geekbench 6 does: to verify that what came
 * out of the decompressor is what went into the compressor. */

void sha1(const uint8_t *data, size_t len, uint8_t digest[20]);

/* Lowercase hexadecimal rendering, NUL-terminated (41 bytes). */
void sha1_hex(const uint8_t *data, size_t len, char hex[41]);

#endif /* LOTOFBENCH_SHA1_H */
