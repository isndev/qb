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

} // namespace qb::io::test

#endif // QB_IO_TESTS_SHARED_COROUTINE_TEST_SUPPORT_H
