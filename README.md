# LotofBench

An open-source, dependency-free CPU benchmark in C, modelled on the
structure of Primate Labs' [Geekbench 6](https://www.geekbench.com):
identifiable workloads, single-core and multi-core results, and a
relative score against a fixed baseline.

**Not affiliated with Primate Labs.** LotofBench scores are its own
calibration and are *not* comparable with Geekbench scores.

## Status — workload 1 of 16

**File Compression** is implemented (the first Geekbench 6 CPU workload):
compress and decompress a corpus with LZ4 at two compression ratios,
verify every round-trip with SHA-1, score the throughput. The full
workload list and the roadmap live in
[docs/benchmarks.md](docs/benchmarks.md).

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
`--json` emits machine-readable results.

Real output on the development box (12 threads, defaults):

```
LotofBench 0.1.0 — File Compression
corpus 100 MB synthetic (seed 1), 5 runs, median

Single-Core
  throughput      106.9 MB/s
  score             428   (baseline 250.0 MB/s = 1000)

Multi-Core (12 threads, shared task)
  throughput      585.4 MB/s
  score            2342
```

## What it measures

- **Metric** — throughput of the full workload cycle (compress +
  decompress + SHA-1 verify at two LZ4 ratios), in decimal MB/s, median
  over the runs.
- **Score** — `round(throughput / baseline × 1000)`; the baseline
  (250.0 MB/s = 1000) is defined in `src/lotofbench.h` and documented in
  [docs/benchmarks.md](docs/benchmarks.md#scoring).
- **Correctness** — a run only counts if every decompressed buffer
  SHA-1-matches its source; a broken codec aborts instead of scoring.

## Repository layout

```
src/lotofbench.h   shared constants, scoring baseline
src/lz4.[ch]       LZ4 block-format codec (from scratch, 2 levels)
src/sha1.[ch]      SHA-1 for round-trip verification
src/main.c         corpus generator, threading harness, scoring, CLI
docs/benchmarks.md the workload list, methodology and scoring rules
```

## License

[MIT](LICENSE) — Cloud Mercato, 2026.
