#ifndef LOTOFBENCH_H
#define LOTOFBENCH_H

/* LotofBench — open-source Geekbench-style CPU benchmark.
 *
 * Shared constants. The scoring baselines live here on purpose: they are
 * the calibration points of the suite, documented in docs/benchmarks.md
 * (section "Scoring"). One baseline per workload — LotofBench scores are
 * its own calibration and are NOT comparable with Geekbench scores. */

#define LOTOFBENCH_VERSION   "0.12.1"

/* Workload identities (human name + machine slug). */
#define FC_WORKLOAD_NAME     "File Compression"
#define FC_WORKLOAD_SLUG     "file-compression"
#define NAV_WORKLOAD_NAME    "Navigation"
#define NAV_WORKLOAD_SLUG    "navigation"
#define HTML5_WORKLOAD_NAME  "HTML5 Browser"
#define HTML5_WORKLOAD_SLUG  "html5-browser"
#define PDF_WORKLOAD_NAME    "PDF Render"
#define PDF_WORKLOAD_SLUG    "pdf-render"
#define PL_WORKLOAD_NAME     "Photo Library"
#define PL_WORKLOAD_SLUG     "photo-library"
#define CL_WORKLOAD_NAME     "Clang"
#define CL_WORKLOAD_SLUG     "clang"
#define TP_WORKLOAD_NAME     "Text Processing"
#define TP_WORKLOAD_SLUG     "text-processing"
#define PF_WORKLOAD_NAME     "Photo Filter"
#define PF_WORKLOAD_SLUG     "photo-filter"
#define HD_WORKLOAD_NAME     "Horizon Detection"
#define HD_WORKLOAD_SLUG     "horizon-detection"
#define RT_WORKLOAD_NAME     "Ray Tracer"
#define RT_WORKLOAD_SLUG     "ray-tracer"
#define SFM_WORKLOAD_NAME    "Structure from Motion"
#define SFM_WORKLOAD_SLUG    "structure-from-motion"
#define AC_WORKLOAD_NAME     "Asset Compression"
#define AC_WORKLOAD_SLUG     "asset-compression"

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

/* Workload #5 — Photo Library: photos ingested per second (each photo
 * through the thumbnail ladder, the conv pass + auto-tag and the
 * record store). The 1000-point reference machine ingests 200.0
 * photos per second (~5 ms per 1280x960 photo). */
#define BASELINE_PHOTOLIBRARY_IPS    200.0
/* Workload #6 — Clang: thousands of source tokens compiled per second
 * (lexed, parsed and emitted as bytecode). The 1000-point reference
 * machine compiles about 8.5 million tokens per second. */
#define BASELINE_CLANG_KTOKENS        8500.0

/* Workload #7 — Text Processing: megabytes of text processed per
 * second (every document scanned once per pattern, then converted
 * Markdown-to-HTML). The 1000-point reference machine processes about
 * 200 million bytes per second. */
#define BASELINE_TEXTPROCESSING_MBPS  200.0

/* Workload #8 — Asset Compression: megabytes of asset input per second
 * (textures through the BC1/BC3 block codecs, meshes through the
 * geometry codec). The 1000-point reference machine pushes 50.0 MB of
 * asset input per second (the 16-asset set in about 93 ms single-core). */
#define BASELINE_ASSETCOMPRESSION_MBPS 50.0

/* Workload #13 — Photo Filter: millions of output pixels per second
 * (each photo contributes its full 1600x1200 filtered frame). The
 * 1000-point reference machine filters 50.0 million pixels per second
 * (a full photo through all five stages in ~37 ms). */
#define BASELINE_PHOTOFILTER_MPIXELS  50.0

/* Workload #12 — Horizon Detection: millions of input pixels processed
 * per second (each photo runs the full grayscale -> Canny -> Hough
 * pipeline). The 1000-point reference machine processes 50.0 million
 * pixels per second (the 8-photo set in ~307 ms, ~38 ms per photo). */
#define BASELINE_HORIZONDETECTION_MPIXELS 50.0

/* Workload #15 — Ray Tracer: millions of traced rays per second
 * (primary + shadow + reflection rays of the whole 8-view set, every
 * one of them counted exactly). The 1000-point reference machine traces
 * about 5.0 million rays per second through the BVH (the 8-view pass in
 * ~1.1 s single-core). */
#define BASELINE_RAYTRACER_MRAYS      5.0

/* Workload #16 — Structure from Motion: image pairs reconstructed per
 * second (each pair runs FAST-9 detection on both views, correspondence
 * pairing, RANSAC essential estimation, pose selection and
 * triangulation of every inlier). The 1000-point reference machine
 * reconstructs 100.0 pairs per second (a 12-pair set in ~120 ms
 * single-core). */
#define BASELINE_SFM_PAIRS            100.0

/* One published workload result (single- or multi-core). */
typedef struct {
    unsigned threads;    /* thread count actually used */
    double   throughput; /* MB/s or Medge/s, workload-dependent */
    long     score;
} lb_result;

#endif /* LOTOFBENCH_H */
