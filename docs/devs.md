# Developers

How the suite tests itself, and where everything lives.

## Tests

```sh
make test
```

A dependency-free suite (no framework, same rules as the benchmark)
covering 188 tests: SHA-1 against the RFC 3174 / NIST vectors, LZ4
round-trips and format rejections, harness medians/scoring, and for
each workload the determinism of its synthetic inputs, their documented
invariants, golden pins on anything promised stable — and a
differential test of Navigation's search against an independent full
Dijkstra on every route, plus a differential test of HTML5 Browser's
rasterizer against an independent brute-force compositor that
re-derives every pixel from the display list, plus HDR's constant-plane
closed-form fusion (the fused value reduces to the documented weight
math and is checked to the bit) and Object Remover's whole-pipeline
differential against an independently written naive fill. The suite is
kept clean
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
src/workload_horizon.[ch]     workload #12 — photo corpus, Canny + Hough
src/workload_raytracer.[ch]   workload #15 — showroom, BVH, Whitted tracer
src/itrig.[ch]               shared integer trig table (0.25-degree bins)
src/workload_photofilter.[ch] workload #13 — photo corpus, filter cycles
src/workload_hdr.[ch]         workload #14 — scene corpus, exposure burst,
                              Mertens exposure-fusion pipeline
src/workload_objectremover.[ch]
                              workload #11 — photo corpus, lasso masks,
                              push-pull pyramid inpainting
src/imgops.[ch]               shared integer image kernels (blur, levels,
                              grayscale, Gaussian blur, color matrix,
                              crop, bilinear scale, box downscale)
src/lz4.[ch]                  LZ4 block-format codec (from scratch, 2 levels)
src/sha1.[ch]                 SHA-1 for round-trip verification
src/main.c                    CLI, suite order, output
tests/                        the unit-test suite (make test)
tools/gen_goldens.c           regenerates the golden pins of the tests
docs/benchmarks.md            the workload list, methodology and scoring rules
docs/what-it-measure.md       per-workload metric, baseline and correctness rules
docs/build.md                 build flavours: default, WITH_CURL=1, Docker image
docs/release.md               release pipeline, prebuilt archives, runbook
docs/devs.md                  this file — tests and repository layout
docs/sample-output.md         a captured default run (text report, composite included)
docs/sample-output.json       the same run's JSON document (schema 1)
graphify-out/graph.json       Graphify knowledge graph of the codebase
graphify-out/graph.html       the same graph, interactive (open in a browser)
```
