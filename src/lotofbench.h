#ifndef LOTOFBENCH_H
#define LOTOFBENCH_H

/* LotofBench — open-source Geekbench-style CPU benchmark.
 *
 * Shared constants. The scoring baseline lives here on purpose: it is the
 * single calibration point of the suite, documented in
 * docs/benchmarks.md (section "Scoring"). */

#define LOTOFBENCH_VERSION   "0.1.0"
#define LOTOFBENCH_WORKLOAD  "File Compression"

/* Corpus and harness defaults — all overridable on the command line. */
#define DEFAULT_CORPUS_MB    100u
#define DEFAULT_RUNS         5u
#define DEFAULT_THREADS_CAP  64u
#define DEFAULT_SEED         1u

/* Hard cap on --runs: per-run state is kept in fixed-size arrays. */
#define MAX_RUNS             64u

/* Score of the reference machine's throughput on this workload, the
 * Geekbench way: score = throughput / baseline * 1000. The baseline is a
 * LotofBench calibration constant, NOT a Geekbench number — lotofbench
 * scores are not comparable with Geekbench scores. */
#define BASELINE_FILECOMPRESSION_MBPS 250.0

#endif /* LOTOFBENCH_H */
