/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/patterns/resilience-overhead.cpp
 * @brief Per-call overhead of the qb resilience primitives (CircuitBreaker, rate_limiter).
 *
 * `qb::CircuitBreaker` and `qb::rate_limiter` (<qb/core/patterns/resilience.h>) are passive,
 * single-thread state machines driven by an injected `VirtualCore` timestamp (`now_ns`) — the
 * `allow/on_success/on_failure` and `try_acquire` probes are exactly the CPU tax every guarded call
 * pays *around* the actual work. Because they take the clock as a parameter, they benchmark cleanly
 * WITHOUT an actor engine: this micro prices the guard math itself (closed happy path, open
 * fail-fast, token-bucket refill) so you know what wrapping an `ask` in resilience actually costs.
 *
 * Methodology (perf harness, never a ctest gate): the injected clock is a monotonically advancing
 * `uint64_t` (no wall-clock in the timed region); every probe result is folded through
 * `benchmark::DoNotOptimize`; a one-shot out-of-loop guard asserts the state machine reached the
 * expected state (a breaker that never trips / a bucket that never throttles = a broken probe).
 */

#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>

#include <qb/core/patterns/resilience.h>

using namespace std::chrono_literals;

namespace {

// --- CircuitBreaker: closed happy path (the common case: allow -> work -> on_success) -----------
void
BM_Resilience_CircuitBreaker_Closed(benchmark::State &state) {
    qb::CircuitBreaker breaker(5u, 1s);
    std::uint64_t      now    = 0;
    std::uint64_t      passed = 0;
    for (auto _ : state) {
        bool ok = breaker.allow(now);
        if (ok) {
            ++passed;
            breaker.on_success();
        }
        now += 1000; // advance 1 µs/op
        benchmark::DoNotOptimize(ok);
    }
    if (passed == 0)
        state.SkipWithError("circuit breaker never admitted a call in the closed state");
    state.SetItemsProcessed(state.iterations());
}

// --- CircuitBreaker: open fail-fast (tripped; every allow() during cooldown returns false) -------
void
BM_Resilience_CircuitBreaker_OpenFastFail(benchmark::State &state) {
    qb::CircuitBreaker breaker(3u, 10s);
    std::uint64_t      now = 0;
    // Trip it out of the timed region: 3 consecutive failures -> open.
    for (int i = 0; i < 3; ++i)
        breaker.on_failure(now);
    if (breaker.state() != qb::CircuitBreaker::State::open) {
        state.SkipWithError("circuit breaker did not open after the failure threshold");
        return;
    }

    std::uint64_t rejected = 0;
    for (auto _ : state) {
        bool ok = breaker.allow(now); // still cooling down (now stays < opened+cooldown)
        if (!ok)
            ++rejected;
        now += 1000; // 1 µs/op — far below the 10 s cooldown, so it stays open
        benchmark::DoNotOptimize(ok);
    }
    if (rejected == 0)
        state.SkipWithError("open breaker admitted calls during cooldown (fast-fail path not exercised)");
    state.SetItemsProcessed(state.iterations());
}

// --- rate_limiter (token bucket): try_acquire with an advancing clock (refill + throttle) --------
void
BM_Resilience_RateLimiter_TryAcquire(benchmark::State &state) {
    // One-shot deterministic guard (independent of the timed-loop iteration count, which we do not
    // control): at a FIXED instant a full bucket grants only its initial burst, then throttles.
    {
        qb::rate_limiter guard(8.0, 100ns);
        int              grants = 0;
        for (int i = 0; i < 100; ++i)
            if (guard.try_acquire(0))
                ++grants;
        if (grants == 0 || grants >= 100) {
            state.SkipWithError("rate limiter did not grant a burst then throttle at a fixed instant");
            return;
        }
    }

    // Timed loop: try_acquire with an advancing clock (40 ns/op < 100 ns/token ⇒ steady throttling
    // once the burst drains). No branch-count assertion here — a low-iteration estimation pass may
    // see only the initial grants; the deterministic guard above is the correctness oracle.
    qb::rate_limiter limiter(8.0, 100ns);
    std::uint64_t    now = 0, granted = 0, throttled = 0;
    for (auto _ : state) {
        bool ok = limiter.try_acquire(now);
        ok ? ++granted : ++throttled;
        now += 40;
        benchmark::DoNotOptimize(ok);
    }
    if (granted + throttled > 0)
        state.counters["grant_ratio"] = benchmark::Counter(static_cast<double>(granted) / static_cast<double>(granted + throttled));
    state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(BM_Resilience_CircuitBreaker_Closed)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Resilience_CircuitBreaker_OpenFastFail)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_Resilience_RateLimiter_TryAcquire)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
