# Release pipeline

How a `lotofbench` version goes from a git tag to a public release with
prebuilt binaries. Written 2026-11-16 (setup + first rehearsal, kanban card
`t_6332fdf4`).

## Overview

```
 developer pushes tag vX.Y.Z
        │
        ▼
 ┌───────────────────────────────────────────────────────────────┐
 │ Gitea: cloud-mercato/lotofbench        (source of truth)      │
 │ .gitea/workflows/release.yml — "Release gate"                 │
 │   1. unit tests (make test)   ← the gate                      │
 │   2. mirror the tag to GitHub (SSH deploy key)                │
 │   3. repository_dispatch "release-build" {tag, sha}           │
 └───────────────────────────────┬───────────────────────────────┘
                                 │ dispatch fires ONLY if the gate is green
                                 ▼
 ┌───────────────────────────────────────────────────────────────┐
 │ GitHub: cloudmercato/lotofbench        (public face)          │
 │ .github/workflows/release.yml — "Release build"               │
 │   build matrix (4 tar.gz archives) → sha256sums → GitHub Release │
 └───────────────────────────────────────────────────────────────┘
```

**A red Gitea pipeline never dispatches, so it produces zero GitHub
releases.** There is no other path to a release: the GitHub workflow has no
tag-push trigger and its preflight job refuses anything that is not a
`release-build` dispatch carrying a `v*` tag (or an explicit manual re-run).

## Repo roles (hypothesis A2, confirmed)

| Repo | Role |
|---|---|
| `cloud-mercato/lotofbench` (Gitea) | Source of truth: development, CI (unit tests, auto-merge, nightly multi-arch Docker image to cm-registry). **The release trigger originates here.** |
| `cloudmercato/lotofbench` (GitHub) | Public face: releases, download links, README. Code reaches it two ways: the on-demand master sync (`~/bin/lotofbench-github-sync.sh` on the GMK, master-only force push) and the tag mirror performed by the release gate itself. |

## Workflows

| File | Repo | Triggers | What it does |
|---|---|---|---|
| `.gitea/workflows/release.yml` | Gitea | push of `v*` tag, workflow_dispatch | Gate: `make test` → mirror tag → `repository_dispatch`. `concurrency: release-gate` (single slot). |
| `.github/workflows/release.yml` | GitHub | `repository_dispatch` type `release-build`, workflow_dispatch (input `tag`) | Build 4 tar.gz archives → `sha256sums` → `gh release create`. `concurrency` keyed per tag. |
| `.gitea/workflows/tests.yml` | Gitea | every push / PR | Existing unit-test pipeline (the same `make test` the gate runs). |

The GitHub workflow **never** runs on a GitHub tag push — that is the
structural guarantee that the trigger chain starts at Gitea.

## Build matrix

| Target | Where | Toolchain | Notes |
|---|---|---|---|
| `linux-amd64` | `ubuntu-24.04` hosted runner, `alpine:3.20` container (native) | gcc (musl), `WITH_CURL=1`, `CFLAGS=-O2 -std=c11 -Wall -Wextra -pedantic -static` + `CURL_STATIC_LIBS` | fully static musl binary with the portal-upload flow; package list and static-lib closure mirror the repo Dockerfile build stage |
| `linux-arm64` | same, `docker run --platform linux/arm64` via QEMU (`docker/setup-qemu-action`) | same | fully static musl binary with the portal-upload flow, smoke-run under emulation |
| `macos-arm64` | `macos-15` hosted runner (Apple silicon) | clang, default `CFLAGS`, `WITH_CURL=1` | native, full smoke; links the SDK libcurl dynamically (`/usr/lib/libcurl.4.dylib`, present on every macOS) |
| `macos-amd64` | `macos-15` hosted runner (Apple silicon) | clang, `CFLAGS="-O2 -std=c11 -Wall -Wextra -pedantic -arch x86_64"`, `WITH_CURL=1` | cross-compiled; Rosetta smoke when available, else file(1) verification (documented in the run log); same dynamic system-libcurl link |

`-ffp-contract=off` is **not** spelled out in the workflow steps because the
Makefile appends it unconditionally (`FPC = -ffp-contract=off` in the compile
rule) — even when `CFLAGS` is overridden on the command line. Every binary
therefore satisfies the bit-exact golden-verification requirement; without it
a run aborts instead of scoring (README "Build", docs/benchmarks.md).

Each matrix job smoke-runs its own binary with
`--size-mb 8 --runs 1 --threads 1` (in the same Alpine container for Linux
targets — musl binary on musl, so the smoke is faithful) before the artifact
is uploaded. A build that cannot execute aborts before reaching the release.

## Asset name contract

**tar.gz archives named after the `install_lotofbench` playbook contract**
(CloudBeast `cs_web/player/playbooks/install_lotofbench.yml`): the playbook
downloads `lotofbench-linux-<arch>.tar.gz` under the `v<version>` tag
(`<arch>` = `x86_64` or `aarch64`, from `ansible_architecture`), runs
`unarchive`, then symlinks the first regular file named `lotofbench` it
finds. Each archive therefore contains exactly one member — the executable
`./lotofbench` — and the fleet installs a release with
`lotofbench_version: <version>` and **no URL override** (remedy (a) of the
D2 release-asset gap, kanban `t_997f6fff`/`t_e9c64ddd`: fix the artifact,
not the deployed playbooks).

```
lotofbench-linux-x86_64.tar.gz
lotofbench-linux-aarch64.tar.gz
lotofbench-macos-x86_64.tar.gz
lotofbench-macos-arm64.tar.gz
lotofbench-<version>-sha256sums.txt
```

Archive names carry no version — the tag does. That makes
`releases/latest/download/lotofbench-linux-x86_64.tar.gz` a stable
always-current URL, while tag-pinned URLs stay reproducible. The macOS
archives follow the same os-arch spelling (`macos-x86_64`, `macos-arm64`)
for uniformity; no fleet automation consumes them. The
`lotofbench-<version>-sha256sums.txt` name keeps the version because it is
the one file referenced by name in the release notes' verification snippet;
it covers exactly the four archives and is generated in one place (the
release job) from the downloaded artifacts, so the checksums always
describe the attached files. Stable download URLs:

```
https://github.com/cloudmercato/lotofbench/releases/download/v<version>/lotofbench-<os>-<arch>.tar.gz
https://github.com/cloudmercato/lotofbench/releases/latest/download/lotofbench-linux-<arch>.tar.gz
```

**Behavior note**: every released binary is built `WITH_CURL=1` — the Linux
ones are fully static musl builds with the same libcurl closure as the
registry image, the macOS ones link the system libcurl. A curl build
uploads by default (`UPLOAD_DEFAULT_ENABLED=1`): scripts that must stay
offline pass `--no-upload` (the CloudBeast `run_lotofbench` playbook
already does).

**Legacy contract (until v1.1.0 included)**: releases shipped raw
versioned binaries — `lotofbench-<version>-linux-amd64`,
`lotofbench-<version>-linux-arm64`, `lotofbench-<version>-macos-amd64`,
`lotofbench-<version>-macos-arm64` — plain (no curl) builds that
`install_lotofbench` could not consume (`get_url`+`unarchive` rejects a
naked ELF; rescue fell back to a source build — Grace's D2 finding,
2026-11-16). v1.1.0 assets stay in place untouched; the archive contract
starts at v1.2.0.

## Secrets (values live in Infisical, never in the repo)

| Secret | Set where | Value (never printed/committed) | Used by |
|---|---|---|---|
| `GH_MIRROR_KEY` | Gitea repo secrets, `cloud-mercato/lotofbench` | private key of the GitHub deploy key `id_ed25519_github_lotofbench` (same key the on-demand sync script uses; lives in the gitea container at `/data/git/.ssh/`) | gate job: `git push` the release tag to GitHub over SSH |
| `GH_DISPATCH_TOKEN` | Gitea repo secrets, `cloud-mercato/lotofbench` | GitHub PAT (Infisical, project `cloud-transparency-platform`, key `GITHUB_TOKEN`) with `contents` access to `cloudmercato/lotofbench` | gate job: `POST /repos/cloudmercato/lotofbench/dispatches` |
| `GITHUB_TOKEN` | built-in | n/a — Actions-issued, `permissions: contents: write` | release job: `gh release create` |

Host-key pinning: `.gitea/known_hosts_github` (committed, public data —
github.com + ssh.github.com keys) is used with `StrictHostKeyChecking=yes`;
the deploy key is written to a `mktemp -d` directory with `umask 077` and
deleted on step exit. It never appears in logs: only HTTP status codes and
git's own output are printed.

## Runbook

**Cut a release** (after the code is merged on the twin):

```sh
git tag v1.2.3 && git push origin v1.2.3
```

Then watch the "Release gate" run on Gitea; on green, the "Release build" run
starts on GitHub and the release appears with 5 assets.

**Re-run a failed gate after a fix** — push a new tag (recommended), or
dispatch the gate workflow manually from the Gitea UI to re-test the same
commit. Note: a manual dispatch only runs the tests job — the publish job's
guard refuses non-tag refs, so it never mirrors or dispatches; publishing
always requires a real `v*` tag push.

**Re-run a failed GitHub build without touching Gitea** —
`gh workflow run release.yml -R cloudmercato/lotofbench -f tag=v1.2.3`
(the tag must already exist on GitHub — the gate mirrored it).

**A release must be pulled** — delete the GitHub release
(`gh release delete <tag> --yes --cleanup-tag`) then delete the tag on Gitea.

**Rotate credentials** — the deploy key is shared with the on-demand sync
script (rotate it in the gitea container `/data/git/.ssh/` and on GitHub →
repo → Settings → Deploy keys, then refresh the `GH_MIRROR_KEY` Gitea
secret); the PAT lives in Infisical `GITHUB_TOKEN` (project
`cloud-transparency-platform`) — rotate it there and refresh
`GH_DISPATCH_TOKEN`. Gitea repo secrets:
`PUT /api/v1/repos/cloud-mercato/lotofbench/actions/secrets/<NAME>`.

## Gate proof (2026-11-16, rehearsals on master `a113a31`)

### Positive path — rehearsal tag `v1.1.0-test`

- Tag pushed on the twin at master `a113a31` (includes `--no-upload` no-op
  flag, PR #25) → gate green (unit tests + mirror + dispatch, 2/2 jobs) →
  GitHub `Release build` run 43805160846 → **all six jobs green** and the
  release published: <https://github.com/cloudmercato/lotofbench/releases/tag/v1.1.0-test>
  with the 5 contract-named assets (4 binaries + `sha256sums.txt`, 400 B).
- **Checksums**: all 4 assets downloaded through the exact stable URL pattern
  (`releases/download/v1.1.0-test/lotofbench-1.1.0-test-<os>-<arch>`),
  `sha256sum --check --ignore-missing` → 4/4 OK. `file(1)` confirms each
  binary's claimed platform (2 × static ELF, 2 × Mach-O).
- **Execution proof per target**:
  - linux/amd64 — smoke on the runner + play-shape invocation
    (`--no-upload --runs 1 --size-mb 1 --json-out`) executed locally: exit 0,
    valid schema-1 JSON (16 workloads, composite 528/2614). This is the D2
    contract (`run_lotofbench.yml` invocation) proven on the released asset.
  - linux/arm64 — full-suite smoke on the runner (QEMU) + play-shape
    execution on an aarch64 host via QEMU binfmt: exit 0, real scores.
  - macos/arm64 — full-suite smoke on the `macos-15` runner (native): exit 0,
    real scores in the job log.
  - macos/amd64 — cross-compiled `-arch x86_64` on `macos-15`, then executed
    under Rosetta 2 on the runner: exit 0, real x86_64 scores in the job log
    (the file(1) fallback documented below was not needed).
- The `v1.1.0-test` release is kept as the D2-usable asset set (its CLI knows
  `--no-upload`) until the first real release; delete it then.

### Negative path — tag `v1.1.0-gate-red` (disposable branch `ci/gate-red-test`)

- Branch with a deliberately failing test suite (`test_failure_count()`
  forced to 1), tagged `v1.1.0-gate-red` and pushed to the twin.
- Result on the twin: `Release gate / Gate — unit tests` = **failure**,
  `Release gate / Mirror tag + dispatch` = **skipped** (`needs:` gate held).
- Result on GitHub: **no release** for the tag (404), the tag was never
  mirrored (tags list still only `v1.1.0-test`), no new `Release build` run.
- Disposable branch and tag deleted afterwards.

### Runner notes (kept for the next release owner)

- `macos-14` arm64 is not provisionable for this account ("runner config
  macos-14-arm64-us-default not supported") and `macos-13` (Intel) sits
  queued — both macOS legs run on `macos-15` (PR #26).
- macOS amd64 is cross-compiled with `-arch x86_64` and smoke-executed under
  Rosetta 2; if Rosetta is ever unavailable on the runner, the step falls
  back to `file(1)` verification and says so in the log.

## Release v1.2.0 — first archive-contract release (2026-11-17, kanban `t_997f6fff`)

### What shipped

- Tag `v1.2.0` cut on master `233c6479` (PR #31 `result_url` + PR #32
  curl-variant tar.gz packaging + PR #33 version bump), GitHub
  "Release build" run 43994154778 — **all six jobs green**, release
  <https://github.com/cloudmercato/lotofbench/releases/tag/v1.2.0> with the
  5 contract assets: `lotofbench-{linux-x86_64,linux-aarch64,macos-x86_64,macos-arm64}.tar.gz`
  + `lotofbench-1.2.0-sha256sums.txt`.
- Asset verification from the GMK: tag-pinned download URLs → HTTP 200 ×5,
  plus the stable `releases/latest/download/lotofbench-linux-x86_64.tar.gz`
  → 200; `sha256sum --check --ignore-missing` → 4/4 OK; every archive lists
  exactly `lotofbench`, extracts as a regular executable file; `file(1)`:
  2 × static musl ELF (x86-64 / aarch64, `WITH_CURL=1`), 2 × Mach-O
  (arm64 / x86_64, dynamic SDK libcurl).
- Released-binary proof on the GMK: JSON document reports
  `version: 1.2.0`, 16/16 workloads on a tiny `--no-upload` run;
  `LOTOFBENCH_UPLOAD_URL=http://127.0.0.1:9/ ./lotofbench --upload …`
  reaches the real libcurl network layer on both released linux binaries
  (amd64 natively, aarch64 under QEMU) — upload flow compiled in, zero
  portal contact.
- macOS curl build confirmed on runners: both macos-15 jobs compiled
  `WITH_CURL=1` against the SDK libcurl and smokes passed (Rosetta smoke
  executed the x86_64 binary).
- Pre-CI local verification (GMK docker, exact workflow commands):
  static curl builds for amd64 + arm64/`QEMU`, tiny smokes green
  (amd64 single 487 / multi 943; arm64 emulated 106/106), `make test`
  213/213.

### Ops lessons for the next release owner

- **Gitea gate is flaky under host DNS/DB pressure.** Attempts 1 and 2 of
  the v1.2.0 gate failed WITHOUT running any job: under bursts of
  `context deadline exceeded` on the remote Postgres (and Docker DNS
  flakes inside the gitea container), Gitea never created the runner task
  for the jobs — the run sits jobless, then the abandoned-job sweeper marks
  it failed (~8–12 min). Signature in the DB: `action_run` row with
  `status=2` but **no matching `action_task` rows** (and no
  `actions_log/<repo>/<hash>/<task>.log.zst` file). Recovery: delete and
  re-push the same tag (attempt 3 was green end-to-end). Don't debug the
  workflow first — check for a task row.
- `~/bin/lotofbench-github-sync.sh` was **recreated 2026-11-17** (joe) —
  the original from the setup card was not found on the GMK. It reads the
  deploy key from the gitea container at runtime and pushes
  `refs/remotes/origin/master` to GitHub master; run it AFTER the release
  commits are merged and BEFORE pushing the tag (the dispatched GitHub
  workflow executes the file from GitHub's default branch). Used for
  v1.2.0: GitHub master `81a2480` → `233c6479`, verified via the public API.
- v1.1.0-test remains deleted; v1.1.0 (raw legacy assets) stays untouched.
