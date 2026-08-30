# Performance tuning

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.1.0 (C++20 default, C++23 supported)

A method-driven guide to the knobs that affect throughput and latency in a qb actor system: core placement, event-loop idle latency, allocation behavior, the lock-free transport, and the two build flags (`QB_ENABLE_NATIVE_ARCH`, `QB_ENABLE_LTO`) that change codegen.

**Prerequisites:** [Engine: `qb::Main` and `VirtualCore`](./../4_qb_core/engine.md), [Event messaging](./../4_qb_core/messaging.md) — **See also:** [CMake options](./../7_reference/cmake_dependencies.md), [Building](./../7_reference/building.md)

## Summary

qb is configured for low overhead by default. Most of the levers below trade one resource for another — CPU for latency, memory for fewer reallocations, portability for host-specific codegen — so none of them is universally correct. Measure first: a profiler tells you which actor, core, or path is hot; this page tells you which knob moves it. The framework does not publish performance numbers, and neither should you without your own benchmark on your own hardware.

The tuning surface splits into three layers:

- **Engine configuration** — how many `VirtualCore`s run, which physical CPUs they pin to, and how long an idle core waits before parking. Set once, before `Main::start()`.
- **Messaging and allocation** — how events move between actors and how the per-pipe buffers grow. Per-call decisions in your handlers.
- **Build flags** — codegen targeting (`QB_ENABLE_NATIVE_ARCH`) and cross-module inlining (`QB_ENABLE_LTO`). Set at configure time.

## Concepts

### The threading model

`qb::Main` spawns exactly one worker thread (`std::jthread`) per `VirtualCore` it launches. A core is launched only if it has a `CoreInitializer` — that is, only if you placed at least one actor on it (via `Main::addActor<T>(core_id, …)`) or touched it through `Main::core(core_id)`. The set of cores that will launch is returned by `Main::usedCoreSet()`.

A registered core with zero actors fails to start: the worker logs `... Started with 0 Actor` (prefixed with its `VirtualCore(<index>).id(<thread>)` identity) and reports `VirtualCore::Error::NoActor`. Touch `Main::core(n)` only for cores you intend to populate.

Each `VirtualCore` owns its actors exclusively and runs them on its single thread. An actor never executes on two threads, so there is no lock around actor state. The cost model that follows from this:

- **Same-core messaging** is a local pipe write — no atomics, no contention.
- **Cross-core messaging** crosses a lock-free MPSC mailbox — efficient, but more expensive than same-core. Co-locate actors that talk frequently.
- **A blocking call inside a handler stalls every actor on that core.** This is the single most common performance defect; see [Pitfalls](#pitfalls).

### Core placement

Place an actor with `Main::addActor<T>(core_id, args…)`, or fluently across one core with `Main::core(core_id).builder()`. Placement is a topology decision: actors that exchange many messages belong on the same core to keep traffic off the cross-core mailbox; an actor that is a throughput bottleneck can be sharded into several instances spread across cores.

```cpp
// src: examples/07-applications/01-taskmanager/src/main.cpp
#include <qb/main.h>
#include <cstdint>
#include <vector>

qb::Main engine;

// Shard the worker pool across cores 1-4, collecting their ids first.
std::vector<qb::ActorId> worker_ids;
worker_ids.reserve(num_workers);
for (uint32_t i = 0; i < num_workers; ++i) {
    const uint32_t core = 1 + (i % 4);
    worker_ids.push_back(
        engine.addActor<actors::TaskManager>(core, pg_uri, redis_uri, static_root));
}

// Co-locate the listener alone on a dedicated accept core, handing it the
// populated worker-id list. Add it last so worker_ids is already filled.
engine.addActor<actors::TcpListener>(/*core=*/0, listen_uri, worker_ids);
```

### CPU affinity

`CoreInitializer::setAffinity(CoreIdSet const&)` pins a `VirtualCore`'s thread to a set of physical CPUs. Pinning prevents OS thread migration, which preserves L1/L2 cache residency for a hot, CPU-bound actor.

```cpp
CoreInitializer &setAffinity(CoreIdSet const &cores = {}) noexcept;  // Main.h
```

Verified semantics:

- The argument is a `qb::CoreIdSet` (alias of `CoreIdBitSet`, a `std::bitset<MaxCores>` of CPU ids).
- An **empty set** leaves scheduling to the OS.
- `qb::NoAffinity` (`== std::numeric_limits<CoreId>::max()`) is a sentinel meaning "no pinning." Any `CoreId >= qb::MaxCores` — including `NoAffinity` — is silently filtered out when the set is built, so `qb::CoreIdSet{qb::NoAffinity}` is well-defined and issues no pinning call rather than throwing. `qb::MaxCores` is 256.

```cpp
// src: qb/src/qb/core/Main.h (NoAffinity doc examples)
#include <qb/main.h>

qb::Main engine;

// Pin core 1's worker thread to physical CPU 2.
engine.core(1).setAffinity(qb::CoreIdSet{2});

// Explicitly let the OS schedule core 0 (NoAffinity is filtered out, yielding an empty set).
engine.core(0).setAffinity(qb::CoreIdSet{qb::NoAffinity});
```

Affinity is a sharp tool. Over-subscribing one physical CPU with several pinned `VirtualCore`s, or pinning every worker to the same core, degrades throughput. Pin only the cores a profiler shows are migration-sensitive, and leave the rest to the OS.

### Event-loop idle latency

When a `VirtualCore` finds no events to process, it can either keep checking (busy-spin) or park on a condition variable for a bounded time. This is governed by `setLatency`, a `qb::duration` (an alias for `std::chrono::nanoseconds`):

```cpp
CoreInitializer &setLatency(qb::duration latency = qb::duration::zero()) noexcept;  // per-core, Main.h
void             setLatency(qb::duration latency = qb::duration::zero());            // all cores, Main.h
```

The per-core form (`engine.core(n).setLatency(…)`) configures one `VirtualCore`. The `Main::setLatency(…)` form writes the same latency to every core that is *already registered* at the time of the call — it iterates the existing initializers and overwrites each one's latency unconditionally (`Main.cpp`), so it does **not** preserve per-core values set earlier. Apply the engine-wide default first, then override individual cores; a later `Main::setLatency(…)` clobbers every per-core value, and a core registered after the call keeps `qb::duration::zero()`. Both forms default to `qb::duration::zero()`.

Verified behavior, from the mailbox `wait()` implementation (`Main.h`):

- **`qb::duration::zero()` (default)** — low-latency mode. The idle core does not park; it stays on the lock-free fast path, polling for events. Lowest event-pickup latency, highest CPU use (a core can sit near 100% on its CPU when idle). Suited to a dedicated hot loop — for example, an accept core.
- **`latency > 0`** — when idle, the core first burns an adaptive spin credit (refilled on any busy iteration), and once that credit is exhausted it parks on a `std::condition_variable` (`_cv.wait_for(lk, _latency)` in the mailbox `wait()`) for up to `latency`, or until a producer enqueues an event and calls `notify()`. This drops idle CPU sharply, at the cost of a worst-case event-pickup delay bounded by `latency`. A producer that exhausts its send-retry budget wakes the destination's mailbox (`VirtualCore.cpp`), so a parked consumer is not left waiting the full interval under back-pressure.

The mixed topology — a zero-latency hot loop plus parked workers — is the common production shape:

```cpp
// src: examples/07-applications/01-taskmanager/src/main.cpp
#include <qb/main.h>

qb::Main engine;

// Accept core: zero latency = minimal accept-to-dispatch delay.
engine.core(0).setLatency(qb::duration::zero());

// Worker cores: 500 µs idle window — balances CPU against response time.
// setLatency is idempotent; calling it again on the same core is safe.
engine.core(1).setLatency(std::chrono::nanoseconds(500'000));
```

The `qb::time_literals` inline namespace re-exports the standard chrono literals, so the duration above can also be written `500us` once those literals are in scope (`#include <qb/system/time.h>`; `using namespace qb::time_literals;`).

There is no published latency-versus-CPU table. Pick a starting value from your tail-latency budget (a server tolerating single-digit milliseconds can park for hundreds of microseconds to milliseconds; a tight control loop uses zero), then measure idle CPU and pickup latency under realistic load.

> All affinity, latency, and placement calls must happen **before** `Main::start()`. Once the engine is running, `Main::core(id)` throws `std::runtime_error` ("Cannot access to CoreInitializers while engine is running").

### Event-loop backend selection

Each `listener` (one per thread/`VirtualCore`) runs a libev loop whose backend is auto-selected (`EVFLAG_AUTO`). The vendored libev recommends the best scalable backend per platform, falling back to `select` only as a last resort:

| Platform | auto-selected backend | last-resort fallback |
|----------|-----------------------|----------------------|
| Linux    | `epoll`               | `select`             |
| macOS    | `kqueue`              | `select`             |
| Windows  | `wepoll` (epoll on IOCP) | `select`          |

`select` is never the default: it is O(number of fds) per poll and capped at `FD_SETSIZE` (~1024), so it cannot scale to a real connection count. `epoll`/`kqueue`/`wepoll` report only the *ready* fds, staying flat as idle connections grow.

For testing, benchmarking, or debugging a backend-specific issue, force a backend with the `QB_EV_BACKEND` environment variable (read once when the thread's `listener` is constructed):

```sh
QB_EV_BACKEND=epoll   ./my_app      # select | poll | epoll | kqueue | iouring | linuxaio | auto
```

Selection is safe: an unknown name, a backend not compiled in, or one that fails to initialise at runtime (e.g. `io_uring` blocked by a container's seccomp policy) all degrade to `auto` with a one-line log notice — never a crash. `io_uring` and `linuxaio` are compiled on Linux but **not** auto-selected (they are not faster than `epoll` for readiness-driven loops); force them explicitly to evaluate them.

To find the best backend on a given machine, run the cross-backend stress benchmark — it sweeps every compiled backend in one run and prints a side-by-side comparison (build with `-DQB_BUILD_BENCHMARKS=ON`):

```sh
./qb-io-bench-ev-backends                     # all available backends
# On a native Linux host (or: docker run --security-opt seccomp=unconfined) io_uring joins the sweep.
```

The same `QB_EV_BACKEND` values can be wired into CI test coverage via the opt-in CMake matrix `-DQB_IO_EV_TEST_BACKENDS="select;poll;…"` (see `tests/io/system/CMakeLists.txt`).

### Messaging cost

The mechanics and the ordering contract are owned by [Event messaging](./../4_qb_core/messaging.md); this section is only the performance lens.

- **`push<Event>(dest, …)`** is the default. It is ordered per source/destination pair and correctly handles non-trivially-destructible payloads. Prefer it.
- **`send<Event>(dest, …)`** drops the ordering guarantee. Reserve it for the narrow case where order genuinely does not matter and the event is trivially destructible. The `qb::trivial_event` concept (`Actor.h`) is exactly `event_type<T> && std::is_trivially_destructible_v<T>`. The potential gain is marginal and the failure mode (silent reordering) is hard to debug — do not reach for it speculatively.
- **`reply(Event&)`** and **`forward(ActorId, Event&)`** reuse the event object already in hand. For request/response and delegation, they avoid a fresh allocation, construction, and copy. Prefer them over constructing a new event.
- **`EventBuilder`** (`actor.to(dest).push<…>(…).push<…>(…)`) chains several sends to the same destination, resolving the destination pipe once.

### Allocation behavior

Each source-to-destination channel is a `qb::VirtualPipe` (an alias of `qb::allocator::pipe<EventBucket>`) — a growable byte buffer that allocates at both front and back and **doubles its capacity** when it runs out of room. Two consequences for hot paths:

1. **Keep events small and move large data by handle.** An event is a data carrier; embed a `std::shared_ptr<T>` or `std::unique_ptr<T>` for a large buffer so only the pointer is copied into the pipe, not the payload. Use `qb::string<N>` (an inline, heap-free fixed-capacity string that truncates silently on overflow) for short strings instead of `std::string`.

2. **Pre-size the pipe for large in-pipe events with `allocated_push`.** When the event's *effective in-pipe size* is large, reserve the exact space up front to avoid the doubling reallocations and copies that `push` would trigger:

   ```cpp
   [[nodiscard]] _Event &allocated_push(std::size_t size, _Args &&...args) const noexcept;  // Pipe.h
   ```

   ```cpp
   // src: qb/src/qb/core/Pipe.h (allocated_push doc example)
   // getPipe returns a qb::Pipe by value; hold it by value, not by reference.
   qb::Pipe pipe = getPipe(dest);
   // `size` is the TRAILING bytes reserved AFTER the event object, not the total:
   // allocated_push adds sizeof(LargeDataEvent) itself, then rounds up to whole buckets.
   const std::size_t trailing_bytes = extra_in_pipe_bytes; // 0 when the payload is heap-owned
   auto &ev = pipe.allocated_push<LargeDataEvent>(trailing_bytes, payload);
   ```

   Pass only the bytes you intend to write *past* the event object. Writing `sizeof(LargeDataEvent) + n` double-counts the event, wasting one event's worth of pipe space per message and halving the usable cross-core size ceiling (see below). When the payload lives behind a smart pointer, the in-pipe size is just the event — pass `0`. The `size` argument is a reservation, not a hard cap.

### Lock-free transport

The cross-core path is built from lock-free primitives, not mutexes:

- Each core's inbound **mailbox is a multi-producer / single-consumer (MPSC) lock-free ring buffer** (`qb::lockfree::mpsc::ringbuffer`). Every other core is a producer; the owning core is the sole consumer. The optional `std::condition_variable` is used only when `latency > 0` — for parking, not for mutual exclusion.
- Concurrently written fields are padded to a cache line (`QB_LOCKFREE_CACHELINE_BYTES`) to avoid false sharing.

You generally do not touch these types directly — the engine routes events through them — but two facts inform tuning. First, cross-core delivery is genuinely lock-free on the fast path, which is why a zero-latency core can poll without contending. Second, the standalone lock-free building blocks (`qb::lockfree::spsc::ringbuffer`, `qb::lockfree::mpsc_unbounded_queue`, `qb::lockfree::SpinLock`) are available if you need a wait-free hand-off *inside* a single actor's owned state. Note that `SpinLock` is a busy-wait TTAS lock for very short critical sections, not a general-purpose mutex.

### Build flags

Both flags are CMake configure-time options, owned by [CMake options](./../7_reference/cmake_dependencies.md). Their performance role:

| Option | Default | Effect |
| --- | --- | --- |
| `QB_ENABLE_NATIVE_ARCH` | `OFF` | Tunes codegen for the build-host CPU: tries `-march=native`, falls back to `-mcpu=native` (older Apple Clang on arm64), then to the compiler default; `/arch:AVX2` on MSVC. |
| `QB_ENABLE_LTO` | `OFF` | Link-Time Optimization: `-flto` on GCC/Clang, `/GL` + `/LTCG` on MSVC. Enables cross-translation-unit inlining at the cost of longer link times. |

`QB_ENABLE_NATIVE_ARCH=ON` produces a **non-portable binary** tuned for the machine that built it. The binary may use instructions absent on an older CPU and fault there. It is **`OFF` by default** precisely for that reason, so you have to ask for it: `-DQB_ENABLE_NATIVE_ARCH=ON`, or the `release-native` / `benchmarks` presets. With it off (the default), qb selects a conservative baseline target (`-march=x86-64` on x86-64, generic `armv8-a` on non-Apple ARM64; Apple Silicon keeps its native target either way).

`QB_ENABLE_LTO=OFF` by default. Enabling it can improve runtime performance through whole-program inlining; verify the gain against your own benchmark, because it lengthens builds and the benefit is workload-dependent.

```bash
# Portable Release build with LTO, no host-specific codegen.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DQB_ENABLE_NATIVE_ARCH=OFF -DQB_ENABLE_LTO=ON
cmake --build build
```

Both flags apply to the Release configuration. A Debug build is not a meaningful performance baseline — always benchmark a Release (or RelWithDebInfo) build.

## Pitfalls

- **Blocking inside a handler stalls the whole core.** A long computation, a synchronous I/O call, or a wait on a mutex or condition variable inside `on(Event&)` or `on(qb::LoopEvent const&)` freezes every actor on that `VirtualCore`. Offload to `qb::io::async::callback` for short non-CPU-bound waits, or to a dedicated worker actor (on another core) that does the blocking work and sends a result event back.
- **Configuring after `start()`.** Affinity, latency, and placement are pre-start only. `Main::core(id)` throws once the engine is running.
- **Touching a core you do not populate.** `Main::core(n)` registers core `n`; if no actor lands there, that worker fails to start, logging `... Started with 0 Actor` and reporting `VirtualCore::Error::NoActor`. Configure latency and affinity inside the same loop that places actors.
- **Reaching for `send` before `push`.** The ordering guarantee `push` gives is cheap; the bugs `send` introduces are not. Use `send` only when order is provably irrelevant, and prefer a `qb::trivial_event` payload — the compiler requires one only when the event derives from `qb::EventQOS0`.
- **Sizing `allocated_push` to the payload, not the in-pipe footprint.** When the payload is behind a smart pointer, the pipe holds only the pointer — a large hint wastes the reservation it was meant to save.
- **Shipping a `QB_ENABLE_NATIVE_ARCH=ON` binary to other machines.** A host-tuned build can fault with an illegal instruction on an older CPU. The default is off, so this only bites when you (or a `release-native` / `benchmarks` preset) turned it on and then shipped the result.
- **Quoting numbers you did not measure.** qb publishes no throughput or latency figures. Every tuning decision here is directional; the magnitude is yours to benchmark, in Release, on your target hardware, under realistic load. Use a system profiler (`perf`, Instruments, VTune) to find the hot core or actor, and `qb::ScopedTimer` / `qb::LogTimer` (`qb/system/time.h`) to time critical sections.

## See also

- [Engine: `qb::Main` and `VirtualCore`](./../4_qb_core/engine.md) — full lifecycle, core configuration, and signal handling.
- [Event messaging](./../4_qb_core/messaging.md) — the authoritative contract for `push`, `send`, `reply`, `forward`, and pipes.
- [CMake options](./../7_reference/cmake_dependencies.md) and [Building](./../7_reference/building.md) — every build flag, including `QB_ENABLE_NATIVE_ARCH` and `QB_ENABLE_LTO`.
- [Error handling and resilience](./error_handling.md) and [Resource management](./resource_management.md) — companion guides.
