#ifndef LOTOFBENCH_LZ4_H
#define LOTOFBENCH_LZ4_H

#include <stddef.h>
#include <stdint.h>

/* LZ4 block-format codec used by the File Compression workload.
 *
 * Both levels emit the standard LZ4 block format (token + literals +
 * little-endian 16-bit offset + match length, last 5 input bytes always
 * encoded as literals). Round-trip correctness is verified by the
 * workload itself through SHA-1, like Geekbench 6 verifies its codecs.
 *
 * Levels, mirroring Geekbench 6's "high and low compression ratios":
 *   LZ4_LEVEL_FAST - single-slot hash table, one candidate per probe
 *   LZ4_LEVEL_HIGH - chained candidates within the 64 KiB window,
 *                    deepest-match search (smaller output, slower)
 */
#define LZ4_LEVEL_FAST 1
#define LZ4_LEVEL_HIGH 2

/* Worst-case compressed size for lz4_compress() on n input bytes. */
size_t lz4_compress_bound(size_t n);

/* Compress src into out. out must hold at least
 * lz4_compress_bound(src_len) bytes. Returns the compressed size in
 * bytes, or 0 on error (input above the single-block size cap). */
size_t lz4_compress(const uint8_t *src, size_t src_len, uint8_t *out,
                    int level);

/* Decompress src into out (out_cap bytes available). Returns the
 * decompressed size in bytes, or 0 on malformed input, truncated
 * stream or capacity overflow. */
size_t lz4_decompress(const uint8_t *src, size_t src_len, uint8_t *out,
                      size_t out_cap);

#endif /* LOTOFBENCH_LZ4_H */
