#!/usr/bin/env bash
#
# NEGATIVE CONTROLS for check-installed-headers.sh.
#
# A gate that has never been shown to reject anything is indistinguishable from no gate, and
# this project has shipped exactly that mistake before: the hostile run below went green on its
# first attempt because IMPORTED_NO_SYSTEM had silently failed to drop -isystem, so it was the
# non-hostile run wearing a different name. Every control here plants ONE defect, requires the
# gate to REJECT it, then restores and verifies the restore is BYTE-EXACT by sha256.
#
# The prefix is COPIED first: a control that fails halfway must not be able to poison the
# artifact the rest of the job consumes.
#
# Usage: check-installed-headers-selftest.sh --prefix DIR [--work DIR] [--cxx COMPILER]
set -euo pipefail

PREFIX=""; WORK=""; CXX_COMPILER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --work)   WORK="$2"; shift 2 ;;
    --cxx)    CXX_COMPILER="$2"; shift 2 ;;
    *) echo "usage error: unknown argument '$1'" >&2; exit 2 ;;
  esac
done
[ -n "$PREFIX" ] || { echo "usage error: --prefix is required" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
GATE="$HERE/check-installed-headers.sh"
[ -x "$GATE" ] || GATE="bash $HERE/check-installed-headers.sh"
WORK="${WORK:-${TMPDIR:-/tmp}/qb-selftest.$$}"
rm -rf "$WORK"; mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

cp -R "$PREFIX" "$WORK/prefix"
P="$WORK/prefix"
ENTRIES="$HERE/installed-entry-points"

sha() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}' || sha256sum "$1" | awk '{print $1}'; }

pass=0; fail=0; na=0
ok()  { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$*"; }
ko()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$*"; }
# N/A is NOT a pass. It is recorded, counted, and repeated in the summary, so that a leg which
# structurally cannot exercise a control never reads as a leg that exercised it.
skip() { na=$((na+1)); printf '  \033[33mN/A \033[0m %s\n' "$*"; }

# run_gate <extra args...> ; returns the gate's exit code, output in $GATE_OUT
GATE_OUT=""
declare -a CXX_ARG=()
[ -n "$CXX_COMPILER" ] && CXX_ARG=(--cxx "$CXX_COMPILER")
run_gate() {
  local bd="$WORK/b.$RANDOM"
  set +e
  GATE_OUT="$($GATE --prefix "$P" --build-dir "$bd" "${CXX_ARG[@]}" "$@" 2>&1)"
  local rc=$?
  set -e
  rm -rf "$bd"
  return $rc
}
QB_ARGS=(--tree qb:140 --package qb --link qb::core --entry-dir "$ENTRIES")

# =========================================================================================
say_control() { printf '\n--- %s\n' "$*"; }

# ---- C0 baseline: the gate must ACCEPT the untouched prefix -----------------------------
# Without this, every rejection below could just mean "the gate rejects everything".
say_control "C0  baseline (untouched prefix) must be ACCEPTED"
if run_gate "${QB_ARGS[@]}"; then ok "clean prefix accepted"; else ko "clean prefix REJECTED -- every control below is meaningless"; echo "$GATE_OUT" | tail -20; fi

# ---- C1 a header that is not self-contained ---------------------------------------------
say_control "C1  a NON-SELF-CONTAINED header must be REJECTED (phase 1)"
V="$P/include/qb/io/async/coroutine/channel.h"; B="$(sha "$V")"
perl -0pi -e 's{^\#include <any>.*\n}{}m' "$V"
if run_gate "${QB_ARGS[@]}"; then ko "planted missing <any> was NOT detected"; else
  echo "$GATE_OUT" | grep -m2 -E "channel\.h.*(no type named 'any'|no member named 'any')" | sed 's/^/      /' || true
  echo "$GATE_OUT" | grep -m1 '::error::' | sed 's/^/      /' || true
  ok "missing include detected"
fi
cp -R "$PREFIX/include/qb/io/async/coroutine/channel.h" "$V"
[ "$(sha "$V")" = "$B" ] && ok "restored byte-exact ($B)" || ko "restore is NOT byte-exact"

# ---- C2 an undefined function template, seen only under -Wundefined-func-template --------
say_control "C2  an UNDEFINED FUNCTION TEMPLATE must be REJECTED by --hostile and ACCEPTED without it"
V="$P/include/qb/string.h"; B="$(sha "$V")"
cat >> "$V" <<'EOF'
namespace qb::selftest_c2 {
template <typename T> T undefined_template(T);              // declared, never defined
inline int use_it() { return undefined_template<int>(1); }  // non-dependent instantiation
} // namespace qb::selftest_c2
EOF
if run_gate "${QB_ARGS[@]}"; then
  ok "without --hostile: accepted (so the flag, not the plant, is what --hostile catches)"
else ko "without --hostile it already failed -- C2 would not isolate -Wundefined-func-template"; fi
if run_gate --hostile "${QB_ARGS[@]}"; then
  # gcc has no -Wundefined-func-template at all. Distinguish "the gate failed to fire" from
  # "this compiler cannot fire it" by reading back what the gate itself said it enabled.
  case "$GATE_OUT" in
    *"not clang, so -Wundefined-func-template did NOT run"*)
      skip "compiler has no -Wundefined-func-template: this leg does NOT cover the undefined-template class (run one leg on clang)" ;;
    *) ko "--hostile did NOT reject an undefined function template, and the compiler is clang" ;;
  esac
else
  echo "$GATE_OUT" | grep -m2 -E 'Wundefined-func-template' | sed 's/^/      /' || true
  ok "--hostile rejected it"
fi
cp -R "$PREFIX/include/qb/string.h" "$V"
[ "$(sha "$V")" = "$B" ] && ok "restored byte-exact ($B)" || ko "restore is NOT byte-exact"

# ---- C3 a LINK-only failure: -c and -fsyntax-only both pass ------------------------------
say_control "C3  a LINK-ONLY failure must be REJECTED (phase 2), and phase 1 must NOT see it"
V="$P/include/qb/io/async/coroutine/task.h"; B="$(sha "$V")"
perl -0pi -e 's{^\#include "scheduler\.h"\n}{}m' "$V"
if run_gate "${QB_ARGS[@]}"; then ko "removing task.h's tail include was NOT detected"; else
  hdrfail="$(echo "$GATE_OUT" | sed -n 's/.*gate: \([0-9]*\) header(s).*/\1/p')"
  echo "$GATE_OUT" | grep -m3 -E 'Undefined symbols|forget_frame_if_current|defer_frame_destruction' | sed 's/^/      /' || true
  echo "$GATE_OUT" | grep -m1 '::error::' | sed 's/^/      /' || true
  if [ "$hdrfail" = "0" ]; then ok "rejected at LINK with 0 compile failures -- exactly the class -c cannot see"
  else ko "rejected, but $hdrfail header(s) also failed to compile: C3 is not isolating the link phase"; fi
fi
cp -R "$PREFIX/include/qb/io/async/coroutine/task.h" "$V"
[ "$(sha "$V")" = "$B" ] && ok "restored byte-exact ($B)" || ko "restore is NOT byte-exact"

# ---- C4 -I vs -isystem, proved through a warning only -I can see -------------------------
say_control "C4  a -Wall/-Wextra finding inside a qb header must be REJECTED by --hostile only"
V="$P/include/qb/string.h"; B="$(sha "$V")"
cat >> "$V" <<'EOF'
namespace qb::selftest_c4 {
inline int unused_param(int used, int ignored) { return used; }   // -Wunused-parameter
} // namespace qb::selftest_c4
EOF
if run_gate "${QB_ARGS[@]}"; then ok "without --hostile: accepted (-isystem silences it, which is the whole finding)"
else ko "non-hostile run rejected a mere warning -- C4 cannot isolate the -I flip"; fi
if run_gate --hostile "${QB_ARGS[@]}"; then ko "--hostile did NOT reject it: it is still running with -isystem"; else
  echo "$GATE_OUT" | grep -m2 -E 'Wunused-parameter' | sed 's/^/      /' || true
  ok "--hostile rejected it -- the prefix really is on -I"
fi
cp -R "$PREFIX/include/qb/string.h" "$V"
[ "$(sha "$V")" = "$B" ] && ok "restored byte-exact ($B)" || ko "restore is NOT byte-exact"

# ---- C5 the anti-vacuous floor ----------------------------------------------------------
say_control "C5  a sweep that finds fewer headers than its floor must be REJECTED"
if run_gate --tree qb:100000 --package qb --link qb::core --entry-dir "$ENTRIES"; then
  ko "an impossible floor was accepted -- the floor is decorative"
else
  { echo "$GATE_OUT" | grep -m1 'floor' | sed 's/^/      /'; } || true; ok "floor enforced"
fi

# ---- C6 a stale exclusion ---------------------------------------------------------------
# Deliberately Actor.tpp, not epoll.h: epoll.h is excluded only on non-Linux, so using it here
# would make this control silently vanish on the ONE platform the gate runs on in CI.
say_control "C6  an exclusion naming a header that no longer exists must be REJECTED"
V="$P/include/qb/core/Actor.tpp"
mv "$V" "$V.moved"
if run_gate "${QB_ARGS[@]}"; then ko "a stale exclusion was accepted -- an excluded name can rot"; else
  { echo "$GATE_OUT" | grep -m1 'stale exclusion' | sed 's/^/      /'; } || true
  ok "stale exclusion detected"
fi
mv "$V.moved" "$V"
[ "$(sha "$V")" = "$(sha "$PREFIX/include/qb/core/Actor.tpp")" ] && ok "restored byte-exact" || ko "restore is NOT byte-exact"

# ---- C7 an entry point that links nothing ------------------------------------------------
say_control "C7  an --entry-dir with no TUs must be REJECTED (a vacuous link phase)"
mkdir -p "$WORK/empty-entries"
if run_gate --tree qb:140 --package qb --link qb::core --entry-dir "$WORK/empty-entries"; then
  ko "an empty entry dir was accepted -- phase 2 can silently disappear"
else
  { echo "$GATE_OUT" | grep -m1 -E 'no \*\.cpp|prove nothing' | sed 's/^/      /'; } || true
  ok "empty entry dir rejected"
fi

printf '\n=== controls: %d passed, %d failed, %d not applicable on this compiler ===\n' "$pass" "$fail" "$na"
[ "$na" -eq 0 ] || echo "NOTE: ${na} control(s) could not run here -- that coverage must come from another leg."
[ "$fail" -eq 0 ] || { echo "::error::check-installed-headers.sh self-test: ${fail} control(s) did not fire"; exit 1; }
