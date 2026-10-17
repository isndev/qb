# IO observability benchmark

`qb-io-bench-observability-overhead` contains four cells in two pairs. Compare within a pair only.
It adds one TU through `qbio_bench(async observability-overhead)`; no runtime option, source or test is changed.

| Family | Cells | Work per batch | Observation contrast |
|---|---|---|---|
| Listener watcher dispatch | `BM_IODispatchTimingOff/On`, `dispatches:4096` | 4096 fed watcher callbacks and NOWAIT turns | `set_dispatch_timing(false/true)` |
| Offload pool snapshot | `BM_IOOffloadStatsUnrequested/Requested`, `jobs:64` | 64 sequential real offloads and resumes | no timed getter / one `current_offload_stats()` consumed after every resume |

Both pairs run both real configurations as untimed preflights, then check every measured batch after timing
stops. The namespace is named for coroutine fixture linkage. All errors are explicit `SkipWithError`; an
unsettled cleanup is also process exit 2. Inspect JSON errors and the roster, not only the process return code.

## Dispatch fixture and exact counters

The fixture arms one raw timer watcher far in the future and feeds EV_CUSTOM before each `listener.run(NOWAIT)`.
This is a real libev dispatch, reusing the raw pending-timer shape in `listener-io-cadence.cpp` and the timing
contract of `listener-dispatch-timing.cpp`. It deliberately does not use `callback(fn,0)` (inline), `defer()` or
coroutine resumes (outside the dispatcher being timed). The fixture stops the watcher and clears its pending
entry before the owning context clears the loop.

The current `ev_run` invokes its dispatcher on entry and at the tail of its single NOWAIT iteration. There
are no prepare/fork watchers, nested runs or deferred work in this fixture, so 4096 calls produce **8192 dispatcher
invocations**, while the callback runs only 4096 times. The timed mode records those invocations, including empty
ones; it is not a count of user callbacks. The oracle separately checks callback count, ordinal order, checksum
and the recorded-span delta. This is a source-derived property of this fixture and current loop, checked by
preflight; do not generalize it to arbitrary async turns or silently change it to callbacks=N.

`init()` is not a reset, and `clear()`/timing-off preserve the recorder. Read before/after snapshots outside
timing. Off requires count/total/last/max unchanged; recent_max may age and is not equated. On requires 8192 new
spans. Feed, NOWAIT dispatch and deadline checks are measured; watcher setup and snapshot getters are excluded.
Raw native watcher callbacks do not pass through the listener's registered-handler counting shim; the explicit
fixture callback counter is the work oracle, not `total_events_processed()`.

## Offload fixture, observation and lifetime

The binary configures one pool worker in main before any offload. A real warmup job creates it and the owner
thread's completion port outside timing. The pool is process-wide, starts once, keeps its counters and threads
between cells, and joins its threads on process exit. It is not recreated or claimed stopped after each batch.
The ordinary lazy/default contract of the library is unchanged; this standalone instrument explicitly selects 1.

The named coroutine submits one pure value-only worker job at a time (return its integer argument), checks
its returned value through an aggregate checksum, and records 64 resumes. In Requested it reads/escapes the full
snapshot and consumes `completed - before.completed`; Unrequested consumes the equivalent local ordinal with
no timed getter. Both observation sums must equal 1+...+64. The measured window includes real job allocation,
queueing, worker wake, completion port, scheduler/pump and the requested read/consumption. **This is end-to-end
incremental cost, not pure getter latency.** There is no requirement that its signal exceed scheduler noise.

`submitted`, `completed` and `discarded` are unconditional relaxed atomic counters. The getter is not a coherent
instant across fields. Here only one job is outstanding and its completion increment precedes the port's mutex
publication and owner resume. No next job is submitted until after the observation. Thus the observation point
is quiescent for this producer; after-batch deltas can require 64 submitted/completed, queued 0 and no new discard.
Two common before/after getter calls are outside timing in both modes; they are not represented as “stats off”.

Result storage outlives the coroutine cleanup guard. Workers capture only copied integers, never stack results
or listener state. On every path, reclaim scheduler frames first while results are alive, drain any finite
abandoned completion while its loop lives, then clear. On timeout/error, inability to settle within the cleanup
budget is fatal to the producer and later cells are refused. No sync bridge or event-loop pump runs inside the
coroutine or on the worker thread. The port's TLS teardown stops its watcher before listener destruction.

## Invariants, validation and scope

Dispatch reports work_units=4096, watcher_callbacks=4096, loop_turns=4096, dispatcher_invocations=8192, recorded_spans
0/8192 and enabled=0/1. Offload reports work_units=64, offloads_completed=64, pool_threads=1, requested_snapshots=0/64
and oracle_snapshots=2. These scalars are emitted only after successful work; item rates use completed work.

Build this target with Release, ASan+UBSan, TSan and coverage, explicitly enabling QB_BUILD_BENCHMARKS where needed.
Run all four cells with `--benchmark_min_time=1x --benchmark_repetitions=1 --benchmark_out=<fresh>.json
--benchmark_out_format=json`; require exactly four nonempty cells and their invariant values, no errors/skips.
Retain an external process watchdog: a library call that never returns cannot check an in-loop deadline.
Owner regression targets are `qb-io-test-unit-listener-dispatch-timing`, `qb-io-test-unit-listener-io-cadence`,
`qb-io-test-system-offload` and `qb-io-test-system-offload-pool`. These benchmarks add no ctest registration.

Negative controls must be planted in private copies and compiled with real producer flags. Cover disabled
timing, omitted callback, wrong returned value, dummy snapshot and a bypassed offload, plus late failures after
healthy preflights. A compiler warning, empty selected cell or external timeout is not a valid caught witness.
The source manifest supplies exact anchors and expected diagnostics; markers for late controls are outside timing.

Tracking/names/dump, HTTP client snapshots and Redis retry hooks remain outside this TU; adding it does not
close the whole QB-248 roster. No time threshold, baseline, calibration or speed claim is included. Collection
follows the admitted-host A/A/copies/layout protocol only after correctness and lifecycle checks.
