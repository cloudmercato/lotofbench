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
- **v0.4** — Workload #6 Clang (lex + parse + codegen of a
  Lua-flavoured subset to bytecode, emitted-blob verification,
  Ktokens/s metric).
- **v0.5** — Workload #7 Text Processing (backtracking regex engine +
  Markdown→HTML over a synthetic corpus, match-count and HTML-digest
  verification, MB/s metric).
- **v0.6** — Workload #4 PDF Render (synthetic park-map PDFs,
  from-scratch parser and 4x4 anti-aliased scanline renderer, digest +
  probe verification, Mpixel/s metric; landed after #6/#7, hence the
  version jump).
- **v0.7** — Workload #13 Photo Filter (saturation, levels, two-pass
  box blur, crop and bilinear scale over synthetic photos, built on
  the shared `src/imgops.c` kernels; reference-digest verification,
  Mpixel/s metric).
- **v0.8** — Workload #12 Horizon Detection (the full Canny chain —
  grayscale, separable Gaussian blur, Sobel, non-maximum suppression,
  double threshold with hysteresis — followed by a 0.25°-bin Hough
  transform over seeded synthetic photos with planted, ground-truth
  horizons; every detection verified against both the reference
  results and the planted line, Mpixel/s metric) + the shared
  `img_grayscale` and `img_gray_gaussian_blur5` kernels in
  `src/imgops.c`.
- **v0.9** — Workload #15 Ray Tracer (a seeded synthetic showroom —
  checker floor, geodesic spheres, boxes, cylinders, chrome/paint/
  matte materials, three point lights — rendered from eight orbit
  cameras by a Whitted tracer over a median-split BVH; ambient +
  Lambert + Phong shading with shadow rays and mirror reflections;
  every frame verified by SHA-1 digest and exact ray count, BVH vs
  brute-force differential, Mray/s metric) + the shared integer trig
  table factored out to `src/itrig.c`.

## Suggested landing order

1. **#5 Photo Library** — box-filter thumbnails + a tiny fixed conv
   pass + an in-memory record store, reusing the imgops kernels.
2. **#8 Asset Compression** — a from-scratch BC1-style block codec,
   the strongest remaining Integer-section workload before composite
   scoring can aggregate both sections.

## The remaining Geekbench 6 CPU workloads

Geekbench 6 groups its CPU workloads into an Integer section (65% of
the composite) and a Floating-Point section (35%). LotofBench follows
the same grouping once it aggregates (see Composite scoring).

### Integer section

| # | Workload | Geekbench 6 does | LotofBench approach |
|---|----------|------------------|---------------------|
| 3 | HTML5 Browser | HTML parse, CSS layout, render | Dependency-free tokenizer → block layout → scanline "render" over a synthetic DOM tree; metric pages/s or elements/s; verify the rendered pixmap hash — **implemented v0.3** |
| 4 | PDF Render | PDFium vector rendering | Synthetic vector scene list → bezier flattening, scanline polygon fill, anti-aliasing; metric Mpixels/s; verify pixel hash — **implemented v0.6** |
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
| 12 | Horizon Detection | Canny edges + Hough transform | From-scratch Canny + Hough over synthetic images with known horizons; metric Mpixels/s; verify detected line parameters against ground truth — **implemented v0.8** |
| 13 | Photo Filter | Color/blur filters, crop, scale | Gaussian/box blur, saturation, crop+scale pipelines over synthetic images; metric Mpixels/s — **implemented v0.7** |
| 14 | HDR | Blending six 16 MP SDR photos | Simplified exposure-fusion (pyramid blend) over six synthetic exposures; metric Mpixels/s |
| 15 | Ray Tracer | Blender BMW scene, Embree-style | Whitted-style tracer with a BVH over a deterministic synthetic scene; metric Mrays/s; verify pixel hash — **implemented v0.9** |
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
