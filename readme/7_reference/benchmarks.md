<!-- Verified-against: qb 3.0.0 (C++20 default, C++23 supported) -->
# Benchmarks

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 3.0.0 (C++20 default, C++23 supported)

A reference for the qb micro-benchmark suites: the Google Benchmark targets gated by `QB_BUILD_BENCHMARKS`, what each one measures, how to build and run them, and how to read their output.

**Prerequisites:** [Building from source](./building.md) — **See also:** [CMake options reference](./cmake_options.md), [CMake and dependencies](./cmake_dependencies.md), [Testing](./testing.md), [Core invariants](./core_invariants.md)

The benchmarks are micro-benchmarks of the actor engine and the qb-io stack. They exist to capture and compare local performance baselines while changing the framework — not to publish numbers. This page documents the target surface, the build switch, and the run procedure; it does not quote results. Build-time options (`QB_BUILD_BENCHMARKS`, `QB_USE_SYSTEM_BENCHMARK`, `QB_GOOGLEBENCHMARK_GIT_TAG`) are owned by [cmake_options.md](./cmake_options.md); Google Benchmark dependency resolution is owned by [cmake_dependencies.md](./cmake_dependencies.md). This page links to those rather than restating them.

## Concepts

The suite uses [Google Benchmark](https://github.com/google/benchmark) (pinned to tag `v1.9.2` by default via `QB_GOOGLEBENCHMARK_GIT_TAG`; `qb/cmake/qbConfig.cmake:105`). Each benchmark source is a separate executable with its own `BENCHMARK_MAIN()` entry point, so there is no aggregate runner — you run one binary per pattern.

Two properties distinguish the benchmark targets from the test targets:

- **They are not CTest targets.** `ctest` never runs them. They build only when `QB_BUILD_BENCHMARKS=ON` and must be launched manually. (`qb/tests/core/CMakeLists.txt:37-38`; `qb/cmake/qbFunctions.cmake:724-727`.)
- **They live in their own output directory.** Every benchmark executable is written to `<build>/bin/benchmarks/`, not next to the test binaries. (`qb/cmake/qbFunctions.cmake:799-802`.)

The sources are organized into topic subgroups under each library's `tests/benchmark/` directory, with the source named for the subject it measures (`<subgroup>/<name>.cpp`). The build derives each target name by prepending `<module>-bench-`, so `messaging/ping-pong-throughput.cpp` produces the executable `qb-core-bench-ping-pong-throughput`. (`qb/tests/core/benchmark/CMakeLists.txt:31-43`.)

### How a target is created

The CMake helper `qb_add_benchmark` builds the executable, applies the shared compiler/property set, and links Google Benchmark — preferring the imported `benchmark::benchmark` target and falling back to a plain `benchmark` target. It returns early (building nothing) when `QB_BUILD_BENCHMARKS` is off. Each directory wraps it in a small per-suite helper (`qbc_bench`/`qbio_bench`) that fixes the `<module>-bench-<name>` naming and the IDE folder. (`qb/cmake/qbFunctions.cmake:713-836`.)

```cmake
# src: qb/tests/core/benchmark/CMakeLists.txt:32-43
function(qbc_bench SUBGROUP NAME)
    qb_add_benchmark(
        NAME    qb-core-bench-${NAME}
        SOURCES ${SUBGROUP}/${NAME}.cpp
        DEPENDS ${PROJECT_NAME}
        MODULE  qb-core)
endfunction()
```

### Core-count cap and build-type scaling

Two compile-time details shape what a benchmark actually exercises:

- **Worker-core range is capped.** Benchmarks that sweep the scheduler core count call `qb::bench::cappedBenchmarkCores()`, which returns `min(effectiveHardwareCores(), kMaxBenchmarkCores)` — the constant `kMaxBenchmarkCores` is `8`. When `hardware_concurrency()` reports `0`, `effectiveHardwareCores()` falls back to `1`. (`qb/tests/core/shared/BenchmarkCores.h:39,42-50`.)
- **Debug builds run a smaller problem.** Several benchmarks gate their iteration counts and event-count shifts on `NDEBUG`, so a `Debug` configuration runs far fewer iterations than a `Release` one. For example, `messaging/ping-pong-throughput.cpp` uses `MAX_BENCHMARK_ITERATION = 10` and `SHIFT_NB_EVENT = 15` under `NDEBUG`, versus `1` and `4` otherwise. (`qb/tests/core/benchmark/messaging/ping-pong-throughput.cpp:48-53`.) Measure in a `Release` (or `RelWithDebInfo`) build; a `Debug` benchmark number is not a performance baseline.

### Timing policy

Every core benchmark declares `UseRealTime()`, because the work runs on `VirtualCore` worker threads rather than the benchmark thread, so wall time is the meaningful measure. Almost all of them also wrap actor-graph construction in `PauseTiming()`/`ResumeTiming()` so that only the engine run is timed. Where the benchmarks differ is whether engine startup is inside the timed region:

- **Startup is timed.** Most throughput benchmarks resume timing before calling the blocking `main.start(true)`, so both startup and the drain (`join()`) fall inside the measurement. (`qb/tests/core/benchmark/messaging/fan-in-contention.cpp:106-114`.)
- **Startup is excluded.** `messaging/ping-pong-throughput.cpp` calls the non-blocking `main.start()` while timing is paused, resumes timing, and measures only the drain phase (`main.join()`). (`qb/tests/core/benchmark/messaging/ping-pong-throughput.cpp:176-183`.)

The two regions are not directly comparable — compare results only across benchmarks that time the same region. (`qb/tests/core/benchmark/messaging/ping-pong-throughput.cpp:30-33`.)

Cross-thread latency benchmarks cannot use a `thread_local` sink: actors write on a worker thread while the benchmark thread reads after `join()`. The shared, mutex-guarded sink in `BenchmarkIterationSink.h` (`record_last_latency` / `last_latency_stats_snapshot`) bridges that gap. (`qb/tests/core/shared/BenchmarkIterationSink.h`.)

## The qb-core benchmark targets

The sources live under `qb/tests/core/benchmark/`, grouped into five subgroups (`micro/`, `system/`, `messaging/`, `topology/`, `patterns/`) and registered in that directory's `CMakeLists.txt`. The executable name is `qb-core-bench-<name>`.

### `micro/` — primitive throughput (no `qb::Main`)

| Source | Executable suffix | Measures |
|---|---|---|
| `micro/mpsc-mailbox-fanin.cpp` | `mpsc-mailbox-fanin` | Lock-free MPSC mailbox fan-in sweep over `qb::lockfree::mpsc::ringbuffer`, mirroring the engine's per-producer SPSC routing — without `qb::Main`. |
| `micro/mpsc-router-dispatch.cpp` | `mpsc-router-dispatch` | MPSC mailbox ingress plus `qb::router::memh` typed dispatch, modeling a single consumer core draining mailboxes. |
| `micro/spinlock-contention.cpp` | `spinlock-contention` | `qb::lockfree::SpinLock` uncontended acquire/release latency and N-thread contended throughput. |
| `micro/jsonb-dump.cpp` | `jsonb-dump` | `qb::json` / `qb::jsonb` serialize (`dump()`) and parse throughput on an HTTP-shaped payload. |
| `micro/parse-numbers.cpp` | `parse-numbers` | Numeric string-to-value parse throughput: `qb::to_number` versus `std::from_chars` versus libc. |

### `system/` — engine bring-up, push allocator, scheduler

| Source | Executable suffix | Measures |
|---|---|---|
| `system/push-allocated-bigmsg.cpp` | `push-allocated-bigmsg` | Large one-way messages: `Actor::push` versus `Pipe::allocated_push`, sweeping the pre-allocation hint. |
| `system/callback-burst-throughput.cpp` | `callback-burst-throughput` | One-way throughput: an `ICallback` producer sending in bursts versus one message per tick. |
| `system/engine-lifecycle.cpp` | `engine-lifecycle` | Engine lifecycle cost: construct → `start(true)` → `join()` of a near-empty `qb::Main`. |
| `system/actor-spawn-throughput.cpp` | `actor-spawn-throughput` | Actor registration and first-frame throughput, plus `KillEvent` broadcast-kill teardown. |
| `system/coroutine-spawn-latency.cpp` | `coroutine-spawn-latency` | Actor `spawn_detached` throughput and suspend/resume latency on the live event loop. |

### `messaging/` — actor-to-actor delivery topologies and `ask` round-trips

| Source | Executable suffix | Measures |
|---|---|---|
| `messaging/fan-in-contention.cpp` | `fan-in-contention` | Many producers into one consumer (single-mailbox write contention). |
| `messaging/producer-consumer-throughput.cpp` | `producer-consumer-throughput` | Single producer / single consumer one-way `push` throughput, mono-core and cross-core. |
| `messaging/broadcast-vs-explicit-fanout.cpp` | `broadcast-vs-explicit-fanout` | `BroadcastId(core)` versus explicit per-actor `push`, with matched total deliveries. |
| `messaging/payload-size-throughput.cpp` | `payload-size-throughput` | Ping-pong throughput as a function of padded event size (`ExtraWords` trailing `uint64_t` words). |
| `messaging/messaging-api-oneway.cpp` | `messaging-api-oneway` | One-way throughput across the send primitives: `push` versus `send` versus `getPipe().push` versus `to().push`. |
| `messaging/core-distance-pingpong.cpp` | `core-distance-pingpong` | Ping-pong throughput as a function of `(ping_core, pong_core)` placement, isolating cross-core cost. |
| `messaging/ping-pong-throughput.cpp` | `ping-pong-throughput` | Ping-pong throughput across `TinyEvent` / `BigEvent` (and heap-owning / checksum) payload shapes, swept over actor count, round-trips, and core count. |
| `messaging/ping-pong-latency.cpp` | `ping-pong-latency` | Per-round-trip ping-pong latency (mono-core, cross-core, and a raw-SPSC reference path). |
| `messaging/forward-vs-direct.cpp` | `forward-vs-direct` | Direct ping-pong versus a one-hop `forward` relay at the same logical TTL. |
| `messaging/ask-roundtrip.cpp` | `ask-roundtrip` | Native coroutine `qb::ask` request/response round-trip latency (same-core and cross-core). |
| `messaging/ask-all-fanout.cpp` | `ask-all-fanout` | Scatter-gather fan-out latency: `qb::ask_all` (await every reply) versus `qb::ask_any` (first wins). |

### `topology/` — chain, fan-out, and diamond latency

| Source | Executable suffix | Measures |
|---|---|---|
| `topology/pipeline-chain-latency.cpp` | `pipeline-chain-latency` | Producer-to-consumer pipeline-chain latency, where each event walks the full chain. |
| `topology/multicast-latency.cpp` | `multicast-latency` | Multicast latency — one event fans out to every consumer once. |
| `topology/topology-zoo.cpp` | `topology-zoo` | The diamond topology plus shared-core placement variants. |

### `patterns/` — pub/sub, saga, and resilience overhead

| Source | Executable suffix | Measures |
|---|---|---|
| `patterns/pubsub-dispatch.cpp` | `pubsub-dispatch` | Topic publish fan-out throughput: `qb::PubSub<Topic>` to N same-core subscribers. |
| `patterns/saga-orchestration.cpp` | `saga-orchestration` | `qb::run_saga` multi-step orchestration overhead against the same steps as raw `qb::ask`s. |
| `patterns/resilience-overhead.cpp` | `resilience-overhead` | Per-call overhead of `qb::CircuitBreaker` and `qb::rate_limiter` (no actor engine — the guards take the clock as a parameter). |

<!-- src: qb/tests/core/benchmark/CMakeLists.txt:45-80 -->

## Building the benchmarks

Configure with `QB_BUILD_BENCHMARKS=ON` in a `Release` build, then build the benchmark targets. Google Benchmark is resolved at configure time: by default qb uses a system package if `find_package` finds one, otherwise it builds the pinned tag from source via `FetchContent` (a from-source fallback needs network access on the first configure). Force a preinstalled package with `QB_USE_SYSTEM_BENCHMARK=ON`. See [cmake_dependencies.md](./cmake_dependencies.md).

```bash
# src: qb/INSTALL.md (build invocation)
cmake -DCMAKE_BUILD_TYPE=Release -DQB_BUILD_BENCHMARKS=ON -B build
cmake --build build --parallel
```

The repository-root `CMakeLists.txt` sets `QB_BUILD_BENCHMARKS=ON` (`qb-dev/CMakeLists.txt:39`) — but **without** `FORCE`, unlike the tests and examples lines around it, so a preset that already put the variable in the cache wins. The superproject's `dev` preset does exactly that: it inherits `debug` → `base`, and `base` sets `QB_BUILD_BENCHMARKS=OFF` (`qb-dev/CMakePresets.json:21`), adding nothing of its own. **`cmake --preset dev` from the repository root therefore does not build the benchmarks** — use the `benchmarks` preset, which is `release` plus `-march=native` (`qb-dev/CMakePresets.json:128-135`), or pass the flag explicitly. In a *standalone* `qb` checkout the qb-only `dev` preset does enable them (`qb/CMakePresets.json:45-51`).

```bash
# src: qb-dev/CMakePresets.json (benchmarks preset)
cmake --preset benchmarks
cmake --build --preset benchmarks --parallel
```

To build a single benchmark instead of the whole suite, name its target:

```bash
cmake --build build --target qb-core-bench-ping-pong-throughput
```

## Running the benchmarks

Each benchmark is a standalone Google Benchmark executable under `<build>/bin/benchmarks/`. Run one directly:

```bash
./build/bin/benchmarks/qb-core-bench-ping-pong-throughput
```

The binaries accept the standard Google Benchmark command-line flags. Common ones:

```bash
# Run only the benchmarks whose name matches a regex
./build/bin/benchmarks/qb-core-bench-ping-pong-throughput --benchmark_filter='BigEvent'

# Repeat each benchmark and report aggregate statistics
./build/bin/benchmarks/qb-core-bench-fan-in-contention --benchmark_repetitions=10

# Emit machine-readable output for archiving or diffing baselines
./build/bin/benchmarks/qb-core-bench-payload-size-throughput \
    --benchmark_format=json --benchmark_out=payload-baseline.json

# List the registered cases without running them
./build/bin/benchmarks/qb-core-bench-core-distance-pingpong --benchmark_list_tests
```

Refer to the Google Benchmark documentation for the full flag set (`--help` on any benchmark binary prints it).

### Reading the output

Targets that report rates expose Google Benchmark counters. For example, `messaging/ping-pong-throughput.cpp` records `round_trips_per_s` and `messages_per_s` as iteration-invariant rate counters, alongside the actual actor counts for the run. (`qb/tests/core/benchmark/messaging/ping-pong-throughput.cpp:188-189`.) A counter declared with `benchmark::Counter::kIsIterationInvariantRate` is multiplied by the total iteration count and divided by the elapsed time, so it reports throughput regardless of how many iterations Google Benchmark chose.

<!-- TODO(verify): this page deliberately quotes no measured throughput, latency, or speedup figures. Add numbers only from a reproducible run on named hardware, with the build type, core count, and Google Benchmark version recorded. -->

## The qb-io benchmark suite

`qb-io` ships its own Google Benchmark suite under `qb/tests/io/benchmark/`, following the same conventions as the core suite: each `<subgroup>/<name>.cpp` source is a standalone Google Benchmark program (`#include <benchmark/benchmark.h>`, `BENCHMARK(...)` registrations, `BENCHMARK_MAIN()`), built by `qb_add_benchmark` into a `qb-io-bench-<name>` target under `<build>/bin/benchmarks/`. The directory is gated by `QB_BUILD_BENCHMARKS` exactly like the core suite — the parent `CMakeLists.txt` wraps `add_subdirectory(benchmark)` in `if (QB_BUILD_BENCHMARKS)`, so nothing builds when the switch is off. Like the core benchmarks, these are not CTest targets and must be launched manually. Three subgroups carry a `REQUIRES ssl` / `REQUIRES compression` compile-gate, so they register only when `QB_HAS_SSL` / `QB_HAS_COMPRESSION` is set. (`qb/tests/io/CMakeLists.txt:37-38`; `qb/tests/io/benchmark/CMakeLists.txt:24-34`; `qb/cmake/qbFunctions.cmake:713-836`.)

| Source | Executable suffix | Measures |
|---|---|---|
| `async/async-hotpaths.cpp` | `async-hotpaths` | The qb-io async event-loop hot paths (callback register/unregister, async-callback fire, scoped-callback construct/cancel, broadcast scratch reuse). |
| `async/ev-backends.cpp` | `ev-backends` | Cross-backend libev stress — finds the best event-loop backend on the host. |
| `async/timer-dispatch.cpp` | `timer-dispatch` | The async timer / callback dispatch machinery. |
| `coroutine/coroutine-scheduler-throughput.cpp` | `coroutine-scheduler-throughput` | The coroutine scheduler, frame, `co_await`, and timer workloads. |
| `coroutine/coroutine-scope.cpp` | `coroutine-scope` | Structured-concurrency fan-out primitives (scopes). |
| `coroutine/coroutine-pipeline.cpp` | `coroutine-pipeline` | Coroutine pipelines — channels, streams, generators. |
| `crypto/crypto-primitives.cpp` | `crypto-primitives` | OpenSSL-backed crypto helpers (hashing, HMAC, AEAD). Built only when `QB_HAS_SSL` (`REQUIRES ssl`). |
| `crypto/crypto-extras.cpp` | `crypto-extras` | Crypto KDFs, asymmetric signatures, and JWT. Built only when `QB_HAS_SSL` (`REQUIRES ssl`). |
| `compression/compress-codecs.cpp` | `compress-codecs` | Compression providers and pipe adapters. Built only when `QB_HAS_COMPRESSION` (`REQUIRES compression`). |
| `protocol/framing-scanners.cpp` | `framing-scanners` | Protocol framing and message-boundary scanning primitives. |
| `serialization/json-pipe-serialize.cpp` | `json-pipe-serialize` | JSON pipe serialization workloads. |
| `uri/uri-parse-encode.cpp` | `uri-parse-encode` | `qb::io::uri` parsing, encoding, and normalization. |
| `io/pipe-buffer-throughput.cpp` | `pipe-buffer-throughput` | The `qb::allocator::pipe<char>` I/O buffer used by transports and protocols. |
| `io/file-stream.cpp` | `file-stream` | File helpers and file-backed streams. |
| `session/session-json.cpp` | `session-json` | JSON session round-trips over loopback TCP and TLS. Built only when `QB_HAS_SSL` (`REQUIRES ssl`). |
| `transport/async-bases-framing.cpp` | `async-bases-framing` | The read→frame→`onMessage`→drain loop of the async I/O bases. |
| `transport/tcp-loopback-echo.cpp` | `tcp-loopback-echo` | Plain-TCP loopback echo round-trip throughput (no TLS, daemon-free). |
| `coroutine/sync-primitives.cpp` | `sync-primitives` | The qb-io coroutine synchronization primitives (`semaphore`, `async_mutex`, `async_rw_lock`, `async_latch`). |

<!-- src: qb/tests/io/benchmark/CMakeLists.txt:36-53 -->

Build and run them the same way as the core benchmarks — each is a standard Google Benchmark binary that accepts the usual flags:

```bash
./build/bin/benchmarks/qb-io-bench-async-hotpaths
```

## Pitfalls

- **Benchmark numbers from a `Debug` build are not baselines.** Several benchmarks shrink their iteration counts and event sizes when `NDEBUG` is not defined. Configure `Release` (or `RelWithDebInfo`) before measuring. (`qb/tests/core/benchmark/messaging/ping-pong-throughput.cpp:48-53`.)
- **`ctest` will not run these.** The benchmark targets are intentionally excluded from CTest. Launch the executables in `<build>/bin/benchmarks/` directly. (`qb/tests/core/CMakeLists.txt:37-38`.)
- **The worker-core sweep tops out at eight.** `cappedBenchmarkCores()` clamps the swept core count to `min(effectiveHardwareCores(), kMaxBenchmarkCores)` with `kMaxBenchmarkCores == 8`; a benchmark will not exercise more than eight scheduler cores even on a larger machine. (`qb/tests/core/shared/BenchmarkCores.h:39,48-49`.)
- **Do not compare across timing regions.** Benchmarks that time the blocking `start(true)` plus the drain and benchmarks (such as `messaging/ping-pong-throughput.cpp`) that time only the drain (`join()`) measure different regions; their absolute numbers are not interchangeable. (`qb/tests/core/benchmark/messaging/ping-pong-throughput.cpp:176-183`; `qb/tests/core/benchmark/messaging/fan-in-contention.cpp:106-114`.)
- **Google Benchmark must be resolvable.** When `QB_USE_SYSTEM_BENCHMARK=ON` and no system package exists, configuration fails with a `find_package(benchmark CONFIG REQUIRED)` error; with the default fetch fallback, the first configure needs network access to clone the pinned tag. See [cmake_dependencies.md](./cmake_dependencies.md).

## See also

- [Building from source](./building.md) — configure, build, and preset reference.
- [CMake options reference](./cmake_options.md) — the full `QB_*` option catalog, including the benchmark switches.
- [CMake and dependencies](./cmake_dependencies.md) — how Google Benchmark, GoogleTest, and zlib are resolved.
- [Testing](./testing.md) — the CTest-registered unit and system test suites.
- [Core invariants](./core_invariants.md) — the engine semantics the messaging benchmarks exercise.
