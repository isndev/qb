#!/usr/bin/env bash
#
# Gate A -- installed-header self-containment + entry-point LINK.
#
# Two things, against an INSTALLED prefix, never against the source tree:
#
#   phase 1  every installed header compiled ALONE (one TU whose entire content is
#            `#include <that/header.h>`). Catches "this header only ever compiled because
#            something else was included first".
#   phase 2  every public entry point LINKED into an executable that ODR-uses its headline
#            API. `-fsyntax-only` and `-c` both PASS on a header that declares a member
#            template whose definition ships in no TU the consumer can see; only the linker
#            says so. That gap is why <qb/main.h> could not emit qb::Actor::push<E> and
#            <qb/io/async/coroutine/task.h> could not destroy a task<T> -- for years, with
#            green CI, because the one consumer job in existence included the exact
#            combination of umbrellas that hides both (install-consume.yml, one TU with
#            <qb/actor.h> AND <qb/main.h>).
#
# CMake drives both phases on purpose. A hand-written -I/-l line is how this check lies:
# an omitted -largon2 turns every row of the matrix into the SAME undefined symbol and the
# real finding scrolls off. Here the flags come from the IMPORTED targets, i.e. from exactly
# what a find_package() consumer gets.
#
# Usage:
#   check-installed-headers.sh --prefix DIR
#                              --tree NAME:MIN ...      subtree of <prefix>/include to sweep
#                              --package NAME ...       find_package() to issue
#                              --link TARGET ...        target(s) to link phase 2 against
#                              --entry-dir DIR ...      directory of entry-point TUs
#                              [--hostile]              -I instead of -isystem, warnings fatal
#                              [--exclude "PATH REASON"] one more excluded header, with its reason
#                              [--build-dir DIR] [--jobs N] [--keep]
#
# --tree takes NAME:MIN, and MIN is not decoration: it is an ANTI-VACUOUS FLOOR, per surface.
# A sweep that FINDS too few headers because a path moved passes every check in this file while
# proving nothing, and a shared floor lets one surface silently absorb another's collapse.
# Raise a floor when a tree grows; never lower one to make a run pass.
#
# The floor is measured against the headers FOUND in the tree, not against the subset swept,
# and that distinction is the whole reason a floor exists: it guards against the tree going
# missing. Exclusions cannot hide behind it -- every one is named (in the table below or on the
# command line), integrity-checked to still exist under the prefix, and printed per tree in the
# run's own log. Counting the floor against the swept subset instead would mean the first
# legitimate conditional exclusion forces the floor DOWN, which is the one edit this comment
# and the one above both forbid.
#
# --exclude exists for exclusions the CALLER can justify and this script cannot see, e.g. a
# header gated on a build capability the installed package does not carry. It is not a shortcut:
# it takes the same "PATH REASON" shape as the table, gets the same existence check, and the
# caller is expected to prove separately whatever the sweep can no longer prove.
set -euo pipefail

PREFIX=""; BUILD_DIR=""; JOBS=""; HOSTILE=0; KEEP=0; CXX_COMPILER=""
declare -a TREES=() PACKAGES=() LINKS=() ENTRY_DIRS=() EXTRA_EXCLUDES=()
while [ $# -gt 0 ]; do
  case "$1" in
    --cxx)       CXX_COMPILER="$2"; shift 2 ;;
    --prefix)    PREFIX="$2"; shift 2 ;;
    --tree)      TREES+=("$2"); shift 2 ;;
    --package)   PACKAGES+=("$2"); shift 2 ;;
    --link)      LINKS+=("$2"); shift 2 ;;
    --entry-dir) ENTRY_DIRS+=("$2"); shift 2 ;;
    --exclude)   EXTRA_EXCLUDES+=("$2"); shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --jobs)      JOBS="$2"; shift 2 ;;
    --hostile)   HOSTILE=1; shift ;;
    --keep)      KEEP=1; shift ;;
    *) echo "usage error: unknown argument '$1'" >&2; exit 2 ;;
  esac
done
[ -n "$PREFIX" ] || { echo "usage error: --prefix is required" >&2; exit 2; }
[ "${#TREES[@]}" -gt 0 ] || { echo "usage error: at least one --tree is required" >&2; exit 2; }
[ "${#LINKS[@]}" -gt 0 ] || { echo "usage error: at least one --link is required" >&2; exit 2; }
PREFIX="$(cd "$PREFIX" && pwd)"
[ -d "$PREFIX/include" ] || { echo "::error::no include/ under prefix $PREFIX" >&2; exit 2; }
BUILD_DIR="${BUILD_DIR:-${TMPDIR:-/tmp}/qb-installed-headers.$$}"
[ "$KEEP" = 1 ] || trap 'rm -rf "$BUILD_DIR"' EXIT

# ---------------------------------------------------------------------------------------
# Headers that are NOT swept, each one named, with the reason it cannot be a plain TU.
#
# This list is CHECKED, not trusted: every entry must still exist under the prefix, or the
# run fails. An exclusion that outlives the file it names is how a real header quietly
# leaves the gate's scope after a rename.
# ---------------------------------------------------------------------------------------
# 3.0 emptied the "by-design fragment" category: qbm-http's router.tpp and qbm-pgsql's
# resultset.inl / transaction_coro.inl were each a set of bodies that could not compile alone,
# and all three were merged into the header that completes them. Nothing shipped is a fragment
# any more, which is why only a vendored table remains below.
#
# One entry was RETIRED rather than kept, and the distinction is the point of this list.
# qbm/pgsql/field_handler.h was named here as DEAD AND BROKEN -- 371 lines that nothing
# includes and that cannot compile alone, because they redefine members the merged tail of
# resultset.h already defines. Naming it here made this gate green by removing the only broken
# file in the shipped surface from the gate's scope, which is the one thing an exclusion list
# must never be used for. It is now excluded from the INSTALL instead
# (qbm/pgsql/CMakeLists.txt, HEADER_EXCLUDE), so it no longer reaches a prefix and this gate
# once again covers everything that does. A header that cannot compile alone belongs either
# in the sweep or out of the package -- never in the package and out of the sweep.
EXCLUDED_HEADERS="
qb/ev/qev_vars.h vendored fragment: libev's X-macro variable table, has no standalone meaning
"

# qb/io/async/epoll.h is installed unconditionally but needs <sys/epoll.h>. Excluding it
# outright would take it out of the gate on the ONE platform where it can be checked, so the
# exclusion is conditional on the host: Linux sweeps it, everyone else names it and moves on.
if [ "$(uname -s)" != "Linux" ]; then
  EXCLUDED_HEADERS="${EXCLUDED_HEADERS}
qb/io/async/epoll.h      platform fragment: needs <sys/epoll.h>; swept on Linux, skipped on $(uname -s)
"
fi

# ...and the caller's own, same shape and same integrity check. Appended AFTER the table so the
# table stays the authoritative record of what this script decides on its own.
for e in ${EXTRA_EXCLUDES[@]+"${EXTRA_EXCLUDES[@]}"}; do
  case "$e" in
    */*\ *) EXCLUDED_HEADERS="${EXCLUDED_HEADERS}${e}
" ;;
    *) echo "usage error: --exclude wants \"PATH REASON\" (a path and a stated reason), got '$e'" >&2; exit 2 ;;
  esac
done

say()  { printf '%s\n' "$*"; }
fail() { printf '::error::%s\n' "$*" >&2; exit 1; }

is_excluded() { case " $(printf '%s' "$EXCLUDED_HEADERS" | awk 'NF && $1 ~ /\// {print $1}' | tr '\n' ' ') " in *" $1 "*) return 0 ;; esac; return 1; }

mkdir -p "$BUILD_DIR/tu" "$BUILD_DIR/entry"

# --- exclusion list integrity ----------------------------------------------------------
# Only the exclusions that belong to a tree this run actually sweeps: the same list serves the
# qb-only leg (qb's own workflow) and the qb+qbm leg (the superproject's), and a qbm entry is
# legitimately absent from a prefix that ships no qbm.
missing=""; checked=0
for h in $(printf '%s' "$EXCLUDED_HEADERS" | awk 'NF && $1 ~ /\// {print $1}'); do
  for spec in "${TREES[@]}"; do
    case "$h" in "${spec%%:*}"/*)
      checked=$((checked + 1))
      [ -f "$PREFIX/include/$h" ] || missing="$missing $h" ;;
    esac
  done
done
[ -z "$missing" ] || fail "stale exclusion(s): no such installed header:$missing"

# --- phase 1: generate one TU per installed header --------------------------------------
: > "$BUILD_DIR/srcs.cmake"
{ echo "# generated by check-installed-headers.sh"; echo "set(SELFC_SRCS"; } >> "$BUILD_DIR/srcs.cmake"
total=0; skipped=0
for spec in "${TREES[@]}"; do
  tree="${spec%%:*}"; floor="${spec#*:}"
  [ "$floor" != "$spec" ] || fail "--tree wants NAME:MIN (the anti-vacuous floor), got '$spec'"
  [ -d "$PREFIX/include/$tree" ] || fail "no such subtree: $PREFIX/include/$tree"
  n=0; found=0; tree_excluded=""
  while IFS= read -r h; do
    found=$((found + 1))
    if is_excluded "$h"; then
      skipped=$((skipped + 1)); tree_excluded="${tree_excluded} ${h}"; continue
    fi
    slug="$(printf '%s' "$h" | tr '/.+-' '____')"
    printf '#include <%s>\n' "$h" > "$BUILD_DIR/tu/$slug.cpp"
    printf '  tu/%s.cpp\n' "$slug" >> "$BUILD_DIR/srcs.cmake"
    n=$((n + 1))
  done < <(cd "$PREFIX/include" && find "$tree" -type f \
             \( -name '*.h' -o -name '*.hpp' -o -name '*.tpp' -o -name '*.inl' \) | LC_ALL=C sort)
  say "tree ${tree}: ${found} headers found, ${n} to sweep (floor ${floor} on FOUND)"
  [ -z "$tree_excluded" ] || say "  not swept:${tree_excluded}"
  # Against FOUND, not against the swept subset -- see the --tree note at the top. Each
  # excluded header is named above and was already proven to exist under the prefix, so the
  # floor keeps doing its one job (the tree did not vanish) without turning every justified
  # exclusion into a floor reduction.
  [ "$found" -ge "$floor" ] || fail "tree ${tree}: only ${found} headers found, floor is ${floor} -- this sweep would be vacuous"
  [ "$n" -gt 0 ] || fail "tree ${tree}: ${found} headers found but every one is excluded -- nothing would be compiled"
  total=$((total + n))
done
echo ")" >> "$BUILD_DIR/srcs.cmake"
say "phase 1: ${total} headers, ${skipped} excluded by name"

# --- phase 2: collect the entry-point TUs ----------------------------------------------
: > "$BUILD_DIR/entries.cmake"
entries=0
for d in "${ENTRY_DIRS[@]:-}"; do
  [ -n "$d" ] || continue
  [ -d "$d" ] || fail "no such --entry-dir: $d"
  for f in "$d"/*.cpp; do
    [ -e "$f" ] || fail "--entry-dir $d contains no *.cpp -- the link phase would be vacuous"
    base="$(basename "$f" .cpp)"
    cp "$f" "$BUILD_DIR/entry/$base.cpp"
    printf 'add_executable(entry_%s entry/%s.cpp)\n' "$base" "$base" >> "$BUILD_DIR/entries.cmake"
    printf 'target_link_libraries(entry_%s PRIVATE ${QB_SELFC_LINK})\n' "$base" >> "$BUILD_DIR/entries.cmake"
    printf 'target_compile_options(entry_%s PRIVATE ${QB_SELFC_WARN})\n' "$base" >> "$BUILD_DIR/entries.cmake"
    entries=$((entries + 1))
  done
done
[ "$entries" -gt 0 ] || fail "no entry-point TUs: phase 2 would prove nothing"
say "phase 2: ${entries} entry points to compile AND link"

# --- the consumer project ---------------------------------------------------------------
{
  echo 'cmake_minimum_required(VERSION 3.24)'
  echo 'project(qb_installed_headers LANGUAGES CXX)'
  for p in "${PACKAGES[@]:-}"; do [ -n "$p" ] && printf 'find_package(%s CONFIG REQUIRED)\n' "$p"; done
  printf 'set(QB_SELFC_LINK'; for t in "${LINKS[@]}"; do printf ' %s' "$t"; done; echo ')'
  if [ "$HOSTILE" = 1 ]; then
    cat <<'EOF'
# HOSTILE MODE (gate B). Drop the -isystem that CMake puts on every IMPORTED target -- and
# -isystem is what silences EVERY -W* inside qb's own headers, for the one configuration CMake
# generates automatically. With plain -I the consumer sees them, which is what a hand-written
# build line has always seen. -Wundefined-func-template is the point: it names a template whose
# definition ships in a .cpp the consumer never sees AT THE POINT OF USE, instead of leaving it
# to the linker (or, worse, to -O0 emitting a weak out-of-line copy that makes the same source
# link in Debug and fail in Release).
#
# It must be set on the WHOLE transitive closure, not on the targets named by --link: the
# include directory that carries <prefix>/include belongs to qb::io, and qb::core merely links
# it. Setting it only on qb::core (or on qbm::redis) leaves -isystem in place and the whole
# gate silently becomes the non-hostile one -- measured, on the first attempt at this file.
# Restricted to qb/qbm's own targets on purpose: OpenSSL and friends arrive through
# find_dependency and their warnings are not qb's to answer for.
# Three properties, and all three are needed -- MEASURED, not assumed:
#   IMPORTED_NO_SYSTEM (3.23) / SYSTEM (3.25) are the documented knobs, and on qb they change
#   NOTHING on their own. qb's export writes an EXPLICIT
#     INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include"
#   on qb::nlohmann (qbDependencies.cmake:336 marks the bundled copy SYSTEM, and the bundled
#   copy's include dir IS <prefix>/include, i.e. qb's whole public include root). An explicit
#   entry outranks both knobs, so a gate that sets only IMPORTED_NO_SYSTEM runs with -isystem
#   and is indistinguishable from the non-hostile gate. Clearing the list is what flips the
#   generated line from `-isystem <prefix>/include` to `-I <prefix>/include`, and the script
#   asserts that below rather than trusting it.
get_property(_qb_imported DIRECTORY PROPERTY IMPORTED_TARGETS)
set(_qb_desystemed "")
foreach(_t IN LISTS _qb_imported)
  if(_t MATCHES "^qbm?::")
    set_target_properties(${_t} PROPERTIES
      IMPORTED_NO_SYSTEM TRUE SYSTEM OFF INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
    list(APPEND _qb_desystemed ${_t})
  endif()
endforeach()
if(NOT _qb_desystemed)
  message(FATAL_ERROR "--hostile: no qb/qbm imported target was de-systemed; the gate would "
                      "run with -isystem and prove nothing")
endif()
message(STATUS "hostile: -I (not -isystem) for ${_qb_desystemed}")
# -Wundefined-func-template is CLANG-ONLY -- gcc has no equivalent and rejects the flag
# outright, which would turn this gate into a compiler-not-found failure on every gcc runner.
# -Wall -Wextra -Werror carry on both, and they are what a -I consumer trips over first.
set(QB_SELFC_WARN -Wall -Wextra -Werror)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  list(APPEND QB_SELFC_WARN -Wundefined-func-template)
  message(STATUS "hostile: -Wundefined-func-template ON (${CMAKE_CXX_COMPILER_ID})")
else()
  message(STATUS "hostile: -Wundefined-func-template SKIPPED -- ${CMAKE_CXX_COMPILER_ID} does "
                 "not have it; this run does NOT cover the 'template defined in a .cpp the "
                 "consumer never sees' class. Run one leg on clang.")
endif()
EOF
  else
    echo 'set(QB_SELFC_WARN "")'
  fi
  # CMAKE_CXX_COMPILER_ID is NOT a cache entry -- grepping CMakeCache.txt for it always misses,
  # which is how the clang-only assertion below silently degraded to "NOTE: not clang" on a
  # machine where the compiler is AppleClang. Hand it to the shell explicitly.
  echo 'file(WRITE "${CMAKE_BINARY_DIR}/qb-selfc-compiler-id.txt" "${CMAKE_CXX_COMPILER_ID}")'
  echo 'include(srcs.cmake)'
  echo 'add_library(selfcontain OBJECT ${SELFC_SRCS})'
  echo 'target_link_libraries(selfcontain PRIVATE ${QB_SELFC_LINK})'
  echo 'target_compile_options(selfcontain PRIVATE ${QB_SELFC_WARN})'
  echo 'include(entries.cmake)'
} > "$BUILD_DIR/CMakeLists.txt"

CM_ARGS=(-S "$BUILD_DIR" -B "$BUILD_DIR/b" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX")
[ -n "$CXX_COMPILER" ] && CM_ARGS+=(-DCMAKE_CXX_COMPILER="$CXX_COMPILER")
command -v ninja > /dev/null 2>&1 && CM_ARGS+=(-G Ninja)
cmake "${CM_ARGS[@]}" > "$BUILD_DIR/configure.log" 2>&1 \
  || { cat "$BUILD_DIR/configure.log"; fail "configure failed against $PREFIX"; }

# --- hostile mode must be OBSERVABLE in the generated command line ----------------------
# Not a formality. The first version of this file set IMPORTED_NO_SYSTEM and nothing else, the
# run went green, and it was green because it had silently kept -isystem -- i.e. it was the
# non-hostile gate wearing its name. Read the generated rules back and require the flip.
if [ "$HOSTILE" = 1 ]; then
  gen="$(find "$BUILD_DIR/b" -maxdepth 1 \( -name 'build.ninja' -o -name 'Makefile' \) | head -1)"
  [ -n "$gen" ] || fail "--hostile: cannot find the generated build file to verify the flags"
  flags="$(cat "$gen" "$BUILD_DIR/b"/CMakeFiles/*/flags.make 2>/dev/null || cat "$gen")"
  case "$flags" in
    *"-isystem $PREFIX/include"*)
      fail "--hostile: the prefix is STILL on -isystem -- every -W* inside qb's headers is silenced, this gate would prove nothing" ;;
  esac
  case "$flags" in
    *"-I$PREFIX/include"*|*"-I $PREFIX/include"*) say "hostile: confirmed -I $PREFIX/include (not -isystem)" ;;
    *) fail "--hostile: the prefix appears on neither -I nor -isystem -- cannot tell what this gate measured" ;;
  esac
  # Clang-only flag: require it when the compiler is clang, and say so loudly when it is not,
  # so a green gcc run is never mistaken for coverage of the undefined-template class.
  cxxid="$(cat "$BUILD_DIR/b/qb-selfc-compiler-id.txt" 2>/dev/null || echo unknown)"
  case "$cxxid" in *Clang*) is_clang=1 ;; *) is_clang=0 ;; esac
  if [ "$is_clang" = 1 ]; then
    case "$flags" in
      *-Wundefined-func-template*) say "hostile: -Wundefined-func-template confirmed on the command line" ;;
      *) fail "--hostile: compiler is clang but -Wundefined-func-template is not on the command line" ;;
    esac
  else
    say "hostile: NOTE -- compiler is '${cxxid}', not clang, so -Wundefined-func-template did NOT run; this leg does not cover the undefined-template class"
  fi
fi

BUILD_ARGS=(--build "$BUILD_DIR/b")
[ -n "$JOBS" ] && BUILD_ARGS+=(--parallel "$JOBS")
# -k 0 / -k: report EVERY broken header in one run. Stopping at the first one turns a
# 23-header finding into a 23-run bisect.
if command -v ninja > /dev/null 2>&1; then BUILD_ARGS+=(-- -k 0); else BUILD_ARGS+=(-- -k); fi
set +e
cmake "${BUILD_ARGS[@]}" > "$BUILD_DIR/build.log" 2>&1
rc=$?
set -e

if [ "$rc" -ne 0 ]; then
  say ""
  say "================ FAILURES ================"
  # `Undefined symbols` / the indented symbol lines under it are how the LINK phase speaks --
  # ld says neither "error:" nor "FAILED:". Grepping only the compiler's vocabulary is how a
  # link gate reports "3 broken" for 4 broken things and prints none of the symbol names.
  grep -E '(^FAILED:|error:|warning:|Undefined symbols|undefined (reference|symbol)|^ +"|^ +_)' \
       "$BUILD_DIR/build.log" | sed "s#$PREFIX/include/##g" | head -250
  say "=========================================="
  bad_h="$(grep -c '^FAILED:.*/tu/' "$BUILD_DIR/build.log" || true)"
  bad_e="$(grep -c '^FAILED:.*[ /]entry_' "$BUILD_DIR/build.log" || true)"
  fail "installed-header gate: ${bad_h} header(s) not self-contained, ${bad_e} entry point(s) broken (log: $BUILD_DIR/build.log)"
fi

if [ "$HOSTILE" = 1 ]; then
  extra="-I -Wall -Wextra -Werror"
  [ "${is_clang:-0}" = 1 ] && extra="$extra -Wundefined-func-template"
  say "OK: ${total} installed headers each compile alone; ${entries} entry points compile AND link under ${extra}"
else
  say "OK: ${total} installed headers each compile alone; ${entries} entry points compile AND link"
fi
