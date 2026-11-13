#!/usr/bin/env bash
# SCRATCH (Huly QB-1270 part 3), never to merge: the macOS runner as the oracle for qb's close_notify witness.
# One machine, two OpenSSL majors (Homebrew openssl@4 -- the lane's -- and openssl@3): the system, the library's
# documentation, a probe of OpenSSL and the kernel alone, then qb built against each and its suite run.
#
#   diag/macos-oracle.sh <logs directory> [suite|witness]
#
# suite: every test target built and the whole suite run per OpenSSL; witness: the witness target alone.
set -uo pipefail

out="${1:?logs directory}"
scope="${2:-suite}"
mkdir -p "$out/docs"
out="$(cd "$out" && pwd)"
jobs="$(sysctl -n hw.logicalcpu)"
p3="$(brew --prefix openssl@3)"
p4="$(brew --prefix openssl@4)"
seven='TlsCloseNotify.ASocketWhosePeerEndedTheSessionAnswersFromDisconnect:TlsCloseNotify.ASocketWhosePeerEndedTheSessionAnswersWithItsOwn:TlsCloseNotify.ASocketThatWroteIntoAResetWritesNothingMore:TlsCloseNotify.ASocketThatReadAResetWritesNothingMore:TlsCloseNotify.AReadErrorTheIoLayerRetriesDoesNotEndTheSession:TlsCloseNotify.AWriteErrorTheIoLayerRetriesDoesNotEndTheSession:TlsCloseNotify.ASocketThatCannotTakeTheAlertIsClosedWithoutIt'

section() { echo; echo "=================== $* ($(date -u +%H:%M:%S))"; }

section "the machine"
{
  sw_vers
  uname -a
  sysctl -n machdep.cpu.brand_string hw.logicalcpu
  sysctl net.inet.tcp.doautosndbuf net.inet.tcp.doautorcvbuf net.inet.tcp.sendspace net.inet.tcp.recvspace \
         net.inet.tcp.autosndbufmax net.inet.tcp.autorcvbufmax net.inet.tcp.autosndbufinc kern.ipc.maxsockbuf 2>&1
  clang++ --version
  cmake --version | head -1
  brew list --versions openssl@3 openssl@4 libngtcp2
  for p in "$p3" "$p4"; do echo "--- $p"; "$p/bin/openssl" version -a; done
} 2>&1 | tee "$out/env.txt"

section "the documentation each OpenSSL installs"
{
  for p in "$p3" "$p4"; do
    echo "--- $p"
    find "$p" -maxdepth 3 -type d | sort
    find "$p" \( -iname 'CHANGES*' -o -iname 'NEWS*' -o -iname 'README*' \) -print
  done
  for page in SSL_shutdown SSL_get_error SSL_read SSL_write SSL_set_shutdown SSL_CTX_set_options SSL_CTX_set_mode BIO_should_retry; do
    for v in 3 4; do
      eval p="\$p$v"
      f="$(ls "$p"/share/man/man3/"$page".3* 2>/dev/null | head -1)"
      if [ -n "$f" ]; then mandoc -T utf8 "$f" 2>/dev/null | col -b > "$out/docs/$page-$v.txt"; else echo "no man page $page for openssl@$v"; fi
    done
    if [ -s "$out/docs/$page-3.txt" ] && [ -s "$out/docs/$page-4.txt" ]; then
      echo "--- $page: openssl@3 -> openssl@4"
      diff -u "$out/docs/$page-3.txt" "$out/docs/$page-4.txt" | tee "$out/docs/$page.diff"
    fi
  done
  for h in ssl.h sslerr.h bio.h tls1.h; do
    diff -u "$p3/include/openssl/$h" "$p4/include/openssl/$h" > "$out/docs/$h.diff"
    echo "--- include/openssl/$h: $(grep -c '^[-+][^-+]' "$out/docs/$h.diff") changed lines"
  done
  echo "--- ssl.h lines of openssl@4 naming SYSCALL, fatal, shutdown or EOF:"
  grep -n -i -E 'SYSCALL|FATAL|SHUTDOWN|EOF' "$p4/include/openssl/ssl.h" | head -80
} 2>&1 | tee "$out/docs.txt"

section "the probe: OpenSSL and the kernel alone"
for v in 4 3; do
  eval p="\$p$v"
  clang++ -std=c++20 -O1 -Wall -Wextra -I"$p/include" diag/tls-macos-probe.cpp -L"$p/lib" -lssl -lcrypto -Wl,-rpath,"$p/lib" \
    -o "$out/probe-$v" 2>&1 | tee "$out/probe-$v-build.txt"
  otool -L "$out/probe-$v" | tee -a "$out/probe-$v-build.txt"
  for run in 1 2; do
    echo "--- openssl@$v, run $run" | tee -a "$out/probe-$v.txt"
    "$out/probe-$v" tests/io/system/resources/ssl/cert.pem tests/io/system/resources/ssl/key.pem 2>&1 | tee -a "$out/probe-$v.txt"
  done
done

for v in 4 3; do
  eval p="\$p$v"
  b="build-ossl$v"
  section "qb against openssl@$v ($scope)"
  # The lane's own environment names openssl@4 (CMAKE_PREFIX_PATH, PKG_CONFIG_PATH): each build names its own, alone.
  env -u CMAKE_PREFIX_PATH -u PKG_CONFIG_PATH -u OPENSSL_EXECUTABLE PKG_CONFIG_PATH="$p/lib/pkgconfig" \
    cmake -S . -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DQB_CXX_STANDARD=20 -DQB_BUILD_TESTS=ON -DQB_BUILD_EXAMPLES=OFF -DQB_BUILD_BENCHMARKS=OFF \
      -DQB_USE_SYSTEM_GTEST=ON -DQB_WITH_SSL=ON -DQB_REQUIRE_FEATURES=ON -DQB_WITH_QUIC=OFF -DQB_ENABLE_NATIVE_ARCH=OFF \
      -DOPENSSL_ROOT_DIR="$p" -DOPENSSL_EXECUTABLE="$p/bin/openssl" \
      -DCMAKE_PREFIX_PATH="$p;$(brew --prefix);$(brew --prefix zlib)" > "$out/configure-$v.log" 2>&1 \
    || { echo "configure failed"; tail -40 "$out/configure-$v.log"; continue; }
  grep -i -E 'openssl|QUIC' "$out/configure-$v.log" | head -12
  if [ "$scope" = suite ]; then
    cmake --build "$b" --parallel "$jobs" > "$out/build-$v.log" 2>&1 || { echo "build failed"; grep -E 'error|FAILED' "$out/build-$v.log" | head -40; continue; }
  else
    cmake --build "$b" --parallel "$jobs" --target qb-io-test-system-tls-close-notify > "$out/build-$v.log" 2>&1 \
      || { echo "build failed"; grep -E 'error|FAILED' "$out/build-$v.log" | head -40; continue; }
  fi
  echo "warnings in the build: $(grep -c 'warning:' "$out/build-$v.log")"
  w="$b/bin/tests/qb-io-test-system-tls-close-notify"
  otool -L "$w"
  section "the witness whole, openssl@$v"
  "$w" --gtest_color=no > "$out/witness-$v.txt" 2>&1
  echo "exit $?"
  grep -E '^\[  (PASSED|FAILED) |^\[==========\]|tests? ran' "$out/witness-$v.txt"
  grep -B2 -A12 -E '^\[ RUN      \] TlsCloseNotify\.(ASocketWhosePeerEnded|ASocketThatWroteIntoAReset|ASocketThatReadAReset|AReadErrorTheIoLayer|AWriteErrorTheIoLayer|ASocketThatCannotTakeTheAlert)' "$out/witness-$v.txt" | head -200
  section "the seven, three times, openssl@$v"
  "$w" --gtest_color=no --gtest_filter="$seven" --gtest_repeat=3 > "$out/witness-seven-$v.txt" 2>&1
  echo "exit $?"
  grep -E '^\[  (PASSED|FAILED) |^\[       OK \]|^\[  FAILED  \] TlsCloseNotify' "$out/witness-seven-$v.txt" | sort | uniq -c
  if [ "$scope" = suite ]; then
    section "the whole suite, openssl@$v"
    (cd "$b" && ctest --output-on-failure --parallel 4 --output-junit "$out/ctest-$v.xml" > "$out/ctest-$v.log" 2>&1)
    echo "ctest exit $?"
    tail -25 "$out/ctest-$v.log"
    python3 - "$out/ctest-$v.xml" <<'PY'
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
done
section "done"
