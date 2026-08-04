#!/usr/bin/env bash
# Proves the link-time configuration fingerprint (qb/utility/abi.h) still works.
#
# WHY THIS EXISTS
# ---------------
# The fingerprint turns a silent header/archive configuration mismatch into an undefined symbol.
# It shipped in 3.0.0 with ZERO automated coverage -- `grep -rn 'qb_abi|abi_fingerprint|QB_ABI'
# tests/ .github/ scripts/` returned nothing -- while being one edit away from doing nothing at
# all, silently:
#
#   * `__attribute__((used))` alone is NOT enough on ELF. GNU ld's `--gc-sections` discards the
#     unreferenced `.data.rel.ro._ZN2qb6detail15abi_fingerprintE` section BEFORE resolving the
#     relocations inside it, so every mismatched configuration linked cleanly under
#     `-ffunction-sections -fdata-sections -Wl,--gc-sections`. Only `retain` (SHF_GNU_RETAIN)
#     restores the failure. That defeat is ELF-only, which is why this script must run on Linux
#     and not just macOS.
#   * If `qb/utility/abi.h` ever stops being reached from `qb/utility/prefix.h` /
#     `qb/utility/build_macros.h`, no consumer TU emits the references and the check evaporates
#     with no diagnostic anywhere.
#
# WHAT IT ASSERTS
#   1. A MATCHING consumer links and runs.
#   2. One MISMATCH per axis fails to link, naming that axis's symbol. The axis values are read
#      back off the installed archive rather than hardcoded, so an axis whose archive value cannot
#      be flipped is reported as SKIPPED, never silently counted as a pass.
#   3. The raw `-I`/`-l` case -- a consumer compiled without qb's CMake usage requirements --
#      fails to link naming `qb_abi_version_unknown__compile_with_qb_s_cmake_usage_requirements`.
#   4. Reference emission is still observable in the object file: on ELF the `abi_fingerprint`
#      section carries the `R` (SHF_GNU_RETAIN) flag -- `readelf -S` prints `WAGR` -- and the
#      mismatch STILL fails under `-Wl,--gc-sections`. On Mach-O `used` implies no_dead_strip;
#      the undefined `_qb_abi_*` references must survive `-O2` and `-Wl,-dead_strip`.
#
# It refuses to pass vacuously: every axis must end in PASS or an explicit SKIP with a reason, and
# a run in which nothing was actually exercised is a failure.
#
# USAGE
#   scripts/check-abi-fingerprint.sh [--prefix DIR] [--build-dir DIR] [--jobs N]
# With no --prefix it configures, builds and installs qb itself into a scratch prefix.

set -uo pipefail

QB_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX=""
BUILD_DIR=""
JOBS="$( { command -v nproc >/dev/null && nproc; } || sysctl -n hw.ncpu 2>/dev/null || echo 4 )"
WORK=""

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)    PREFIX="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        -h|--help)   sed -n '1,40p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

PASS=0
FAIL=0
SKIP=0
FAILED_NAMES=""

say()  { printf '%s\n' "$*"; }
pass() { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$*"; }
skip() { SKIP=$((SKIP + 1)); printf 'SKIP  %s\n' "$*"; }
fail() { FAIL=$((FAIL + 1)); FAILED_NAMES="${FAILED_NAMES}  - $1"$'\n'; printf 'FAIL  %s\n' "$*"; }

cleanup() { [ -n "$WORK" ] && [ -d "$WORK" ] && rm -rf "$WORK"; }
trap cleanup EXIT

WORK="$(mktemp -d 2>/dev/null || mktemp -d -t qb-abi)"

# ---------------------------------------------------------------------------------------------
# 0. Get an installed qb.
# ---------------------------------------------------------------------------------------------
if [ -z "$PREFIX" ]; then
    PREFIX="$WORK/prefix"
    [ -n "$BUILD_DIR" ] || BUILD_DIR="$WORK/build"
    say "== building and installing qb into $PREFIX =="
    cmake -S "$QB_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DQB_BUILD_TESTS=OFF -DQB_BUILD_EXAMPLES=OFF >"$WORK/install.log" 2>&1 \
      && cmake --build "$BUILD_DIR" --parallel "$JOBS" >>"$WORK/install.log" 2>&1 \
      && cmake --install "$BUILD_DIR" >>"$WORK/install.log" 2>&1
    if [ $? -ne 0 ]; then
        say "could not build/install qb; last 40 lines:"; tail -40 "$WORK/install.log"; exit 1
    fi
fi

ARCHIVE=""
for cand in "$PREFIX/lib/libqb-io.a" "$PREFIX/lib64/libqb-io.a"; do
    [ -f "$cand" ] && ARCHIVE="$cand" && break
done
if [ -z "$ARCHIVE" ]; then
    say "::error::no libqb-io.a under $PREFIX (lib/ or lib64/)"; exit 1
fi

# ---------------------------------------------------------------------------------------------
# 1. Read the archive's own fingerprint. Hardcoding the axis values would make this script lie on
#    any host whose defaults differ (macOS libc++ has advertised no __cpp_lib_jthread for years,
#    so std_jthread=0 there and the "force the fallback" knob cannot produce a mismatch at all).
# ---------------------------------------------------------------------------------------------
FP="$(strings - "$ARCHIVE" 2>/dev/null | grep -m1 '^qb-abi ' || true)"
if [ -z "$FP" ]; then
    say "::error::no 'qb-abi ...' string in $ARCHIVE -- the archive is not publishing its"
    say "          fingerprint, so nothing a consumer references can ever resolve."
    exit 1
fi
say "archive fingerprint: $FP"
axis_value() { printf '%s\n' "$FP" | tr ' ' '\n' | grep "^$1=" | cut -d= -f2; }
A_CACHELINE="$(axis_value cacheline)"
A_EXCEPTIONS="$(axis_value exceptions)"
A_CORODEBUG="$(axis_value coroutine_debug)"
A_JTHREAD="$(axis_value std_jthread)"

case "$(uname -s)" in
    Darwin) OBJFMT=macho ;;
    *)      OBJFMT=elf   ;;
esac
say "object format: $OBJFMT"

# ---------------------------------------------------------------------------------------------
# 2. Consumer project. One source file, reaching qb through a public header so that
#    qb/utility/prefix.h -> qb/utility/abi.h is parsed and the reference array is emitted.
# ---------------------------------------------------------------------------------------------
SRC="$WORK/consumer"
mkdir -p "$SRC"
cat > "$SRC/main.cpp" <<'EOF'
#include <qb/actor.h>
#include <qb/main.h>
#include <cstdio>
int main() {
    qb::Main engine;
    engine.core(0);
    std::printf("consumer ok: qb::Event is %zu bytes\n", sizeof(qb::Event));
    return 0;
}
EOF
cat > "$SRC/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(qb_abi_fingerprint_consumer LANGUAGES CXX)
find_package(qb CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE qb::core)
if(QB_ABI_EXTRA_DEFS)
    target_compile_definitions(consumer PRIVATE ${QB_ABI_EXTRA_DEFS})
endif()
if(QB_ABI_EXTRA_OPTS)
    target_compile_options(consumer PRIVATE ${QB_ABI_EXTRA_OPTS})
endif()
if(QB_ABI_GC_SECTIONS)
    target_compile_options(consumer PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(consumer PRIVATE "LINKER:--gc-sections")
endif()
if(QB_ABI_DEAD_STRIP)
    target_link_options(consumer PRIVATE "LINKER:-dead_strip")
endif()
EOF

# build_consumer <tag> <extra-defs> <extra-opts> <gc> <deadstrip> -> 0 built, 1 failed
# Output (configure + build) lands in $WORK/<tag>.log.
build_consumer() {
    local tag="$1" defs="$2" opts="$3" gc="$4" ds="$5"
    local bdir="$WORK/b-$tag"
    rm -rf "$bdir"
    {
        cmake -S "$SRC" -B "$bdir" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_PREFIX_PATH="$PREFIX" \
            -DQB_ABI_EXTRA_DEFS="$defs" \
            -DQB_ABI_EXTRA_OPTS="$opts" \
            -DQB_ABI_GC_SECTIONS="$gc" \
            -DQB_ABI_DEAD_STRIP="$ds" 2>&1 \
        && cmake --build "$bdir" --parallel "$JOBS" 2>&1
    } > "$WORK/$tag.log" 2>&1
    local rc=$?
    printf '%s\n' "$bdir" > "$WORK/$tag.dir"
    return $rc
}

# ---------------------------------------------------------------------------------------------
# 3. The matching consumer must link AND run. Without this the whole script could "pass" by
#    failing to link for a completely unrelated reason.
# ---------------------------------------------------------------------------------------------
say ""
say "== control: a matching consumer links and runs =="
if build_consumer match "" "" "" ""; then
    if "$WORK/b-match/consumer" >"$WORK/match.run" 2>&1; then
        pass "matching consumer links and runs ($(head -1 "$WORK/match.run"))"
    else
        fail "matching-consumer-runs"
        say "      the matching consumer linked but did not run:"; sed 's/^/      /' "$WORK/match.run"
    fi
else
    fail "matching-consumer-links"
    say "      a consumer with the SAME configuration as the archive failed to build."
    say "      Every mismatch assertion below would be meaningless. Last 30 lines:"
    tail -30 "$WORK/match.log" | sed 's/^/      /'
fi

# ---------------------------------------------------------------------------------------------
# 4. One mismatch per axis: must FAIL to link, naming that axis's symbol.
# ---------------------------------------------------------------------------------------------
# expect_link_failure <tag> <symbol> <defs> <opts> <gc> <what>
expect_link_failure() {
    local tag="$1" sym="$2" defs="$3" opts="$4" gc="$5" what="$6"
    if build_consumer "$tag" "$defs" "$opts" "$gc" ""; then
        fail "$tag"
        say "      $what LINKED. A mismatched configuration is silent again."
    elif grep -q "$sym" "$WORK/$tag.log"; then
        pass "$what -> link fails naming $sym"
    else
        fail "$tag"
        say "      $what failed to build, but NOT with an undefined $sym --"
        say "      so the failure is not the fingerprint. Last 25 lines:"
        tail -25 "$WORK/$tag.log" | sed 's/^/      /'
    fi
}

say ""
say "== one mismatch per axis must fail at LINK =="

# -- cacheline ------------------------------------------------------------------------------
OTHER_CL=128; [ "$A_CACHELINE" = "128" ] && OTHER_CL=64
expect_link_failure "cacheline" "qb_abi_cacheline_${OTHER_CL}" \
    "KNOWN_L1_CACHE_LINE_SIZE=${OTHER_CL}" "" "" \
    "cacheline: consumer ${OTHER_CL} vs archive ${A_CACHELINE}"

# -- exceptions -----------------------------------------------------------------------------
if [ "$A_EXCEPTIONS" = "1" ]; then
    expect_link_failure "exceptions" "qb_abi_exceptions_0" "" "-fno-exceptions" "" \
        "exceptions: consumer -fno-exceptions vs archive ${A_EXCEPTIONS}"
else
    skip "exceptions: archive is already exceptions=0; no knob turns a consumer back ON"
fi

# -- coroutine debug ------------------------------------------------------------------------
if [ "$A_CORODEBUG" = "0" ]; then
    expect_link_failure "corodebug" "qb_abi_coroutine_debug_1" "QB_DEBUG_COROUTINES" "" "" \
        "coroutine_debug: consumer 1 vs archive ${A_CORODEBUG}"
else
    skip "coroutine_debug: archive is already 1; QB_DEBUG_COROUTINES cannot un-set it"
fi

# -- std::jthread ---------------------------------------------------------------------------
if [ "$A_JTHREAD" = "1" ]; then
    expect_link_failure "jthread" "qb_abi_std_jthread_0" "QB_COMPAT_FORCE_THREAD_FALLBACK" "" "" \
        "std_jthread: consumer forced to the fallback vs archive ${A_JTHREAD}"
else
    skip "std_jthread: this toolchain does not advertise __cpp_lib_jthread, so the archive is \
already 0 and the FORCE_THREAD_FALLBACK knob cannot create a mismatch"
fi

# -- version, via the raw -I/-l case --------------------------------------------------------
# The one axis that cannot be expressed as a CMake definition, because its whole point is a
# consumer built WITHOUT qb's CMake usage requirements: no QB_VERSION_*, and therefore also no
# QB_HAS_SSL / QB_HAS_QUIC / QB_HAS_COMPRESSION.
say ""
say "== the raw -I/-l consumer (no CMake usage requirements) must fail at LINK =="
LIBDIR="$(dirname "$ARCHIVE")"
RAW_LOG="$WORK/rawlink.log"
if "${CXX:-c++}" -std=c++20 -O2 -I"$PREFIX/include" "$SRC/main.cpp" \
        -L"$LIBDIR" -lqb-core -lqb-io -o "$WORK/raw_consumer" >"$RAW_LOG" 2>&1; then
    fail "raw-include-link"
    say "      a hand-written -I/-l consumer LINKED. It has no QB_VERSION_* and no feature"
    say "      macros, so its inline answers contradict the archive's out-of-line ones."
elif grep -q 'qb_abi_version_unknown__compile_with_qb_s_cmake_usage_requirements' "$RAW_LOG"; then
    pass "raw -I/-l consumer -> link fails naming qb_abi_version_unknown__..."
elif grep -qi 'error:.*qb/actor.h\|fatal error' "$RAW_LOG" && ! grep -q 'qb_abi' "$RAW_LOG"; then
    # A raw consumer may not even compile on a host needing extra -I for a system dep. That is a
    # weaker outcome than a link error, so say so rather than banking it as a pass.
    skip "raw -I/-l consumer did not reach the link step on this host (see $RAW_LOG)"
else
    fail "raw-include-link"
    say "      the raw consumer failed, but not on qb_abi_version_unknown__... Last 25 lines:"
    tail -25 "$RAW_LOG" | sed 's/^/      /'
fi

# ---------------------------------------------------------------------------------------------
# 5. Reference emission is still observable, and still survives the linker's garbage collector.
#    This is the half that nearly shipped broken.
# ---------------------------------------------------------------------------------------------
say ""
say "== reference emission survives the linker's garbage collector =="

OBJ="$(find "$WORK/b-match" -name 'main.cpp.o' 2>/dev/null | head -1)"
if [ -z "$OBJ" ]; then
    fail "reference-emission-object"
    say "      could not find the matching consumer's object file to inspect."
elif [ "$OBJFMT" = "elf" ]; then
    # SHF_GNU_RETAIN is the observable: readelf spells the flag set WAGR.
    SECLINE="$(readelf -S -W "$OBJ" 2>/dev/null | grep 'abi_fingerprint' | head -1)"
    if [ -z "$SECLINE" ]; then
        fail "abi-fingerprint-section"
        say "      no abi_fingerprint section in $OBJ -- the reference array is not being emitted."
    else
        say "      $SECLINE"
        # Flags column: R must be present alongside WA.
        FLAGS="$(printf '%s\n' "$SECLINE" | awk '{for(i=1;i<=NF;i++) if($i ~ /^[WAXGRMSILTOopE]+$/ && $i ~ /R/ && $i ~ /A/) print $i}' | head -1)"
        if printf '%s\n' "$SECLINE" | grep -qE '\bWAGR\b|\bWAR\b'; then
            pass "abi_fingerprint carries SHF_GNU_RETAIN (flags ${FLAGS:-WAGR})"
        else
            fail "abi-fingerprint-retain"
            say "      abi_fingerprint exists but has NO R (retain) flag. \`used\` alone does not"
            say "      stop GNU ld's --gc-sections from discarding it before it resolves the"
            say "      relocations inside, so every mismatch links silently. Re-add"
            say "      __attribute__((retain)) / QB_ABI_RETAIN in qb/utility/abi.h."
        fi
    fi
    # And the behavioural half: the SAME mismatch must still fail under --gc-sections.
    expect_link_failure "cacheline-gc" "qb_abi_cacheline_${OTHER_CL}" \
        "KNOWN_L1_CACHE_LINE_SIZE=${OTHER_CL}" "" "ON" \
        "cacheline mismatch under -ffunction-sections -fdata-sections -Wl,--gc-sections"
else
    # Mach-O: `used` implies no_dead_strip. The observable is the undefined references surviving
    # -O2 in the object, and the mismatch still failing under -Wl,-dead_strip.
    NREF="$(nm -u "$OBJ" 2>/dev/null | grep -c '_qb_abi_')"
    if [ "${NREF:-0}" -ge 5 ]; then
        pass "consumer object carries $NREF undefined _qb_abi_* references at -O2"
    else
        fail "abi-fingerprint-references"
        say "      expected 5 undefined _qb_abi_* references in $OBJ, found ${NREF:-0}."
        say "      QB_ABI_USED is no longer keeping the reference array alive."
    fi
    if build_consumer "cacheline-ds" "KNOWN_L1_CACHE_LINE_SIZE=${OTHER_CL}" "" "" "ON"; then
        fail "cacheline-ds"
        say "      cacheline mismatch LINKED under -Wl,-dead_strip."
    elif grep -q "qb_abi_cacheline_${OTHER_CL}" "$WORK/cacheline-ds.log"; then
        pass "cacheline mismatch still fails under -Wl,-dead_strip"
    else
        fail "cacheline-ds"
        say "      failed under -dead_strip but not on the fingerprint symbol. Last 25 lines:"
        tail -25 "$WORK/cacheline-ds.log" | sed 's/^/      /'
    fi
fi

# ---------------------------------------------------------------------------------------------
# 6. Verdict. A run that exercised nothing is a failure, not a pass.
# ---------------------------------------------------------------------------------------------
say ""
say "================================================================"
say "  PASS $PASS   FAIL $FAIL   SKIP $SKIP"
if [ "$FAIL" -gt 0 ]; then
    say "  failing checks:"; printf '%s' "$FAILED_NAMES"
fi
say "================================================================"

# Floor: control + 3 mismatch axes are reachable on every supported host (cacheline, exceptions,
# coroutine_debug) plus the emission checks. Anything less means the script stopped testing.
MIN_PASS=6
if [ "$FAIL" -gt 0 ]; then
    say "::error::the ABI configuration fingerprint is not doing its job"
    exit 1
fi
if [ "$PASS" -lt "$MIN_PASS" ]; then
    say "::error::only $PASS checks ran (floor is $MIN_PASS) -- this run proved almost nothing."
    say "         Refusing to report success vacuously."
    exit 1
fi
say "the ABI configuration fingerprint is armed on every axis this host can flip."
exit 0
