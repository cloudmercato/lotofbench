/* LotofBench — File Compression workload driver.
 *
 * Workload #1 of the suite, mirroring the first Geekbench 6 CPU workload
 * (File Compression): compress and decompress a corpus with LZ4 at two
 * compression ratios, verify every round-trip with SHA-1, and score the
 * resulting throughput single-core and multi-core. Multi-core follows
 * Geekbench 6's "shared task" model: every thread owns one equal slice
 * of the corpus and the score measures the task as a whole.
 *
 * Methodology:
 *   - deterministic synthetic corpus (fixed --seed), generated off the
 *     clock, byte-identical on every machine so scores are comparable
 *   - per run, wall-clock time around the full
 *     compress + decompress + verify cycle (CLOCK_MONOTONIC)
 *   - runs are synchronised through a start gate: every thread waits
 *     until all of them are ready, so multi-core elapsed time measures
 *     the whole task and not each thread's own wake-up jitter
 *   - reported throughput is the median over --runs runs
 *   - throughput is corpus megabytes (1 MB = 1e6 bytes) per second of
 *     full compress+decompress cycle
 */

#define _POSIX_C_SOURCE 200809L

#include "lotofbench.h"
#include "lz4.h"
#include "sha1.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- deterministic corpus -------------------------------------------------*/

#define LETTER_TABLE_SIZE 1024

static uint64_t rng_state;

static uint64_t rng_next(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ull;
}

static uint32_t rng_below(uint32_t n)
{
    return (uint32_t)(rng_next() % n);
}

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
static void fill_corpus(uint8_t *buf, size_t len,
                        const uint8_t letters[LETTER_TABLE_SIZE])
{
    size_t i = 0;

    while (i < len) {
        uint32_t mode = rng_below(100);
        if (mode < 8) {
            size_t n = 64 + rng_below(4032);
            if (n > len - i)
                n = len - i;
            memset(buf + i, 0, n);
            i += n;
        } else if (mode < 20 && i > 2048) {
            size_t n = 16 + rng_below(80);
            size_t back = 512 + rng_below(512);
            if (n > len - i)
                n = len - i;
            memcpy(buf + i, buf + i - back, n);
            i += n;
        } else {
            size_t n = 1 + rng_below(12);
            for (size_t k = 0; k < n && i < len; k++, i++)
                buf[i] = letters[rng_below(LETTER_TABLE_SIZE)];
            if (i < len)
                buf[i++] = ' ';
        }
    }
}

/* --- timing ----------------------------------------------------------------*/

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int double_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return (x > y) - (x < y);
}

static double median(double *values, size_t n)
{
    qsort(values, n, sizeof(double), double_cmp);
    if (n % 2)
        return values[n / 2];
    return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

/* --- workload ---------------------------------------------------------------*/

/* One full File Compression cycle over one buffer: compress with both
 * ratios, decompress both, SHA-1-verify both against the source. Returns
 * elapsed seconds, or -1.0 if any stage failed. */
static double cycle(const uint8_t *src, size_t len, uint8_t *cbuf,
                    uint8_t *dbuf, const char ref_hex[41])
{
    double t0 = now_sec();

    for (int level = LZ4_LEVEL_FAST; level <= LZ4_LEVEL_HIGH; level++) {
        size_t clen = lz4_compress(src, len, cbuf, level);
        size_t dlen;
        char hex[41];

        if (clen == 0)
            return -1.0;
        dlen = lz4_decompress(cbuf, clen, dbuf, len);
        if (dlen != len)
            return -1.0;
        sha1_hex(dbuf, dlen, hex);
        if (memcmp(hex, ref_hex, 41) != 0)
            return -1.0;
    }
    return now_sec() - t0;
}

/* --- threaded harness --------------------------------------------------------*/

typedef struct {
    const uint8_t *src;
    size_t len;
    uint8_t *cbuf;
    uint8_t *dbuf;
    char ref_hex[41];
    unsigned runs;
    int failed;
} job;

static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cv = PTHREAD_COND_INITIALIZER;
static unsigned live_threads = 0;
static unsigned ready_count = 0, done_count = 0, run_index = 0;
static double run_t0[MAX_RUNS], run_t1[MAX_RUNS];

static void *worker(void *arg)
{
    job *j = arg;

    for (unsigned r = 0; r < j->runs; r++) {
        pthread_mutex_lock(&sync_lock);
        while (run_index < r) /* next run opens when the previous closes */
            pthread_cond_wait(&sync_cv, &sync_lock);
        if (++ready_count == live_threads) {
            run_t0[r] = now_sec(); /* start gate: all threads ready */
            pthread_cond_broadcast(&sync_cv);
        } else {
            while (ready_count < live_threads)
                pthread_cond_wait(&sync_cv, &sync_lock);
        }
        pthread_mutex_unlock(&sync_lock);

        double t = cycle(j->src, j->len, j->cbuf, j->dbuf, j->ref_hex);

        pthread_mutex_lock(&sync_lock);
        if (t < 0.0)
            j->failed = 1;
        if (++done_count == live_threads) {
            run_t1[r] = now_sec(); /* last thread out closes the run */
            done_count = 0;
            ready_count = 0;
            run_index++;
            pthread_cond_broadcast(&sync_cv);
        } else {
            while (run_index == r)
                pthread_cond_wait(&sync_cv, &sync_lock);
        }
        pthread_mutex_unlock(&sync_lock);
    }
    return NULL;
}

/* Run the full workload with `threads` threads. On return *threads holds
 * the thread count actually used (it can be clamped down when the corpus
 * is small) and *throughput_mbps the median MB/s over the runs. Returns
 * 0 on success, -1 if any round-trip verification failed. */
static int run_suite(const uint8_t *corpus, size_t corpus_len, unsigned runs,
                     unsigned *threads, double *throughput_mbps)
{
    unsigned t_count = *threads;
    size_t chunk = corpus_len / t_count;

    chunk -= chunk % 64;
    if (chunk < 64) { /* corpus too small for this many threads */
        chunk = 64;
        t_count = (unsigned)(corpus_len / chunk);
        if (t_count == 0)
            t_count = 1;
    }

    live_threads = t_count;
    ready_count = 0;
    done_count = 0;
    run_index = 0;

    pthread_t *tids = malloc(t_count * sizeof(pthread_t));
    job *jobs = calloc(t_count, sizeof(job));
    double times[MAX_RUNS];
    int rc = 0;

    if (!tids || !jobs) {
        free(tids);
        free(jobs);
        return -1;
    }

    for (unsigned t = 0; t < t_count; t++) {
        size_t start = chunk * t;
        size_t len = (t == t_count - 1) ? corpus_len - start : chunk;

        jobs[t].src = corpus + start;
        jobs[t].len = len;
        jobs[t].cbuf = malloc(lz4_compress_bound(len));
        jobs[t].dbuf = malloc(len);
        jobs[t].runs = runs;
        jobs[t].failed = 0;
        if (!jobs[t].cbuf || !jobs[t].dbuf) {
            rc = -1;
            goto out_alloc_fail;
        }
        sha1_hex(jobs[t].src, len, jobs[t].ref_hex);
        pthread_create(&tids[t], NULL, worker, &jobs[t]);
    }
    for (unsigned t = 0; t < t_count; t++)
        pthread_join(tids[t], NULL);

    for (unsigned t = 0; t < t_count; t++) {
        if (jobs[t].failed) {
            rc = -1;
            goto out;
        }
    }
    for (unsigned r = 0; r < runs; r++) {
        double elapsed = run_t1[r] - run_t0[r];

        times[r] = elapsed > 0.0 ? (double)corpus_len / elapsed : -1.0;
    }
    *throughput_mbps = median(times, runs) / 1e6;
    *threads = t_count;

out:
    for (unsigned t = 0; t < t_count; t++) {
        free(jobs[t].cbuf);
        free(jobs[t].dbuf);
    }
out_alloc_fail:
    free(tids);
    free(jobs);
    return rc;
}

/* --- scoring and output --------------------------------------------------------*/

static long score_of(double mbps)
{
    return (long)(mbps / BASELINE_FILECOMPRESSION_MBPS * 1000.0 + 0.5);
}

typedef struct {
    unsigned corpus_mb, runs, seed;
    unsigned threads; /* requested multi-core thread count */
    int json;
} options;

static void usage(FILE *out)
{
    fprintf(out,
            "usage: lotofbench [--size-mb N] [--runs N] [--threads N] "
            "[--seed N] [--json]\n");
}

static options parse_args(int argc, char **argv)
{
    options o = { DEFAULT_CORPUS_MB, DEFAULT_RUNS, DEFAULT_SEED, 0, 0 };
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);

    if (nproc < 1)
        nproc = 1;
    o.threads = (unsigned)nproc;
    if (o.threads > DEFAULT_THREADS_CAP)
        o.threads = DEFAULT_THREADS_CAP;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size-mb") && i + 1 < argc) {
            o.corpus_mb = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--runs") && i + 1 < argc) {
            o.runs = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            o.threads = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            o.seed = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--json")) {
            o.json = 1;
        } else if (!strcmp(argv[i], "--help")) {
            usage(stdout);
            exit(0);
        } else {
            fprintf(stderr, "lotofbench: bad or incomplete option '%s'\n",
                    argv[i]);
            usage(stderr);
            exit(2);
        }
    }
    if (o.corpus_mb < 1)
        o.corpus_mb = 1;
    if (o.runs < 1 || o.runs > MAX_RUNS) {
        fprintf(stderr, "lotofbench: --runs must be between 1 and %u\n",
                MAX_RUNS);
        exit(2);
    }
    if (o.threads < 1)
        o.threads = 1;
    return o;
}

int main(int argc, char **argv)
{
    options opt = parse_args(argc, argv);
    size_t corpus_len = (size_t)opt.corpus_mb * 1000000u;
    uint8_t letters[LETTER_TABLE_SIZE];
    uint8_t *corpus;
    double single_mbps = 0.0, multi_mbps = 0.0;
    unsigned single_threads = 1, multi_threads = opt.threads;

    corpus = malloc(corpus_len);
    if (!corpus) {
        fprintf(stderr, "lotofbench: cannot allocate %zu-byte corpus\n",
                corpus_len);
        return 1;
    }

    /* Deterministic corpus, generated off the clock. */
    rng_state = (uint64_t)opt.seed * 0x9E3779B97F4A7C15ull +
                0xA0761D6478BD642Full;
    rng_next();
    build_letter_table(letters);
    fill_corpus(corpus, corpus_len, letters);

    if (run_suite(corpus, corpus_len, opt.runs, &single_threads,
                  &single_mbps) != 0 ||
        run_suite(corpus, corpus_len, opt.runs, &multi_threads,
                  &multi_mbps) != 0) {
        fprintf(stderr,
                "lotofbench: workload failed — a compress/decompress "
                "round-trip did not verify\n");
        free(corpus);
        return 1;
    }

    if (opt.json) {
        printf("{\n");
        printf("  \"benchmark\": \"lotofbench\",\n");
        printf("  \"version\": \"%s\",\n", LOTOFBENCH_VERSION);
        printf("  \"workload\": \"file-compression\",\n");
        printf("  \"corpus_mb\": %u,\n", opt.corpus_mb);
        printf("  \"seed\": %u,\n", opt.seed);
        printf("  \"runs\": %u,\n", opt.runs);
        printf("  \"baseline_mbps\": %.1f,\n",
               BASELINE_FILECOMPRESSION_MBPS);
        printf("  \"single\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
               "\"score\": %ld},\n",
               single_threads, single_mbps, score_of(single_mbps));
        printf("  \"multi\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
               "\"score\": %ld}\n",
               multi_threads, multi_mbps, score_of(multi_mbps));
        printf("}\n");
    } else {
        printf("LotofBench %s — %s\n", LOTOFBENCH_VERSION,
               LOTOFBENCH_WORKLOAD);
        printf("corpus %u MB synthetic (seed %u), %u runs, median\n\n",
               opt.corpus_mb, opt.seed, opt.runs);
        printf("Single-Core\n");
        printf("  throughput  %9.1f MB/s\n", single_mbps);
        printf("  score       %9ld   (baseline %.1f MB/s = 1000)\n\n",
               score_of(single_mbps), BASELINE_FILECOMPRESSION_MBPS);
        printf("Multi-Core (%u threads, shared task)\n", multi_threads);
        printf("  throughput  %9.1f MB/s\n", multi_mbps);
        printf("  score       %9ld\n", score_of(multi_mbps));
    }

    free(corpus);
    return 0;
}
