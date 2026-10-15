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
 * @brief Actor coroutine spawn/resume throughput and complete child-token lifecycle cost.
 *
 * Three actor-level benchmark families; the first two use the `spawn_detached(lambda → task<void>)`
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
 *   - `BM_ChildTokenChurn` — on the benchmark thread's real actor, create/drop 64 or 1024
 *     child tokens per iteration, optionally cancelling each. Parent cancellation and destruction
 *     stay inside every iteration, including any child history the parent retained.
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
 *   - every timed run checks its delivery count and watchdog after pausing timing; a failed run
 *     produces no nominal-work rate. Descriptor counters are assigned once after a valid loop;
 *   - a one-shot, out-of-loop probe runs one full fan-out and asserts every coroutine delivered and
 *     the watchdog never fired, so a coroutine that never resumes is caught before timing.
 * Child-token churn instead owns the only State loop inside a synchronous onInit first drive:
 * timing starts after engine setup and ends before engine teardown. Every iteration checks its
 * checksum and cancellation counts; the final engine/work/destruction guard runs outside timing.
 *
 * New bench (no predecessor in the flat `benchmark/` set); pairs with the coroutine system suite.
 */

#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
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
        state.PauseTiming();
        if (g_watchdog_fired.load(std::memory_order_relaxed) || g_delivered.load(std::memory_order_relaxed) != count) {
            state.SkipWithError("not all coroutines delivered (watchdog fired or count mismatch)");
            return;
        }
        state.ResumeTiming();
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

namespace child_token_churn_bench {

struct Observation {
    std::thread::id benchmark_thread = std::this_thread::get_id();
    std::uint64_t   iterations       = 0;
    std::uint64_t   children         = 0;
    std::uint64_t   child_callbacks  = 0;
    std::uint64_t   parent_callbacks = 0;
    std::uint64_t   last_checksum    = 0;
    unsigned        destroyed        = 0;
    bool            entered          = false;
    bool            failed           = false;
    bool            rejected         = false;
};

class ChildTokenChurnActor final : public qb::Actor {
    benchmark::State   &_state;
    Observation        &_observed;
    const std::uint64_t _count;
    const bool          _cancel_children;

public:
    ChildTokenChurnActor(benchmark::State &state, Observation &observed, std::uint64_t count, bool cancel_children)
        : _state(state)
        , _observed(observed)
        , _count(count)
        , _cancel_children(cancel_children) {}

    ~ChildTokenChurnActor() final {
        ++_observed.destroyed;
    }

    qb::io::async::task<bool>
    onInit() final {
        if (std::this_thread::get_id() != _observed.benchmark_thread) {
            _observed.failed = true;
            co_return false; // never access State from another thread
        }
        _observed.entered            = true;
        const auto expected_checksum = _count * (_count + 1) / 2;
        // This is the only State loop. start(false) keeps it on the benchmark thread;
        // engine setup precedes StartKeepRunning, engine teardown follows FinishKeepRunning.
        for (auto _ : _state) {
            std::uint64_t created          = 0;
            std::uint64_t checksum         = 0;
            std::uint64_t child_callbacks  = 0;
            std::uint64_t parent_callbacks = 0;
            bool          valid            = true;
            {
                qb::io::async::cancellation_token parent;
                const qb::ScopedCoroContext       ctx(this, parent);
                parent.on_cancel([&parent_callbacks] { ++parent_callbacks; });
                for (std::uint64_t i = 0; i < _count; ++i) {
                    auto child = ctx.child_token();
                    ++created;
                    valid = valid && static_cast<bool>(child) && !child.is_cancelled();
                    checksum += static_cast<bool>(child) ? i + 1 : 0;
                    if (_cancel_children) {
                        child.on_cancel([&child_callbacks] { ++child_callbacks; });
                        child.cancel();
                        valid = valid && child.is_cancelled();
                    }
                    benchmark::DoNotOptimize(child);
                } // every public child handle is dropped before parent cancellation
                parent.cancel();
                valid = valid && parent.is_cancelled() && parent_callbacks == 1;
            } // include context, parent and all retained child-history destruction
            benchmark::DoNotOptimize(checksum);
            benchmark::DoNotOptimize(created);
            benchmark::DoNotOptimize(child_callbacks);
            benchmark::DoNotOptimize(parent_callbacks);
            if (!valid || created != _count || checksum != expected_checksum || child_callbacks != (_cancel_children ? _count : 0)) {
                _observed.failed = true;
                _state.SkipWithError("child-token lifecycle count, checksum or cancellation mismatch");
                _observed.rejected = true;
                break;
            }
            ++_observed.iterations;
            _observed.children += created;
            _observed.child_callbacks += child_callbacks;
            _observed.parent_callbacks += parent_callbacks;
            _observed.last_checksum = checksum;
        }
        kill();
        co_return true;
    }
};

void
BM_ChildTokenChurn(benchmark::State &state) {
    const auto  count           = static_cast<std::uint64_t>(state.range(0));
    const bool  cancel_children = state.range(1) != 0;
    Observation observed;
    {
        qb::Main main;
        main.addActor<ChildTokenChurnActor>(0, std::ref(state), std::ref(observed), count, cancel_children);
        main.start(false);
        main.join();
        if (main.hasError()) {
            if (!observed.rejected)
                state.SkipWithError("child-token actor failed to initialize or complete");
            return;
        }
    }
    if (observed.failed) {
        if (!observed.rejected)
            state.SkipWithError("child-token actor did not run on the benchmark thread");
        return;
    }
    if (!observed.entered || observed.destroyed != 1 || observed.iterations == 0
        || observed.iterations != static_cast<std::uint64_t>(state.iterations())
        || observed.iterations > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / count
        || observed.children != observed.iterations * count || observed.parent_callbacks != observed.iterations
        || observed.child_callbacks != (cancel_children ? observed.children : 0)) {
        state.SkipWithError("child-token actor did not complete exactly the requested lifecycle work");
        return;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(observed.children));
    state.counters["children"]                       = static_cast<double>(count);
    state.counters["cancel_children"]                = cancel_children ? 1.0 : 0.0;
    state.counters["checksum"]                       = static_cast<double>(observed.last_checksum);
    state.counters["child_callbacks_per_iteration"]  = cancel_children ? static_cast<double>(count) : 0.0;
    state.counters["parent_callbacks_per_iteration"] = 1.0;
}

} // namespace child_token_churn_bench

BENCHMARK(BM_Coroutine_SpawnThroughput)->Arg(100)->Arg(1000)->Arg(5000)->ArgName("coroutines")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Coroutine_SuspendResumeLatency)
    ->Arg(100)
    ->Arg(500)
    ->Arg(2000)
    ->ArgName("coroutines")
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

BENCHMARK(child_token_churn_bench::BM_ChildTokenChurn)
    ->Args({64, 0})
    ->Args({1024, 0})
    ->Args({64, 1})
    ->Args({1024, 1})
    ->ArgNames({"children", "cancel_children"})
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

BENCHMARK_MAIN();
