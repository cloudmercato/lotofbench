# LotofBench

An open-source, dependency-free CPU benchmark in C, modelled on the
structure of Primate Labs' [Geekbench 6](https://www.geekbench.com):
identifiable workloads, single-core and multi-core results, and a
relative score against a fixed baseline.

**Not affiliated with Primate Labs.** LotofBench scores are its own
calibration and are *not* comparable with Geekbench scores.

## Status — workload 15 of 16

**File Compression**, **Navigation**, **HTML5 Browser**, **PDF Render**,
**Photo Library**, **Clang**, **Text Processing**, **Asset
Compression**, **Object Detection**, **Background Blur**, **Horizon
Detection**, **Photo Filter**, **HDR**, **Ray Tracer** and **Structure
from Motion** are implemented (fifteen of the Geekbench
6 CPU workloads). The full
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
- **Photo Library** — ingest 16 seeded synthetic photos (eight content
  classes, 1280×960 px) through the import pipeline: an exact box
  1/4 / 1/8 / 1/16 thumbnail ladder, a fixed eight-filter integer conv
  pass over the 1/8 thumbnail with nearest-centroid auto-tagging, and
  an in-memory record store with integrity queries, verify every
  thumbnail digest and the planted tags, score the photos-per-second
  rate.
- **Clang** — lex, parse and compile 32 seeded synthetic translation
  units (a Lua-flavoured subset, six archetype families) to bytecode
  with a from-scratch lexer, recursive-descent parser and code
  generator, verify every emitted blob, score the token rate.
- **Text Processing** — scan 32 seeded synthetic Markdown documents
  (six archetype families) with 8 regex patterns each through a
  from-scratch backtracking engine, convert every document to HTML
  with a from-scratch Markdown converter, verify every match count
  and every HTML digest, score the bytes-processed rate.
- **Asset Compression** — push 16 seeded assets (eight opaque 256x256
  textures, four alpha textures, four triangle-strip meshes) through
  from-scratch BC1 (DXT1) block codecs at two encoder efforts, BC3
  (DXT5) for the alpha textures and a Draco-style quantize +
  parallelogram + zigzag + varint geometry codec, verify every
  encoded-stream digest, every decoded artifact and the exact encode
  error, score the asset-bytes rate.
- **Object Detection** — run eight seeded synthetic photos (six
  archetype families, 300×300 px, 1-4 planted ground-truth objects)
  through a fixed deterministic int8 MobileNetV1-shaped net with an
  SSD-lite decode head — 12 convolutions, 316,225,344 exact int8 MACs
  per photo — verify the feature-map, head-tensor and detection
  digests, score the photos-per-second rate.
- **Background Blur** — blur the background behind the subject of
  twelve seeded synthetic 1920×1080 frames (four static-camera
  archetype families, one planted subject per frame) through a
  100%-integer pipeline — 1/4 box downscale, exact
  background-subtraction segmentation, 3x3 majority filter, bilinear
  mask upsample, radius-9 box blur, mask-modulated composite — verify
  every mask and output digest, score the frames-per-second rate.
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
- **HDR** — fuse four seeded synthetic 1600×1200 scenes (backlit
  window, sunset, gallery, night street), each a six-exposure bracket
  1 EV apart whose bright stops clip the highlights and whose dark
  stops drown the shadows, through a 100%-integer Mertens-style
  exposure fusion — fixed-point weight maps, per-pixel normalization,
  Laplacian/Gaussian pyramids, per-level blend, collapse — verify
  every weight-stack and fused-frame digest plus the exact counters,
  score the megapixel rate.
- **Ray Tracer** — render eight seeded views of a synthetic showroom
  (checker floor, geodesic spheres, boxes and cylinders in chrome,
  paint and matte finishes, three point lights) with a Whitted tracer
  over a BVH — ambient + Lambert + Phong shading, shadow rays, mirror
  reflections — verify every frame digest and the exact ray count,
  score the rays-per-second rate.
- **Structure from Motion** — reconstruct twelve seeded stereo pairs of
  a synthetic point room (640×480 px views, ~250 visible points per
  pair, 1/7 planted mismatches) with FAST-9 corners, nearest-corner
  matching, a Hartley-normalized 8-point RANSAC essential estimator,
  cheirality pose selection and midpoint triangulation — verify the
  digest, the exact inlier split, the planted pose and sub-pixel
  reprojection, score the pairs-per-second rate.

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
LotofBench 0.11.0
seed 1, 5 runs, median

File Compression (corpus 100 MB synthetic)
Single-Core
  throughput      107.8 MB/s
  score             431   (baseline 250.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      608.0 MB/s
  score            2432

Navigation (grid 512x512, 64 routes)
Single-Core
  throughput       43.6 Medge/s
  score             436   (baseline 100.0 Medge/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      233.7 Medge/s
  score            2337

HTML5 Browser (32 pages, 800x600 viewport, 3544 DOM elements)
Single-Core
  throughput      302.4 pages/s
  score             465   (baseline 650.0 pages/s = 1000)

Multi-Core (12 threads, shared task)
  throughput     2352.0 pages/s
  score            3618

PDF Render (4 park maps, 2 pages each, 1224x1584 px @2x)
Single-Core
  throughput        3.1 Mpixel/s
  score             308   (baseline 10.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       13.9 Mpixel/s
  score            1391

Photo Library (16 photos, 1280x960 px, 8 classes)
Single-Core
  throughput      193.1 img/s
  score             966   (baseline 200.0 img/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      949.8 img/s
  score            4749

Clang (32 units, 986328 source tokens)
Single-Core
  throughput     4728.5 Ktokens/s
  score             556   (baseline 8500.0 Ktokens/s = 1000)

Multi-Core (12 threads, shared task)
  throughput    23312.5 Ktokens/s
  score            2743

Text Processing (32 docs, 8 patterns/doc)
Single-Core
  throughput       97.3 MB/s
  score             487   (baseline 200.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      288.9 MB/s
  score            1445

Asset Compression (16 assets (12 textures, 4 meshes))
Single-Core
  throughput       46.5 MB/s
  score             930   (baseline 50.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      212.4 MB/s
  score            4248

Horizon Detection (8 photos, 1600x1200 px, 6 archetypes, Canny+Hough)
Single-Core
  throughput       50.2 Mpixel/s
  score            1004   (baseline 50.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      292.4 Mpixel/s
  score            5848

Photo Filter (10 photos, 1600x1200 px, 6 archetypes)
Single-Core
  throughput       28.5 Mpixel/s
  score             570   (baseline 50.0 Mpixel/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      151.2 Mpixel/s
  score            3023

HDR (4 scenes x 6 exposures, 1600x1200 px, Mertens fusion)
Single-Core
  throughput        5.6 Mpixel/s
  score             940   (baseline 6.0 Mpixel/s = 1000)

Multi-Core (4 threads, shared task)
  throughput       20.3 Mpixel/s
  score            3387

Ray Tracer (8 views, 480x360 px, 984 triangles, Whitted)
Single-Core
  throughput        5.1 Mray/s
  score            1026   (baseline 5.0 Mray/s = 1000)

Multi-Core (12 threads, shared task)
  throughput       26.9 Mray/s
  score            5385

Structure from Motion (12 pairs, 640x480 px, 544 scene points, FAST+RANSAC)
Single-Core
  throughput       36.9 pairs/s
  score             369   (baseline 100.0 pairs/s = 1000)

Multi-Core (4 threads, shared task)
  throughput      133.9 pairs/s
  score            1339
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

### HDR

- **Metric** — millions of fused output pixels per second (Mpixel/s):
  every run fuses the whole four-scene set through the full
  weight-map → pyramid → blend → collapse pipeline, and the throughput
  is the exact fused-pixel count (four 1600×1200 frames) divided by
  the wall-clock time, median over the runs. The pixel count is exact
  and machine-independent, like the corpus size for File Compression.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (6.0 Mpixel/s = 1000) is defined in `src/lotofbench.h`.
- **Correctness** — every scene is verified inside the timed cycle
  against its prepare-time reference: the SHA-1 of the normalized
  weight stack and of the fused frame must match exactly, as must the
  three counters (weight-starved pixels, clamped output pixels, black
  output pixels), with the uniform-fallback and black-output guards at
  zero; a broken pipeline aborts instead of scoring.

### Ray Tracer

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

### Photo Library

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

### Asset Compression

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

## Tests

```sh
make test
```

A dependency-free suite (no framework, same rules as the benchmark)
covering 171 tests: SHA-1 against the RFC 3174 / NIST vectors, LZ4
round-trips and format rejections, harness medians/scoring, and for
each workload the determinism of its synthetic inputs, their documented
invariants, golden pins on anything promised stable — and a
differential test of Navigation's search against an independent full
Dijkstra on every route, plus a differential test of HTML5 Browser's
rasterizer against an independent brute-force compositor that
re-derives every pixel from the display list, plus HDR's constant-plane
closed-form fusion (the fused value reduces to the documented weight
math and is checked to the bit). The suite is kept clean
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
src/workload_photolibrary.[ch]  workload #5 — photo corpus, thumbnail ladder,
                                conv + auto-tag, record store
src/workload_clang.[ch]       workload #6 — units, lexer, parser, codegen
src/workload_textprocessing.[ch] workload #7 — corpus, regex, Markdown→HTML
src/workload_assetcompression.[ch]
                              workload #8 — asset corpus, BC1/BC3 codecs,
                              geometry codec, verification
src/workload_objectdetection.[ch]
                              workload #9 — photo corpus, int8 conv net,
                              SSD-lite decode head
src/workload_backgroundblur.[ch]
                              workload #10 — frame corpus, background
                              models, segmentation + blur pipeline
src/workload_horizon.[ch]    workload #12 — photo corpus, Canny + Hough
src/workload_raytracer.[ch]  workload #15 — showroom, BVH, Whitted tracer
src/itrig.[ch]               shared integer trig table (0.25-degree bins)
src/workload_photofilter.[ch] workload #13 — photo corpus, filter cycles
src/workload_hdr.[ch]         workload #14 — scene corpus, exposure burst,
                              Mertens exposure-fusion pipeline
src/imgops.[ch]               shared integer image kernels (blur, levels,
                              grayscale, Gaussian blur, color matrix,
                              crop, bilinear scale, box downscale)
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
