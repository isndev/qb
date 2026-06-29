/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/async/cancellation-awaiters.cpp
 * @brief Cancellation-aware coroutine awaiters driven by the qb-io event loop.
 *
 * These tests exercise the *loop-driven* cancellation surface of `cancellation.h` — the part that
 * needs the scheduler + timers, as opposed to the pure token mechanics in
 * unit/coroutine/cancellation-token.cpp:
 *   - `check_cancelled(token)`        — suspends and throws `cancelled_error` when the token fires.
 *   - `cancellable_sleep(d, token)`   — a sleep that wakes *immediately* on cancel (no polling),
 *                                       throws when pre-cancelled, and deregisters its on_cancel
 *                                       hook on each normal completion (no callback accumulation).
 *   - `yield_or_cancel(token)`        — yields to the scheduler, throwing on resume if cancelled.
 *   - `make_cancellable(task, token, throw_on_cancel)` / `cancellable_operation<T>` — wraps an inner
 *                                       task; on cancel either throws (`throw_on_cancel=true`) or
 *                                       returns/propagates the inner result/exception.
 *
 * Tier: SYSTEM (each test drives `coro_scheduler().spawn()` + the event loop). Every wait uses the
 * shared bounded pump `qb::io::test::pump_until` instead of a fixed `run_for(Nms)` budget, so a
 * stalled coroutine fails loudly with a greppable message rather than silently reading a stale flag.
 *
 * Strengthened over the original coroutine/test-coroutine-cancellation.cpp:
 *   - the formerly VACUOUS `MakeCancellableWrapper` now asserts the cancel actually surfaced
 *     `cancelled_error` and that the inner `done` stayed false;
 *   - "wakes immediately on cancel" asserts *observed elapsed < sleep duration* rather than a fixed
 *     wall-clock budget;
 *   - new: `make_cancellable(throw_on_cancel=false)` returns the inner value when not cancelled and
 *     propagates an inner exception when not cancelled; `yield_or_cancel` on an already-cancelled
 *     token throws immediately with zero iterations.
 *
 * Deadline regressions that used to live alongside these have moved to
 * system/async/deadline-combinator.cpp.
 */

#include <atomic>
#include <chrono>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/coroutine_reclaim_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::reclaim_fast_winner;
using qb::io::test::run_reclaim_driver;

namespace {

class CancellationAwaiters : public ::testing::Test {
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

// Free-function canceller: sleeps, then cancels a shared token. A free function (not a lambda)
// keeps the captured token alive across the suspend point.
task<void>
cancel_token_after(cancellation_token tok, qb::duration d) {
    co_await sleep(d);
    tok.cancel();
}

} // namespace

// ---------------------------------------------------------------------------
// Polling loop that observes is_cancelled()
// ---------------------------------------------------------------------------

TEST_F(CancellationAwaiters, PollingLoopExitsOnCancel) {
    cancellation_token token;
    std::atomic<bool>  started{false};
    std::atomic<bool>  cancelled{false};
    std::atomic<bool>  done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        started = true;
        for (int i = 0; i < 100; ++i) {
            if (token.is_cancelled()) {
                cancelled = true;
                break;
            }
            co_await sleep(2ms);
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return started.load(); })) << "worker never started";
    token.cancel();

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "polling worker never observed cancel";
    EXPECT_TRUE(cancelled.load());
}

// ---------------------------------------------------------------------------
// check_cancelled awaiter
// ---------------------------------------------------------------------------

TEST_F(CancellationAwaiters, CheckCancelledThrowsAfterCancel) {
    cancellation_token token;
    std::atomic<bool>  caught{false};
    std::atomic<bool>  done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await sleep(2ms);
            co_await check_cancelled(token); // suspends; throws once cancelled
            co_await sleep(200ms);
        } catch (const cancelled_error &) {
            caught = true;
        }
        done = true;
    });

    // Let the worker park on check_cancelled, then cancel.
    EXPECT_TRUE(qb::io::test::pump_until([&] { return token.get_state()->callbacks.size() == 1u; }))
        << "worker never parked on check_cancelled";
    token.cancel();

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "check_cancelled worker never resumed";
    EXPECT_TRUE(caught.load());
}

// ---------------------------------------------------------------------------
// cancellable_sleep
// ---------------------------------------------------------------------------

TEST_F(CancellationAwaiters, CancellableSleepThrowsOnCancelMidSleep) {
    cancellation_token token;
    std::atomic<bool>  caught{false};
    std::atomic<bool>  done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await cancellable_sleep(2000ms, token);
        } catch (const cancelled_error &) {
            caught = true;
        }
        done = true;
    });

    // It must still be sleeping (one on_cancel hook registered, not resumed).
    EXPECT_TRUE(qb::io::test::pump_until([&] { return token.get_state()->callbacks.size() == 1u; })) << "cancellable_sleep never parked";
    EXPECT_FALSE(done.load());

    token.cancel();
    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "cancellable_sleep never woke on cancel";
    EXPECT_TRUE(caught.load());
}

TEST_F(CancellationAwaiters, CancellableSleepWakesImmediatelyOnCancelObservedElapsed) {
    cancellation_token        token;
    std::atomic<bool>         done{false};
    std::chrono::milliseconds observed{0};
    constexpr auto            kSleep = 5000ms; // huge: we must wake far earlier than this

    coro_scheduler().spawn([&]() -> task<void> {
        const auto start = std::chrono::steady_clock::now();
        try {
            co_await cancellable_sleep(kSleep, token);
        } catch (const cancelled_error &) {
            observed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        }
        done = true;
    });
    // Cancel from another coroutine after a short delay, all inside the loop.
    coro_scheduler().spawn(cancel_token_after(token, 10ms));

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "cancellable_sleep never woke on cancel";
    // The contract is "wake on cancel", not "wait the full duration": observed elapsed must be
    // a tiny fraction of the requested sleep, proving there is no polling fallback.
    EXPECT_LT(observed, kSleep / 2) << "cancellable_sleep waited too long after cancel — polling regression?";
}

TEST_F(CancellationAwaiters, CancellableSleepPreCancelledThrowsImmediately) {
    cancellation_token token;
    token.cancel(); // cancel BEFORE the sleep — await_ready() short-circuits
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        const auto start = std::chrono::steady_clock::now();
        try {
            co_await cancellable_sleep(5000ms, token);
            ADD_FAILURE() << "pre-cancelled cancellable_sleep must not run the sleep";
        } catch (const cancelled_error &) {
            // Must complete essentially instantly, never near 5000ms.
            EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
            caught = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "pre-cancelled worker never finished";
    EXPECT_TRUE(caught.load());
}

TEST_F(CancellationAwaiters, CancellableSleepLoopDeregistersOnEachIteration) {
    cancellation_token token;
    std::atomic<int>   iterations{0};

    coro_scheduler().spawn([&]() -> task<void> {
        for (int i = 0; i < 50; ++i) {
            co_await cancellable_sleep(1ms, token);
            ++iterations;
        }
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return iterations.load() == 50; })) << "cancellable_sleep loop stalled";
    // The token outlives the coroutine; every per-sleep on_cancel must have been removed — this is
    // the awaiter-level proof of the unbounded-growth fix.
    EXPECT_TRUE(token.get_state()->callbacks.empty())
        << "cancellable_sleep left " << token.get_state()->callbacks.size() << " dead callbacks — the unbounded-growth bug is back";
}

// ---------------------------------------------------------------------------
// yield_or_cancel
// ---------------------------------------------------------------------------

TEST_F(CancellationAwaiters, YieldOrCancelYieldsThenThrowsOnCancel) {
    std::atomic<int>  iterations{0};
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token;
        try {
            for (int i = 0; i < 100; ++i) {
                ++iterations;
                if (i == 3)
                    token.cancel();
                co_await yield_or_cancel(token);
            }
        } catch (const cancelled_error &) {
            caught = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "yield_or_cancel worker never finished";
    // Iterations 0,1,2,3 run; iteration 3 cancels, the following yield_or_cancel resume throws.
    EXPECT_EQ(iterations.load(), 4);
    EXPECT_TRUE(caught.load());
}

TEST_F(CancellationAwaiters, YieldOrCancelYieldsWithoutCancel) {
    std::atomic<int>  iterations{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token;
        for (int i = 0; i < 5; ++i) {
            ++iterations;
            co_await yield_or_cancel(token);
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "yield_or_cancel never completed";
    EXPECT_EQ(iterations.load(), 5);
}

TEST_F(CancellationAwaiters, YieldOrCancelAlreadyCancelledThrowsWithZeroIterations) {
    std::atomic<int>  iterations{0};
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token;
        token.cancel(); // already cancelled before the first yield
        try {
            for (int i = 0; i < 5; ++i) {
                co_await yield_or_cancel(token); // first resume throws immediately
                ++iterations;                    // never reached
            }
        } catch (const cancelled_error &) {
            caught = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "yield_or_cancel worker never finished";
    EXPECT_TRUE(caught.load());
    EXPECT_EQ(iterations.load(), 0) << "an already-cancelled token must throw before any iteration body runs";
}

// ---------------------------------------------------------------------------
// make_cancellable / cancellable_operation
// ---------------------------------------------------------------------------

TEST_F(CancellationAwaiters, MakeCancellableThrowsAndInnerNeverFinishesWhenCancelled) {
    cancellation_token token;
    std::atomic<bool>  started{false};
    std::atomic<bool>  inner_done{false};
    std::atomic<bool>  caught{false};
    std::atomic<bool>  done{false};

    auto inner = [&]() -> task<int> {
        started = true;
        co_await sleep(2000ms); // long enough that cancel always wins
        inner_done = true;      // must NOT happen when cancelled mid-flight
        co_return 42;
    };

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            int result = co_await make_cancellable(inner(), token, /*throw_on_cancel=*/true);
            (void) result;
            ADD_FAILURE() << "make_cancellable must throw cancelled_error when cancelled";
        } catch (const cancelled_error &) {
            caught = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return started.load(); })) << "inner task never started";
    EXPECT_FALSE(inner_done.load());
    token.cancel();

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "make_cancellable never resumed after cancel";
    EXPECT_TRUE(caught.load()) << "cancelled_error did not surface from make_cancellable";
    EXPECT_FALSE(inner_done.load()) << "the inner task must be torn down, not allowed to complete, on cancel";
}

TEST_F(CancellationAwaiters, MakeCancellableReturnsInnerValueWhenNotCancelled) {
    cancellation_token token;
    std::atomic<int>   value{0};
    std::atomic<bool>  done{false};

    auto inner = []() -> task<int> {
        co_await sleep(5ms);
        co_return 7;
    };

    coro_scheduler().spawn([&]() -> task<void> {
        // throw_on_cancel=false: when no cancel happens, the inner value flows through unchanged.
        value = co_await make_cancellable(inner(), token, /*throw_on_cancel=*/false);
        done  = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "make_cancellable never delivered the value";
    EXPECT_EQ(value.load(), 7);
}

TEST_F(CancellationAwaiters, MakeCancellablePropagatesInnerExceptionWhenNotCancelled) {
    cancellation_token token;
    std::atomic<bool>  caught{false};
    std::atomic<bool>  done{false};

    auto exploding = []() -> task<int> {
        co_await sleep(5ms);
        throw std::runtime_error("inner-cancellable-boom");
        co_return 0;
    };

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            // throw_on_cancel=false: with no cancel, an inner throw must propagate (not be masked).
            int v = co_await make_cancellable(exploding(), token, /*throw_on_cancel=*/false);
            (void) v;
        } catch (const std::runtime_error &e) {
            caught = std::string(e.what()) == "inner-cancellable-boom";
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "make_cancellable never finished";
    EXPECT_TRUE(caught.load()) << "make_cancellable must not swallow inner exceptions";
}

TEST_F(CancellationAwaiters, MakeCancellableSequentialReuseOnSharedToken) {
    std::atomic<int>   completed{0};
    std::atomic<int>   cancelled_count{0};
    std::atomic<bool>  done{false};
    cancellation_token token;

    coro_scheduler().spawn([&]() -> task<void> {
        for (int i = 0; i < 4; ++i) {
            auto op = [i]() -> task<int> {
                co_await sleep(20ms);
                co_return i;
            };
            try {
                int v = co_await make_cancellable(op(), token);
                (void) v;
                ++completed;
            } catch (const cancelled_error &) {
                ++cancelled_count;
            }
        }
        done = true;
    });

    // Let at least two operations complete on the shared token, then cancel the rest.
    EXPECT_TRUE(qb::io::test::pump_until([&] { return completed.load() >= 2; })) << "shared-token reuse stalled";
    token.cancel();

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "sequential make_cancellable never finished";
    EXPECT_GE(completed.load(), 2);
    EXPECT_GE(cancelled_count.load(), 1);
    EXPECT_EQ(completed.load() + cancelled_count.load(), 4) << "every operation must resolve exactly once";
}

// =============================================================================
// Destroy-while-parked reclamation (see shared/coroutine_reclaim_support.h)
//
// make_cancellable / cancellable_sleep spawn a detached runner/timer holding the shared state; when
// the awaiter's frame is reclaimed (when_any loser) the dtor must tear that runner/timer down so its
// late completion does not resume the freed frame. (cancellable_sleep's small library frame is
// pooled → ASan is masked here; the definitive proof is the pool-disabled repro. This still exercises
// the teardown path + the no-spurious-resume guard + that nothing hangs.)
// =============================================================================

TEST_F(CancellationAwaiters, CancellableOperationReclaimedWhileParked) {
    run_reclaim_driver([]() -> task<void> {
        cancellation_token tok;
        auto               park = [](cancellation_token t) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            co_await make_cancellable(
                []() -> task<int> {
                    co_await sleep(40ms);
                    co_return 7;
                }(),
                std::move(t));
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return (int) big[1];
        };
        auto r = co_await when_any(park(tok), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        co_await sleep(60ms); // the detached inner op completes late — must NOT resume the freed frame
    });
}

TEST_F(CancellationAwaiters, CancellableSleepReclaimedWhileParked) {
    run_reclaim_driver([]() -> task<void> {
        cancellation_token tok;
        auto               park = [](cancellation_token t) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            co_await cancellable_sleep(40ms, std::move(t));
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return (int) big[1];
        };
        auto r = co_await when_any(park(tok), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        co_await sleep(60ms); // the detached timer fires late — must NOT resume the freed frame
    });
}
