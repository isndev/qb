#!/usr/bin/env bash
#
# doc-lint.sh — documentation anti-drift guard for qb.
#
# Fails (non-zero exit) on:
#   1. Retired/forbidden tokens in documentation (e.g. qb::Timestamp).
#   2. Broken internal Markdown links.
#   3. Missing mandatory governance files.
# Warns (does not fail) on:
#   4. Narrative pages missing a "Verified-against" front-matter marker.
#
# Scope: qb's own Markdown (README.md, readme/**, governance files). It does not
# scan source code (which is verified by the build) or the parent repo's llm/ and
# .cursor/ surfaces.
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

# ---------------------------------------------------------------------------
echo "== 1. Forbidden token scan =="
# Capitalized retired types only; the lowercase qb::duration etc. are valid.
FORBIDDEN='qb::Timestamp|qb::Duration|qb::TimePoint|to_timestamp\(|to_time_point\('
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
echo "== 3. Governance presence =="
missing=0
for g in README.md INSTALL.md VERSIONING.md CHANGELOG.md SECURITY.md SUPPORT.md CONTRIBUTING.md CODE_OF_CONDUCT.md LICENSE; do
  if [ ! -f "$g" ]; then red "  missing: ${g}"; missing=1; fi
done
if [ "$missing" -eq 0 ]; then grn "  all governance files present"; else fail=1; fi

# ---------------------------------------------------------------------------
echo "== 4. Verified-against marker (warning) =="
nomarker=0
while read -r f; do
  case "$f" in CHANGELOG.md|CODE_OF_CONDUCT.md) continue ;; esac   # external formats
  if ! grep -q 'Verified-against' "$f" 2>/dev/null; then
    ylw "  no Verified-against: ${f}"
    nomarker=$((nomarker + 1))
  fi
done < <(doc_files)
if [ "$nomarker" -eq 0 ]; then grn "  all pages carry a Verified-against marker"; else warn=1; fi

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
