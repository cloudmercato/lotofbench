<div align="center">

# LotofBench

[![Release build](https://github.com/cloudmercato/lotofbench/actions/workflows/release.yml/badge.svg)](https://github.com/cloudmercato/lotofbench/actions/workflows/release.yml)
[![Release](https://img.shields.io/github/v/release/cloudmercato/lotofbench)](https://github.com/cloudmercato/lotofbench/releases)
[![License: MIT](https://img.shields.io/github/license/cloudmercato/lotofbench)](LICENSE)

</div>

An open-source, dependency-free CPU benchmark in C, modelled on the
structure of Primate Labs' [Geekbench 6](https://www.geekbench.com):
identifiable workloads, single-core and multi-core results, a relative
score against a fixed baseline per workload, and a Geekbench-6-style
composite (geometric section means, weighted 65/35).

**Not affiliated with Primate Labs.** LotofBench scores are its own
calibration and are *not* comparable with Geekbench scores.

## Install

Two ways to get a `lotofbench` binary.

### Use release

Every green release pipeline publishes prebuilt tar.gz archives on the
[GitHub releases](https://github.com/cloudmercato/lotofbench/releases)
page: four of them (Linux x86_64 and aarch64 — fully static, portal
upload enabled; macOS Intel and Apple silicon — system libcurl) plus
one SHA-256 checksum file. Download, verify and unpack them following
[docs/release.md](docs/release.md).

### Compile

```sh
cc -O2 -std=c11 -ffp-contract=off -pthread src/*.c -o lotofbench -lm
```

or `make` (same flags, tracked by the Makefile). No external library, any
C11 compiler, Linux/macOS/BSD. Do not drop `-ffp-contract=off`: without
it the compiler may contract float operations into FMAs and the
bit-exact verification aborts instead of scoring. The opt-in
`make WITH_CURL=1` portal build and the Docker image are detailed in
[docs/build.md](docs/build.md).

## Usage

```sh
./lotofbench [--size-mb N] [--runs N] [--threads N] [--seed N] \
             [--json] [--json-out PATH] [--upload | --no-upload]
```

Defaults: 100 MB corpus, 5 runs (median), one thread per online CPU.
`--json` prints machine-readable results (schema 1: top-level `schema`,
`sections` and `composite` keys alongside the per-workload `workloads`
array). `--json-out PATH` additionally stores that JSON document in a
file while the usual text report goes to stdout. `--size-mb` sizes the
File Compression corpus; every other workload uses fixed-size inputs
(the map is the map, the pages are the pages) that vary only with
`--seed`.

**Portal upload.** With libcurl (the Docker image, or `make
WITH_CURL=1`) the result document is submitted to the LotofBench ingest
portal by default — `--no-upload` turns the flow off entirely (no
session request, no socket). The flow is phased to satisfy the
portal's anti-cheat cross-checks: the single-use session is requested
`POST /api/v1/session/` **before** the passes run (the portal rejects a
submission whose claimed runtime exceeds the session age), the passes
run, then the portal's SHA-256 proof of work is solved (progress lines
on stderr, never stdout) and `POST /api/v1/submit/` receives the
schema-1 document wrapped with the session id, nonce and signature.
The 201 submit body carries the result URL (`result_url` — the
`/results/<aggregate-key>/` detail page once the result aggregate is
synced, `/charts/` otherwise); the CLI prefers it and falls back to
the legacy `<site>/result/<id>/` when talking to an older portal. The
URL is printed to
stdout in text mode and to stderr in `--json` mode so the machine
stream stays pure. The base URL defaults to the production portal and
is overridden with the `LOTOFBENCH_UPLOAD_URL` environment variable
(used by the testing environment).

Every run starts with a hardware report, printed before the tests in
text mode and embedded as a top-level `hardware` object in the JSON
document (schema 1, additive): CPU brand, vendor, family/model,
sockets, cores, threads, frequency, L2/L3 caches, total/free RAM,
total/free swap, the NUMA layout (numactl-style node map), the
1-minute load average, OS/kernel and the compiler that built the
binary. It works on Linux and macOS, x86 (CPUID) and ARM. Every field
is best-effort: what a platform does not expose is skipped in text and
`""`/`0` in JSON, and capture never fails the run.

Two more additive schema-1 fields feed the portal: `runtime_seconds`,
the wall-clock time the passes took (single plus multi-threaded,
`CLOCK_MONOTONIC`), cross-checked server-side against the session age;
and `system`, a minimal environment profile — OS, architecture, CPU
model, core count, RAM and the execution environment (`container`,
`vm` or `bare-metal`, classified via cgroup/hypervisor probes) with,
when detected, the virtualization product (`docker`, `vmware`, …).
The profile carries no hostname, no IP address, nothing that
identifies a person or a specific machine — the portal needs to know
*what kind of box* ran the suite, not *which* box.

Real output of a default run on the development box (12 threads) is
kept in [docs/sample-output.md](docs/sample-output.md), with the same
run's machine-readable results in
[docs/sample-output.json](docs/sample-output.json).

## What it measures

Sixteen workloads — the full Geekbench 6 CPU workload set, grouped
into an Integer and a Floating-Point section — each run against
deterministic seeded inputs and verify their results bit-exactly
inside the timed cycle: a run that fails verification aborts instead
of scoring. Every workload is scored
`round(throughput / baseline × 1000)` against a fixed baseline of its
own (defined in `src/lotofbench.h`), and the composite — published
single-core and multi-core, the Geekbench 6 way — is the geometric
mean of each section, weighted 65% Integer / 35% Floating Point,
rounded half up. The scores are LotofBench's own calibration and are
never comparable with Geekbench scores.

Per-workload metric, baseline and correctness rules:
[docs/what-it-measure.md](docs/what-it-measure.md). Workload
descriptions and methodology:
[docs/benchmarks.md](docs/benchmarks.md).

## Development

The dependency-free unit-test suite (`make test`) and the full
repository layout are documented in [docs/devs.md](docs/devs.md).

A Graphify knowledge graph of the codebase ships in
[graphify-out/](graphify-out/): `graph.html` opens the codebase as an
interactive graph in a browser, `graph.json` is the same graph as
data, and `GRAPH_REPORT.md` is the generated report — so architecture
questions can be queried directly with
`graphify query "<question>"` from the repository root.

## License

[MIT](LICENSE) — Cloud Mercato, 2026.
