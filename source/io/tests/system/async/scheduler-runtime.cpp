/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/async/scheduler-runtime.cpp
 * @brief Coroutine scheduler runtime bridge — `run_sync()` and `active_count()`.
 *
 * The sync bridge (`run_sync`, qb/io/async/coroutine/utils.h) and the scheduler's live-frame
 * accounting (`CoroutineScheduler::active_count`, scheduler.h) are the runtime glue between blocking
 * call sites (test SetUp, drain/shutdown loops) and the cooperative coroutine loop. Both need a real
 * event loop, so these are SYSTEM tests.
 *
 * Contracts proven:
 *   - `run_sync(task<int>)` blocks the caller until the coroutine completes and returns its value,
 *     without busy-spinning at 100% CPU when nothing is ready (it must wait, not poll);
 *   - `run_sync(task<void>)` runs the coroutine to completion and propagates its side effects;
 *   - `run_sync` rethrows an exception thrown inside the coroutine;
 *   - `active_count()` counts BOTH ready and *suspended* frames (Finding 2.B.6) — a coroutine parked
 *     on a libev-watcher awaiter (timer/socket) is still active. Only the watcher-backed awaiters in
 *     awaiter.h call CoroutineScheduler::register_suspended(); the in-memory sync primitives in
 *     sync.h (async_event, mutex, latch, …) park their waiter in their OWN list and are deliberately
 *     NOT counted by active_count(). The barrier here is therefore a long `cancellable_sleep` (a real
 *     timer frame that registers as suspended), de-raced with `pump_until`: we pump until the worker
 *     has parked, measure active_count() while it is provably suspended, then `token.cancel()` to
 *     release it cleanly (the detached timer helper is reclaimed via cancel_spawned — no leaked frame
 *     at teardown).
 *
 * Re-homed from the run_sync/active_count core of coroutine/test-coroutine-regression.cpp. The
 * `current_ptr`/listener-reset case stays with unit/coroutine/scheduler-lifecycle.cpp (owned by the
 * scheduler-lifecycle file), not duplicated here.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class SchedulerRuntime : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// run_sync
// ---------------------------------------------------------------------------

/**
 * @regression run_sync busy-spun at 100% CPU when no events were ready; it must block/wait instead.
 * We can't measure CPU here, but we assert it neither hangs nor returns far too early (which would
 * mean it never actually waited on the timer).
 */
TEST_F(SchedulerRuntime, RunSyncReturnsValueAndActuallyWaits) {
    const auto start = std::chrono::steady_clock::now();

    int result = run_sync([]() -> task<int> {
        co_await sleep(50ms);
        co_return 42;
    }());

    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(result, 42);
    EXPECT_GE(elapsed, 40ms) << "run_sync returned before the 50ms sleep — it did not wait";
    EXPECT_LT(elapsed, 2000ms) << "run_sync took far too long — possible busy-spin / stall";
}

TEST_F(SchedulerRuntime, RunSyncVoidTaskRunsToCompletion) {
    std::atomic<bool> executed{false};

    run_sync([&executed]() -> task<void> {
        co_await sleep(10ms);
        executed = true;
    }());

    EXPECT_TRUE(executed.load());
}

TEST_F(SchedulerRuntime, RunSyncRethrowsCoroutineException) {
    EXPECT_THROW(
        {
            run_sync([]() -> task<int> {
                co_await sleep(5ms);
                throw std::runtime_error("run-sync-boom");
                co_return 0;
            }());
        },
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// active_count — counts suspended frames (de-raced with a barrier, not sleep)
// ---------------------------------------------------------------------------

TEST_F(SchedulerRuntime, ActiveCountIncludesSuspendedFrames) {
    // Deterministic barrier: the worker parks on a long `cancellable_sleep`. The detached timer that
    // drives the sleep is a real libev-timer frame that registers itself as suspended, so it shows up
    // in active_count() while parked. (An async_event would NOT — sync primitives keep their waiter in
    // their own list and are intentionally excluded from active_count(); only watcher-backed awaiters
    // register.) We pump until it has parked, measure, then cancel to release it cleanly.
    cancellation_token token;
    std::atomic<bool>  cancelled{false};
    std::atomic<bool>  done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await cancellable_sleep(5000ms, token); // park on a real timer frame
        } catch (const cancelled_error &) {
            cancelled = true;
        }
        done = true;
    });

    ASSERT_TRUE(qb::io::test::pump_until([&] { return coro_scheduler().active_count() >= 1; }))
        << "worker never parked / active_count never saw the suspended timer frame";

    const std::size_t count_while_parked = coro_scheduler().active_count();
    EXPECT_GE(count_while_parked, 1u) << "active_count() silently ignored a suspended frame";

    // Release the worker and confirm it drains — active_count returns to baseline.
    token.cancel();
    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "worker never resumed after token.cancel()";
    EXPECT_TRUE(cancelled.load());
    EXPECT_EQ(coro_scheduler().active_count(), 0u) << "the resumed frame must be reclaimed";
}

TEST_F(SchedulerRuntime, ActiveCountZeroOnIdleScheduler) {
    EXPECT_EQ(coro_scheduler().active_count(), 0u);
    EXPECT_FALSE(coro_scheduler().has_ready());
}
