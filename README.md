# LotofBench

An open-source, dependency-free CPU benchmark in C, modelled on the
structure of Primate Labs' [Geekbench 6](https://www.geekbench.com):
identifiable workloads, single-core and multi-core results, and a
relative score against a fixed baseline.

**Not affiliated with Primate Labs.** LotofBench scores are its own
calibration and are *not* comparable with Geekbench scores.

## Status — workload 8 of 16

**File Compression**, **Navigation**, **HTML5 Browser**, **PDF Render**,
**Clang**, **Text Processing**, **Horizon Detection** and
**Photo Filter** are implemented (eight of the Geekbench 6 CPU
workloads). The full
workload list, the roadmap and the suggested landing order live in
[docs/benchmarks.md](docs/benchmarks.md) and
[ROADMAP.md](ROADMAP.md).

- **File Compression** — compress and decompress a corpus with LZ4 at
  two compression ratios, verify every round-trip with SHA-1, score the
  throughput.
- **Navigation** — plan a seeded set of routes with early-exit Dijkstra
  over a deterministic synthetic road grid (lattice streets, diagonal
  shortcuts, long-range highways), verify every route, score the
  edge-relaxation rate.
- **HTML5 Browser** — parse, lay out and render 32 seeded synthetic
  pages (six site archetypes) into an 800×600 framebuffer with a
  hand-rolled tokenizer, block layout and scanline rasterizer, verify
  every render, score the page rate.
- **PDF Render** — parse and rasterize four seeded synthetic park-map
  PDFs (cover + map page, US Letter at 144 dpi) with a from-scratch
  parser and a 4x4 anti-aliased scanline renderer (fills, strokes,
  dashes, clipping, CMYK), verify every pixel digest, score the
  megapixel rate.
- **Clang** — lex, parse and compile 32 seeded synthetic translation
  units (a Lua-flavoured subset, six archetype families) to bytecode
  with a from-scratch lexer, recursive-descent parser and code
  generator, verify every emitted blob, score the token rate.
- **Text Processing** — scan 32 seeded synthetic Markdown documents
  (six archetype families) with 8 regex patterns each through a
  from-scratch backtracking engine, convert every document to HTML
  with a from-scratch Markdown converter, verify every match count
  and every HTML digest, score the bytes-processed rate.
- **Horizon Detection** — detect the horizon in eight seeded synthetic
  photos (six archetype families, 1600×1200 px, planted ground-truth
  boundary) through the full Canny chain — grayscale, separable
  Gaussian blur, Sobel, non-maximum suppression, hysteresis — then a
  0.25°-bin Hough transform, verify every detection against the
  reference results and the planted line, score the megapixel rate.
- **Photo Filter** — filter ten seeded synthetic photos (six archetype
  families, 1600×1200 px) through a fixed five-stage stack —
  saturation, levels, two-pass box blur, central crop, bilinear
  rescale — built on shared integer image kernels, verify every
  filtered digest, score the megapixel rate.

## Build (one command)

```sh
cc -O2 -std=c11 -pthread src/*.c -o lotofbench -lm
```

or `make` (same flags, tracked by the Makefile). No external library, any
C11 compiler, Linux/macOS/BSD.

## Usage

```sh
./lotofbench [--size-mb N] [--runs N] [--threads N] [--seed N] [--json]
```

Defaults: 100 MB corpus, 5 runs (median), one thread per online CPU.
`--json` emits machine-readable results. `--size-mb` sizes the File
Compression corpus; every other workload uses fixed-size inputs (the
map is the map, the pages are the pages) that vary only with `--seed`.

Real output on the development box (12 threads, defaults):

```
LotofBench 0.8.0
seed 1, 5 runs, median

File Compression (corpus 100 MB synthetic)
Single-Core
  throughput      108.5 MB/s
  score             434   (baseline 250.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      666.5 MB/s
  score            2666

Navigation (grid 512x512, 64 routes)
Single-Core
  throughput       43.9 Medge/s
  score             439   (baseline 100.0 Medge/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      239.0 Medge/s
  score            2390

HTML5 Browser (32 pages, 800x600 viewport, 3544 DOM elements)
Single-Core
  throughput      300.3 pages/s
  score             462   (baseline 650.0 pages/s = 1000)

Multi-Core (12 threads, shared task)
  throughput     1811.6 pages/s
  score            2787

PDF Render (4 park maps, 2 pages each, 1224x1584 px @2x)
Single-Core
  throughput        3.1 Mpixel/s
  score             312   (baseline 10.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       13.8 Mpixel/s
  score            1383

Clang (32 units, 986328 source tokens)
Single-Core
  throughput     4844.5 Ktokens/s
  score             570   (baseline 8500.0 Ktokens/s = 1000)

Multi-Core (12 threads, shared task)
  throughput    22770.5 Ktokens/s
  score            2679

Text Processing (32 docs, 8 patterns/doc)
Single-Core
  throughput       96.0 MB/s
  score             480   (baseline 200.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      499.3 MB/s
  score            2496

Horizon Detection (8 photos, 1600x1200 px, 6 archetypes, Canny+Hough)
Single-Core
  throughput       49.8 Mpixel/s
  score             995   (baseline 50.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      267.7 Mpixel/s
  score            5354

Photo Filter (10 photos, 1600x1200 px, 6 archetypes)
Single-Core
  throughput       28.6 Mpixel/s
  score             571   (baseline 50.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      155.5 Mpixel/s
  score            3109
```

## What it measures

### File Compression

- **Metric** — throughput of the full workload cycle (compress +
  decompress + SHA-1 verify at two LZ4 ratios), in decimal MB/s, median
  over the runs.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (250.0 MB/s = 1000) is defined in `src/lotofbench.h` and documented in
  [docs/benchmarks.md](docs/benchmarks.md#scoring).
- **Correctness** — a run only counts if every decompressed buffer
  SHA-1-matches its source; a broken codec aborts instead of scoring.

### Navigation

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

### HTML5 Browser

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

### Horizon Detection

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

### Photo Filter

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

### PDF Render

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

## Tests

```sh
make test
```

A dependency-free suite (no framework, same rules as the benchmark)
covering 88 tests: SHA-1 against the RFC 3174 / NIST vectors, LZ4
round-trips and format rejections, harness medians/scoring, and for
each workload the determinism of its synthetic inputs, their documented
invariants, golden pins on anything promised stable — and a
differential test of Navigation's search against an independent full
Dijkstra on every route, plus a differential test of HTML5 Browser's
rasterizer against an independent brute-force compositor that
re-derives every pixel from the display list. The suite is kept clean
under `-fsanitize=address,undefined`; golden values are regenerated
with `tools/gen_goldens.c` after deliberate format changes only.

## Repository layout

```
src/lotofbench.h              shared constants, per-workload scoring baselines
src/rng.[ch]                  deterministic PRNG stream (xorshift*)
src/harness.[ch]              shared-task threading: start gate, timing, median
src/workload_compression.[ch] workload #1 — corpus, LZ4 cycles, SHA-1 verify
src/workload_navigation.[ch]  workload #2 — road grid, Dijkstra, verification
src/workload_html5browser.[ch] workload #3 — pages, tokenizer, layout, raster
src/workload_pdfrender.[ch]   workload #4 — park-map PDF corpus, cycles
src/pdfparse.c                workload #4 kernel — from-scratch PDF reader
src/pdfrender.c               workload #4 kernel — scanline rasterizer, 4x4 AA
src/workload_clang.[ch]       workload #6 — units, lexer, parser, codegen
src/workload_textprocessing.[ch] workload #7 — corpus, regex, Markdown→HTML
src/workload_horizon.[ch]    workload #12 — photo corpus, Canny + Hough
src/workload_photofilter.[ch] workload #13 — photo corpus, filter cycles
src/imgops.[ch]               shared integer image kernels (blur, levels,
                              grayscale, Gaussian blur, color matrix,
                              crop, bilinear scale)
src/lz4.[ch]                  LZ4 block-format codec (from scratch, 2 levels)
src/sha1.[ch]                 SHA-1 for round-trip verification
src/main.c                    CLI, suite order, output
tests/                        the unit-test suite (make test)
tools/gen_goldens.c           regenerates the golden pins of the tests
docs/benchmarks.md            the workload list, methodology and scoring rules
ROADMAP.md                    remaining workloads, cross-cutting plan, test rules
```

## License

[MIT](LICENSE) — Cloud Mercato, 2026.
