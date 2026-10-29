/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file shared/coroutine_test_support.h
 * @brief The systemic de-flake pump for every qb-io coroutine / async test.
 *
 * Every coroutine and async test in this suite has the same shape: drive a coroutine (or any
 * async operation), then wait for it to finish before asserting. Historically each file
 * re-invented that wait as a fixed `run_for(N ms)` plus a `bool done` poll — three byte-for-byte
 * clones lived in test-coroutine-awaiters.cpp, test-coroutine-channel-payload.cpp and
 * test-coroutine-comprehensive.cpp alone. That pattern is doubly wrong: a *fixed* duration is
 * either too short (flaky on a loaded CI box) or wastefully long, and when the coroutine never
 * completes the test simply reads a still-false flag and fails with a useless message — or, worse,
 * the bare `run_for(Nms)` form blocks for the full budget on a hang with no signal at all.
 *
 * `pump_until()` replaces all of them with a single deadline-bounded predicate pump:
 *
 *   - It drives the qb-io event loop through the framework's own
 *     `qb::io::async::run_for()` in small steps, so the libev watchers AND the ready-coroutine
 *     queue are both drained exactly the way production drains them (one source of truth — we do
 *     not re-implement the EVRUN_NOWAIT / run_ready dance, and we inherit its no-nested-drain
 *     guard and its idle 1 ms yield, so the pump never busy-spins).
 *   - It stops the instant `pred()` becomes true (snappy: a coroutine that finishes in 1 ms costs
 *     1 ms, not the whole timeout), or when the wall-clock deadline elapses.
 *   - On timeout it returns `false` LOUDLY rather than hanging or silently leaving a flag unset.
 *     The mandated idiom is to assert on the return value so a hung coroutine yields a precise,
 *     greppable failure:
 *
 *         EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); }))
 *             << "coroutine never completed";
 *
 *     Because the message lives at the call site, the failure names the exact coroutine that stalled
 *     — the timeout is the test failing fast, never the test wedging the runner.
 *
 * `wait_until()` is the same contract with the `step` made explicit (the fuller form), for the few
 * call sites that want a coarser or finer pump granularity. `reset_async_context()` is the one-liner
 * SetUp() helper that matches what the fixtures already call (`qb::io::async::init()`), named for
 * intent so the per-test fresh-loop reset reads as such.
 *
 * Header-only, no test cases. All helpers live in namespace `qb::io::test`. The predicate is taken
 * by `std::function<bool()>` so the helper is non-templated (one definition, trivial to call) and
 * any callable — lambda, functor, bound member — drops straight in.
 *
 * ---------------------------------------------------------------------------------------------
 * WHY THE TIMER ASSERTIONS IN THIS SUITE ARE NOT MARGIN-LIMITED  (read before "widening" one)
 * ---------------------------------------------------------------------------------------------
 * Several tests here look margin-limited at a glance — `sleep(15ms)` vs `sleep(10ms)` deciding an
 * order, or `EXPECT_GE(elapsed, 50ms)` after a `sleep(50ms)` with no slack at all. Ordinary
 * scheduler jitter on a loaded box is tens of milliseconds, so those margins read as coin flips
 * and invite the two bad "fixes": raising the constants (slower, still fragile) or loosening the
 * floors (which deletes the only regression detector for I2 below). Neither is needed. Two
 * structural invariants carry these assertions; the nominal margin is not what protects them.
 *
 * I1 — ORDER assertions are immune to jitter because jitter is COMMON MODE.
 *      `CoroutineScheduler::spawn` does not start the body: `spawn_tracked` only enqueues
 *      (`ready_queue_.push_back({handle, true})`, CoroutineScheduler::spawn_tracked), so every task spawned before a
 *      pump starts in ONE `run_ready()` drain and arms its timer in ONE loop turn, into libev's
 *      single deadline-ordered heap. `timers_reify` then pops strictly in deadline order
 *      (`ANHE_at(timers[HEAP0]) < mn_now`, ev.c::timers_reify). A stall therefore delays every
 *      timer equally and cannot reorder them — it makes them all fire back-to-back, in order.
 *      Measured (SIGSTOP/SIGCONT injection, 40 ms stall windows, release, macOS): the realized
 *      inter-completion gap of the 15 ms/10 ms pair fell from 5.30 ms to 0.023 ms — 0.5% of its
 *      nominal 5 ms budget — and the 10 ms-apart five-timer ladder fell from 10 ms to 0.002 ms,
 *      with ZERO order inversions in 60 + 60 runs (240 adjacent pairs). The margin was consumed
 *      essentially completely and the property still held; that is the signature of common mode.
 *      Injecting the stall between two `spawn()` CALLS (40 ms) also changed nothing, because of
 *      the lazy start above.
 *      I1 is bounded, not absolute, and the bound is the useful number: the order CAN be inverted
 *      by a stall that lands between two consecutive arms INSIDE the drain and outlasts the
 *      nominal separation — verified, 3/3 failures with 40 ms injected there. But that window is
 *      the cost of starting one coroutine plus one `clock_gettime`: measured across 40 runs under
 *      40 ms SIGSTOP stalls, the whole five-timer ladder armed within 8.8-54.7 us and the worst
 *      ADJACENT arm gap was 3.6-41.3 us. So the exposure is a ~10 us window, not the millisecond
 *      margin — which is exactly why raising the sleep constants is the wrong lever: it does not
 *      narrow the window at all, it only raises the stall length needed to exploit it, while
 *      making every run slower. Nothing inverted in 125 ordinary + jittered runs per site.
 *      What I1 does NOT cover: two timers armed in DIFFERENT loop turns, separated by real work.
 *      Those carry independent jitter and are genuinely margin-limited — see the note in
 *      core/system/coroutine/coroutine-resilience.cpp, which is the one case of that shape here.
 *
 * I2 — ELAPSED-FLOOR assertions (`EXPECT_GE(elapsed, nominal)`) are one-sided regression checks.
 *      A delayed callback normally adds elapsed time, but these checks also depend on conversion
 *      and on the relationship between the loop clock and the clock the test reads:
 *        - `to_ev_seconds` uses `duration_cast<duration<double>>`, avoiding integer-ms truncation;
 *          floating-point conversion does not guarantee that a delay rounds upward;
 *        - `timer_awaiter::await_suspend` refreshes the loop clock before arming its watcher;
 *          `async::callback` delegates the same refresh to event::timer::start, which calls
 *          `ev_now_update` before arming, so both use a fresh deadline rather than cached time;
 *        - `timers_reify` in qb/src/qb/ev/ev.c fires only when mn_now is strictly past the deadline;
 *          get_clock reads the clock steady_clock reads: EV_MONOTONIC_CLOCK, QueryPerformanceCounter
 *          on Windows.
 *      Tests measure with steady_clock/qb::mono_now(), the loop's own clock with the supported
 *      toolchains (Huly QB-1181; macOS with libc++). On
 *      macOS the loop read CLOCK_MONOTONIC until then -- the wall clock less the boot time, slewed
 *      by timed -- against libc++'s CLOCK_MONOTONIC_RAW: an analysis recorded +7.5 ppm between
 *      them, and a correction under way (+27.65 ms) made the 50 x 1 ms chain 27 us SHORT.
 *      Recorded worst headroom over 125 release runs per site: the tightest floor was the
 *      50 x 1 ms chain in core/system/timer/async-callback-ordering.cpp at +0.34 ms. Under load,
 *      that chain's worst case grew to +334 ms and the 2 x 20 ms retry floor's to +58 ms;
 *      none of those recorded runs went negative. These are historical observations, not a
 *      universal proof for every host or clock configuration. A lost ev_now_update is one cause
 *      this check can expose. Investigate a failure rather than silently widening the floor.
 */

#ifndef QB_IO_TESTS_SHARED_COROUTINE_TEST_SUPPORT_H
#define QB_IO_TESTS_SHARED_COROUTINE_TEST_SUPPORT_H

#include <chrono>
#include <functional>

#include <gtest/gtest.h>
#include <qb/io/async.h>

namespace qb::io::test {

using namespace std::chrono_literals;

/// Default total budget a pump waits before declaring the predicate dead.
inline constexpr std::chrono::milliseconds kDefaultPumpTimeout{2000};
/// Default granularity of a single event-loop slice between predicate checks.
inline constexpr std::chrono::milliseconds kDefaultPumpStep{5};

// ---------------------------------------------------------------------------
// wait_until — deadline-bounded predicate pump with an explicit step.
//
// Drives the qb-io event loop (via qb::io::async::run_for, which itself pumps
// EVRUN_NOWAIT and drains ready coroutines) in `step`-sized slices until `pred()`
// is true or `timeout` elapses. Returns the final value of `pred()`: `true` iff the
// predicate became (or already was) satisfied within the budget, `false` on timeout.
//
// A `false` return is the LOUD timeout — callers must surface it:
//     EXPECT_TRUE(wait_until([&]{ return done; }, 2s, 5ms)) << "never completed";
// Never discards the result into a bare statement; that re-creates the silent hang
// this helper exists to kill.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool
wait_until(std::function<bool()> pred, std::chrono::milliseconds timeout, std::chrono::milliseconds step) {
    // Check up-front: an already-satisfied predicate must cost zero loop time.
    if (pred())
        return true;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        // run_for already drains pending coroutines first, pumps the libev loop with
        // EVRUN_NOWAIT, and yields ~1ms when idle — so one source of truth for "how to
        // advance the loop", and no busy-spin here.
        qb::io::async::run_for(step);
        if (pred())
            return true;
    } while (std::chrono::steady_clock::now() < deadline);

    // One final check: the predicate may have flipped during the last slice right at
    // the deadline boundary. Report the true outcome, never a stale `false`.
    return pred();
}

// ---------------------------------------------------------------------------
// pump_until — the everyday form: same contract, default 2s timeout / 5ms step.
//
//     EXPECT_TRUE(qb::io::test::pump_until([&]{ return done.load(); }))
//         << "coroutine never completed";
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool
pump_until(std::function<bool()> pred, std::chrono::milliseconds timeout = kDefaultPumpTimeout,
           std::chrono::milliseconds step = kDefaultPumpStep) {
    return wait_until(std::move(pred), timeout, step);
}

// ---------------------------------------------------------------------------
// reset_async_context — fresh per-test event loop, matching fixture SetUp().
//
// Aliases qb::io::async::init() under an intent-revealing name. Call it from a
// fixture's SetUp() exactly where the current tests call init(). TearDown remains
// the caller's responsibility (the existing pattern is
// `listener::current.reset_coro_scheduler()` followed by `listener::current.clear()`),
// since teardown ordering is suite-specific and not something this helper should own.
// ---------------------------------------------------------------------------
inline void
reset_async_context() noexcept {
    qb::io::async::init();
}

// Declare after borrowed test state, before spawning work that refers to it.
// Reclaim frames without resuming them while their captures are still alive.
class coroutine_scope_cleanup {
public:
    coroutine_scope_cleanup()                                           = default;
    coroutine_scope_cleanup(const coroutine_scope_cleanup &)            = delete;
    coroutine_scope_cleanup &operator=(const coroutine_scope_cleanup &) = delete;
    ~coroutine_scope_cleanup() {
        if (qb::io::async::listener::current.has_coro_scheduler())
            qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_COROUTINE_TEST_SUPPORT_H
