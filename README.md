# LotofBench

An open-source, dependency-free CPU benchmark in C, modelled on the
structure of Primate Labs' [Geekbench 6](https://www.geekbench.com):
identifiable workloads, single-core and multi-core results, and a
relative score against a fixed baseline.

**Not affiliated with Primate Labs.** LotofBench scores are its own
calibration and are *not* comparable with Geekbench scores.

## Status — workload 2 of 16

**File Compression** and **Navigation** are implemented (the first two
Geekbench 6 CPU workloads). The full workload list, the roadmap and the
suggested landing order live in
[docs/benchmarks.md](docs/benchmarks.md) and
[ROADMAP.md](ROADMAP.md).

- **File Compression** — compress and decompress a corpus with LZ4 at
  two compression ratios, verify every round-trip with SHA-1, score the
  throughput.
- **Navigation** — plan a seeded set of routes with early-exit Dijkstra
  over a deterministic synthetic road grid (lattice streets, diagonal
  shortcuts, long-range highways), verify every route, score the
  edge-relaxation rate.

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
Compression corpus; Navigation uses a fixed-size input (the map is the
map) that varies only with `--seed`.

Real output on the development box (12 threads, defaults):

```
LotofBench 0.2.0
seed 1, 5 runs, median

File Compression (corpus 100 MB synthetic)
Single-Core
  throughput      109.8 MB/s
  score             439   (baseline 250.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      722.5 MB/s
  score            2890

Navigation (grid 512x512, 64 routes)
Single-Core
  throughput       45.7 Medge/s
  score             457   (baseline 100.0 Medge/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      245.2 Medge/s
  score            2452
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

## Tests

```sh
make test
```

A dependency-free suite (no framework, same rules as the benchmark)
covering 27 tests: SHA-1 against the RFC 3174 / NIST vectors, LZ4
round-trips and format rejections, harness medians/scoring, and for
each workload the determinism of its synthetic inputs, their documented
invariants, golden pins on anything promised stable — and a
differential test of Navigation's search against an independent full
Dijkstra on every route. The suite is kept clean under
`-fsanitize=address,undefined`; golden values are regenerated with
`tools/gen_goldens.c` after deliberate format changes only.

## Repository layout

```
src/lotofbench.h              shared constants, per-workload scoring baselines
src/rng.[ch]                  deterministic PRNG stream (xorshift*)
src/harness.[ch]              shared-task threading: start gate, timing, median
src/workload_compression.[ch] workload #1 — corpus, LZ4 cycles, SHA-1 verify
src/workload_navigation.[ch]  workload #2 — road grid, Dijkstra, verification
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
