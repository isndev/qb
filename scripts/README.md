# `scripts/` — what is in here, and who runs it

Every file in this directory is a **guard or a CI helper**. None of it is needed to *use* qb, none of
it is installed (`cmake --install` copies no file from here — asserted by `check-installed-headers.sh`
and by the superproject's `package-consume.yml`), and a consumer never runs any of it.

Kept here rather than in the private development superproject because each one guards **this
repository's own content** — its headers, its citations, its vendored licences — and a guard that
lives in a different repository from the thing it guards is a guard that stops running the day the
two are checked out separately.

## `scripts/` vs `script/` — the singular one is not a typo

| | audience | contents |
|---|---|---|
| **`script/`** (singular) | **consumers** | `qb-new-project.sh`, `qb-new-module.sh` — project/module scaffolding |
| **`scripts/`** (plural) | **CI and maintainers** | everything below |

The split is load-bearing and must not be collapsed: `README.md` and `readme/6_guides/` publish the
scaffolding scripts as `curl`-able URLs pinned to the **released** line —
`https://raw.githubusercontent.com/isndev/qb/main/script/qb-new-project.sh` — so renaming `script/`
breaks a documented, publicly advertised entry point for every existing user. (`doc-lint.sh`
section 2b validates those URLs' repository name and git ref, but it cannot know that a path moved.)

## Who invokes what

Verified by grep across all six repositories, not by filename.

| script | invoked by |
|---|---|
| `doc-lint.sh` | `.github/workflows/doc-lint.yml`; the superproject's `dev/agent/verify.sh`; humans (`CONTRIBUTING.md`, the PR template) |
| `cite-check.py` | `doc-lint.sh` §1b |
| `llm-guard.py` | `doc-lint.sh` §1c |
| `gen-llms-txt.py` | `doc-lint.sh` §1d |
| `llm-guard.baseline`, `llm-cite-digest.baseline` | data for `llm-guard.py`; both are `paths:` triggers in `doc-lint.yml` |
| `check-abi-macro-split.py` | `.github/workflows/format-check.yml` |
| `check-header-extensions.py` | `.github/workflows/format-check.yml` |
| `check-header-linkage.py` | `.github/workflows/format-check.yml` |
| `check-namespace-scoped-includes.py` | `.github/workflows/format-check.yml` |
| `check-abi-fingerprint.sh` | `.github/workflows/abi-fingerprint.yml` |
| `check-cross-compiler-statics.sh` | `.github/workflows/cmake.yml` |
| `check-installed-headers.sh` | `.github/workflows/install-consume.yml`; the superproject's `package-consume.yml` |
| `check-installed-headers-selftest.sh` | `.github/workflows/install-consume.yml` (negative controls for the above) |
| `installed-entry-points/` | `check-installed-headers.sh` phase 2, via `--entry-dir` |
| `ci-install-{linux,macos}-dependencies.sh`, `ci-install-windows-dependencies.ps1` | every build workflow here, plus the superproject's `benchmarks.yml` and `package-consume.yml` |
| `check-spawn-dangling-closure.py` | **only** the superproject (`dev/agent/verify.sh`, root `guards.yml`) — see below |
| `check-vendor-attribution.py` | **only** the superproject (same) — see below |
| `clang-tidy.sh` | **humans only, deliberately.** `CONTRIBUTING.md` and `readme/7_reference/building.md` both state that clang-tidy is not a CI gate; run it locally before submitting |

Three of those are not run by this repository's own CI:

* `clang-tidy.sh` is human-only **by documented design**, not by neglect.
* `check-spawn-dangling-closure.py` runs standalone here perfectly well (`python3
  scripts/check-spawn-dangling-closure.py`, exit 0 over the qb tree). Nothing but the superproject
  drives it, which is a CI-wiring gap rather than a placement problem — it guards qb's own coroutine
  code, and it caught 23 real UB sites in qb's own tests.
* `check-vendor-attribution.py` **refuses** to run from a standalone checkout — it exits 2 with
  "expected the superproject root", because its manifest spans qb *and* the three qbm modules. Its
  subject (qb's vendored licences) is still qb's own, so it stays here; running it needs the
  superproject.
