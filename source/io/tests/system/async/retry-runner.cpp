/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/async/retry-runner.cpp
 * @brief Loop-driven retry execution — `with_retry` / `with_retry_until` / `make_retryable` / `retry`.
 *
 * These tests drive the *execution* side of `retry.h` (the policy/backoff math is unit-tested in
 * unit/coroutine/retry-policy.cpp). Each runner co_awaits the user operation, applies the policy's
 * `is_retryable` predicate + `on_retry` callback, and sleeps `calculate_delay()` between tries — so
 * they need the scheduler and real timers and are SYSTEM tier.
 *
 * Contracts proven:
 *   - success on the first try (1 attempt) and after N transient failures (N attempts, value flows);
 *   - exhaustion → `retry_exhausted` carrying the attempt count + last error (`rethrow_last()`);
 *   - `is_retryable` returning false aborts immediately, rethrowing the ORIGINAL exception;
 *   - `is_retryable` consulted *mid-sequence*: retry the transient ones, abort on a fatal one;
 *   - `on_retry(attempt, exception)` fires once per retry with the correct 1-based index + exception;
 *   - fixed backoff actually attempts 3× and the op succeeds; exponential backoff's delay grows;
 *   - `with_retry_until(predicate)` retries until the result predicate holds / exhausts;
 *   - `make_retryable` builds a reusable retrying callable; `retry` uses the default policy;
 *   - the void overload retries; a NON-`std::exception` throwable (`throw 42`) is still retried.
 *
 * Every wait uses the shared bounded pump `qb::io::test::pump_until` (loud bounded timeout). Where
 * timing is asserted it is measured *inside* the coroutine and checked with tolerance, never via a
 * fixed `run_for` window. `EXPECT_FALSE(true)` unreachable-guards become `ADD_FAILURE()` (the
 * non-returning gtest failure — a bare `FAIL()` injects a `return;`, which is illegal inside a
 * coroutine body).
 *
 * Merged here: coroutine/test-coroutine-retry.cpp (the `with_retry*` half) + the retry regressions
 * from coroutine/test-coroutine-regression.cpp (void overload, non-std-exception throw).
 */

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class RetryRunner : public ::testing::Test {
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
// with_retry — success / retry-then-success / exhaustion / non-retryable
// ---------------------------------------------------------------------------

TEST_F(RetryRunner, SuccessOnFirstAttempt) {
    std::atomic<int>  attempts{0};
    std::atomic<int>  value{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto op = [&attempts]() -> task<int> {
            ++attempts;
            co_return 42;
        };
        value = co_await with_retry(op, retry_policy{.max_attempts = 3});
        done  = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry never completed";
    EXPECT_EQ(attempts.load(), 1);
    EXPECT_EQ(value.load(), 42);
}

TEST_F(RetryRunner, SucceedsAfterTransientFailures) {
    std::atomic<int>  attempts{0};
    std::atomic<int>  value{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto op = [&attempts]() -> task<int> {
            if (++attempts < 3)
                throw std::runtime_error("temporary failure");
            co_return 42;
        };
        value = co_await with_retry(op, retry_policy{.max_attempts = 5, .base_delay = 5ms});
        done  = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry never completed";
    EXPECT_EQ(attempts.load(), 3);
    EXPECT_EQ(value.load(), 42);
}

TEST_F(RetryRunner, ExhaustsAllAttemptsThrowsRetryExhausted) {
    std::atomic<int>  attempts{0};
    std::atomic<bool> caught{false};
    std::atomic<int>  reported_attempts{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto op = [&attempts]() -> task<int> {
            ++attempts;
            throw std::runtime_error("always fails");
            co_return 0;
        };
        try {
            co_await with_retry(op, retry_policy{.max_attempts = 3, .base_delay = 5ms, .strategy = backoff_strategy::fixed});
            ADD_FAILURE() << "with_retry must throw retry_exhausted when all attempts fail";
        } catch (const retry_exhausted &e) {
            caught            = true;
            reported_attempts = static_cast<int>(e.attempts());
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry never finished";
    EXPECT_TRUE(caught.load());
    EXPECT_EQ(attempts.load(), 3);
    EXPECT_EQ(reported_attempts.load(), 3);
}

TEST_F(RetryRunner, NonRetryableErrorAbortsImmediatelyRethrowingOriginal) {
    std::atomic<int>  attempts{0};
    std::atomic<bool> caught_fatal{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto op = [&attempts]() -> task<int> {
            ++attempts;
            throw std::runtime_error("fatal");
            co_return 0;
        };
        try {
            co_await with_retry(op, retry_policy{.max_attempts = 5,
                                                 .base_delay   = 5ms,
                                                 .is_retryable = [](const std::exception &e) {
                                                     return std::string(e.what()) != "fatal";
                                                 }});
            ADD_FAILURE() << "a non-retryable error must abort with_retry";
        } catch (const std::runtime_error &e) {
            caught_fatal = std::string(e.what()) == "fatal";
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry never finished";
    EXPECT_EQ(attempts.load(), 1) << "non-retryable error must not be retried";
    EXPECT_TRUE(caught_fatal.load()) << "the ORIGINAL exception must be rethrown, not retry_exhausted";
}

TEST_F(RetryRunner, IsRetryableConsultedMidSequence) {
    // Two transient failures (retried), then a fatal one mid-sequence that aborts the runner before
    // exhausting max_attempts.
    std::atomic<int>  attempts{0};
    std::atomic<bool> caught_fatal{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto op = [&attempts]() -> task<int> {
            const int n = ++attempts;
            if (n <= 2)
                throw std::runtime_error("transient");
            throw std::runtime_error("fatal-mid-sequence");
            co_return 0;
        };
        try {
            co_await with_retry(op, retry_policy{.max_attempts = 10,
                                                 .base_delay   = 2ms,
                                                 .strategy     = backoff_strategy::fixed,
                                                 .is_retryable = [](const std::exception &e) {
                                                     return std::string(e.what()) == "transient";
                                                 }});
            ADD_FAILURE() << "the fatal mid-sequence error must abort with_retry";
        } catch (const std::runtime_error &e) {
            caught_fatal = std::string(e.what()) == "fatal-mid-sequence";
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry never finished";
    EXPECT_EQ(attempts.load(), 3) << "two transient retries then one fatal attempt";
    EXPECT_TRUE(caught_fatal.load());
}

// ---------------------------------------------------------------------------
// Backoff behaviour — fixed (succeeds + tolerance timing), exponential (grows)
// ---------------------------------------------------------------------------

TEST_F(RetryRunner, FixedBackoffSucceedsAndWaitsBaseDelayPerRetry) {
    std::atomic<int>          attempts{0};
    std::atomic<int>          value{0};
    std::atomic<bool>         done{false};
    std::chrono::milliseconds elapsed{0};

    coro_scheduler().spawn([&]() -> task<void> {
        auto op = [&attempts]() -> task<int> {
            if (++attempts < 3)
                throw std::runtime_error("fail");
            co_return 42;
        };
        const auto start = std::chrono::steady_clock::now();
        value            = co_await with_retry(
            op, retry_policy{.max_attempts = 3, .base_delay = 20ms, .strategy = backoff_strategy::fixed});
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        done    = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "fixed-backoff retry never completed";
    EXPECT_EQ(attempts.load(), 3) << "must attempt exactly 3 times";
    EXPECT_EQ(value.load(), 42) << "the operation must ultimately succeed";
    // Two retries × 20ms fixed delay = at least ~40ms; allow scheduling slack on the upper bound.
    EXPECT_GE(elapsed, 38ms) << "fixed backoff must sleep base_delay before each retry";
    EXPECT_LT(elapsed, 1000ms);
}

TEST_F(RetryRunner, ExponentialBackoffDelayGrows) {
    std::atomic<int>          attempts{0};
    std::atomic<bool>         done{false};
    std::chrono::milliseconds elapsed{0};

    coro_scheduler().spawn([&]() -> task<void> {
        auto op = [&attempts]() -> task<int> {
            if (++attempts < 4)
                throw std::runtime_error("fail");
            co_return 42;
        };
        const auto start = std::chrono::steady_clock::now();
        co_await with_retry(op, retry_policy{.max_attempts = 4, .base_delay = 10ms, .strategy = backoff_strategy::exponential});
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        done    = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 4000ms)) << "exponential-backoff retry never completed";
    EXPECT_EQ(attempts.load(), 4);
    // Delays before retries 1..3 = 10 + 20 + 40 = 70ms minimum (exponential grows, not constant).
    EXPECT_GE(elapsed, 65ms) << "exponential backoff delay must grow across retries";
}

// ---------------------------------------------------------------------------
// on_retry callback — fires per retry with the correct 1-based index + exception
// ---------------------------------------------------------------------------

TEST_F(RetryRunner, OnRetryReceivesCorrectIndexAndException) {
    std::atomic<int>         attempts{0};
    std::atomic<bool>        done{false};
    std::vector<size_t>      retry_indices;
    std::vector<std::string> retry_messages;

    coro_scheduler().spawn([&]() -> task<void> {
        auto op = [&attempts]() -> task<int> {
            if (++attempts < 3)
                throw std::runtime_error("flaky-" + std::to_string(attempts.load()));
            co_return 42;
        };
        co_await with_retry(op, retry_policy{.max_attempts = 3,
                                             .base_delay   = 2ms,
                                             .strategy     = backoff_strategy::fixed,
                                             .on_retry     = [&](size_t attempt, const std::exception &e) {
                                                 retry_indices.push_back(attempt);
                                                 retry_messages.emplace_back(e.what());
                                             }});
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry never completed";
    ASSERT_EQ(retry_indices.size(), 2u) << "two failures → on_retry fires twice";
    EXPECT_EQ(retry_indices[0], 1u) << "1-based attempt index";
    EXPECT_EQ(retry_indices[1], 2u);
    EXPECT_EQ(retry_messages[0], "flaky-1");
    EXPECT_EQ(retry_messages[1], "flaky-2");
}

// ---------------------------------------------------------------------------
// with_retry_until — result-predicate retry
// ---------------------------------------------------------------------------

TEST_F(RetryRunner, WithRetryUntilSucceedsWhenPredicateMet) {
    std::atomic<int>  value{0};
    std::atomic<int>  calls{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        value = co_await with_retry_until([&]() -> task<int> { co_return ++calls; }, [](int v) { return v >= 3; },
                                          retry_policy{.max_attempts = 10, .base_delay = 1ms, .strategy = backoff_strategy::fixed});
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry_until never completed";
    EXPECT_EQ(value.load(), 3);
    EXPECT_EQ(calls.load(), 3);
}

TEST_F(RetryRunner, WithRetryUntilExhaustsAttempts) {
    std::atomic<bool> caught{false};
    std::atomic<int>  reported_attempts{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await with_retry_until([]() -> task<int> { co_return 0; }, [](int v) { return v > 0; },
                                      retry_policy{.max_attempts = 3, .base_delay = 1ms, .strategy = backoff_strategy::fixed});
            ADD_FAILURE() << "with_retry_until must exhaust when the predicate never holds";
        } catch (const retry_exhausted &e) {
            caught            = true;
            reported_attempts = static_cast<int>(e.attempts());
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry_until never finished";
    EXPECT_TRUE(caught.load());
    EXPECT_EQ(reported_attempts.load(), 3);
}

// ---------------------------------------------------------------------------
// make_retryable / retry (default policy)
// ---------------------------------------------------------------------------

TEST_F(RetryRunner, MakeRetryableBuildsReusableRetryingCallable) {
    std::atomic<int>  calls{0};
    std::atomic<int>  value{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto retryable = make_retryable(
            [&]() -> task<int> {
                if (++calls < 3)
                    throw std::runtime_error("fail");
                co_return 42;
            },
            retry_policy{.max_attempts = 5, .base_delay = 1ms, .strategy = backoff_strategy::fixed});
        value = co_await retryable();
        done  = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "make_retryable never completed";
    EXPECT_EQ(value.load(), 42);
    EXPECT_EQ(calls.load(), 3);
}

TEST_F(RetryRunner, RetryDefaultPolicySucceeds) {
    std::atomic<int>  calls{0};
    std::atomic<int>  value{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        value = co_await retry([&]() -> task<int> {
            if (++calls < 2)
                throw std::runtime_error("transient");
            co_return 99;
        });
        done = true;
    });

    // The default policy's first backoff is exponential from 100ms; give the pump headroom.
    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 4000ms)) << "retry(default) never completed";
    EXPECT_EQ(value.load(), 99);
    EXPECT_EQ(calls.load(), 2);
}

// ---------------------------------------------------------------------------
// retry_exhausted accessors
// ---------------------------------------------------------------------------

TEST_F(RetryRunner, RetryExhaustedExposesAttemptsAndLastError) {
    std::atomic<int>  reported_attempts{0};
    std::atomic<bool> last_error_nonnull{false};
    std::atomic<bool> rethrow_matched{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await with_retry(
                []() -> task<int> {
                    throw std::runtime_error("test-err");
                    co_return 0;
                },
                retry_policy{.max_attempts = 2, .base_delay = 1ms, .strategy = backoff_strategy::fixed});
            ADD_FAILURE() << "with_retry must throw retry_exhausted";
        } catch (const retry_exhausted &e) {
            reported_attempts  = static_cast<int>(e.attempts());
            last_error_nonnull = (e.last_error() != nullptr);
            try {
                e.rethrow_last();
                ADD_FAILURE() << "rethrow_last must re-throw the original error";
            } catch (const std::runtime_error &inner) {
                rethrow_matched = std::string(inner.what()) == "test-err";
            }
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry never finished";
    EXPECT_EQ(reported_attempts.load(), 2);
    EXPECT_TRUE(last_error_nonnull.load());
    EXPECT_TRUE(rethrow_matched.load());
}

// ---------------------------------------------------------------------------
// Regressions absorbed from test-coroutine-regression.cpp
// ---------------------------------------------------------------------------

TEST_F(RetryRunner, VoidOverloadRetries) {
    std::atomic<int>  attempts{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await with_retry(
            [&attempts]() -> task<void> {
                if (++attempts < 2)
                    throw std::runtime_error("fail");
                co_return;
            },
            retry_policy{.max_attempts = 3, .base_delay = 5ms});
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry(void) never completed";
    EXPECT_EQ(attempts.load(), 2);
}

/**
 * @regression Finding 2.C.14: a NON-`std::exception` throwable (`throw 42`) must not bypass retry
 * bookkeeping — it is captured and retried like any other failure, ending in `retry_exhausted`.
 */
TEST_F(RetryRunner, CatchesNonStdExceptionThrowable) {
    std::atomic<int>  attempts{0};
    std::atomic<bool> caught_exhausted{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto flaky = [&attempts]() -> task<int> {
            ++attempts;
            throw 42; // int — not a std::exception
            co_return 0;
        };
        try {
            (void) co_await with_retry(flaky, retry_policy{.max_attempts = 3, .base_delay = 1ms, .max_delay = 5ms, .strategy = backoff_strategy::fixed});
        } catch (const retry_exhausted &) {
            caught_exhausted = true;
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry never finished";
    EXPECT_EQ(attempts.load(), 3);
    EXPECT_TRUE(caught_exhausted.load()) << "with_retry must treat non-std::exception throwables as retryable";
}

// ---------------------------------------------------------------------------
// on_retry is NOT fired on the budget-exhausting attempt → exactly max_attempts-1 notifications.
// Complements OnRetryReceivesCorrectIndexAndException (which succeeds on the last try); here EVERY
// attempt fails, so the count must still be max_attempts-1 (the off-by-one regression fired it
// max_attempts times, notifying on the final permanent failure that has no retry after it).
// ---------------------------------------------------------------------------

TEST_F(RetryRunner, OnRetryDoesNotFireOnBudgetExhaustingAttempt) {
    std::atomic<int>  attempts{0};
    std::atomic<int>  notifications{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await with_retry(
                [&]() -> task<void> {
                    ++attempts;
                    throw std::runtime_error("always fails");
                    co_return;
                },
                retry_policy{.max_attempts = 3,
                             .base_delay   = 1ms,
                             .strategy     = backoff_strategy::fixed,
                             .on_retry     = [&](size_t, const std::exception &) { ++notifications; }});
        } catch (...) {
            // exhausted → retry_exhausted; expected
        }
        done = true;
    });

    EXPECT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "with_retry never completed";
    EXPECT_EQ(attempts.load(), 3) << "should attempt max_attempts times";
    EXPECT_EQ(notifications.load(), 2) << "on_retry must fire max_attempts-1 times, not on the final failure";
}
