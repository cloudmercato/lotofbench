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
- **v0.10** — Workload #5 Photo Library (16 seeded synthetic photos in
  eight content classes through the import pipeline: a 1/4 / 1/8 / 1/16
  exact box thumbnail ladder, a fixed 8-filter integer conv pass over
  the 1/8 thumbnail — the MobileNet stand-in — with nearest-centroid
  auto-tag that must recover the planted class, and a per-slice
  in-memory record store with integrity queries — the SQLite
  stand-in; every thumbnail SHA-1-verified, img/s metric) + the shared
  `img_box_downscale` kernel in `src/imgops.c`.
- **v0.11** — Workload #16 Structure from Motion (twelve seeded stereo
  pairs of a 544-point synthetic room through the full SfM pipeline:
  FAST-9 with ring-SAD non-max suppression, nearest-corner matching,
  Hartley-normalized 8-point RANSAC over cyclic-Jacobi eigensolvers,
  cheirality pose selection over the four (R, ±t) candidates, midpoint
  triangulation; every reconstruction verified against the planted
  pose, the exact inlier split, sub-pixel reprojection bounds and a
  prepare-time SHA-1 digest, pairs/s metric) + the public low-level API
  (`sfm_essential_ransac`, `sfm_decompose_essential`,
  `sfm_triangulate_midpoint`, `sfm_essential_from_pose`)
  differentially tested against the planted geometry.

## Suggested landing order

1. **#8 Asset Compression** — a from-scratch BC1-style block codec,
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
| 5 | Photo Library | JPEG decode, thumbnails, MobileNet classify + SQLite | Synthetic photos → 1/4-1/8-1/16 box thumbnail ladder + fixed 8-filter int conv pass + nearest-centroid auto-tag + in-memory record store; metric images/s; verify thumbnail digests + planted tags — **implemented v0.10** |
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
| 16 | Structure from Motion | 3D geometry from 2D image pairs | FAST-9 detection + 8-point RANSAC essential + midpoint triangulation over synthetic stereo pairs; verify reprojection error and planted pose; metric pairs/s — **implemented v0.11** |

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
