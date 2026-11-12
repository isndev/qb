#!/usr/bin/env bash
# mac_witness.sh <dir with the confirmation's files> -- the witnesses the Mac can run (kqueue, dyld): E5 at the loop on
# the standalone qev, then the qb-level witnesses on the base and on the converged prototype. Scratch only: it builds in
# a temporary directory, commits nothing. Needs: cmake, ninja, python3, a clang with AddressSanitizer.
#   the directory holds: tars/qb-9053b0c7.tar tars/qev-14ce8239.tar gen/mk_cv.py dc/*.cpp dc/CMakeLists.txt
set -uo pipefail
S=${1:?the directory holding tars/, gen/ and dc/}
W=$(mktemp -d); echo "work in $W"
say() { printf '%s\n' "$*"; }

# ---- 1. E5 at the loop, kqueue ---------------------------------------------------------------------------------------
for t in base e5; do
  mkdir -p "$W/qev-$t/src" && tar -xf "$S/tars/qev-14ce8239.tar" -C "$W/qev-$t/src"
done
mkdir -p "$W/qb-e5" && tar -xf "$S/tars/qb-9053b0c7.tar" -C "$W/qb-e5" && python3 "$S/gen/mk_cv.py" "$W/qb-e5" --variant E > /dev/null
cp "$W/qb-e5/src/qb/ev/ev.c" "$W/qev-e5/src/ev.c"
for t in base e5; do
  B="$W/qev-$t/bld"
  cmake -S "$W/qev-$t/src" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" > "$B.configure.log" 2>&1 && cmake --build "$B" > "$B.build.log" 2>&1 \
    || { say "qev $t: build FAILED ($B.build.log)"; continue; }
  (cd "$B" && ctest --output-on-failure > ../ctest.log 2>&1); say "qev $t: its own suite: $(grep -E 'tests passed|tests failed' "$W/qev-$t/ctest.log")"
  c++ -std=c++17 -g -fsanitize=address -DE5_QEV_STANDALONE -I"$B/include" "$S/dc/e5_ev.cpp" "$(ls "$B"/libqev*.a | head -1)" -o "$B/e5_ev" 2> "$B.e5.log" \
    || { say "qev $t: e5_ev does not build ($B.e5.log)"; continue; }
  for b in kqueue poll select; do
    out=$("$B/e5_ev" $b 2>&1); rc=$?
    say "qev $t: e5_ev $b exit $rc -- $(printf '%s\n' "$out" | grep -E '^\[e5\] (asked|PASS|FAIL [0-9]+ watcher)' | tr '\n' ' ')"
  done
  out=$("$B/e5_ev" kqueue stops-first 2>&1); rc=$?
  say "qev $t: e5_ev kqueue stops-first exit $rc -- $(printf '%s\n' "$out" | grep -m1 -oE 'ERROR: AddressSanitizer: [a-z-]+|\[e5\] PASS.*')"
done
say "EXPECTED: base -- FAIL N watcher(s) still on a loop that is gone, and heap-use-after-free with stops-first;"
say "          e5   -- PASS on kqueue, poll and select, and its own suite as on the base."

# ---- 2. the qb-level witnesses -----------------------------------------------------------------------------------------
for t in base cv; do
  mkdir -p "$W/qb-$t" && tar -xf "$S/tars/qb-9053b0c7.tar" -C "$W/qb-$t"
  printf '\nif(DEFINED DC_WITNESS_DIR)\n    add_subdirectory(${DC_WITNESS_DIR} ${CMAKE_BINARY_DIR}/dc)\nendif()\n' >> "$W/qb-$t/CMakeLists.txt"
  [ $t = cv ] && python3 "$S/gen/mk_cv.py" "$W/qb-$t" --variant E > /dev/null
  B="$W/qb-$t/bld"
  cmake -S "$W/qb-$t" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DQB_SANITIZE=address -DQB_BUILD_TESTS=OFF -DQB_BUILD_EXAMPLES=OFF \
        -DQB_INSTALL=OFF -DDC_WITNESS_DIR="$S/dc" > "$B.configure.log" 2>&1 \
    && cmake --build "$B" --target dc_exit dc_probe ch_w cv_w > "$B.build.log" 2>&1 || { say "qb $t: build FAILED ($B.build.log)"; continue; }
  run() { # exe case...
    local exe=$1; shift
    out=$(ASAN_OPTIONS=detect_leaks=0 "$B/bin/$exe" "$@" 2>&1); rc=$?
    say "qb $t: $exe $* exit $rc -- $(printf '%s\n' "$out" | grep -m1 -oE 'ERROR: AddressSanitizer: [a-z-]+' || echo no-asan-report) $(printf '%s\n' "$out" | grep -c ABANDONED) abandoned line(s), $(printf '%s\n' "$out" | grep -c '^qb: a coroutine asked for') named stop(s)"
  }
  for c in chan_task task_sleep task_timeout task_recvfor sched_sleep io_obj tls_task tls_chan run_late; do run dc_exit $c; done
  for c in tmp_sched tmp_task ready_late ready_io_late reent_pump exit_in_coro late_finish late_yield late_coro_scheduler; do run ch_w $c; done
  for c in reent_forever exit_in_nested exit_in_parent exit_static_any run_late_bound xthread_exit poison_control poison_threads; do run cv_w $c; done
  for f in sleep timeout when_any recv mutex; do for m in S D G H; do run dc_probe $f $m; done; done
done
say "EXPECTED on the Mac, if dyld destroys the main thread's thread-locals before the statics (as read in dyld-1378):"
say "  base: heap-use-after-free on task_sleep, task_timeout, task_recvfor, tls_task, tmp_task, ready_io_late, exit_in_coro, exit_in_nested;"
say "  cv:   no heap-use-after-free anywhere; poison_control IS a use-after-poison (the order and the lane guard, both);"
say "        run_late, late_yield, run_late_bound a use-after-poison in listener::loop (a named stop without -fsanitize);"
say "        late_coro_scheduler one named stop; mode D of dc_probe abandons with the reason AFTER its thread's event loop."
say "If poison_control exits 0 with 'it read ... and nothing reported it', the thread-local outlived the statics on this"
say "platform: the record's transition would need to move, not the design."
