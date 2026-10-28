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
| 2 | Navigation                    | Dijkstra routing on OpenStreetMap graphs | planned |
| 3 | HTML5 Browser                 | HTML parse, CSS layout, render     | planned |
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

## Metric

Each run reports a **throughput**: corpus bytes divided by the wall-clock
seconds of the full compress + decompress + verify cycle, in decimal
megabytes per second (1 MB = 1,000,000 bytes). The workload result is the
**median** over `--runs` runs (default 5).

Example ratios on the reference corpus: zero runs compress ~158:1, repeated
phrases 2-4:1, pseudo-text roughly 1.03:1 with the fast level.

## Scoring

```
score = round(throughput / baseline * 1000)
```

- `baseline` = `BASELINE_FILECOMPRESSION_MBPS` = **250.0 MB/s**, defined in
  `src/lotofbench.h`. A machine doing exactly 250.0 MB/s scores 1000.
- The baseline is a **LotofBench calibration constant**, chosen when the
  workload landed. It is not a Geekbench number.
- **LotofBench scores are not comparable with Geekbench scores.** Geekbench
  6 calibrates its composite at 2500 (Core i7-12700 reference) and folds
  its 16 workloads into Integer/Floating-Point subsections with geometric
  means, then weights them 65/35. LotofBench will adopt the same
  aggregation once more workloads exist; with a single workload there is
  nothing to aggregate yet, so each workload publishes its own score.

## Multi-core model

Geekbench 6 moved to a **shared task** model: threads cooperate on one
task, each handling a slice, instead of running independent copies.
LotofBench does the same — the corpus is split into equal 64-byte-aligned
chunks, one per thread. Runs are synchronised through a start gate (every
thread must be ready before the clock starts, and the clock stops when the
last thread finishes), so elapsed time measures the whole task.

Default thread count: every online CPU (capped at 64), override with
`--threads`. Single-core is always one thread over the whole corpus.
