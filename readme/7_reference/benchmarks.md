<!-- Verified-against: qb 2.0.0 (C++20 default, C++23 supported) -->
# Benchmarks

> **Audience:** Contributor · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

A reference for the qb-core micro-benchmark suite: the Google Benchmark targets gated by `QB_BUILD_BENCHMARKS`, what each one measures, how to build and run them, and how to read their output.

**Prerequisites:** [Building from source](./building.md) — **See also:** [CMake options reference](./cmake_options.md), [CMake and dependencies](./cmake_dependencies.md), [Testing](./testing.md), [Core invariants](./core_invariants.md)

The benchmarks are micro-benchmarks of the actor engine's messaging and throughput paths. They exist to capture and compare local performance baselines while changing the engine — not to publish numbers. This page documents the target surface, the build switch, and the run procedure; it does not quote results. Build-time options (`QB_BUILD_BENCHMARKS`, `QB_USE_SYSTEM_BENCHMARK`, `QB_GOOGLEBENCHMARK_GIT_TAG`) are owned by [cmake_options.md](./cmake_options.md); Google Benchmark dependency resolution is owned by [cmake_dependencies.md](./cmake_dependencies.md). This page links to those rather than restating them.

## Concepts

The suite uses [Google Benchmark](https://github.com/google/benchmark) (pinned to tag `v1.9.2` by default via `QB_GOOGLEBENCHMARK_GIT_TAG`; `qb/cmake/qbConfig.cmake:78`). Each benchmark source is a separate executable with its own `BENCHMARK_MAIN()` entry point, so there is no aggregate runner — you run one binary per pattern.

Two properties distinguish the benchmark targets from the test targets:

- **They are not CTest targets.** `ctest` never runs them. They build only when `QB_BUILD_BENCHMARKS=ON` and must be launched manually. (`qb/source/core/tests/CMakeLists.txt:33-35`; `qb/cmake/qbFunctions.cmake:361-363`.)
- **They live in their own output directory.** Every benchmark executable is written to `<build>/bin/benchmarks/`, not next to the test binaries. (`qb/cmake/qbFunctions.cmake:403-406`.)

The sources reside in `qb/source/core/tests/benchmark/`, named `bm-<pattern>.cpp`. The build derives each target name by stripping the `bm-` prefix and prepending `<project>-benchmark-`, so `bm-ping-pong.cpp` produces the executable `qb-core-benchmark-ping-pong`. (`qb/source/core/tests/benchmark/CMakeLists.txt:64-83`.)

### How a target is created

The CMake helper `qb_add_benchmark` builds the executable, applies the shared compiler/property set, and links Google Benchmark — preferring the imported `benchmark::benchmark` target and falling back to a plain `benchmark` target. It returns early (building nothing) when `QB_BUILD_BENCHMARKS` is off. (`qb/cmake/qbFunctions.cmake:348-424`.)

```cmake
# src: qb/source/core/tests/benchmark/CMakeLists.txt
qb_add_benchmark(
        NAME    qb-core-benchmark-ping-pong
        SOURCES bm-ping-pong.cpp
        DEPENDS qb-core
)
```

### Core-count cap and build-type scaling

Two compile-time details shape what a benchmark actually exercises:

- **Worker-core range is capped.** Benchmarks that sweep the scheduler core count call `qb::bench::cappedBenchmarkCores()`, which returns `min(hardware_concurrency, 8)` — the constant `kMaxBenchmarkCores` is `8`. With `hardware_concurrency()` reporting `0`, it falls back to `1`. (`qb/source/core/tests/shared/BenchmarkIterationSink.h:36-47`.)
- **Debug builds run a smaller problem.** Several benchmarks gate their iteration counts and event-count shifts on `NDEBUG`, so a `Debug` configuration runs far fewer iterations than a `Release` one. For example, `bm-ping-pong.cpp` uses `MAX_BENCHMARK_ITERATION = 10` and `SHIFT_NB_EVENT = 15` under `NDEBUG`, versus `1` and `4` otherwise. (`qb/source/core/tests/benchmark/bm-ping-pong.cpp:42-48`.) Measure in a `Release` (or `RelWithDebInfo`) build; a `Debug` benchmark number is not a performance baseline.

### Timing policy

Every core benchmark declares `UseRealTime()`, because the work runs on `VirtualCore` worker threads rather than the benchmark thread, so wall time is the meaningful measure. Almost all of them also wrap actor-graph construction in `PauseTiming()`/`ResumeTiming()` so that only the engine run is timed. Where the benchmarks differ is whether engine startup is inside the timed region:

- **Startup is timed.** Most throughput benchmarks resume timing before calling the blocking `main.start(true)`, so both startup and the drain (`join()`) fall inside the measurement. (`qb/source/core/tests/benchmark/bm-fan-in.cpp:90-113`.)
- **Startup is excluded.** `bm-ping-pong.cpp` calls the non-blocking `main.start()` while timing is paused, resumes timing, and measures only the drain phase (`main.join()`). (`qb/source/core/tests/benchmark/bm-ping-pong.cpp:138-155`.)

The two regions are not directly comparable — compare results only across benchmarks that time the same region. (`qb/source/core/tests/benchmark/bm-ping-pong.cpp:18-23`.)

Cross-thread latency benchmarks cannot use a `thread_local` sink: actors write on a worker thread while the benchmark thread reads after `join()`. The shared, mutex-guarded sink in `BenchmarkIterationSink.h` (`record_last_latency` / `last_latency_stats_snapshot`) bridges that gap. (`qb/source/core/tests/shared/BenchmarkIterationSink.h:1-8,57-74`.)

## The benchmark targets

All sixteen sources live in `qb/source/core/tests/benchmark/` and are registered in that directory's `CMakeLists.txt` under two categories. The executable name is `qb-core-benchmark-<pattern>`.

### Messaging patterns

| Source | Executable suffix | Measures |
|---|---|---|
| `bm-ping-pong.cpp` | `ping-pong` | Ping-pong throughput across `TinyEvent` / `BigEvent` / `DynamicEvent`, swept over actor count, round-trips, and core count. |
| `bm-ping-pong-latency.cpp` | `ping-pong-latency` | Per-round-trip ping-pong latency, with an SPSC reference path for comparison. |

### Throughput patterns

| Source | Executable suffix | Measures |
|---|---|---|
| `bm-allocated-push.cpp` | `allocated-push` | Large one-way messages: `Actor::push` versus `Pipe::allocated_push` (with a pre-allocation hint) on a ~1 KiB payload. |
| `bm-broadcast-fanout.cpp` | `broadcast-fanout` | `BroadcastId(core)` versus explicit per-actor `push`, with matched total deliveries. |
| `bm-core-distance.cpp` | `core-distance` | Ping-pong throughput as a function of `(ping_core, pong_core)` placement, isolating cross-core cost. |
| `bm-disruptor-latency.cpp` | `disruptor-latency` | Throughput and latency across unicast and other actor communication topologies. |
| `bm-fan-in.cpp` | `fan-in` | Many producers into one consumer (mailbox contention). |
| `bm-forward-reply.cpp` | `forward-reply` | Direct ping-pong versus a one-hop `forward` relay at the same logical TTL. |
| `bm-messaging-api.cpp` | `messaging-api` | One-way throughput across the send primitives: `push` versus `send` versus `getPipe().push` versus `to().push`. |
| `bm-multicast-latency.cpp` | `multicast-latency` | Latency of multicast delivery from one producer to many subscribers. |
| `bm-mpsc-mailbox-sweep.cpp` | `mpsc-mailbox-sweep` | Lock-free MPSC mailbox sweep over `qb::lockfree::mpsc::ringbuffer`, mirroring the engine's per-producer SPSC routing. |
| `bm-mpsc-router-mailbox.cpp` | `mpsc-router-mailbox` | MPSC `EventBucket` ingress plus `router::memh` dispatch, modeling a single consumer core draining mailboxes. |
| `bm-payload-throughput.cpp` | `payload-throughput` | Ping-pong throughput as a function of padded event size (`ExtraWords` trailing `uint64_t` words). |
| `bm-pipeline-latency.cpp` | `pipeline-latency` | Producer-to-consumer pipeline-chain latency, where each event walks the full chain of `NB_ACTORS`. |
| `bm-producer-burst.cpp` | `producer-burst` | One-way throughput: an `ICallback` producer sending in bursts versus one message per tick. |
| `bm-producer-consumer.cpp` | `producer-consumer` | Single producer / single consumer one-way `push` throughput, mono-core and cross-core. |

<!-- src: qb/source/core/tests/benchmark/CMakeLists.txt:31-52 -->

## Building the benchmarks

Configure with `QB_BUILD_BENCHMARKS=ON` in a `Release` build, then build the benchmark targets. Google Benchmark is resolved at configure time: by default qb uses a system package if `find_package` finds one, otherwise it builds the pinned tag from source via `FetchContent` (a from-source fallback needs network access on the first configure). Force a preinstalled package with `QB_USE_SYSTEM_BENCHMARK=ON`. See [cmake_dependencies.md](./cmake_dependencies.md).

```bash
# src: qb/INSTALL.md (build invocation)
cmake -DCMAKE_BUILD_TYPE=Release -DQB_BUILD_BENCHMARKS=ON -B build
cmake --build build --parallel
```

The repository-root `CMakeLists.txt` forces `QB_BUILD_BENCHMARKS=ON` (`qb-dev/CMakeLists.txt:15`), and the `dev` CMake preset enables it as well (`qb/CMakePresets.json`). When building from the repository root or with that preset, the flag is already set:

```bash
# src: qb/CMakePresets.json (dev preset)
cmake --preset dev
cmake --build build/dev --parallel
```

To build a single benchmark instead of the whole suite, name its target:

```bash
cmake --build build --target qb-core-benchmark-ping-pong
```

## Running the benchmarks

Each benchmark is a standalone Google Benchmark executable under `<build>/bin/benchmarks/`. Run one directly:

```bash
./build/bin/benchmarks/qb-core-benchmark-ping-pong
```

The binaries accept the standard Google Benchmark command-line flags. Common ones:

```bash
# Run only the benchmarks whose name matches a regex
./build/bin/benchmarks/qb-core-benchmark-ping-pong --benchmark_filter='BigEvent'

# Repeat each benchmark and report aggregate statistics
./build/bin/benchmarks/qb-core-benchmark-fan-in --benchmark_repetitions=10

# Emit machine-readable output for archiving or diffing baselines
./build/bin/benchmarks/qb-core-benchmark-payload-throughput \
    --benchmark_format=json --benchmark_out=payload-baseline.json

# List the registered cases without running them
./build/bin/benchmarks/qb-core-benchmark-core-distance --benchmark_list_tests
```

Refer to the Google Benchmark documentation for the full flag set (`--help` on any benchmark binary prints it).

### Reading the output

Targets that report rates expose Google Benchmark counters. For example, `bm-ping-pong.cpp` records `round_trips_per_s` and `messages_per_s` as iteration-invariant rate counters, alongside the actual actor counts for the run. (`qb/source/core/tests/benchmark/bm-ping-pong.cpp:157-165`.) A counter declared with `benchmark::Counter::kIsIterationInvariantRate` is multiplied by the total iteration count and divided by the elapsed time, so it reports throughput regardless of how many iterations Google Benchmark chose.

<!-- TODO(verify): this page deliberately quotes no measured throughput, latency, or speedup figures. Add numbers only from a reproducible run on named hardware, with the build type, core count, and Google Benchmark version recorded. -->

## The qb-io micro-benchmark

`qb-io` ships one separate micro-benchmark, `qb/source/io/tests/system/bench-io-plan.cpp`, which measures allocation-heavy hot paths (callback register/unregister, async callback fire, scoped-callback construction, broadcast scratch reuse). It is **not** a Google Benchmark program — it has its own `main()` and emits a plain-text report — and it builds as the `qb-io-bench-io-plan` executable under `<build>/bin/benches/` whenever the test tree is built (`QB_BUILD_TESTS=ON`, the default), independent of `QB_BUILD_BENCHMARKS`. Like the core benchmarks, it is not a CTest target. (`qb/source/io/tests/system/CMakeLists.txt:63-72`; `qb/source/io/CMakeLists.txt:116-118`; `qb/source/io/tests/system/bench-io-plan.cpp:193-224`.)

It accepts two optional flags and writes a report to a file (default `/tmp/qb-io-bench.txt`):

```bash
# src: qb/source/io/tests/system/bench-io-plan.cpp (argument parsing)
./build/bin/benches/qb-io-bench-io-plan --label baseline --out /tmp/qb-io-bench.txt
```

## Pitfalls

- **Benchmark numbers from a `Debug` build are not baselines.** Several benchmarks shrink their iteration counts and event sizes when `NDEBUG` is not defined. Configure `Release` (or `RelWithDebInfo`) before measuring. (`qb/source/core/tests/benchmark/bm-ping-pong.cpp:42-48`.)
- **`ctest` will not run these.** The benchmark targets are intentionally excluded from CTest. Launch the executables in `<build>/bin/benchmarks/` directly. (`qb/source/core/tests/CMakeLists.txt:33-35`.)
- **The worker-core sweep tops out at eight.** `cappedBenchmarkCores()` clamps the swept core count to `min(hardware_concurrency, 8)`; a benchmark will not exercise more than eight scheduler cores even on a larger machine. (`qb/source/core/tests/shared/BenchmarkIterationSink.h:36-47`.)
- **Do not compare across timing regions.** Benchmarks that time the blocking `start(true)` plus the drain and benchmarks (such as `bm-ping-pong.cpp`) that time only the drain (`join()`) measure different regions; their absolute numbers are not interchangeable. (`qb/source/core/tests/benchmark/bm-ping-pong.cpp:138-155`; `qb/source/core/tests/benchmark/bm-fan-in.cpp:90-113`.)
- **Google Benchmark must be resolvable.** When `QB_USE_SYSTEM_BENCHMARK=ON` and no system package exists, configuration fails with a `find_package(benchmark CONFIG REQUIRED)` error; with the default fetch fallback, the first configure needs network access to clone the pinned tag. See [cmake_dependencies.md](./cmake_dependencies.md).

## See also

- [Building from source](./building.md) — configure, build, and preset reference.
- [CMake options reference](./cmake_options.md) — the full `QB_*` option catalog, including the benchmark switches.
- [CMake and dependencies](./cmake_dependencies.md) — how Google Benchmark, GoogleTest, and zlib are resolved.
- [Testing](./testing.md) — the CTest-registered unit and system test suites.
- [Core invariants](./core_invariants.md) — the engine semantics the messaging benchmarks exercise.
