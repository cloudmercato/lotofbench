# What it measures

Metric, score and correctness rule for every workload, plus the
composite. The workload descriptions and the benchmark methodology
live in [docs/benchmarks.md](benchmarks.md); the README summarises
this page in one paragraph.

## Composite Score

Published alongside the per-workload scores, the Geekbench 6 way. The
workloads are split into an Integer section (File Compression,
Navigation, HTML5 Browser, PDF Render, Photo Library, Clang, Text
Processing, Asset Compression) and a Floating-Point section (Object
Detection, Background Blur, Object Remover, Horizon Detection, Photo
Filter, HDR, Ray Tracer, Structure from Motion). Each section score is
the geometric mean of its member workloads' scores, rounded half up;
the composite is the weighted arithmetic mean of the two sections
(65% Integer / 35% Floating Point), rounded half up. A machine
scoring exactly 1000 on every workload scores 1000 on the composite.
Both single-core and multi-core composites are published — and never
compared with Geekbench scores (own baselines).

## File Compression

- **Metric** — throughput of the full workload cycle (compress +
  decompress + SHA-1 verify at two LZ4 ratios), in decimal MB/s, median
  over the runs.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (250.0 MB/s = 1000) is defined in `src/lotofbench.h` and documented in
  [docs/benchmarks.md](benchmarks.md#scoring).
- **Correctness** — a run only counts if every decompressed buffer
  SHA-1-matches its source; a broken codec aborts instead of scoring.

## Navigation

- **Metric** — millions of graph edges relaxed per second (Medge/s):
  every directed edge scanned by the searches is counted exactly, and
  the score is that count divided by the wall-clock time of the whole
  route set, median over the runs. The count is deterministic — same
  seed, same edge count, whatever the machine — so it plays the role
  the corpus size plays for File Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (100.0 Medge/s = 1000, i.e. one edge relaxed per nanosecond) is
  defined in `src/lotofbench.h`.
- **Correctness** — every found route is verified inside the timed
  cycle: the parent chain is walked back to the source and re-summed
  edge by edge (it must reproduce the settled travel time exactly, and
  the result must also clear a straight-line lower bound); a broken
  search aborts instead of scoring.

## HTML5 Browser

- **Metric** — pages per second: every run parses, lays out and
  rasters the 32-page set from scratch, and the throughput is the page
  count divided by the wall-clock time, median over the runs. The
  element count of every page is deterministic — same seed, same DOM,
  whatever the machine — so it plays the role the corpus size plays
  for File Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (650.0 pages/s = 1000, i.e. about 1.5 ms per page) is defined in
  `src/lotofbench.h`.
- **Correctness** — every page is verified inside the timed cycle
  against an off-clock reference render: element count, display-list
  record count and SHA-1 digest of the rendered framebuffer must all
  match exactly; a broken pipeline aborts instead of scoring.

## Horizon Detection

- **Metric** — millions of input pixels detected per second
  (Mpixel/s): every run runs the whole photo set through the full
  Canny chain and Hough transform, and the throughput is the exact
  input-pixel count (eight 1600×1200 frames) divided by the wall-clock
  time, median over the runs. The pixel count is exact and
  machine-independent, like the corpus size for File Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline is
  defined in `src/lotofbench.h`.
- **Correctness** — every photo is detected inside the timed cycle and
  verified twice: the detection must reproduce the prepare-time
  reference result bit-for-bit, and it must match the planted ground
  truth (normal within 1°, rho within 3 px, above the vote floor); a
  broken pipeline aborts instead of scoring.

## Photo Filter

- **Metric** — millions of output pixels filtered per second
  (Mpixel/s): every run filters the whole ten-photo set through all
  five stages, and the throughput is the exact output-pixel count (ten
  1600×1200 frames) divided by the wall-clock time, median over the
  runs. The pixel count is exact and machine-independent, like the
  corpus size for File Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline is
  defined in `src/lotofbench.h`.
- **Correctness** — every photo is verified inside the timed cycle
  against its prepare-time reference digest: the filtered output's
  SHA-1 must match exactly; a broken pipeline aborts instead of
  scoring.

## HDR

- **Metric** — millions of fused output pixels per second (Mpixel/s):
  every run fuses the whole four-scene set through the full
  weight-map → pyramid → blend → collapse pipeline, and the throughput
  is the exact fused-pixel count (four 1600×1200 frames) divided by the
  wall-clock time, median over the runs. The pixel count is exact and
  machine-independent, like the corpus size for File Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (6.0 Mpixel/s = 1000) is defined in `src/lotofbench.h`.
- **Correctness** — every scene is verified inside the timed cycle
  against its prepare-time reference: the SHA-1 of the normalized
  weight stack and of the fused frame must match exactly, as must the
  three counters (weight-starved pixels, clamped output pixels, black
  output pixels), with the uniform-fallback and black-output guards at
  zero; a broken pipeline aborts instead of scoring.

## Ray Tracer

- **Metric** — millions of traced rays per second (Mray/s): every run
  re-renders the whole 8-view set through the BVH, and the throughput
  is the exact ray count (primary + shadow + reflection rays, all
  counted) divided by the wall-clock time, median over the runs. The
  count is deterministic — same seed, same count, whatever the machine
  — so it plays the role the corpus size plays for File Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline is
  defined in `src/lotofbench.h`.
- **Correctness** — every view is verified inside the timed cycle
  against its prepare-time reference: the SHA-1 digest of the frame and
  the exact ray count must both match, and the BVH renderer is
  bit-identical to the brute-force renderer by construction (the
  differential test validates the acceleration structure); a broken
  tracer aborts instead of scoring.

## PDF Render

- **Metric** — millions of device pixels rasterized per second
  (Mpixel/s): every run parses and renders the whole four-document set
  from scratch, and the throughput is the exact device-pixel count (four
  documents, two 1224×1584 pages each) divided by the wall-clock time,
  median over the runs. The pixel count is exact and
  machine-independent, like the corpus size for File Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (10.0 Mpixel/s = 1000, i.e. a full map page in about 193 ms) is
  defined in `src/lotofbench.h`.
- **Correctness** — the whole document set is parsed and rendered
  inside the timed cycle and verified against prepare-time references:
  content-stream SHA-1 digests, rendered-page digests, probe pixels and
  the operator count must all match exactly; any mismatch aborts
  instead of scoring.

## Photo Library

- **Metric** — photos ingested per second (img/s): every run re-imports
  the whole 16-photo set through the pipeline (thumbnail ladder, conv
  pass + auto-tag, record store), and the throughput is the exact photo
  count divided by the wall-clock time, median over the runs. The photo
  count is exact and machine-independent, like the corpus size for File
  Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (200.0 img/s = 1000, i.e. a 1280×960 photo in about 5 ms) is defined
  in `src/lotofbench.h`.
- **Correctness** — every photo is imported inside the timed cycle and
  verified on the way: the three thumbnail digests must match the
  prepare-time references, the auto-tag must recover the planted class,
  and the store's count and per-field queries must hold; any mismatch
  aborts instead of scoring.

## Asset Compression

- **Metric** — asset bytes pushed through the codecs per second
  (MB/s): every run re-encodes and decodes the whole 16-asset set
  (BC1 fast + high per opaque texture, BC3 per alpha texture, the
  geometry codec per mesh), and the throughput is the exact input-byte
  count divided by the wall-clock time, median over the runs. The byte
  count is exact and machine-independent, like the corpus size for
  File Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (50.0 MB/s = 1000, i.e. the 16-asset set in about 93 ms) is defined
  in `src/lotofbench.h`.
- **Correctness** — every codec pass is verified inside the timed
  cycle: the encoded stream must SHA-1-match its prepare-time
  reference, the decoded artifact must too, and the total squared RGB
  error of the decode must reproduce the pinned value (mesh round-trips
  are exact); any drift aborts instead of scoring.
