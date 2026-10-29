# Benchmarks

Human documentation of the LotofBench workload list, what each workload
measures, and how scores are computed.

LotofBench follows the structure of [Geekbench 6](https://www.geekbench.com)
(Primate Labs). This project is **not affiliated with** Primate Labs; scores
are **not comparable** with Geekbench scores (see [Scoring](#scoring)).

## The Geekbench 6 CPU workload list

Geekbench 6 runs 16 CPU workloads, single-core and multi-core, grouped into
Integer and Floating-Point sections (composite weights: Integer 65%,
Floating Point 35%). The official list, with the LotofBench status:

| # | Geekbench 6 workload          | What it exercises                  | LotofBench      |
|---|-------------------------------|------------------------------------|-----------------|
| 1 | File Compression              | LZ4 + ZSTD compress/decompress, SHA-1 verify | **implemented v0.1** |
| 2 | Navigation                    | Dijkstra routing on OpenStreetMap graphs | **implemented v0.2** |
| 3 | HTML5 Browser                 | HTML parse, CSS layout, render     | **implemented v0.3** |
| 4 | PDF Render                    | Vector PDF rendering (PDFium)      | planned |
| 5 | Photo Library                 | JPEG decode, thumbnails, MobileNet classify + SQLite | planned |
| 6 | Clang                         | Compiling the Lua interpreter      | planned |
| 7 | Text Processing               | Regex parsing, Markdown→HTML (Python) | planned |
| 8 | Asset Compression             | ASTC/BC7/DXT5 texture codecs, Draco mesh | planned |
| 9 | Object Detection              | MobileNet v1 SSD over photos       | planned |
| 10| Background Blur               | DeepLabV3+ segmentation on video frames | planned |
| 11| Object Remover                | Inpainting (content-aware fill)    | planned |
| 12| Horizon Detection             | Canny edges + Hough transform      | planned |
| 13| Photo Filter                  | Color/blur filters, crop, scale    | planned |
| 14| HDR                           | Blending six 16 MP SDR photos      | planned |
| 15| Ray Tracer                    | Blender BMW scene, Embree-style    | planned |
| 16| Structure from Motion         | 3D geometry from 2D image pairs    | planned |

Geekbench also runs GPU compute workloads; they are out of scope for now.

## Workload 1 — File Compression (implemented)

Geekbench 6 compresses and decompresses the Ruby 3.1.2 source archive
(75 MB, 9,841 files) with LZ4 and ZSTD at high and low compression ratios,
verifies every output file with SHA-1, all inside an in-memory encrypted
file system (working set: 105.5 MB single-core, 418.2 MB multi-core).

LotofBench v0.1 reproduces the shape of that workload, dependency-free:

- **Corpus**: 100 MB (configurable `--size-mb`) of deterministic synthetic
  data — Zipf-distributed pseudo-text words, repeated phrases at assorted
  back-distances, and zero runs. Fixed `--seed` means byte-identical
  corpora on every machine, so scores are comparable across runs. It is
  generated off the clock.
- **Codecs**: a from-scratch LZ4 block-format implementation, run at two
  ratios — `LZ4_LEVEL_FAST` (single-slot hash table, one candidate) and
  `LZ4_LEVEL_HIGH` (chained candidates within the 64 KiB window, deepest
  match). This mirrors Geekbench's "high and low compression ratios"
  without pulling in external libraries. (Divergence: Geekbench 6 pairs
  LZ4 with ZSTD; lotofbench uses LZ4 at two depths instead.)
- **Verification**: every decompressed buffer is SHA-1-checked against the
  source before the run counts — same spirit as Geekbench 6, which
  verifies its codecs with SHA-1. A failed round-trip aborts the run.
- **Cycle**: one run = compress + decompress + verify at both ratios,
  timed as a whole with `CLOCK_MONOTONIC`.

Known v0.1 divergences from Geekbench 6, all deliberate: synthetic corpus
instead of the Ruby archive, no in-memory encrypted file system, fixed
corpus size instead of the 105.5/418.2 MB working sets.

## Workload 2 — Navigation (implemented)

Geekbench 6's Navigation workload plans routes with Dijkstra's algorithm
over OpenStreetMap data. LotofBench v0.2 reproduces the shape of that
workload, dependency-free:

- **Graph**: a deterministic synthetic road network (fixed `--seed`,
  generated off the clock) — a 512×512 lattice of nodes whose streets
  carry an integer travel time of 10 per grid unit ±20% jitter, ~40% of
  cells add a north-east diagonal shortcut, and 2,048 long-range
  "highways" (endpoints at least 256 units apart) run at 2.5× the base
  speed. The lattice alone guarantees connectivity; shortcuts and
  highways give the search real routing decisions. Roads are
  bidirectional, stored as a directed CSR adjacency (~1.26 M directed
  edges; 1,259,432 on the default seed).
- **Routes**: 64 seeded source/target pairs, straight-line distance
  between 300 and 700 grid units. Each route is an early-exit Dijkstra
  from its source: the search stops as soon as the target is settled.
- **Search**: indexed binary min-heap keyed by travel time, with
  decrease-key (no lazy deletion), so the heap stays bounded by the node
  count. All arithmetic is integer, so results are machine-independent.
- **Verification**: inside the timed cycle, every settled route is
  checked before the run counts — the parent chain is walked back to the
  source and re-summed edge by edge (it must reproduce the settled
  travel time exactly, which catches corrupt parents or wrong keys), the
  chain must use real edges, and the settled time must clear a
  straight-line lower bound of 3 time units per grid unit (highways run
  4 × 0.8 = 3.2, rounded down). A broken search aborts the run.
- **Cycle**: one run = the whole route set, timed as a whole with
  `CLOCK_MONOTONIC`.

Known v0.2 divergences from Geekbench 6, all deliberate: synthetic grid
instead of OpenStreetMap data, early-exit point-to-point Dijkstra, no
contraction hierarchies or other speed-up techniques — the workload
measures the plain search.

## Workload 3 — HTML5 Browser (implemented)

Geekbench 6's HTML5 Browser workload opens real-world web pages in a
headless browser: HTML parse, CSS layout, then render of the text and
images (GB6 itself uses Gumbo, litehtml, FreeType, AGG and libjpeg).
LotofBench v0.3 reproduces the shape of that workload, dependency-free:

- **Pages**: 32 seeded synthetic pages (GB6 opens 8 single-core / 32
  multi-core; LotofBench uses 32 for both, split across threads).
  Six archetype families cycle — portal, article, wiki, feed, search
  results, landing page — with paragraphs, inline formatting, links,
  per-page styles, comments, entities (`&amp;` `&lt;` `&#65;`), void
  elements and images. Pages are generated off the clock (the stand-in
  for the network fetch) and are byte-identical for a given seed.
- **Tokenizer**: hand-rolled HTML tokenizer — tags, attributes
  (quoted and unquoted), comments, doctype, entities, void elements,
  implied list-item closes, stray-close tolerance. The tokenizer
  produces a flat DOM with parent/child/next links.
- **Layout**: block flow — margins collapse to explicit block spacing,
  inline content is word-wrapped into text runs, images are placed at
  their seeded `width`/`height` attributes (280–480 px wide for
  photos, 600–640 px for landing heroes, defaults 240×160 when the
  attributes are absent). Everything is integer arithmetic, so layouts
  are machine-independent. The layout result is a display list of
  records: background fills, border strips, word blits and image
  blocks.
- **Render**: a scanline rasterizer paints the display list into an
  800×600 RGBA framebuffer. Text comes from a built-in 5×7 bitmap
  font (no FreeType), images from procedural patterns (photo
  gradients, bar charts, concentric-ring avatars — no libjpeg). Like
  GB6, rendering is a plain overdraw with no occlusion culling.
- **Verification**: inside the timed cycle, every page is parsed,
  laid out and rendered from scratch, and the result is compared
  against an off-clock reference render of the same page: element
  count, display-list record count and SHA-1 digest of the rendered
  framebuffer must all match exactly. A tokenizer bug, a layout
  drift or a rasterizer corruption aborts the run.
- **Cycle**: one run = the whole page set (parse + layout + render of
  all 32 pages), timed as a whole with `CLOCK_MONOTONIC`.

Known v0.3 divergences from Geekbench 6, all deliberate: synthetic
pages instead of archived real sites, block layout only (no CSS
cascade, floats or flexbox), a bitmap font instead of hinted TrueType,
procedural images instead of JPEG/PNG decode, 800×600 instead of
1280×720.

## Metric

Each run reports a **throughput**, and the workload result is the
**median** over `--runs` runs (default 5).

- **File Compression** — corpus bytes divided by the wall-clock seconds
  of the full compress + decompress + verify cycle, in decimal
  megabytes per second (1 MB = 1,000,000 bytes). Example ratios on the
  reference corpus: zero runs compress ~158:1, repeated phrases 2-4:1,
  pseudo-text roughly 1.03:1 with the fast level.
- **Navigation** — edge relaxations divided by the wall-clock seconds of
  the full route set, in decimal millions of edges per second (Medge/s).
  An edge relaxation is one scan of one directed adjacency entry by the
  search; the count is exact and deterministic (same seed, same count),
  so it is the navigation analogue of the corpus size. On the default
  seed the suite relaxes 52,954,982 edges per pass.
- **HTML5 Browser** — pages rendered divided by the wall-clock seconds
  of the full page set, in pages per second. A page "render" is the
  whole pipeline (parse + layout + raster), so the unit is honest at
  the workload level even though it compresses three distinct stages.

## Scoring

```
score = round(throughput / baseline * 1000)
```

Baselines are **LotofBench calibration constants**, chosen when each
workload landed, defined in `src/lotofbench.h`. They are not Geekbench
numbers, and **LotofBench scores are not comparable with Geekbench
scores**: Geekbench 6 calibrates its composite at 2500 (Core i7-12700
reference) and folds its 16 workloads into Integer/Floating-Point
subsections with geometric means, then weights them 65/35. LotofBench
will adopt the same aggregation once more workloads exist; with three
workloads there is still nothing to aggregate, so each workload
publishes its own score.

- `BASELINE_FILECOMPRESSION_MBPS` = **250.0 MB/s** = 1000 (workload #1).
- `BASELINE_NAVIGATION_MEDGES` = **100.0 Medge/s** = 1000 (workload #2)
  — the reference machine relaxes one edge per nanosecond.
- `BASELINE_HTML5BROWSER_PPS` = **650.0 pages/s** = 1000 (workload #3)
  — the reference machine renders a page (parse + layout + raster of
  an 800×600 viewport) in about 1.5 milliseconds.

## Multi-core model

Geekbench 6 moved to a **shared task** model: threads cooperate on one
task, each handling a slice, instead of running independent copies.
LotofBench does the same for all three workloads:

- **File Compression** — the corpus is split into equal 64-byte-aligned
  chunks, one per thread.
- **Navigation** — the route set is split into equal contiguous slices,
  one per thread.
- **HTML5 Browser** — the page set is split into equal contiguous
  slices, one per thread; each thread tokenizes, lays out and rasters
  its pages, and verifies them against the reference render.

Runs are synchronised through a start gate (every thread must be ready
before the clock starts, and the clock stops when the last thread
finishes), so elapsed time measures the whole task.

Default thread count: every online CPU (capped at 64), override with
`--threads`. Single-core is always one thread over the whole task.
