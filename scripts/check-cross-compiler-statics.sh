#!/usr/bin/env bash
# check-cross-compiler-statics.sh — refuse a function-local static whose mangled
# name differs between clang and gcc.
#
# WHY THIS EXISTS
# ---------------
# A gcc-built consumer linked against a clang-built qb aborted with
# "free(): invalid pointer" the first time it destroyed an EMPTY qb::unordered_map
# whose key or value contained a std::string (fixed in 7e2eefdc).
#
# When T carries libstdc++'s cxx11 ABI tag, clang appends `B5cxx11` to the mangled
# name of a function-LOCAL STATIC and gcc does not:
#
#   clang  _ZZN3ska9detailv1018sherwood_v10_entryI...13empty_pointerEvE6resultB5cxx11
#   gcc    _ZZN3ska9detailv1018sherwood_v10_entryI...13empty_pointerEvE6result
#
# The two names do not merge at link time, so the final binary holds TWO copies of
# what the code assumes is one shared object. ska's empty-table sentinel is exactly
# that: every empty table points at it, and deallocate_data() decides whether to
# free by comparing against it. A table created on one side of the boundary and
# destroyed on the other takes the free branch and hands the allocator static
# storage. Silent heap corruption, no link-time diagnostic.
#
# WHY IT MUST BE A LINUX JOB
# --------------------------
# The tag is a libstdc++ construct. macOS/libc++ has no cxx11 tag at all, so a
# macOS build — including the maintainer's own machine and the macos-clang CI job —
# is STRUCTURALLY BLIND to this class. Only a Linux job that compiles the SAME
# translation unit with BOTH toolchains can see it, which is what this script does.
#
# WHAT IT CHECKS
# --------------
# Compiles one probe TU with clang++ and with g++, lists every function-local
# entity from each object file (Itanium mangling: `_ZZ…` for the entity, `_ZGVZ…`
# for its guard variable), and fails if a symbol present in one compiler's output
# is another compiler's symbol plus an ABI-tag suffix. That is the divergence
# class, stated directly: same entity, two names, one of them tagged.
#
# It deliberately does NOT flag a tag both compilers agree on — a function that
# returns std::string is tagged identically on both and is not a defect.
#
# WHAT TO DO WHEN IT FIRES
# ------------------------
# Do not add an exemption. Take the tagged type out of the static:
#   * hoist it to an `inline static` DATA member if it is constant-initialized
#     (a variable is mangled from its class template's arguments, which both
#     compilers spell identically), or
#   * give the static tag-free storage — raw bytes plus placement new — if
#     hoisting would cost constant initialization.
# Both cures are in src/qb/vendor/ska_hash/, with the reasoning for choosing
# between them written at each site.
#
# USAGE
# -----
#   ./scripts/check-cross-compiler-statics.sh                  # from the qb root
#   CLANGXX=clang++-22 GXX=g++-14 ./scripts/check-cross-compiler-statics.sh
#   ./scripts/check-cross-compiler-statics.sh --std c++23 --keep
#
# Exit 0 = the two compilers agree on every function-local static.
# Exit 1 = at least one divergent static.
# Exit 2 = the check could not run (missing compiler, TU failed to compile). A
#          check that cannot run must never report success.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STD="c++20"
KEEP=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --std) STD="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    -h|--help) sed -n '2,60p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "check-cross-compiler-statics: unknown argument: $1" >&2; exit 2 ;;
  esac
done

# ── toolchains ──────────────────────────────────────────────────────────────────
find_first() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { echo "$c"; return 0; }; done; return 1; }

CLANGXX="${CLANGXX:-$(find_first clang++-22 clang++-21 clang++-20 clang++-19 clang++ || true)}"
GXX="${GXX:-$(find_first g++-14 g++-15 g++-13 g++ || true)}"

for pair in "clang++:${CLANGXX:-}" "g++:${GXX:-}"; do
  kind="${pair%%:*}"; exe="${pair#*:}"
  if [[ -z "${exe}" ]] || ! command -v "${exe}" >/dev/null 2>&1; then
    echo "check-cross-compiler-statics: needs BOTH a clang++ and a g++; no usable ${kind} (got '${exe:-none}')." >&2
    echo "check-cross-compiler-statics: this check is meaningless with one toolchain — not skipping, failing." >&2
    exit 2
  fi
done

# The tag this looks for is libstdc++'s. Both compilers must therefore be using
# libstdc++, which on Linux they are by default. On a libc++ platform the whole
# class is unobservable and a green run would be vacuous, so say so and stop.
if [[ "$(uname -s)" != "Linux" ]]; then
  echo "check-cross-compiler-statics: libstdc++'s cxx11 ABI tag does not exist on $(uname -s)." >&2
  echo "check-cross-compiler-statics: a green run here would be vacuous. Run this on Linux." >&2
  exit 2
fi

command -v nm >/dev/null 2>&1 || { echo "check-cross-compiler-statics: nm not found." >&2; exit 2; }

WORK="$(mktemp -d)"
cleanup() { [[ "${KEEP}" -eq 1 ]] || rm -rf "${WORK}"; }
trap cleanup EXIT
[[ "${KEEP}" -eq 1 ]] && echo "check-cross-compiler-statics: work dir kept at ${WORK}"

# ── probe TU ────────────────────────────────────────────────────────────────────
# Headers alone emit nothing: the sentinel is a member of a class TEMPLATE, so it
# exists only once something instantiates it. Every alias below is instantiated
# with a cxx11-tagged type (std::string in the key, the value, or both) and both
# CONSTRUCTED and DESTROYED, because the sentinel and the comparison against it
# live on opposite ends of that pair — an instantiation that never destroys is
# not a probe, it is a compile.
cat > "${WORK}/probe.cpp" <<'PROBE'
#include <qb/system/container/unordered_map.h>
#include <qb/system/container/unordered_set.h>

#include <string>
#include <vector>

namespace {

// qb::unordered_map / qb::unordered_set — ska::detailv10::sherwood_v10_entry,
// whose empty sentinel is the one the cross-compiler abort came from.
using MapSS  = qb::unordered_map<std::string, std::string>;
using MapSV  = qb::unordered_map<std::string, std::vector<std::string>>;
using MapIS  = qb::unordered_map<int, std::string>;
using SetS   = qb::unordered_set<std::string>;
// qb::unordered_flat_map / _set — ska::detailv3::sherwood_v3_entry, the second
// sentinel, which is fixed a different way and must be swept the same way.
using FlatSS = qb::unordered_flat_map<std::string, std::string>;
using FlatSI = qb::unordered_flat_map<std::string, int>;
using FlatSetS = qb::unordered_flat_set<std::string>;
// The case-insensitive string maps qb publishes on top of them.
using CiMap  = qb::icase_unordered_map<std::string>;

template <typename T>
void round_trip() {
    T *empty = new T();      // touches the empty sentinel
    delete empty;            // …and the deallocate path that compares against it
    T grown;
    grown.reserve(8);        // forces a real allocation, so both branches exist
    grown.clear();
}

// icase_basic_map inherits its base PRIVATELY, so reserve() is not reachable
// through it. new/delete is what matters here anyway: it is the pair that
// touches the sentinel and the comparison against it.
template <typename T>
void round_trip_opaque() {
    delete new T();
}

} // namespace

extern "C" void qb_probe_cross_compiler_statics();
void
qb_probe_cross_compiler_statics() {
    round_trip<MapSS>();
    round_trip<MapSV>();
    round_trip<MapIS>();
    round_trip<SetS>();
    round_trip<FlatSS>();
    round_trip<FlatSI>();
    round_trip<FlatSetS>();
    round_trip_opaque<CiMap>();
}
PROBE

compile() { # compile <compiler> <output.o>
  local cxx="$1" out="$2" log="${2%.o}.log"
  if ! "${cxx}" -std="${STD}" -O2 -c "${WORK}/probe.cpp" -I "${ROOT}/src" -o "${out}" >"${log}" 2>&1; then
    echo "check-cross-compiler-statics: probe TU failed to compile with ${cxx}:" >&2
    sed 's/^/    /' "${log}" >&2
    exit 2
  fi
}

compile "${CLANGXX}" "${WORK}/probe_clang.o"
compile "${GXX}"     "${WORK}/probe_gcc.o"

# ── symbol sets ─────────────────────────────────────────────────────────────────
# `_ZZ…` is the Itanium encoding for an entity local to a function; `_ZGVZ…` is
# its guard variable. Those two prefixes ARE the defect class — a namespace-scope
# variable is mangled from its own template arguments and cannot diverge this way.
locals_of() { nm "$1" | grep -oE '_Z(GV)?Z[A-Za-z0-9_]+' | sort -u; }

locals_of "${WORK}/probe_clang.o" > "${WORK}/clang.syms"
locals_of "${WORK}/probe_gcc.o"   > "${WORK}/gcc.syms"

nc=$(wc -l < "${WORK}/clang.syms" | tr -d ' ')
ng=$(wc -l < "${WORK}/gcc.syms" | tr -d ' ')

# A run that found no function-local statics at all proves nothing — it means the
# probe did not instantiate what it meant to, not that the tree is clean.
if [[ "${nc}" -eq 0 && "${ng}" -eq 0 ]]; then
  echo "check-cross-compiler-statics: neither object file contains a function-local static." >&2
  echo "check-cross-compiler-statics: the probe is vacuous — refusing to report success." >&2
  exit 2
fi

# ── the comparison ──────────────────────────────────────────────────────────────
# For every symbol only one compiler emitted, strip ABI tags and look for the
# other compiler's spelling of the same entity. A hit is a divergent static.
# Reported in both directions: which compiler appends the tag is not the point,
# the disagreement is.
findings=0
report_divergence() { # report_divergence <only-file> <other-file> <tagged-by> <plain-by>
  local only="$1" other="$2" tagged="$3" plain="$4" sym base
  while IFS= read -r sym; do
    [[ -n "${sym}" ]] || continue
    base="$(printf '%s' "${sym}" | sed -E 's/B[0-9]+cxx11//g')"
    [[ "${base}" != "${sym}" ]] || continue          # not tagged at all
    if grep -qxF "${base}" "${other}"; then
      echo "  DIVERGENT function-local static:"
      echo "    ${tagged}: ${sym}"
      echo "    ${plain}: ${base}"
      findings=$((findings + 1))
    fi
  done < "${only}"
}

comm -23 "${WORK}/clang.syms" "${WORK}/gcc.syms" > "${WORK}/clang_only.syms"
comm -13 "${WORK}/clang.syms" "${WORK}/gcc.syms" > "${WORK}/gcc_only.syms"

report_divergence "${WORK}/clang_only.syms" "${WORK}/gcc.syms"   "${CLANGXX}" "${GXX}"
report_divergence "${WORK}/gcc_only.syms"   "${WORK}/clang.syms" "${GXX}"     "${CLANGXX}"

if [[ "${findings}" -gt 0 ]]; then
  cat >&2 <<EOF

check-cross-compiler-statics: ${findings} divergent function-local static(s).
  ${CLANGXX} emitted ${nc}, ${GXX} emitted ${ng}.

A program mixing a clang-built qb with a gcc-built consumer holds TWO copies of
each of the above. If any of them is used as a shared sentinel or a cache whose
IDENTITY matters, that is silent heap corruption with no link-time diagnostic.

Fix by keeping the tagged type out of the static — see the header of this script
and the two worked examples in src/qb/vendor/ska_hash/.
EOF
  exit 1
fi

echo "check-cross-compiler-statics: OK — ${CLANGXX} and ${GXX} agree on every"
echo "  function-local static in the probe (${nc} and ${ng} symbols, std=${STD})."
