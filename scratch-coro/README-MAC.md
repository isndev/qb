# macOS witness for the converged coroutine-teardown design (QB-1246, Finding 2.B.8) -- pilot, 2026-11-12

A scratch branch, never to merge: run it, send the outputs, delete the tree.

This branch is qb `9053b0c7` (QB-1246 part 2) plus this directory. `mac_witness.sh` builds everything it needs in a
temporary directory from two tars, the generator `gen/mk_cv.py` (the converged prototype with E5, applied to the qb
tar) and the witnesses in `dc/`.

    mkdir -p tars
    git archive 9053b0c7 -o tars/qb-9053b0c7.tar                      # in this qb clone
    git -C <your qev clone> archive 14ce8239 -o "$PWD/tars/qev-14ce8239.tar"   # qev develop, QB-1277
    bash mac_witness.sh "$PWD"                                        # run from scratch-coro/, with tars/ beside gen/ and dc/

Needs cmake, ninja, python3 and Apple clang with AddressSanitizer (LeakSanitizer is not available on arm64: the
witnesses do not depend on it).

What to read (the script prints the expectation after each part):
- part 1, qev standalone, kqueue / poll / select: base `FAIL N watcher(s) still on a loop that is gone` and a
  heap-use-after-free with `stops-first`; E5 `PASS` on the three, and qev's own suite as on the base;
- part 2, qb level: on the base, heap-use-after-free on task_sleep, task_timeout, task_recvfor, tls_task, tmp_task,
  ready_io_late, exit_in_coro, exit_in_nested; on the prototype, none anywhere, and `cv_w poison_control` a
  use-after-poison. If `poison_control` exits 0 with "it read ... and nothing reported it", dyld destroyed the main
  thread's thread-locals AFTER the statics: say so, that is the one result that moves the design.
