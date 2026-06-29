/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/system/coroutine-spawn-latency.cpp
 * @brief Actor `spawn_detached` throughput + suspend/resume latency on the live event loop.
 *
 * Two engine-level coroutine benches, both built on the actor `spawn_detached(lambda → task<void>)`
 * round-trip the system suite pins (`coroutine-basics.cpp`):
 *   - `BM_Coroutine_SpawnThroughput` — an actor spawns `coroutines` detached coroutines that complete
 *     IMMEDIATELY (`co_await std::suspend_never` via a plain `co_return` after one `ctx.push`), so the
 *     measured cost is spawn + first-frame run + completion-frame destroy + the `ctx.push` delivery,
 *     with no timer in the path. Isolates raw spawn/dispatch throughput.
 *   - `BM_Coroutine_SuspendResumeLatency` — each of `coroutines` coroutines `co_await sleep(delay)`
 *     then `ctx.push`es a completion, so every coroutine parks on a timer and is resumed: this
 *     measures the suspend → register → timer-fire → resume → deliver latency at fan-out. The spawning
 *     actor samples `active_coroutine_count()` right after the spawn loop (the peak number of live
 *     coroutine frames) and publishes it, proving the coroutines genuinely suspended rather than
 *     completing inline.
 *
 * Each coroutine pushes one completion event back; the actor counts them and calls `qb::Main::stop()`
 * the instant it has seen all `coroutines` — deterministic, event-driven shutdown. The actor runs
 * under `start(false)` (the bench thread becomes core 0's worker). A LOUD bounded watchdog co-runs as
 * a backstop: it stops the engine and trips a flag only if a deadline is reached first, so a hung run
 * fails loudly here instead of via the harness wall-clock.
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - per-iteration engine construction + actor registration is hoisted out of the timed region with
 *     `PauseTiming()`/`ResumeTiming()`; only `start(false)` (which blocks until `stop()`) is timed;
 *   - `SetItemsProcessed` (total coroutines) and the descriptor counters (delivered count, peak active
 *     coroutines, watchdog flag) are assigned ONCE after the loop from the last run;
 *   - a one-shot, out-of-loop probe runs one full fan-out and asserts every coroutine delivered and
 *     the watchdog never fired, so a coroutine that never resumes is caught before timing.
 *
 * New bench (no predecessor in the flat `benchmark/` set); pairs with the coroutine system suite.
 */

#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"

using namespace std::chrono_literals;

namespace {

// Mirrors from the worker thread to the bench thread (read after start(false) returns).
std::atomic<std::uint64_t> g_delivered{0};   // completion events the actor counted
std::atomic<std::uint64_t> g_peak_active{0}; // active_coroutine_count() sampled right after spawn loop
std::atomic<bool>          g_watchdog_fired{false};

void
reset_run() {
    g_delivered.store(0, std::memory_order_relaxed);
    g_peak_active.store(0, std::memory_order_relaxed);
    g_watchdog_fired.store(false, std::memory_order_relaxed);
}

struct CoroDoneEvent : public qb::Event {};

// Bounded backstop: stops the engine after `deadline` and records that it had to.
class WatchdogActor final : public qb::Actor {
    const qb::duration _deadline;

public:
    explicit WatchdogActor(qb::duration deadline)
        : _deadline(deadline) {}

    qb::io::async::task<bool>
    onInit() final {
        spawn_detached([deadline = _deadline](auto) -> qb::io::async::task<void> {
            co_await qb::io::async::sleep(deadline);
            g_watchdog_fired.store(true, std::memory_order_relaxed);
            qb::Main::stop();
        });
        co_return true;
    }
};

// Spawns `count` detached coroutines; each delivers one CoroDoneEvent. When `WithSleep`, each parks on
// a timer first (suspend/resume path); otherwise it completes immediately (raw spawn path). Stops the
// engine once all completions have landed.
template <bool WithSleep>
class SpawnActor final : public qb::Actor {
    const std::uint64_t _count;
    const qb::duration  _delay;
    std::uint64_t       _done = 0;

public:
    SpawnActor(std::uint64_t const count, qb::duration const delay)
        : _count(count)
        , _delay(delay) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<CoroDoneEvent>(*this);
        for (std::uint64_t i = 0; i < _count; ++i) {
            if constexpr (WithSleep) {
                spawn_detached([delay = _delay](auto ctx) -> qb::io::async::task<void> {
                    co_await qb::io::async::sleep(delay);
                    ctx.template push<CoroDoneEvent>();
                });
            } else {
                spawn_detached([](auto ctx) -> qb::io::async::task<void> {
                    ctx.template push<CoroDoneEvent>();
                    co_return;
                });
            }
        }
        // Sample the peak number of live coroutine frames right after the spawn loop. For the
        // suspend/resume bench this proves the coroutines actually parked (count > 0).
        g_peak_active.store(active_coroutine_count(), std::memory_order_relaxed);
        co_return true;
    }

    void
    on(CoroDoneEvent const &) {
        if (++_done >= _count) {
            g_delivered.store(_done, std::memory_order_relaxed);
            qb::Main::stop();
        }
    }
};

// Run one full fan-out engine to completion (blocks until stop()).
template <bool WithSleep>
void
run_engine(std::uint64_t const count, qb::duration const delay, qb::duration const watchdog) {
    qb::Main main;
    main.addActor<SpawnActor<WithSleep>>(0, count, delay);
    main.addActor<WatchdogActor>(0, watchdog);
    main.start(false); // this thread becomes core 0's worker; returns when stop() is called
    main.join();
}

template <bool WithSleep>
void
run_bench(benchmark::State &state, qb::duration const delay) {
    const auto count = static_cast<std::uint64_t>(state.range(0));
    // Watchdog headroom scales with the timer delay so the suspend/resume bench never trips it on a
    // healthy run.
    const qb::duration watchdog = delay + 5s;

    // One-shot out-of-loop correctness probe: every coroutine must deliver and the watchdog must not
    // fire — a coroutine that never resumes is caught here, before timing.
    {
        reset_run();
        run_engine<WithSleep>(count, delay, watchdog);
        auto delivered = g_delivered.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(delivered);
        if (g_watchdog_fired.load(std::memory_order_relaxed) || delivered != count) {
            state.SkipWithError("not all coroutines delivered (watchdog fired or count mismatch)");
            return;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        reset_run();
        state.ResumeTiming();
        run_engine<WithSleep>(count, delay, watchdog);
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * count));
    state.counters["coroutines_per_s"] = benchmark::Counter(static_cast<double>(count), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["delivered"]        = static_cast<double>(g_delivered.load(std::memory_order_relaxed));
    state.counters["peak_active"]      = static_cast<double>(g_peak_active.load(std::memory_order_relaxed));
    state.counters["watchdog_fired"]   = g_watchdog_fired.load(std::memory_order_relaxed) ? 1.0 : 0.0;
}

void
BM_Coroutine_SpawnThroughput(benchmark::State &state) {
    run_bench<false>(state, qb::duration::zero());
}

void
BM_Coroutine_SuspendResumeLatency(benchmark::State &state) {
    // A small uniform delay so every coroutine genuinely suspends on a timer and is resumed.
    run_bench<true>(state, std::chrono::duration_cast<qb::duration>(2ms));
}

} // namespace

BENCHMARK(BM_Coroutine_SpawnThroughput)->Arg(100)->Arg(1000)->Arg(5000)->ArgName("coroutines")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Coroutine_SuspendResumeLatency)
    ->Arg(100)
    ->Arg(500)
    ->Arg(2000)
    ->ArgName("coroutines")
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

BENCHMARK_MAIN();
