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
 │   build matrix (4 targets) → sha256sums → GitHub Release      │
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
| `.github/workflows/release.yml` | GitHub | `repository_dispatch` type `release-build`, workflow_dispatch (input `tag`) | Build 4 targets → `sha256sums` → `gh release create`. `concurrency` keyed per tag. |
| `.gitea/workflows/tests.yml` | Gitea | every push / PR | Existing unit-test pipeline (the same `make test` the gate runs). |

The GitHub workflow **never** runs on a GitHub tag push — that is the
structural guarantee that the trigger chain starts at Gitea.

## Build matrix

| Target | Where | Toolchain | Notes |
|---|---|---|---|
| `linux-amd64` | `ubuntu-24.04` hosted runner, `alpine:3.20` container (native) | gcc (musl), `CFLAGS=-O2 -std=c11 -Wall -Wextra -pedantic -static` | fully static musl binary |
| `linux-arm64` | same, `docker run --platform linux/arm64` via QEMU (`docker/setup-qemu-action`) | same | fully static musl binary, smoke-run under emulation |
| `macos-arm64` | `macos-14` hosted runner (Apple silicon, free for public repos) | clang, default `CFLAGS` | native |
| `macos-amd64` | `macos-13` hosted runner (Intel) | clang, default `CFLAGS` | native |

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

Stable, automation-friendly, no archives — the playbook and scripts download
and `chmod +x` directly:

```
lotofbench-<version>-linux-amd64
lotofbench-<version>-linux-arm64
lotofbench-<version>-macos-amd64
lotofbench-<version>-macos-arm64
lotofbench-<version>-sha256sums.txt
```

`<version>` = tag without the leading `v` (e.g. tag `v1.2.3` → version
`1.2.3`). The `sha256sums.txt` covers exactly the four binaries and is
generated in one place (the release job) from the downloaded artifacts, so
the checksums always describe the attached files. Stable download URL:

```
https://github.com/cloudmercato/lotofbench/releases/download/v<version>/lotofbench-<version>-<os>-<arch>
```

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
dispatch the gate workflow manually from the Gitea UI (it re-tests the same
commit; note the tag mirror step is idempotent, `git push` of an identical
tag is a no-op).

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

## Gate proof (2026-11-16, rehearsal on master 42876b7)

- **Positive path**: disposable tag `v1.1.0-test` pushed on the twin → gate
  green (full test suite) → tag mirrored + dispatched → GitHub release
  `v1.1.0-test` published with 4 binaries + `sha256sums.txt`; `linux-arm64`
  binary executed (`--size-mb 8 --runs 1 --threads 1`) on an aarch64 host
  (QEMU, binfmt); `linux-amd64` executed natively; macOS binaries verified
  via the GitHub runner smoke logs (no mac host on the LAN — noted
  limitation, runners are the build hosts themselves). Disposable tag and
  rehearsal release deleted afterwards.
- **Negative path**: tag `v1.1.0-gate-red` pushed on a branch whose tests
  deliberately fail → gate red → **no dispatch, zero GitHub releases, zero
  GitHub workflow runs** for that tag. Disposable branch and tag deleted
  afterwards.
