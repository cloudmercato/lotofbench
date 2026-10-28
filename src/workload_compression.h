#ifndef WORKLOAD_COMPRESSION_H
#define WORKLOAD_COMPRESSION_H

#include <stddef.h>
#include <stdint.h>

/* Workload #1 — File Compression.
 *
 * Compress and decompress a deterministic synthetic corpus with LZ4 at
 * two compression ratios, SHA-1-verify every round-trip, score the
 * throughput (corpus MB per second of the full cycle). See
 * docs/benchmarks.md, section "Workload 1". */

typedef struct fc_ctx fc_ctx;

/* Build the corpus (off the clock; fixed seed => byte-identical
 * everywhere, and identical to v0.1's corpus for the same seed). */
fc_ctx *fc_prepare(unsigned corpus_mb, unsigned seed);
void    fc_free(fc_ctx *ctx);

/* One workload pass with *threads threads (clamped down to what the
 * corpus size supports). Fills *throughput_mbps with the median over the
 * runs. Returns 0, or -1 if a round-trip verification failed. */
int fc_run(fc_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mbps);

#endif /* WORKLOAD_COMPRESSION_H */
