#ifndef LOTOFBENCH_H
#define LOTOFBENCH_H

/* LotofBench — open-source Geekbench-style CPU benchmark.
 *
 * Shared constants. The scoring baselines live here on purpose: they are
 * the calibration points of the suite, documented in docs/benchmarks.md
 * (section "Scoring"). One baseline per workload — LotofBench scores are
 * its own calibration and are NOT comparable with Geekbench scores. */

#define LOTOFBENCH_VERSION   "0.2.0"

/* Workload identities (human name + machine slug). */
#define FC_WORKLOAD_NAME     "File Compression"
#define FC_WORKLOAD_SLUG     "file-compression"
#define NAV_WORKLOAD_NAME    "Navigation"
#define NAV_WORKLOAD_SLUG    "navigation"

/* Suite and harness defaults — all overridable on the command line
 * (except the thread cap). */
#define DEFAULT_CORPUS_MB    100u
#define DEFAULT_RUNS         5u
#define DEFAULT_THREADS_CAP  64u
#define DEFAULT_SEED         1u

/* Hard cap on --runs: per-run state is kept in fixed-size arrays. */
#define MAX_RUNS             64u

/* Score of the reference machine's throughput on a workload, the
 * Geekbench way: score = throughput / baseline * 1000. Baselines are
 * LotofBench calibration constants, NOT Geekbench numbers — LotofBench
 * scores are not comparable with Geekbench scores. */

/* Workload #1 — File Compression: corpus megabytes per second. */
#define BASELINE_FILECOMPRESSION_MBPS 250.0

/* Workload #2 — Navigation: millions of graph edges relaxed per second.
 * The 1000-point reference machine relaxes 100.0 million edges per
 * second — one edge per nanosecond, a round and honest yardstick. */
#define BASELINE_NAVIGATION_MEDGES    100.0

/* One published workload result (single- or multi-core). */
typedef struct {
    unsigned threads;    /* thread count actually used */
    double   throughput; /* MB/s or Medge/s, workload-dependent */
    long     score;
} lb_result;

#endif /* LOTOFBENCH_H */
