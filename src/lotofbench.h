#ifndef LOTOFBENCH_H
#define LOTOFBENCH_H

/* LotofBench — open-source Geekbench-style CPU benchmark.
 *
 * Shared constants. The scoring baselines live here on purpose: they are
 * the calibration points of the suite, documented in docs/benchmarks.md
 * (section "Scoring"). One baseline per workload — LotofBench scores are
 * its own calibration and are NOT comparable with Geekbench scores. */

#define LOTOFBENCH_VERSION   "0.6.0"

/* Workload identities (human name + machine slug). */
#define FC_WORKLOAD_NAME     "File Compression"
#define FC_WORKLOAD_SLUG     "file-compression"
#define NAV_WORKLOAD_NAME    "Navigation"
#define NAV_WORKLOAD_SLUG    "navigation"
#define HTML5_WORKLOAD_NAME  "HTML5 Browser"
#define HTML5_WORKLOAD_SLUG  "html5-browser"
#define PDF_WORKLOAD_NAME    "PDF Render"
#define PDF_WORKLOAD_SLUG    "pdf-render"
#define CL_WORKLOAD_NAME     "Clang"
#define CL_WORKLOAD_SLUG     "clang"
#define TP_WORKLOAD_NAME     "Text Processing"
#define TP_WORKLOAD_SLUG     "text-processing"

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

/* Workload #3 — HTML5 Browser: pages rendered per second (parse +
 * layout + raster of one 800x600 page). The 1000-point reference
 * machine renders 650 pages per second — about 1.5 milliseconds per
 * page, a round and honest yardstick. */
#define BASELINE_HTML5BROWSER_PPS     650.0

/* Workload #4 — PDF Render: millions of device pixels rasterized per
 * second. The 1000-point reference machine rasterizes 10.0 million
 * pixels per second (a full 1224x1584 park-map page in ~193 ms). */
#define BASELINE_PDFRENDER_MPIXELS    10.0
/* Workload #6 — Clang: thousands of source tokens compiled per second
 * (lexed, parsed and emitted as bytecode). The 1000-point reference
 * machine compiles about 8.5 million tokens per second. */
#define BASELINE_CLANG_KTOKENS        8500.0

/* Workload #7 — Text Processing: megabytes of text processed per
 * second (every document scanned once per pattern, then converted
 * Markdown-to-HTML). The 1000-point reference machine processes about
 * 200 million bytes per second. */
#define BASELINE_TEXTPROCESSING_MBPS  200.0

/* One published workload result (single- or multi-core). */
typedef struct {
    unsigned threads;    /* thread count actually used */
    double   throughput; /* MB/s or Medge/s, workload-dependent */
    long     score;
} lb_result;

#endif /* LOTOFBENCH_H */
