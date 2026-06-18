/**
 * @file qb/io/tests/coroutine/test-coroutine-cancellation.cpp
 * @brief Coroutine cancellation tests
 *
 * This file contains tests for cancellation_token, cancellation callbacks,
 * cancellation-aware awaiters, cancellable sleep operations, deadline handling, and
 * cancellation edge cases.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Tests
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Cancellation Token
// =============================================================================

class CancellationTokenTests : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Basic cancellation
 * @brief Token can be cancelled and checked
 */
TEST_F(CancellationTokenTests, BasicCancel) {
    cancellation_token token;

    EXPECT_FALSE(token.is_cancelled());

    token.cancel();

    EXPECT_TRUE(token.is_cancelled());
}

/**
 * @test Token is copyable
 * @brief Copy shares cancellation state
 */
TEST_F(CancellationTokenTests, TokenCopySharesState) {
    cancellation_token token1;
    cancellation_token token2 = token1;

    EXPECT_FALSE(token1.is_cancelled());
    EXPECT_FALSE(token2.is_cancelled());

    token1.cancel();

    EXPECT_TRUE(token1.is_cancelled());
    EXPECT_TRUE(token2.is_cancelled());
}

/**
 * @test Cancellation callback
 * @brief Callback invoked on cancel
 */
TEST_F(CancellationTokenTests, CancellationCallback) {
    cancellation_token token;
    std::atomic<bool>  callback_invoked{false};

    token.on_cancel([&callback_invoked]() { callback_invoked = true; });

    EXPECT_FALSE(callback_invoked);

    token.cancel();

    EXPECT_TRUE(callback_invoked);
}

/**
 * @test Callback invoked immediately if already cancelled
 * @brief Late subscriber gets immediate callback
 */
TEST_F(CancellationTokenTests, CallbackImmediateIfCancelled) {
    cancellation_token token;
    token.cancel();

    std::atomic<bool> callback_invoked{false};

    token.on_cancel([&callback_invoked]() { callback_invoked = true; });

    EXPECT_TRUE(callback_invoked);
}

/**
 * @test throw_if_cancelled
 * @brief Throws if cancelled
 */
TEST_F(CancellationTokenTests, ThrowIfCancelled) {
    cancellation_token token;

    // Should not throw when not cancelled
    EXPECT_NO_THROW(token.throw_if_cancelled());

    token.cancel();

    // Should throw when cancelled
    EXPECT_THROW(token.throw_if_cancelled(), cancelled_error);
}

// =============================================================================
// TEST SUITE: Cancellation in Coroutines
// =============================================================================

class CancellationCoroutineTests : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Cancellation stops loop
 * @brief Coroutine checks token in loop
 */
TEST_F(CancellationCoroutineTests, CancelStopsLoop) {
    cancellation_token token;
    std::atomic<bool>  started{false};
    std::atomic<bool>  cancelled{false};

    auto worker = [&token, &started, &cancelled]() -> task<void> {
        started = true;

        for (int i = 0; i < 100; ++i) {
            if (token.is_cancelled()) {
                cancelled = true;
                co_return;
            }
            co_await sleep(10ms);
        }
    };

    auto t = worker();
    coro_scheduler().spawn(std::move(t));

    // Let it start
    run_for(50ms);
    EXPECT_TRUE(started);

    // Cancel it
    token.cancel();

    run_for(100ms);
    EXPECT_TRUE(cancelled);
}

/**
 * @test check_cancelled awaiter
 * @brief Awaiter throws when cancelled
 */
TEST_F(CancellationCoroutineTests, CheckCancelledAwaiter) {
    cancellation_token token;
    std::atomic<bool>  caught{false};

    auto worker = [&token, &caught]() -> task<void> {
        try {
            co_await sleep(10ms);
            co_await check_cancelled(token);
            co_await sleep(100ms);
        } catch (const cancelled_error &) {
            caught = true;
        }
    };

    auto t = worker();
    coro_scheduler().spawn(std::move(t));

    run_for(50ms);
    token.cancel();

    run_for(100ms);
    EXPECT_TRUE(caught);
}

/**
 * @test cancellable_sleep
 * @brief Sleep that can be interrupted; throws on cancel
 */
TEST_F(CancellationCoroutineTests, CancellableSleep) {
    cancellation_token token;
    std::atomic<bool>  done{false};

    auto worker = [&token, &done]() -> task<void> {
        try {
            co_await cancellable_sleep(500ms, token);
        } catch (const cancelled_error &) {
            done = true;
        }
    };

    auto t = worker();
    coro_scheduler().spawn(std::move(t));

    run_for(100ms);
    EXPECT_FALSE(done); // Should still be sleeping

    token.cancel();

    run_for(100ms);
    EXPECT_TRUE(done); // Cancelled and threw
}

/**
 * @test cancellable_sleep wakes immediately on cancel
 * @brief No 10ms polling; token.cancel() schedules the waiter right away
 */
TEST_F(CancellationCoroutineTests, CancellableSleepWakesImmediatelyOnCancel) {
    cancellation_token token;
    std::atomic<bool>  done{false};

    auto worker = [&token, &done]() -> task<void> {
        try {
            co_await cancellable_sleep(1000ms, token);
        } catch (const cancelled_error &) {
            done = true;
        }
    };

    coro_scheduler().spawn(worker());
    run_for(5ms);
    token.cancel();
    run_for(25ms);

    EXPECT_TRUE(done);
}

/**
 * @test make_cancellable wrapper
 * @brief Wraps task with cancellation
 */
TEST_F(CancellationCoroutineTests, MakeCancellableWrapper) {
    cancellation_token token;
    std::atomic<bool>  started{false};
    std::atomic<bool>  done{false};

    auto inner = [&started, &done]() -> task<int> {
        started = true;
        co_await sleep(200ms);
        done = true;
        co_return 42;
    };

    auto wrapper = [&token, &inner]() -> task<void> {
        try {
            auto result = co_await make_cancellable(inner(), token, true);
            (void) result;
        } catch (const cancelled_error &) {
            // Expected
        }
    };

    auto t = wrapper();
    coro_scheduler().spawn(std::move(t));

    run_for(50ms);
    EXPECT_TRUE(started);
    EXPECT_FALSE(done);

    token.cancel();

    run_for(100ms);
    // Task may or may not complete depending on timing
}

// =============================================================================
// TEST SUITE: Cancellation Edge Cases
// =============================================================================

class CancellationEdgeCases : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Multiple cancels are idempotent
 * @brief Second cancel has no effect
 */
TEST_F(CancellationEdgeCases, CancelIdempotent) {
    cancellation_token token;
    std::atomic<int>   callback_count{0};

    token.on_cancel([&callback_count]() { callback_count++; });

    token.cancel();
    token.cancel();
    token.cancel();

    EXPECT_EQ(callback_count, 1);
    EXPECT_TRUE(token.is_cancelled());
}

// =============================================================================
// TEST SUITE: with_deadline
// =============================================================================

class WithDeadlineTests : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test with_deadline success
 * @brief Operation completes before deadline
 */
TEST_F(WithDeadlineTests, WithDeadlineSuccess) {
    std::atomic<bool> done{false};
    auto              deadline = std::chrono::steady_clock::now() + 200ms;

    auto coro_fn = [deadline, &done]() -> task<void> {
        int result = co_await with_deadline(
            []() -> task<int> {
                co_await sleep(20ms);
                co_return 42;
            }(),
            deadline);
        EXPECT_EQ(result, 42);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(100ms);
    EXPECT_TRUE(done);
}

/**
 * @test with_deadline already passed
 * @brief Throws timeout_error when deadline is already in the past
 */
TEST_F(WithDeadlineTests, WithDeadlineAlreadyPassed) {
    std::atomic<bool> caught{false};
    auto              deadline = std::chrono::steady_clock::now() - 10ms;

    auto coro_fn = [deadline, &caught]() -> task<void> {
        try {
            (void) co_await with_deadline(
                []() -> task<int> {
                    co_return 1;
                }(),
                deadline);
        } catch (const timeout_error &) {
            caught = true;
        }
    };

    coro_scheduler().spawn(coro_fn());
    run_for(50ms);
    EXPECT_TRUE(caught);
}

/**
 * @test with_deadline fires before operation
 * @brief The operation takes longer than the deadline → timeout_error.
 *        Critical: the operation's coroutine (spawned in when_any) must not
 *        crash after with_deadline's frame is destroyed on exception.
 */
TEST_F(WithDeadlineTests, WithDeadlineTimesOut) {
    std::atomic<bool> caught{false};
    auto              deadline = std::chrono::steady_clock::now() + 30ms;

    auto coro_fn = [deadline, &caught]() -> task<void> {
        try {
            (void) co_await with_deadline(
                []() -> task<int> {
                    co_await sleep(500ms); // much longer than deadline
                    co_return 1;
                }(),
                deadline);
            ADD_FAILURE() << "Expected timeout_error";
        } catch (const timeout_error &) {
            caught = true;
        }
    };

    coro_scheduler().spawn(coro_fn());
    run_for(600ms);
    EXPECT_TRUE(caught);
}

/**
 * @test with_deadline repeated back-to-back
 * @brief Verifies the frame-lifetime fix holds across multiple invocations
 *        without any accumulated state corruption.
 */
TEST_F(WithDeadlineTests, WithDeadlineRepeated) {
    std::atomic<int> successes{0};
    std::atomic<int> timeouts{0};

    auto coro_fn = [&]() -> task<void> {
        // 3 success, 3 timeout
        for (int i = 0; i < 6; ++i) {
            bool should_timeout = (i % 2 != 0);
            auto dl             = std::chrono::steady_clock::now() + (should_timeout ? 10ms : 200ms);
            auto op             = [should_timeout]() -> task<int> {
                if (should_timeout)
                    co_await sleep(500ms);
                co_return 1;
            };
            try {
                (void) co_await with_deadline(op(), dl);
                ++successes;
            } catch (const timeout_error &) {
                ++timeouts;
            }
        }
    };

    coro_scheduler().spawn(coro_fn());
    run_for(2000ms);
    EXPECT_EQ(successes.load(), 3);
    EXPECT_EQ(timeouts.load(), 3);
}

/**
 * @test cancellable_sleep cancelled before starting
 * @brief Token already cancelled → await_ready() returns true, await_resume()
 *        calls throw_if_cancelled() → cancelled_error is thrown immediately,
 *        without waiting 500ms. The coroutine never reaches the sleep.
 */
TEST_F(CancellationCoroutineTests, CancellableSleepPreCancelled) {
    std::atomic<bool>  caught{false};
    cancellation_token token;
    token.cancel(); // cancel BEFORE sleep

    auto coro_fn = [&]() -> task<void> {
        try {
            co_await cancellable_sleep(500ms, token);
            // If we reach here, the sleep completed without throwing — fail.
            ADD_FAILURE() << "Expected cancelled_error";
        } catch (const cancelled_error &) {
            caught = true;
        }
    };

    coro_scheduler().spawn(coro_fn());
    run_for(50ms); // must complete well before 500ms
    EXPECT_TRUE(caught);
}

/**
 * @test make_cancellable back-to-back
 * @brief Multiple sequential cancellable operations on the same token to verify
 *        that the shared_state pattern introduced in cancellable_operation is
 *        safe for reuse without leaking state between calls.
 */
TEST_F(CancellationCoroutineTests, MakeCancellableMultipleSequential) {
    std::atomic<int>   completed{0};
    std::atomic<int>   cancelled_count{0};
    cancellation_token token;

    auto coro_fn = [&]() -> task<void> {
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
    };

    coro_scheduler().spawn(coro_fn());

    // Windows can schedule two successive 20ms sleeps a bit later than 50ms.
    // Wait until 2 completions are observed, or until a generous bound is hit.
    for (int i = 0; i < 12 && completed.load() < 2; ++i) {
        run_for(10ms);
    }
    token.cancel();
    run_for(200ms);

    // At least 2 completed before cancel, rest were cancelled.
    EXPECT_GE(completed.load(), 2);
    EXPECT_GE(cancelled_count.load(), 1);
    EXPECT_EQ(completed.load() + cancelled_count.load(), 4);
}

// =============================================================================
// TEST SUITE: Advanced Cancellation APIs
// =============================================================================

class CancellationAdvancedTests : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

TEST_F(CancellationAdvancedTests, YieldOrCancelYieldsAndThrows) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token;
        int                iterations = 0;
        try {
            for (int i = 0; i < 100; ++i) {
                ++iterations;
                if (i == 3)
                    token.cancel();
                co_await yield_or_cancel(token);
            }
        } catch (const cancelled_error &) {
        }
        EXPECT_EQ(iterations, 4);
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(CancellationAdvancedTests, YieldOrCancelYieldsWithoutCancel) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token;
        int                iterations = 0;
        for (int i = 0; i < 5; ++i) {
            ++iterations;
            co_await yield_or_cancel(token);
        }
        EXPECT_EQ(iterations, 5);
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(CancellationAdvancedTests, WithDeadlineAlreadyCancelledToken) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token;
        token.cancel();
        auto deadline = std::chrono::steady_clock::now() + 5s;
        try {
            co_await with_deadline(
                []() -> task<int> {
                    co_await sleep(100ms);
                    co_return 42;
                }(),
                deadline, token);
            ADD_FAILURE() << "Should throw";
        } catch (const cancelled_error &) {
        }
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
