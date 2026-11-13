#!/usr/bin/env bash
# SCRATCH (Huly QB-1270 part 4), never to merge: the macOS runner as the oracle for qb's "a TLS read that must write
# first" witness. One machine, two OpenSSL majors (Homebrew openssl@4 -- the lane's -- and openssl@3): the system, then
# qb built against each and its tests run -- the tree as committed here (the candidate), and the same tree with src/ put
# back at the base (the red run of its tests).
#
#   diag/macos-oracle.sh <logs directory> [suite|witness] [base commit]
#
# suite: the candidate's every test target built and the whole suite run per OpenSSL; witness: the witness targets.
set -uo pipefail

out="${1:?logs directory}"
scope="${2:-suite}"
base="${3:-}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
jobs="$(sysctl -n hw.logicalcpu)"
p3="$(brew --prefix openssl@3)"
p4="$(brew --prefix openssl@4)"
witnesses=(qb-io-test-system-tls-read-wants-write qb-io-test-system-tls-close-notify qb-io-test-system-ssl-socket-loopback)

section() { echo; echo "=================== $* ($(date -u +%H:%M:%S))"; }

section "the machine"
{
  sw_vers
  uname -a
  sysctl -n machdep.cpu.brand_string hw.logicalcpu
  sysctl kern.ipc.maxsockbuf net.inet.tcp.sendspace net.inet.tcp.recvspace net.inet.tcp.autorcvbufmax net.inet.tcp.autosndbufmax 2>/dev/null
  clang++ --version | head -1
  brew list --versions openssl@3 openssl@4
  for p in "$p3" "$p4"; do "$p/bin/openssl" version; done
  git log --oneline -3
  [ -n "$base" ] && git diff --stat "$base" HEAD -- src tests
} 2>&1 | tee "$out/env.txt"

# $1 tree, $2 openssl major, $3 label, $4 suite|witness
build_and_run() {
  local tree="$1" v="$2" label="$3" what="$4" p b
  eval p="\$p$v"
  b="$PWD/build-$label-ossl$v"
  section "$label against openssl@$v ($what)"
  # The lane's own environment names openssl@4 (CMAKE_PREFIX_PATH, PKG_CONFIG_PATH): each build names its own, alone.
  env -u CMAKE_PREFIX_PATH -u PKG_CONFIG_PATH -u OPENSSL_EXECUTABLE PKG_CONFIG_PATH="$p/lib/pkgconfig" \
    cmake -S "$tree" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DQB_CXX_STANDARD=20 -DQB_BUILD_TESTS=ON -DQB_BUILD_EXAMPLES=OFF -DQB_BUILD_BENCHMARKS=OFF \
      -DQB_USE_SYSTEM_GTEST=ON -DQB_WITH_SSL=ON -DQB_REQUIRE_FEATURES=ON -DQB_WITH_QUIC=OFF -DQB_ENABLE_NATIVE_ARCH=OFF \
      -DOPENSSL_ROOT_DIR="$p" -DOPENSSL_EXECUTABLE="$p/bin/openssl" \
      -DCMAKE_PREFIX_PATH="$p;$(brew --prefix);$(brew --prefix zlib)" > "$out/configure-$label-$v.log" 2>&1 \
    || { echo "configure failed"; tail -40 "$out/configure-$label-$v.log"; return; }
  grep -E 'Found OpenSSL' "$out/configure-$label-$v.log"
  if [ "$what" = suite ]; then
    cmake --build "$b" --parallel "$jobs" > "$out/build-$label-$v.log" 2>&1
  else
    cmake --build "$b" --parallel "$jobs" --target "${witnesses[@]}" > "$out/build-$label-$v.log" 2>&1
  fi || { echo "build failed"; grep -E 'error|FAILED' "$out/build-$label-$v.log" | head -40; return; }
  echo "warnings in the build: $(grep -c 'warning:' "$out/build-$label-$v.log")"
  otool -L "$b/bin/tests/qb-io-test-system-tls-read-wants-write" | grep -i ssl
  for w in "${witnesses[@]}"; do
    "$b/bin/tests/$w" --gtest_color=no > "$out/$w-$label-$v.txt" 2>&1
    echo "$w: exit $?"
    grep -E '^\[  (PASSED|FAILED) |^\[==========\] [0-9]+ tests' "$out/$w-$label-$v.txt"
    grep -E '^\[  FAILED  \] [A-Za-z]+\.[A-Za-z]+ \(|MEASURED|PROBE|OPENSSL \]' "$out/$w-$label-$v.txt" | cut -c1-300 | head -60
  done
  "$b/bin/tests/qb-io-test-system-tls-read-wants-write" --gtest_color=no --gtest_repeat=5 > "$out/witness-repeat-$label-$v.txt" 2>&1
  echo "the witness five times: exit $?; $(grep -c '^\[  PASSED  \]' "$out/witness-repeat-$label-$v.txt") passed runs"
  grep -E '^\[  FAILED  \] TlsReadWantsWrite\.[A-Za-z]+ \(' "$out/witness-repeat-$label-$v.txt" | sed 's/ (.*//' | sort | uniq -c
  if [ "$what" = suite ]; then
    (cd "$b" && ctest --output-on-failure --parallel 4 --output-junit "$out/ctest-$label-$v.xml" > "$out/ctest-$label-$v.log" 2>&1)
    echo "ctest exit $?"
    tail -6 "$out/ctest-$label-$v.log"
    python3 - "$out/ctest-$label-$v.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET
cases = list(ET.parse(sys.argv[1]).getroot().iter('testcase'))
skipped = [c.get('name') for c in cases if c.find('skipped') is not None or c.get('status') == 'notrun']
failed = [c.get('name') for c in cases if c.find('failure') is not None or c.find('error') is not None]
print(f'registered {len(cases)} executed {len(cases) - len(skipped)} skipped {len(skipped)} failed {len(failed)}')
for name in skipped:
    print('  skipped', name)
for name in failed:
    print('  failed', name)
PY
  fi
}

if [ -n "$base" ]; then
  rm -rf ../qb-base && cp -R . ../qb-base && (cd ../qb-base && git checkout -q "$base" -- src && git status --short | head -5)
  for v in 4 3; do build_and_run "$PWD/../qb-base" "$v" base witness; done
fi
for v in 4 3; do build_and_run "$PWD" "$v" candidate "$scope"; done
section "done"
