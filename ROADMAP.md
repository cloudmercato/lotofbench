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
- **v0.11** — Workload #8 Asset Compression (16 seeded assets — eight
  opaque 256x256 textures, four alpha textures, four triangle-strip
  meshes — through from-scratch BC1 block codecs at two encoder
  efforts, BC3 for the alpha textures, and a Draco-style geometry
  codec: 12-bit position quantization, parallelogram prediction,
  zigzag + varints; every encoded stream, decoded artifact and encode
  error verified against prepare-time references, MB/s metric).
- **v0.12** — Workload #16 Structure from Motion (twelve seeded stereo
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
- **v0.12.1** — Photo Library corpus robustness fix: a seeded photo
  whose big accent shapes carried it across a nearest-centroid
  boundary (e.g. seed 15, photo 7) aborted the whole run at prepare
  time; the photo is now redrawn deterministically from the same
  stream until the classifier recovers the planted class, so every
  seed yields a valid corpus (seeds 1-40 verified, seed-1 digests
  unchanged).
- **v0.13** — Workload #9 Object Detection (eight seeded 300x300
  synthetic photos with planted ground-truth objects through a fixed
  deterministic int8 MobileNetV1-shaped net with an SSD-lite decode
  head — 12 convolutions, 316,225,344 exact MACs per image, round
  half-up requantization with RMS-calibrated per-layer shifts; every
  timed cycle verifies the SHA-1 of the final feature map, the head
  tensor and the decoded detections against prepare-time references,
  img/s metric) + 13 tests including a full differential of the NHWC
  pipeline against an independent CHW reference and a degeneracy
  guard on the activation statistics.
- **v0.14** — Workload #10 Background Blur (twelve seeded 1920x1080
  RGB frames in four static-camera archetype families — one planted
  subject per frame, its box and tone exposed as ground truth —
  through a 100%-integer portrait-blur pipeline: 1/4 box downscale,
  per-cell max-channel background subtraction against an exact
  prepare-time model per archetype (threshold 24), 3x3 majority
  filter, bilinear x4 mask upsample, radius-9 separable box blur and
  the per-pixel mask-modulated composite; every timed cycle verifies
  the SHA-1 of the mask and of the composited output against
  prepare-time references, fps metric) + 13 tests including a
  differential of the whole pipeline against an independently written
  naive reference and ground-truth checks that the subject stays
  sharp while the far background is exactly the blurred frame.
- **v0.15** — Workload #14 HDR (four seeded 1600x1200 scenes — backlit
  window, sunset, gallery, night street — each mapped onto a
  six-exposure bracket 1 EV apart whose bright stops clip the
  highlights and whose dark stops drown the shadows, through a
  100%-integer Mertens-style exposure fusion: fixed-point weight maps
  (contrast, saturation, well-exposedness), per-pixel normalization
  summing to exactly 255, 6-level Laplacian and weight pyramids,
  per-level int32 blending and a signed 16.16 collapse; every timed
  cycle verifies the SHA-1 of the normalized weight stack and of the
  fused frame plus three exact counters against prepare-time
  references, Mpixel/s metric) + 13 tests including the constant-plane
  closed-form fusion (every pyramid level of a constant plane is that
  constant, so the fused value reduces to the documented weight math
  and is checked to the bit) and the kernel units for the weights,
  the normalization, the downsample and the expand.

- **v0.16** — Workload #11 Object Remover (four seeded 1600x1200
  photos — beach, forest, city, studio — each planting a two-shape
  lasso mask (a disc radius 64..96 plus an overlapping rectangle, 48
  px inside the frame) whose hidden bytes are never read, through a
  100%-integer push-pull pyramid inpainting: coarse cells are the
  half-up mean of their distinct known children, the coarsest plane
  takes the known-mean fallback (a degeneracy-guarded non-zero count —
  every disc covers a fully-masked 64x64 block), then coarse to fine a
  bilinear pull plus 24 in-place Gauss-Seidel Laplace sweeps per level
  over the unknown cells only, known cells fixed (Dirichlet); every
  timed cycle verifies the SHA-1 of the mask and of the filled frame
  plus three exact counters against prepare-time references, Mpixel/s
  metric) + 14 tests including the constant-photo closed-form fill
  (the harmonic fill of a constant boundary is the constant, to the
  byte) and a differential of the whole pipeline against an
  independently written naive fill.

## Suggested landing order

1. ~~#9 Object Detection~~ — **landed in v0.13**.
2. ~~#10 Background Blur~~ — **landed in v0.14**.
3. ~~#14 HDR~~ — **landed in v0.15**.
4. ~~#11 Object Remover~~ — **landed in v0.16**; all 16 Geekbench 6
   CPU workloads are now implemented.
5. ~~Composite scoring~~ — **landed in 1.0.0**: the section scores are
   the geometric means of the Integer / Floating-Point member scores
   and the composite their 65/35 weighted arithmetic mean, published
   alongside the per-workload scores.

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
| 8 | Asset Compression | ASTC/BC7/DXT5 texture codecs, Draco mesh | From-scratch BC1/BC3 block codecs (two encoder efforts) over synthetic textures + Draco-style quantize/parallelogram/zigzag/varint geometry codec; metric MB/s; verify stream + artifact digests + exact encode error — **implemented v0.11** |

### Floating-Point section

| # | Workload | Geekbench 6 does | LotofBench approach |
|---|----------|------------------|---------------------|
| 9 | Object Detection | MobileNet v1 SSD over photos | Fixed deterministic quantized (int8) conv net over synthetic images; metric images/s; verify output tensor hash — **implemented v0.13** |
| 10 | Background Blur | DeepLabV3+ segmentation on video | Simplified segmentation mask + per-pixel blur strength over synthetic frames; metric fps — **implemented v0.14** |
| 11 | Object Remover | Inpainting (content-aware fill) | Push-pull pyramid fill over synthetic masks; metric Mpixels/s; verify the filled-frame hash — **implemented v0.16** |
| 12 | Horizon Detection | Canny edges + Hough transform | From-scratch Canny + Hough over synthetic images with known horizons; metric Mpixels/s; verify detected line parameters against ground truth — **implemented v0.8** |
| 13 | Photo Filter | Color/blur filters, crop, scale | Gaussian/box blur, saturation, crop+scale pipelines over synthetic images; metric Mpixels/s — **implemented v0.7** |
| 14 | HDR | Blending six 16 MP SDR photos | Simplified exposure-fusion (pyramid blend) over six synthetic exposures; metric Mpixels/s — **implemented v0.15** |
| 15 | Ray Tracer | Blender BMW scene, Embree-style | Whitted-style tracer with a BVH over a deterministic synthetic scene; metric Mrays/s; verify pixel hash — **implemented v0.9** |
| 16 | Structure from Motion | 3D geometry from 2D image pairs | FAST-9 detection + 8-point RANSAC essential + midpoint triangulation over synthetic stereo pairs; verify reprojection error and planted pose; metric pairs/s — **implemented v0.12** |

## Cross-cutting

- **Composite scoring** — **landed in 1.0.0**: section scores are the
  geometric means of the member workloads' scores, the composite the
  65/35 weighted arithmetic mean of the two sections
  (docs/benchmarks.md, "The composite score"). Per-workload scores are
  still published alongside; scores stay non-comparable with Geekbench
  (own baselines).
- **Calibration discipline** — one baseline per workload, chosen when
  it lands, pinned in `src/lotofbench.h` and documented in
  `docs/benchmarks.md`. Baselines never move after landing; a move is a
  major version bump.
- **Golden pins** — inputs promised stable across versions (the corpus,
  the road network, the PRNG stream, encoder formats) are pinned by the
  unit tests. After a *deliberate* format change, regenerate the pins
  with `tools/gen_goldens.c` and update the tests in the same commit.
- **CLI** — add `--workload <slug>` (run a subset) and
  `--list-workloads`; the `--json` output is versioned through its
  top-level `"schema"` field (1 since the composite landed) — keep it
  backward-compatible when it changes. `--json-out PATH` (landed with
  the composite) stores the JSON document in a file.
- **CI** — run `make` and `make test` on every push via Gitea Actions;
  pending the Gitea runner being functional again.
- **Versioning** — 1.0.0 shipped all 16 workloads and the live
  composite; from there, minor bumps for feature changes, major bumps
  when a baseline moves (a move rescales every published score).

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
