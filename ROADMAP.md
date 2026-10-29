# Roadmap

Where LotofBench is going: the remaining Geekbench 6 CPU workloads, the
cross-cutting pieces (composite scoring, CLI, CI), and the testing
conventions every new workload must follow. The authoritative workload
list and implementation status live in
[docs/benchmarks.md](docs/benchmarks.md).

## Done

- **v0.1** — Workload #1 File Compression (from-scratch LZ4 at two
  ratios, SHA-1 round-trip verification, synthetic deterministic
  corpus).
- **v0.2** — Workload #2 Navigation (early-exit Dijkstra over a
  deterministic synthetic road grid, parent-chain verification,
  Medge/s metric) + the shared-task harness factored out to
  `src/harness.c` + the unit-test suite (27 tests, `make test`).
- **v0.3** — Workload #3 HTML5 Browser (seeded synthetic pages, from
  scratch tokenizer → block layout → scanline rasterizer into an
  800×600 framebuffer, reference-render verification inside the timed
  cycle, pages/s metric) + 9 more tests (36 total) including a
  differential test of the rasterizer against an independent
  brute-force compositor.

## Suggested landing order

1. **#13 Photo Filter** — blur, color, crop, scale over synthetic
   images; establishes the deterministic image helpers every later
   image workload reuses.
2. **#12 Horizon Detection** — Canny + Hough, with synthetic
   ground-truth horizons to verify against.
3. **#15 Ray Tracer** — Whitted-style tracer + BVH over a synthetic
   scene; the flagship floating-point workload.
4. **#4 PDF Render** — scanline rasterizer over synthetic vector
   scenes (bezier flattening, polygon fill, anti-aliasing).

## The remaining Geekbench 6 CPU workloads

Geekbench 6 groups its CPU workloads into an Integer section (65% of
the composite) and a Floating-Point section (35%). LotofBench follows
the same grouping once it aggregates (see Composite scoring).

### Integer section

| # | Workload | Geekbench 6 does | LotofBench approach |
|---|----------|------------------|---------------------|
| 3 | HTML5 Browser | HTML parse, CSS layout, render | Dependency-free tokenizer → block layout → scanline "render" over a synthetic DOM tree; metric pages/s or elements/s; verify the rendered pixmap hash |
| 4 | PDF Render | PDFium vector rendering | Synthetic vector scene list → bezier flattening, scanline polygon fill, anti-aliasing; metric Mpixels/s; verify pixel hash |
| 5 | Photo Library | JPEG decode, thumbnails, MobileNet classify + SQLite | Synthetic images → box-filter thumbnails + a tiny fixed conv pass + an in-memory record store; metric images/s |
| 6 | Clang | Compiling the Lua interpreter | Lexer/parser/codegen of a small deterministic language subset (no system cc — that would break reproducibility); metric Ktokens/s; verify emitted bytes hash — **implemented v0.4** |
| 7 | Text Processing | Regex parsing, Markdown→HTML | From-scratch regex engine (NFA or backtracking) + Markdown→HTML subset over synthetic texts; metric MB/s — **implemented v0.5** |
| 8 | Asset Compression | ASTC/BC7/DXT5 texture codecs, Draco mesh | From-scratch BC1-style block codec over synthetic textures + simple mesh quantization; metric Mpixels/s |

### Floating-Point section

| # | Workload | Geekbench 6 does | LotofBench approach |
|---|----------|------------------|---------------------|
| 9 | Object Detection | MobileNet v1 SSD over photos | Fixed deterministic quantized (int8) conv net over synthetic images; metric images/s; verify output tensor hash |
| 10 | Background Blur | DeepLabV3+ segmentation on video | Simplified segmentation mask + per-pixel blur strength over synthetic frames; metric fps |
| 11 | Object Remover | Inpainting (content-aware fill) | Iterative Laplacian/pyramid fill over synthetic masks; metric Mpixels/s |
| 12 | Horizon Detection | Canny edges + Hough transform | From-scratch Canny + Hough over synthetic images with known horizons; metric Mpixels/s; verify detected line parameters against ground truth |
| 13 | Photo Filter | Color/blur filters, crop, scale | Gaussian/box blur, saturation, crop+scale pipelines over synthetic images; metric Mpixels/s |
| 14 | HDR | Blending six 16 MP SDR photos | Simplified exposure-fusion (pyramid blend) over six synthetic exposures; metric Mpixels/s |
| 15 | Ray Tracer | Blender BMW scene, Embree-style | Whitted-style tracer with a BVH over a deterministic synthetic scene; metric Mrays/s; verify pixel hash |
| 16 | Structure from Motion | 3D geometry from 2D image pairs | Feature detection (FAST/Harris) + RANSAC essential matrix + triangulation over synthetic correspondences; verify reprojection error; metric pairs/s |

## Cross-cutting

- **Composite scoring** — currently each workload publishes its own
  score. Once the Integer and Floating-Point sections each hold at
  least two workloads, adopt the Geekbench 6 aggregation: geometric
  mean per section, then the 65/35 weighted composite; keep publishing
  per-workload scores alongside. Scores stay non-comparable with
  Geekbench (own baselines).
- **Calibration discipline** — one baseline per workload, chosen when
  it lands, pinned in `src/lotofbench.h` and documented in
  `docs/benchmarks.md`. Baselines never move after landing; a move is a
  major version bump.
- **Golden pins** — inputs promised stable across versions (the corpus,
  the road network, the PRNG stream, encoder formats) are pinned by the
  unit tests. After a *deliberate* format change, regenerate the pins
  with `tools/gen_goldens.c` and update the tests in the same commit.
- **CLI** — add `--workload <slug>` (run a subset) and
  `--list-workloads`; keep the `--json` output stable and versioned
  (add a `"schema"` field).
- **CI** — run `make` and `make test` on every push via Gitea Actions;
  pending the Gitea runner being functional again.
- **Versioning** — minor bump per workload landing (0.3.0 with the
  third), 1.0.0 when all 16 exist and the composite is live.

## Testing conventions (every new workload lands with)

- **Determinism test** — same seed ⇒ identical inputs, checked through
  the workload's read-only accessors.
- **Input invariants** — the documented properties of the synthetic
  input (like Navigation's route distance band) asserted, not assumed.
- **Differential test** — the workload's optimized core compared
  against an independent, naive reference implementation (Navigation's
  early-exit Dijkstra vs a full lazy-heap Dijkstra is the model).
- **Golden pins** — for anything whose exact bytes are part of the
  contract; generated by `tools/gen_goldens.c`, never hand-edited.
- **Hygiene** — `make test` green, no warnings under
  `-Wall -Wextra -pedantic`, and the suite clean under
  `cc -fsanitize=address,undefined`.
