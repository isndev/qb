#!/usr/bin/env bash
#
# doc-lint.sh — documentation anti-drift guard for qb.
#
# Fails (non-zero exit) on:
#   1. Retired/forbidden tokens in documentation (e.g. qb::Timestamp).
#   2. Broken internal Markdown links.
#   2b. Cross-repo URLs naming a dead repository or a non-released git ref.
#   3. Missing mandatory governance files.
# Warns (does not fail) on:
#   4. Narrative pages missing a "Verified-against" front-matter marker.
#
# Scope: qb's own Markdown (README.md, readme/**, governance files). It does not
# scan source code (which is verified by the build).
#
# llm/ IS in scope, but through section 1c rather than doc_files(): the agent-facing
# llm/*.llm.md + llm/*.llm.api.md moved into this repo from the qb-dev superproject,
# and they need rules this script does not have (symbol existence, content digest) while
# legitimately NAMING retired tokens to warn agents off them, which section 1's cue-less
# scan would report as usage. scripts/llm-guard.py owns that surface.
#
# .cursor/ still lives at the superproject root, out of reach of a script that runs
# INSIDE this submodule; the superproject's dev/agent/llm-guard.py covers it (and keeps
# reading llm/ too, from where it can see qb and all three modules at once).
#
# Usage:  ./scripts/doc-lint.sh        (run from the qb root)
#
set -uo pipefail

# Resolve the qb root as the parent of this script's directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}" || exit 2

fail=0
warn=0

red()  { printf '\033[31m%s\033[0m\n' "$1"; }
grn()  { printf '\033[32m%s\033[0m\n' "$1"; }
ylw()  { printf '\033[33m%s\033[0m\n' "$1"; }

# Documentation surfaces (Markdown only). internal/ is maintainer-only and excluded.
doc_files() {
  { echo "README.md"
    echo "INSTALL.md"; echo "VERSIONING.md"; echo "CHANGELOG.md"
    echo "SECURITY.md"; echo "SUPPORT.md"; echo "CONTRIBUTING.md"; echo "CODE_OF_CONDUCT.md"
    find readme -name '*.md' 2>/dev/null
  } | sort -u | while read -r f; do [ -f "$f" ] && echo "$f"; done
}

# Expected framework version for the "Verified-against" markers.
#
# cmake/qbConfig.cmake is the single source of truth (VERSIONING.md says so), so it is read
# here rather than written out again. Resolving it is the whole point of the check: the
# markers used to be tested for EXISTENCE only, and 129 of them sat at "qb 2.6.0" across two
# version bumps without anything noticing.
#
# Not being able to determine the version is a HARD STOP, not a skip. A lint that quietly
# passes when it cannot find its expected value is indistinguishable from the unchecked
# marker it replaced -- which is exactly the state this closes.
QB_CONFIG="cmake/qbConfig.cmake"
EXPECTED_VERSION="$(sed -n 's/^[[:space:]]*set(QB_FRAMEWORK_VERSION[[:space:]]*"\([0-9][0-9.]*\)").*/\1/p' \
                    "${QB_CONFIG}" 2>/dev/null | head -1)"
if [ -z "${EXPECTED_VERSION}" ]; then
  red "doc-lint: cannot read QB_FRAMEWORK_VERSION from ${QB_CONFIG}"
  red "          refusing to validate Verified-against markers against an unknown version"
  exit 2
fi

# ---------------------------------------------------------------------------
echo "== 1. Forbidden token scan =="
# Capitalized retired types only; the lowercase qb::duration etc. are valid.
# Also the renamed time header: the canonical path is qb/system/time.h (timestamp.h was removed).
FORBIDDEN='qb::Timestamp|qb::Duration|qb::TimePoint|to_timestamp\(|to_time_point\(|qb/system/timestamp\.h'
# These pages document the removal/migration of the old types, so they may name them.
is_allowed() {
  case "$1" in
    CHANGELOG.md|CONTRIBUTING.md|readme/6_guides/migration_guide.md) return 0 ;;
    *) return 1 ;;
  esac
}
hits=0
while read -r f; do
  is_allowed "$f" && continue
  if grep -nE "${FORBIDDEN}" "$f" >/dev/null 2>&1; then
    grep -nE "${FORBIDDEN}" "$f" | while IFS= read -r line; do
      red "  ${f}: ${line}"
    done
    hits=1
  fi
done < <(doc_files)
if [ "$hits" -eq 0 ]; then grn "  no forbidden tokens"; else fail=1; fi

# ---------------------------------------------------------------------------
echo "== 1b. Citation integrity (src: file + line ranges) =="
# Validates every <!-- src: -->, // src:, and (src: ...) citation: cited file
# exists and each line range is within the file's current length.
if command -v python3 >/dev/null 2>&1; then
  python3 "${SCRIPT_DIR}/cite-check.py" || fail=1
else
  # HARD FAILURE, not a skip. This used to print a yellow "skipping citation check" and
  # carry on, which meant a run with no python3 checked the forbidden-token scan, the
  # links and the governance files, printed no red, and exited 0 -- while every citation
  # in the book went unverified. That is the exact shape of defect this whole battery
  # exists to catch: a guard that degrades into a pass. cite-check.py is not optional
  # here, so its interpreter is not optional either.
  red "  python3 not found — cite-check.py cannot run, and this lint does not pass without it"
  red "  install python3 (>= 3.8) and re-run; do NOT treat a skipped citation check as green"
  fail=1
fi

# ---------------------------------------------------------------------------
echo "== 1c. Agent-facing llm/ docs (symbols, citations, digest, paths, retired tokens, version marker) =="
# `llm/*.llm.md` + `llm/*.llm.api.md` moved into this repo from the qb-dev superproject, so the
# doc that describes this code now travels with it and this repo is independently indexable.
#
# doc_files() above deliberately does NOT list them, and that is measured rather than assumed:
# its forbidden-token scan matches per line with no negation cue, and these files NAME retired
# tokens in order to warn agents off them. Replaying each repo's own pattern and filter over its
# own llm/*.md: 11 lines would be flagged across the four repos (qb 3, qbm-http 5, qbm-pgsql 2,
# qbm-redis 1), every one the doc doing its job. scripts/llm-guard.py owns that surface instead, with the cue, plus the two rules
# nothing else here has: does every documented symbol still EXIST, and do the cited lines still
# SAY what they said. It also validates the `Verified-against:` marker by value, which for these
# files reached no check at all before the move.
if command -v python3 >/dev/null 2>&1; then
  python3 "${SCRIPT_DIR}/llm-guard.py" || fail=1
else
  # Same hard-failure policy as 1b: a guard that degrades into a pass is the defect this
  # battery exists to catch, so its interpreter is not optional either.
  red "  python3 not found -- llm-guard.py cannot run, and this lint does not pass without it"
  fail=1
fi

# ---------------------------------------------------------------------------
echo "== 1d. Published agent index (llms.txt / llms-full.txt are what the generator produces) =="
# `/llms.txt` and `/llms-full.txt` are what an agent actually fetches: GitMCP turns any public
# GitHub repo into an MCP endpoint and reads them FIRST (its documented order is llms.txt, then
# an AI-optimised docs build, then README.md). They are generated from `llm/` and from the files
# in this checkout, never hand-written, and this check regenerates them in memory and fails on
# any byte of difference -- so editing `llm/` without regenerating is a red build rather than a
# published file that quietly describes the previous state. It also asserts the llmstxt.org
# shape (H1, blockquote, prose, H2 link lists, `## Optional`) and that every published URL
# names a file that exists here.
if command -v python3 >/dev/null 2>&1; then
  python3 "${SCRIPT_DIR}/gen-llms-txt.py" --check || fail=1
else
  red "  python3 not found -- gen-llms-txt.py cannot run, and this lint does not pass without it"
  fail=1
fi

echo "== 2. Internal link check =="
broken=0
while read -r f; do
  dir="$(dirname "$f")"
  # Extract relative link targets: ](target) excluding http(s), mailto, anchors.
  # Strip fenced code blocks first so C++ lambdas like []( ) are not mistaken for links,
  # and reject targets containing spaces (real link targets have none).
  awk 'BEGIN{c=0} /^[[:space:]]*```/{c=!c; next} !c{print}' "$f" 2>/dev/null \
    | grep -oE '\]\([^) ]+\)' 2>/dev/null | sed -E 's/^\]\(//; s/\)$//' | while IFS= read -r target; do
    case "$target" in
      http://*|https://*|mailto:*|\#*) continue ;;
    esac
    path="${target%%#*}"                 # strip anchor
    [ -z "$path" ] && continue
    case "$path" in
      /*) resolved="${ROOT}${path}" ;;   # absolute-from-root (rare)
      *)  resolved="${dir}/${path}" ;;
    esac
    if [ ! -e "$resolved" ]; then
      red "  ${f} -> ${target} (missing)"
      echo "BROKEN" >> /tmp/doc-lint-broken.$$
    fi
  done
done < <(doc_files)
if [ -f /tmp/doc-lint-broken.$$ ]; then broken=$(wc -l < /tmp/doc-lint-broken.$$); rm -f /tmp/doc-lint-broken.$$; fi
if [ "${broken:-0}" -eq 0 ]; then grn "  all internal links resolve"; else fail=1; fi

# ---------------------------------------------------------------------------
echo "== 2b. Cross-repo URL check (repo name + git ref of absolute isndev links) =="
# Section 2 deliberately skips http(s) targets, so a link into a SIBLING repo was validated by
# nothing at all. That blind spot shipped 35 dead URLs across the doc books: they named
# github.com/isndev/cube -- the repo's old PRIVATE name, since published as isndev/qb -- on
# branch c++23, which no longer exists. Both halves 404 for a reader of the released docs, and
# four green doc-lint runs never saw them.
#
# The check stays offline (no network, no API rate limit, a few milliseconds): it does not
# resolve the URL, it validates the only two parts that rot -- the repository name and the git
# ref. Docs must cite the RELEASED line, so `main` or a 40-hex permalink; a link into a moving
# development branch is rejected because it silently rots again on the next merge.
# The two scaffolding templates joined this list when the scripts that clone them were finally
# documented. Both verified present and public with `gh repo view` (default branch `master`, which
# is what the scripts' `+master:master` push targets) — and `isndev/qbm-sample`, the 404 that
# `qb-new-module.sh` cloned through 2.6.0, still resolves to "Could not resolve to a Repository".
# That is the whole point of this check: the name a script clones and the name a doc links must
# both be real, and neither was validated by anything before.
ISNDEV_REPOS='qb qb-dev qb-ev qb-examples qbm-http qbm-pgsql qbm-redis qb-sample-project qb-sample-module'
while read -r f; do
  grep -oE 'https://github\.com/isndev/[A-Za-z0-9_.+-]+(/(blob|tree|raw)/[^/)" ]+)?' "$f" 2>/dev/null \
    | while IFS= read -r u; do
    repo="$(printf '%s\n' "$u" | cut -d/ -f5)"; repo="${repo%.git}"
    ref="$(printf '%s\n' "$u" | cut -d/ -f7)"
    case " ${ISNDEV_REPOS} " in
      *" ${repo} "*) ;;
      *) red "  ${f}: unknown repository 'isndev/${repo}' -> ${u}"; echo X >> /tmp/doc-lint-badurl.$$ ;;
    esac
    [ -z "${ref}" ] && continue
    [ "${ref}" = "main" ] && continue
    if [ "${#ref}" -eq 40 ]; then
      case "${ref}" in *[!0-9a-f]*) ;; *) continue ;; esac    # 40-hex commit permalink: pinned, fine
    fi
    red "  ${f}: ref '${ref}' is not main or a permalink -> ${u}"; echo X >> /tmp/doc-lint-badurl.$$
  done
done < <(doc_files)
badurl=0; [ -f /tmp/doc-lint-badurl.$$ ] && { badurl=$(wc -l < /tmp/doc-lint-badurl.$$); rm -f /tmp/doc-lint-badurl.$$; }
[ "${badurl:-0}" -eq 0 ] && grn "  all cross-repo URLs name a live repo on main" || fail=1

echo "== 3. Governance presence =="
missing=0
for g in README.md INSTALL.md VERSIONING.md CHANGELOG.md SECURITY.md SUPPORT.md CONTRIBUTING.md CODE_OF_CONDUCT.md LICENSE; do
  if [ ! -f "$g" ]; then red "  missing: ${g}"; missing=1; fi
done
if [ "$missing" -eq 0 ]; then grn "  all governance files present"; else fail=1; fi

# ---------------------------------------------------------------------------
echo "== 4. Verified-against marker (missing: warning · wrong version: error) =="
nomarker=0
badmarker=0
while read -r f; do
  case "$f" in CHANGELOG.md|CODE_OF_CONDUCT.md) continue ;; esac   # external formats
  marker="$(grep -m1 'Verified-against' "$f" 2>/dev/null)"
  if [ -z "${marker}" ]; then
    ylw "  no Verified-against: ${f}"
    nomarker=$((nomarker + 1))
    continue
  fi
  # Rightmost "qb <x.y.z>" in the marker: the qbm-style form is "qbm-http @ qb 3.0.0", and
  # "qbm" never matches because the pattern requires the space after "qb".
  found="$(printf '%s\n' "${marker}" | grep -oE 'qb [0-9]+\.[0-9]+\.[0-9]+' | tail -1 | awk '{print $2}')"
  if [ -z "${found}" ]; then
    red "  ${f}: Verified-against names no qb version (expected qb ${EXPECTED_VERSION}): ${marker}"
    badmarker=$((badmarker + 1))
  elif [ "${found}" != "${EXPECTED_VERSION}" ]; then
    red "  ${f}: Verified-against says qb ${found}, but ${QB_CONFIG} says qb ${EXPECTED_VERSION}"
    badmarker=$((badmarker + 1))
  fi
done < <(doc_files)
if [ "$nomarker" -ne 0 ]; then warn=1; fi
if [ "$badmarker" -ne 0 ]; then
  red "  ${badmarker} page(s) verified against a qb version that is not ${EXPECTED_VERSION}"
  fail=1
elif [ "$nomarker" -eq 0 ]; then
  grn "  all pages carry a Verified-against marker naming qb ${EXPECTED_VERSION}"
fi

# ---------------------------------------------------------------------------
echo
if [ "$fail" -ne 0 ]; then
  red "doc-lint: FAILED"
  exit 1
fi
if [ "$warn" -ne 0 ]; then
  ylw "doc-lint: passed with warnings"
else
  grn "doc-lint: passed"
fi
exit 0
