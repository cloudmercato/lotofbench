/* LotofBench — File Compression workload (workload #1).
 *
 * Mirrors the first Geekbench 6 CPU workload: compress and decompress a
 * corpus with LZ4 at two compression ratios, verify every round-trip
 * with SHA-1, and score the resulting throughput. The methodology lives
 * in the shared harness (harness.c): deterministic corpus generated off
 * the clock, wall-clock timing around the full
 * compress + decompress + verify cycle, start-gated runs, median over
 * the runs.
 *
 * The corpus generator is byte-for-byte the v0.1 one: same PRNG stream
 * (rng.c), same Zipf letter table, same mix of pseudo-text words,
 * repeated phrases and zero runs. */

#include "workload_compression.h"

#include "harness.h"
#include "lotofbench.h"
#include "lz4.h"
#include "rng.h"
#include "sha1.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LETTER_TABLE_SIZE 1024

struct fc_ctx {
    uint8_t *corpus;
    size_t len;
};

/* Zipf-distributed letters make the corpus behave like real source text:
 * uneven symbol frequencies are what an entropy-compression workload is
 * built to squeeze, uniform noise would compress to nothing interesting. */
static void build_letter_table(uint8_t table[LETTER_TABLE_SIZE])
{
    double weights[26], total = 0.0;

    for (int i = 0; i < 26; i++) {
        weights[i] = 1.0 / pow((double)i + 1.0, 1.1);
        total += weights[i];
    }
    size_t t = 0;
    for (int i = 0; i < 26; i++) {
        size_t count =
            (size_t)(weights[i] / total * LETTER_TABLE_SIZE + 0.5);
        while (count-- && t < LETTER_TABLE_SIZE)
            table[t++] = (uint8_t)('a' + i);
    }
    while (t < LETTER_TABLE_SIZE)
        table[t++] = 'a';
}

/* Corpus mix: pseudo-text words (most of the data), repeated phrases at
 * various back-distances (match candidates with real offsets) and zero
 * runs (long RLE-style matches). Same seed => same bytes everywhere. */
static void fill_corpus(rng_t *rng, uint8_t *buf, size_t len,
                        const uint8_t letters[LETTER_TABLE_SIZE])
{
    size_t i = 0;

    while (i < len) {
        uint32_t mode = rng_below(rng, 100);
        if (mode < 8) {
            size_t n = 64 + rng_below(rng, 4032);
            if (n > len - i)
                n = len - i;
            memset(buf + i, 0, n);
            i += n;
        } else if (mode < 20 && i > 2048) {
            size_t n = 16 + rng_below(rng, 80);
            size_t back = 512 + rng_below(rng, 512);
            if (n > len - i)
                n = len - i;
            memcpy(buf + i, buf + i - back, n);
            i += n;
        } else {
            size_t n = 1 + rng_below(rng, 12);
            for (size_t k = 0; k < n && i < len; k++, i++)
                buf[i] = letters[rng_below(rng, LETTER_TABLE_SIZE)];
            if (i < len)
                buf[i++] = ' ';
        }
    }
}

fc_ctx *fc_prepare(unsigned corpus_mb, unsigned seed)
{
    fc_ctx *ctx = calloc(1, sizeof(*ctx));

    if (!ctx)
        return NULL;
    ctx->len = (size_t)corpus_mb * 1000000u;
    ctx->corpus = malloc(ctx->len);
    if (!ctx->corpus) {
        free(ctx);
        return NULL;
    }

    rng_t rng;
    uint8_t letters[LETTER_TABLE_SIZE];

    rng_init(&rng, seed);
    build_letter_table(letters);
    fill_corpus(&rng, ctx->corpus, ctx->len, letters);
    return ctx;
}

void fc_free(fc_ctx *ctx)
{
    if (!ctx)
        return;
    free(ctx->corpus);
    free(ctx);
}

/* One thread's slice of the corpus. */
typedef struct {
    const uint8_t *src;
    size_t len;
    uint8_t *cbuf;
    uint8_t *dbuf;
    char ref_hex[41];
} fc_slice;

/* One full File Compression cycle over one slice: compress with both
 * ratios, decompress both, SHA-1-verify both against the source. */
static int fc_cycle(void *arg)
{
    fc_slice *s = arg;

    for (int level = LZ4_LEVEL_FAST; level <= LZ4_LEVEL_HIGH; level++) {
        size_t clen = lz4_compress(s->src, s->len, s->cbuf, level);
        size_t dlen;
        char hex[41];

        if (clen == 0)
            return -1;
        dlen = lz4_decompress(s->cbuf, clen, s->dbuf, s->len);
        if (dlen != s->len)
            return -1;
        sha1_hex(s->dbuf, dlen, hex);
        if (memcmp(hex, s->ref_hex, 41) != 0)
            return -1;
    }
    return 0;
}

int fc_run(fc_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mbps)
{
    unsigned t_count = *threads;
    size_t chunk = ctx->len / t_count;

    chunk -= chunk % 64;
    if (chunk < 64) { /* corpus too small for this many threads */
        chunk = 64;
        t_count = (unsigned)(ctx->len / chunk);
        if (t_count == 0)
            t_count = 1;
    }

    fc_slice *slices = calloc(t_count, sizeof(*slices));
    void **ctxs = malloc(t_count * sizeof(*ctxs));
    double elapsed[MAX_RUNS], times[MAX_RUNS];
    int rc = -1;

    if (!slices || !ctxs)
        goto out;

    for (unsigned t = 0; t < t_count; t++) {
        size_t start = (size_t)chunk * t;
        size_t len = (t == t_count - 1) ? ctx->len - start : chunk;

        slices[t].src = ctx->corpus + start;
        slices[t].len = len;
        slices[t].cbuf = malloc(lz4_compress_bound(len));
        slices[t].dbuf = malloc(len);
        if (!slices[t].cbuf || !slices[t].dbuf)
            goto out;
        sha1_hex(slices[t].src, len, slices[t].ref_hex);
        ctxs[t] = &slices[t];
    }

    rc = harness_run(t_count, runs, fc_cycle, ctxs, elapsed);
    if (rc == 0) {
        for (unsigned r = 0; r < runs; r++)
            times[r] = elapsed[r] > 0.0 ? (double)ctx->len / elapsed[r]
                                        : -1.0;
        *throughput_mbps = lb_median(times, runs) / 1e6;
        *threads = t_count;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++) {
            free(slices[t].cbuf);
            free(slices[t].dbuf);
        }
    free(slices);
    free(ctxs);
    return rc;
}
