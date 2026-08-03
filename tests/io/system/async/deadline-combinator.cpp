/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/async/deadline-combinator.cpp
 * @brief `with_deadline(task, deadline[, token])` — race an operation against a wall-clock deadline.
 *
 * `with_deadline` (qb/io/async/coroutine/cancellation.h) runs an inner task in a `when_any` race
 * against a single self-stopping deadline timer, optionally cancellable by a `cancellation_token`:
 *   - the operation finishes first  → its value is returned verbatim (the race result is
 *     authoritative — never reclassified as a timeout, Finding 2.B.5);
 *   - the deadline fires first       → `timeout_error`;
 *   - the token is cancelled first   → `cancelled_error`;
 *   - the deadline is already past on entry → `timeout_error` thrown synchronously.
 * Crucially the *losing* branch is reclaimed immediately (the detached timer when the op wins; the
 * still-running op when the deadline/token wins) instead of leaking its frame + watcher until the
 * far-off deadline — these tests assert that with the per-thread `live_frames` allocator counter.
 *
 * Tier: SYSTEM (drives `coro_scheduler().spawn()` + the event loop + real timers). Every wait uses
 * the shared bounded pump `qb::io::test::pump_until` (loud bounded timeout, never a silent hang).
 *
 * Merged here from coroutine/test-coroutine-cancellation.cpp (the `WithDeadline*` cluster) and the
 * deadline regressions from coroutine/test-coroutine-regression.cpp
 * (`WithDeadlineInThePastThrowsTimeoutImmediately` — the stronger twin of the old
 * `WithDeadlineAlreadyPassed`). New: an operation that throws a *non-timeout* exception must
 * propagate it, not mask it as a timeout.
 */

#include <atomic>
#include <chrono>
#include <stdexcept>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class DeadlineCombinator : public ::testing::Test {
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

// Free-function canceller: keeps the captured token alive across the suspend point.
task<void>
cancel_token_after(cancellation_token tok, qb::duration d) {
    co_await sleep(d);
    tok.cancel();
}

} // namespace

// ---------------------------------------------------------------------------
// Happy path + boundary conditions
// ---------------------------------------------------------------------------

TEST_F(DeadlineCombinator, OperationCompletesBeforeDeadlineReturnsValue) {
    std::atomic<int>  result{0};
    std::atomic<bool> done{false};
    const auto        deadline = std::chrono::steady_clock::now() + 2000ms;

    coro_scheduler().spawn([deadline, &result, &done]() -> task<void> {
        result = co_await with_deadline(
            []() -> task<int> {
                co_await sleep(10ms);
                co_return 42;
            }(),
            deadline);
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_deadline never completed";
    EXPECT_EQ(result.load(), 42);
}

TEST_F(DeadlineCombinator, DeadlineAlreadyInThePastThrowsTimeoutSynchronously) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};
    std::atomic<bool> op_ran{false};
    const auto        deadline = std::chrono::steady_clock::now() - 1ms;

    coro_scheduler().spawn([deadline, &caught, &done, &op_ran]() -> task<void> {
        auto op = [&op_ran]() -> task<int> {
            op_ran = true; // must not even be entered — the pre-check throws first
            co_await sleep(50ms);
            co_return 7;
        };
        try {
            (void) co_await with_deadline(op(), deadline);
            ADD_FAILURE() << "an already-past deadline must throw timeout_error";
        } catch (const timeout_error &) {
            caught = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_deadline never resolved";
    EXPECT_TRUE(caught.load());
}

TEST_F(DeadlineCombinator, DeadlineFiresBeforeOperationThrowsTimeout) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};
    const auto        deadline = std::chrono::steady_clock::now() + 30ms;

    coro_scheduler().spawn([deadline, &caught, &done]() -> task<void> {
        try {
            (void) co_await with_deadline(
                []() -> task<int> {
                    co_await sleep(5000ms); // far longer than the deadline
                    co_return 1;
                }(),
                deadline);
            ADD_FAILURE() << "Expected timeout_error";
        } catch (const timeout_error &) {
            caught = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_deadline never timed out";
    EXPECT_TRUE(caught.load());
}

TEST_F(DeadlineCombinator, AlreadyCancelledTokenThrowsCancelled) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token;
        token.cancel();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        try {
            co_await with_deadline(
                []() -> task<int> {
                    co_await sleep(100ms);
                    co_return 42;
                }(),
                deadline, token);
            ADD_FAILURE() << "an already-cancelled token must throw cancelled_error";
        } catch (const cancelled_error &) {
            caught = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_deadline never resolved";
    EXPECT_TRUE(caught.load());
}

TEST_F(DeadlineCombinator, OperationExceptionPropagatesNotMaskedAsTimeout) {
    std::atomic<bool> caught_runtime{false};
    std::atomic<bool> caught_timeout{false};
    std::atomic<bool> done{false};
    const auto        deadline = std::chrono::steady_clock::now() + 2000ms; // generous: op wins the race

    coro_scheduler().spawn([deadline, &caught_runtime, &caught_timeout, &done]() -> task<void> {
        try {
            (void) co_await with_deadline(
                []() -> task<int> {
                    co_await sleep(10ms);
                    throw std::runtime_error("op-exploded");
                    co_return 0;
                }(),
                deadline);
            ADD_FAILURE() << "the operation threw — with_deadline must surface it";
        } catch (const timeout_error &) {
            caught_timeout = true; // wrong: the op won the race, this is NOT a timeout
        } catch (const std::runtime_error &e) {
            caught_runtime = std::string(e.what()) == "op-exploded";
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_deadline never resolved";
    EXPECT_TRUE(caught_runtime.load()) << "the inner exception must propagate";
    EXPECT_FALSE(caught_timeout.load()) << "a winning operation's exception must not be reclassified as a timeout";
}

// ---------------------------------------------------------------------------
// Repeated back-to-back invocations — frame-lifetime fix holds across calls
// ---------------------------------------------------------------------------

TEST_F(DeadlineCombinator, RepeatedSuccessAndTimeoutInterleave) {
    std::atomic<int>  successes{0};
    std::atomic<int>  timeouts{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        for (int i = 0; i < 6; ++i) {
            const bool should_timeout = (i % 2 != 0);
            const auto dl             = std::chrono::steady_clock::now() + (should_timeout ? 10ms : 2000ms);
            auto       op             = [should_timeout]() -> task<int> {
                if (should_timeout)
                    co_await sleep(5000ms);
                co_return 1;
            };
            try {
                (void) co_await with_deadline(op(), dl);
                ++successes;
            } catch (const timeout_error &) {
                ++timeouts;
            }
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 5000ms)) << "repeated with_deadline stalled";
    EXPECT_EQ(successes.load(), 3);
    EXPECT_EQ(timeouts.load(), 3);
}

// ---------------------------------------------------------------------------
// Loser-branch reclamation — live_frames oracle (no leaked frame/watcher)
// ---------------------------------------------------------------------------

TEST_F(DeadlineCombinator, OperationWinsReclaimsDetachedTimerNoLeak) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};
    const auto        deadline = std::chrono::steady_clock::now() + 5000ms; // far off

    coro_scheduler().spawn([deadline, &done]() -> task<void> {
        int result = co_await with_deadline(
            []() -> task<int> {
                co_await sleep(10ms);
                co_return 7;
            }(),
            deadline);
        EXPECT_EQ(result, 7);
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_deadline never completed";
    coro_scheduler().run_ready(); // drain any deferred-destroy frames
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "with_deadline leaked " << (after - baseline)
                               << " frame(s) — the detached deadline timer must be reclaimed when the operation wins";
}

TEST_F(DeadlineCombinator, DeadlineWinsReclaimsRunningOperationNoLeak) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> caught{false};
    const auto        deadline = std::chrono::steady_clock::now() + 20ms;

    coro_scheduler().spawn([deadline, &caught]() -> task<void> {
        try {
            (void) co_await with_deadline(
                []() -> task<int> {
                    co_await sleep(5000ms); // must be torn down well before this elapses
                    co_return 1;
                }(),
                deadline);
            ADD_FAILURE() << "Expected timeout_error";
        } catch (const timeout_error &) {
            caught = true;
        }
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return caught.load(); })) << "with_deadline never timed out";
    coro_scheduler().run_ready();
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "with_deadline leaked " << (after - baseline)
                               << " frame(s) — the losing operation must be reclaimed when the deadline fires";
}

TEST_F(DeadlineCombinator, TokenCancelReclaimsTimerAndOperationNoLeak) {
    const long         baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool>  cancelled{false};
    cancellation_token token;
    const auto         deadline = std::chrono::steady_clock::now() + 5000ms; // far off

    coro_scheduler().spawn([deadline, token, &cancelled]() -> task<void> {
        try {
            (void) co_await with_deadline(
                []() -> task<int> {
                    co_await sleep(5000ms);
                    co_return 1;
                }(),
                deadline, token);
            ADD_FAILURE() << "Expected cancelled_error";
        } catch (const cancelled_error &) {
            cancelled = true;
        }
    });
    coro_scheduler().spawn(cancel_token_after(token, 20ms));

    EXPECT_TRUE(qb::io::test::pump_until([&] { return cancelled.load(); })) << "with_deadline never cancelled";
    coro_scheduler().run_ready();
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "with_deadline leaked " << (after - baseline)
                               << " frame(s) — token cancel must reclaim the deadline timer + operation";
}
