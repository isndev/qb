# macOS witness for the converged close design (QB-1270 item 3) -- pilot, 2026-11-12

A scratch branch, never to merge: run it, send the outputs, delete the tree.

## 1. The kernel (two minutes, no build)

    python3 scratch-close/tcp_close_witness.py | tee mac-tcp-close.txt

Rows to compare with Linux and Windows: K1, K2, K8, K10 (is LAST lost?), K6 (does close() block?), K3 (how many
octets before the writer blocks). XNU's source says macOS behaves like Linux (queued data handed out before the reset).

## 2. The qb-io witnesses on kqueue (the converged prototype)

Everything is on the scratch branch `p2/qb-1270-close-converged-mac` of isndev/qb (one commit on 50ed9117, QB-1278's
tip): the converged design in `src/qb/io/async/` (`lingering_close.{h,cpp}`, `teardown.h`, `io.h`) and
`src/qb/io/tcp/ssl/socket.{h,cpp}`, the six witnesses under `tests/`, registered at the end of
`tests/io/system/CMakeLists.txt` and `tests/core/system/CMakeLists.txt`.

    git fetch origin p2/qb-1270-close-converged-mac
    git worktree add --detach ../qb-close FETCH_HEAD

Then qb standalone, Release, `QB_TESTS_WERROR=ON`: build, run the six witnesses one at a time (each in its own process),
then the whole qb-io system tier (`ctest -L tier:system -L module:qb-io`). Report registered / executed / skipped.

Rows to read first: C2, C8, C9 (a second watcher on a descriptor number whose first watcher was stopped in the same
loop pass -- the kqueue question), F3b (what the pipe holds on macOS: the byte budget is 16 MiB), F5 (our unsent
octets delivered before the close), W1-W9, S1-S6.

3. Do the same witnesses on the unpatched 50ed9117 (base) if time allows: base reds are expected (W1, W2, W7, W9...).

Delete the trees afterwards; send the outputs (attach them to QB-1143).
