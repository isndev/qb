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
# marker, the shared region must be at least SHARED_FLOOR lines, and render() must be at least
# RENDER_FLOOR lines and contain a sed. A diff of two empty strings is equal, a version check that
# finds no literal to compare would pass forever, and a token search over an empty haystack finds
# what it is looking for exactly as often as one over a correct function.
#
# THE FOUR CHECKS, and the hole each of the last three closed
# ----------------------------------------------------------
#  1. QB_SHIPPED_VERSION == QB_FRAMEWORK_VERSION, and assigned EXACTLY ONCE. The count is the
#     later addition: `sed … | head -1` read the FIRST assignment while bash uses the LAST, so a
#     second, stale line was blessed as OK.
#  2. The shared body below the marker is byte-identical in the two scripts.
#  3. render() carries one sed expression per placeholder — searched INSIDE the function body.
#     It used to be a whole-file grep, which a comment satisfied: render() reduced to `cat`, the
#     six expressions left in a comment above it, printed OK and exited 0.
#  4. No version literal in the shared body. This is the one that was invisible to BOTH 1 and 2 at
#     once: `QB_VERSION_TAG="v2.6.0"` in place of `"v${QB_SHIPPED_VERSION}"`, identical in the two
#     copies, leaves the identity check green and never touches the line check 1 reads.
#
# All four, plus their vacuity paths, are planted and asserted-rejected by the superproject's
# dev/agent/scaffold-consistency-negative-control.sh. Run it after editing this file: a green exit
# from a guard you just changed proves only that it did not crash.
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

# Same idea one level down, for check 3: the render() body must be substantial before its contents
# are allowed to satisfy anything. Deleting the function, or reducing it to `cat`, must be a
# finding rather than a vacuous pass.
RENDER_FLOOR=6

# Check 4's shape. THREE components, optionally v-prefixed: exactly the form QB_FRAMEWORK_VERSION
# takes (3.0.0) and the tag built from it (v3.0.0). Deliberately not `[0-9]+\.[0-9]+`, which would
# also match a `sleep 0.5` or a `timeout 2.5` — see the false-positive measurement in check 4.
#
# Exported and read back through ENVIRON rather than passed with `awk -v`, and that is not style:
# `awk -v shape='…\.…'` PROCESSES ESCAPE SEQUENCES in the assigned value, so awk received
# `v?[0-9]+.[0-9]+.[0-9]+` — three digit runs separated by ANY character, which quietly matches
# `for i in 1 2 3`. It still rejected v2.6.0 and still passed the clean tree, so nothing observable
# said the rule was not the rule as written. ENVIRON does no such processing: the regex behaves as
# it reads. Control F5 pins this by planting a line the mangled form matches and the correct one
# does not.
export VERSION_SHAPE='v?[0-9]+\.[0-9]+\.[0-9]+'

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
  # EXACTLY ONE assignment. This used to be `… | head -1`, which reads the FIRST assignment while
  # bash uses the LAST — so appending `QB_SHIPPED_VERSION=2.6.0` after the good line made this
  # check print "3.0.0 OK" over a script that pins v2.6.0. The guard was validating a value its
  # subject does not use, which is the exact failure class it exists to stop.
  #
  # The fix REJECTS the duplicate rather than emulating bash's last-wins, and that choice is the
  # defensible one: assignments can be indented, exported, conditional, inside a function or in a
  # sourced file, so any `sed`-level emulation of "what bash would end up with" is an approximation
  # that a determined defect walks straight past — and reading the last one would also silently
  # bless a file carrying a dead, stale, misleading pin. "Exactly one top-level assignment" is a
  # structural property, decidable by looking, and it is what every correct version of this file
  # has. A scaffolder with two version pins is malformed whichever one wins.
  #
  # Counted with a DELIBERATELY WIDER pattern than the one parsed below: an indented or exported
  # second assignment takes effect in bash and would be invisible to the canonical form.
  n_assign="$(grep -cE '^[[:space:]]*(export[[:space:]]+)?QB_SHIPPED_VERSION=' "${f}")"
  if [ "${n_assign}" -gt 1 ]; then
    red "  ${f}: QB_SHIPPED_VERSION is assigned ${n_assign} times; exactly one is allowed"
    grep -nE '^[[:space:]]*(export[[:space:]]+)?QB_SHIPPED_VERSION=' "${f}" | sed 's/^/      /'
    red "    bash uses the LAST assignment, so a second one silently re-pins the scaffolder."
    fail=1
    continue
  fi

  got="$(sed -n 's/^QB_SHIPPED_VERSION=\([0-9][0-9.]*\)[[:space:]]*$/\1/p' "${f}")"
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
#
# SCOPED TO THE FUNCTION BODY, and that scoping is the check. This used to grep the whole file,
# which the comment above never claimed and which a comment could satisfy: replacing render()'s
# body with `cat` and leaving the six expressions in a comment above it printed
# "6 placeholders, both scaffolders OK" and exited 0, over a scaffolder that rendered nothing and
# would have shipped literal @QB_NAME@ into a user's tree.
vocab_ok=1
for f in "${PROJECT_SCRIPT}" "${MODULE_SCRIPT}"; do
  render_body="$(sed -n '/^render() {/,/^}/p' "${f}")"
  render_lines="$(printf '%s' "${render_body}" | grep -c '' || true)"

  # Anti-vacuity, in the same spirit as SHARED_FLOOR: an empty or gutted body must be a finding,
  # never an empty haystack that every token search passes over. Deleting or renaming render()
  # lands here rather than producing six identical "does not substitute" lines.
  if [ -z "${render_body}" ]; then
    red "  ${f}: no render() function found (expected 'render() {' at column 0)"
    red "    Every placeholder check below reads its body; refusing to search an empty haystack."
    fail=1; vocab_ok=0; continue
  fi
  if [ "${render_lines}" -lt "${RENDER_FLOOR}" ]; then
    red "  ${f}: render() body is ${render_lines} lines, floor is ${RENDER_FLOOR}"
    red "    Either the function was gutted or its closing brace moved."
    fail=1; vocab_ok=0; continue
  fi
  if ! printf '%s\n' "${render_body}" | grep -q 'sed'; then
    red "  ${f}: render() body contains no sed expression at all"
    red "    A render() that does not substitute is a scaffolder that ships raw placeholders."
    fail=1; vocab_ok=0; continue
  fi

  for token in @QB_NAME@ @QB_NAME_LOWER@ @QB_NAME_UPPER@ @QB_REF@ @QB_VERSION@ @QB_TEMPLATE_REF@; do
    if ! printf '%s\n' "${render_body}" | grep -q -F -- "s|${token}|"; then
      red "  ${f}: render() does not substitute ${token}"
      fail=1; vocab_ok=0
    fi
  done
done
[ "${vocab_ok}" -eq 1 ] && echo "  substitution vocabulary: 6 placeholders, inside render(), both scaffolders OK"

# ---------------------------------------------------------------------------
# 4. No version literal in the shared body.
# ---------------------------------------------------------------------------
# The hole this closes was invisible to BOTH checks above at once, which is what makes it the
# dangerous one. Replace `QB_VERSION_TAG="v${QB_SHIPPED_VERSION}"` with `QB_VERSION_TAG="v2.6.0"`
# in BOTH scripts and: check 2 is green, because the copies are still identical to each other;
# check 1 is green, because it only ever looks at the QB_SHIPPED_VERSION line, which is untouched
# and still correct. The scaffolder pins the wrong qb and every check reports OK.
#
# Rule: on a shared-body line that is not a whole-line comment, no version-shaped literal. Every
# version reference in the body already flows through ${QB_SHIPPED_VERSION}, directly or via
# QB_VERSION_TAG, so this costs the correct scripts nothing.
#
# FALSE-POSITIVE MEASUREMENT, because a rule that cries wolf gets ignored rather than fixed:
#   over both shared bodies (398 lines each), as they stand today —
#     any `x.y`, all lines .............. 1 finding per script, 1 LEGITIMATE  → 100% FP, declined
#     `v?x.y.z`, all lines .............. 1 finding per script, 1 LEGITIMATE  → 100% FP, declined
#     `v?x.y.z`, non-comment lines ...... 0 findings                          → 0% FP, SHIPPED
#   The rejected finding in both cases is the same line, and it is documentation, not a pin:
#     `#     .../qb/v3.0.0/script/...  -> exactly that release`
#   Hence: whole-line comments are exempt, and the shape is three components. Two components
#   (`[0-9]+\.[0-9]+`) was not shipped because it would match a `sleep 0.5`.
literals_ok=1
for f in "${PROJECT_SCRIPT}" "${MODULE_SCRIPT}"; do
  hits="$(awk -v marker="${MARKER}" '
    BEGIN { shape = ENVIRON["VERSION_SHAPE"] }
    $0 ~ marker { inbody = 1 }
    inbody && $0 !~ /^[[:space:]]*#/ && $0 ~ shape { printf "      %d: %s\n", NR, $0 }
  ' "${f}")"
  if [ -n "${hits}" ]; then
    red "  ${f}: version literal hardcoded in the shared body"
    printf '%s\n' "${hits}"
    red "    Identical in both copies, this is invisible to the identity check AND to the"
    red "    QB_SHIPPED_VERSION check. Derive it from \${QB_SHIPPED_VERSION} instead."
    fail=1; literals_ok=0
  fi
done
[ "${literals_ok}" -eq 1 ] && echo "  shared body carries no hardcoded version literal OK"

# ---------------------------------------------------------------------------
if [ "${fail}" -eq 0 ]; then
  grn "scaffolder consistency: OK"
else
  red "scaffolder consistency: FAILED"
fi
exit "${fail}"
