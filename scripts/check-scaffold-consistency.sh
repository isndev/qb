#!/usr/bin/env bash
#
# check-scaffold-consistency.sh — the guard that makes the scaffolders' version binding
# impossible to leave stale, and their shared implementation impossible to fork.
#
# WHY THIS EXISTS
# ---------------
# script/qb-new-project.sh and script/qb-new-module.sh decide, at scaffold time, which qb the
# generated tree builds against. They decide it from QB_SHIPPED_VERSION, a literal in each
# script. That literal is the fourth version string in this tree, and the other three all went
# stale before they were validated by value (the three qbm project() lines, the User-Agent, the
# Verified-against markers — 129 of which sat at "qb 2.6.0" across two version bumps).
#
# A literal is fine. A literal nothing checks is a stale copy waiting to happen, and the failure
# mode here is the exact one the templates were rebuilt to fix: a user scaffolds a project, the
# script writes a qb ref that does not correspond to the qb it shipped with, and the mismatch is
# discovered at a compile error days later — or not at all, because the wrong pairing still
# builds. So: assert it, from cmake/qbConfig.cmake, which VERSIONING.md names as the single
# source of truth.
#
# The second check is the same argument applied to code. The two scaffolders share ~300 lines of
# body — name validation, ref resolution, the copy/render pipeline, four anti-vacuity checks, the
# git-init dance. They are two separate files because each is fetched on its own by `curl | bash`,
# where a shared implementation would mean a second network fetch, a second file to read before
# running, and a skew between the two. That is the same trade this project already makes for
# scripts/llm-guard.py (four byte-identical copies) and .github/ci/superbuild/CMakeLists.txt
# (three) — and, like those, it is only safe with a guard that the copies have not drifted. They
# had drifted before: the module script cloned `isndev/qbm-sample`, which does not exist, while
# its own `cd` named `qb-sample-module`, which does.
#
# ANTI-VACUITY
# ------------
# Every check here can be made to pass by DELETING what it looks at, so each one first asserts
# that its subject is present and substantial: both scripts must exist, both must carry the
# marker, and the shared region must be at least SHARED_FLOOR lines. A diff of two empty strings
# is equal, and a version check that finds no literal to compare would pass forever.
#
# Usage:  ./scripts/check-scaffold-consistency.sh     (run from anywhere; resolves its own root)
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}" || exit 2

red() { printf '\033[31m%s\033[0m\n' "$1"; }
grn() { printf '\033[32m%s\033[0m\n' "$1"; }

fail=0

PROJECT_SCRIPT="script/qb-new-project.sh"
MODULE_SCRIPT="script/qb-new-module.sh"
QB_CONFIG="cmake/qbConfig.cmake"
MARKER='^# >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> SHARED BODY'

# A floor, not a target: the body grows. It exists so that gutting the shared region — or moving
# the marker to the last line — cannot turn the identity check into a comparison of nothing.
SHARED_FLOOR=200

echo "== scaffolder consistency =="

# ---------------------------------------------------------------------------
# 0. The subjects exist.
# ---------------------------------------------------------------------------
for f in "${PROJECT_SCRIPT}" "${MODULE_SCRIPT}" "${QB_CONFIG}"; do
  if [ ! -f "${f}" ]; then
    red "  MISSING: ${f}"
    red "  Every check below reads it; refusing to report a pass over an absent file."
    exit 2
  fi
done

# ---------------------------------------------------------------------------
# 1. QB_SHIPPED_VERSION == QB_FRAMEWORK_VERSION, in both scripts.
# ---------------------------------------------------------------------------
EXPECTED_VERSION="$(sed -n 's/^[[:space:]]*set(QB_FRAMEWORK_VERSION[[:space:]]*"\([0-9][0-9.]*\)").*/\1/p' \
                    "${QB_CONFIG}" 2>/dev/null | head -1)"
if [ -z "${EXPECTED_VERSION}" ]; then
  red "  cannot read QB_FRAMEWORK_VERSION from ${QB_CONFIG}"
  red "  refusing to validate the scaffolders against an unknown version"
  exit 2
fi
echo "  qbConfig.cmake QB_FRAMEWORK_VERSION = ${EXPECTED_VERSION}"

for f in "${PROJECT_SCRIPT}" "${MODULE_SCRIPT}"; do
  got="$(sed -n 's/^QB_SHIPPED_VERSION=\([0-9][0-9.]*\)[[:space:]]*$/\1/p' "${f}" | head -1)"
  if [ -z "${got}" ]; then
    red "  ${f}: no QB_SHIPPED_VERSION=<version> assignment found"
    red "    That literal is what decides which qb a scaffolded tree builds against."
    fail=1
  elif [ "${got}" != "${EXPECTED_VERSION}" ]; then
    red "  ${f}: QB_SHIPPED_VERSION=${got}, but ${QB_CONFIG} says ${EXPECTED_VERSION}"
    red "    A scaffolded project would be pinned to the wrong qb. Update the literal."
    fail=1
  else
    echo "  ${f}: QB_SHIPPED_VERSION=${got} OK"
  fi
done

# ---------------------------------------------------------------------------
# 2. The shared body is byte-identical.
# ---------------------------------------------------------------------------
tmp_p="$(mktemp)"; tmp_m="$(mktemp)"
trap 'rm -f "${tmp_p}" "${tmp_m}"' EXIT

sed -n "/${MARKER}/,\$p" "${PROJECT_SCRIPT}" > "${tmp_p}"
sed -n "/${MARKER}/,\$p" "${MODULE_SCRIPT}"  > "${tmp_m}"

lines_p=$(wc -l < "${tmp_p}" | tr -d '[:space:]')
lines_m=$(wc -l < "${tmp_m}" | tr -d '[:space:]')

if [ "${lines_p}" -lt "${SHARED_FLOOR}" ] || [ "${lines_m}" -lt "${SHARED_FLOOR}" ]; then
  red "  shared body is ${lines_p} / ${lines_m} lines, floor is ${SHARED_FLOOR}"
  red "    Either the marker is missing/misplaced, or the shared implementation was gutted."
  red "    Without this floor, a missing marker would make the identity check compare nothing."
  fail=1
else
  if diff -u "${tmp_p}" "${tmp_m}" > /dev/null 2>&1; then
    echo "  shared body: ${lines_p} lines, byte-identical in both scaffolders OK"
  else
    red "  shared body has DRIFTED between the two scaffolders:"
    diff -u "${tmp_p}" "${tmp_m}" | sed 's/^/    /' | head -60
    red "    Edit the body once and copy it to both; do not patch one copy."
    fail=1
  fi
fi

# ---------------------------------------------------------------------------
# 3. The substitution vocabulary is closed.
# ---------------------------------------------------------------------------
# The scripts own the set of @QB_...@ placeholders; a template cannot extend it silently,
# because rendering fails on an unresolved one. That guarantee is only real if the scripts
# actually substitute every token they claim to, so the render function is checked to carry
# one sed expression per documented placeholder.
for token in @QB_NAME@ @QB_NAME_LOWER@ @QB_NAME_UPPER@ @QB_REF@ @QB_VERSION@ @QB_TEMPLATE_REF@; do
  for f in "${PROJECT_SCRIPT}" "${MODULE_SCRIPT}"; do
    if ! grep -q -- "s|${token}|" "${f}"; then
      red "  ${f}: render() does not substitute ${token}"
      fail=1
    fi
  done
done
[ "${fail}" -eq 0 ] && echo "  substitution vocabulary: 6 placeholders, both scaffolders OK"

# ---------------------------------------------------------------------------
if [ "${fail}" -eq 0 ]; then
  grn "scaffolder consistency: OK"
else
  red "scaffolder consistency: FAILED"
fi
exit "${fail}"
