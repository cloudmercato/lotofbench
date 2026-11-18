# Build

How to build `lotofbench` from source: the zero-dependency default,
the opt-in portal-upload build and the shipped Docker image.

## One command

```sh
cc -O2 -std=c11 -ffp-contract=off -pthread src/*.c -o lotofbench -lm
```

or `make` (same flags, tracked by the Makefile). No external library, any
C11 compiler, Linux/macOS/BSD.

`-ffp-contract=off` is mandatory: without it a compiler may contract
float operations into FMAs and the bit-exact verification of the timed
cycles aborts the run instead of scoring it
([docs/benchmarks.md](benchmarks.md), "Workload 4"). This default build
never opens a network socket: `--upload` is refused (exit 2),
`--no-upload` is the default.

## Portal build (WITH_CURL=1)

A second, opt-in build adds the portal upload: libcurl (for HTTPS to the
LotofBench ingest portal), everything else identical.

```sh
make WITH_CURL=1
```

`WITH_CURL=1` compiles the same sources with `-DHAVE_CURL` and links
`-lcurl`; the upload then defaults to on (`--no-upload` turns it off).
The shipped Docker image is built this way; outside the container the
default stays the zero-dependency build.

## Docker

```sh
docker build -t lotofbench .
docker run --rm lotofbench --runs 5
```

`--json-out` must point at a mounted volume (`-v "$PWD":/out … /out/x.json`):
the `FROM scratch` runtime has no writable paths — not even `/tmp`.

The image is a two-stage build: an Alpine 3.24 stage compiles a fully
static musl binary (`-ffp-contract=off`, `WITH_CURL=1`, stripped to
~8 MB) and a `FROM scratch` stage ships the binary plus the CA bundle,
so the `docker run` default is the upload-enabled benchmark with no
runtime dependencies. Multi-arch: linux/amd64 + linux/arm64.

Release images are published to Docker Hub as
`cloudmercato/lotofbench:<version>` (e.g. `1.2.0`) plus a rolling
`latest` tag; the nightly pipeline publishes the same image (amd64 +
arm64) to the internal registry ([docs/release.md](release.md)). The
build stage mirrors the release workflow's Linux toolchain (Alpine
3.24, where c-ares ships its static archive in `c-ares-dev` instead of
the old `c-ares-static` split).
