/**
 * @file qb/io/tests/coroutine/test-coroutine-retry.cpp
 * @brief Coroutine retry policy tests
 *
 * This file contains tests for retry helpers and retry policies, including success and
 * exhaustion paths, non-retryable failures, fixed and exponential backoff, retry
 * callbacks, predefined policies, and retry wrapper helpers.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
// TEST SUITE: Basic Retry
// =============================================================================

class RetryBasicTests : public ::testing::Test {
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

/**
 * @test Success on first attempt
 * @brief No retry needed
 */
TEST_F(RetryBasicTests, SuccessFirstAttempt) {
    std::atomic<int> attempts{0};

    auto op = [&attempts]() -> task<int> {
        attempts++;
        co_return 42;
    };

    auto coro_fn = [&op]() -> task<void> {
        auto result = co_await with_retry(op, retry_policy{.max_attempts = 3});
        EXPECT_EQ(result, 42);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    EXPECT_EQ(attempts, 1);
}

/**
 * @test Success after retries
 * @brief Eventually succeeds
 */
TEST_F(RetryBasicTests, SuccessAfterRetries) {
    std::atomic<int> attempts{0};

    auto op = [&attempts]() -> task<int> {
        attempts++;
        if (attempts < 3) {
            throw std::runtime_error("temporary failure");
        }
        co_return 42;
    };

    auto coro_fn = [&op]() -> task<void> {
        auto result = co_await with_retry(op, retry_policy{.max_attempts = 5, .base_delay = 10ms});
        EXPECT_EQ(result, 42);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(200ms);

    EXPECT_EQ(attempts, 3);
}

/**
 * @test Exhaust all retries
 * @brief Throws after max attempts
 */
TEST_F(RetryBasicTests, ExhaustRetries) {
    std::atomic<int> attempts{0};

    auto op = [&attempts]() -> task<int> {
        attempts++;
        throw std::runtime_error("always fails");
    };

    auto coro_fn = [&op]() -> task<void> {
        try {
            co_await with_retry(op, retry_policy{.max_attempts = 3, .base_delay = 10ms});
            EXPECT_FALSE(true); // Should not reach
        } catch (const retry_exhausted &e) {
            EXPECT_EQ(e.attempts(), 3);
        }
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    EXPECT_EQ(attempts, 3);
}

/**
 * @test Non-retryable error
 * @brief Fail immediately
 */
TEST_F(RetryBasicTests, NonRetryableError) {
    std::atomic<int> attempts{0};

    auto op = [&attempts]() -> task<int> {
        attempts++;
        throw std::runtime_error("fatal");
    };

    auto coro_fn = [&op]() -> task<void> {
        try {
            co_await with_retry(op, retry_policy{.max_attempts = 5, .base_delay = 10ms, .is_retryable = [](const std::exception &e) {
                                                     return e.what() != std::string("fatal");
                                                 }});
            EXPECT_FALSE(true);
        } catch (const std::runtime_error &e) {
            EXPECT_EQ(e.what(), std::string("fatal"));
        }
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    EXPECT_EQ(attempts, 1);
}

// =============================================================================
// TEST SUITE: Retry Policies
// =============================================================================

class RetryPolicyTests : public ::testing::Test {
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

/**
 * @test Fixed backoff
 * @brief Constant delay between retries
 */
TEST_F(RetryPolicyTests, FixedBackoff) {
    std::atomic<int> attempts{0};
    auto             start = std::chrono::steady_clock::now();

    auto op = [&attempts]() -> task<int> {
        attempts++;
        if (attempts < 3) {
            throw std::runtime_error("fail");
        }
        co_return 42;
    };

    auto coro_fn = [&op]() -> task<void> {
        co_await with_retry(op, retry_policy{.max_attempts = 3, .base_delay = 20ms, .strategy = backoff_strategy::fixed});
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(200ms);

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 40ms); // 2 retries * 20ms
}

/**
 * @test Exponential backoff
 * @brief Delay doubles each retry
 */
TEST_F(RetryPolicyTests, ExponentialBackoff) {
    std::atomic<int> attempts{0};

    auto op = [&attempts]() -> task<int> {
        attempts++;
        if (attempts < 4) {
            throw std::runtime_error("fail");
        }
        co_return 42;
    };

    auto coro_fn = [&op]() -> task<void> {
        co_await with_retry(op, retry_policy{.max_attempts = 4, .base_delay = 10ms, .strategy = backoff_strategy::exponential});
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(300ms);

    EXPECT_EQ(attempts, 4);
}

/**
 * @test Retry callback
 * @brief Called on each retry
 */
TEST_F(RetryPolicyTests, RetryCallback) {
    std::atomic<int> attempts{0};
    std::atomic<int> callback_count{0};

    auto op = [&attempts]() -> task<int> {
        attempts++;
        if (attempts < 3) {
            throw std::runtime_error("fail");
        }
        co_return 42;
    };

    auto coro_fn = [&op, &callback_count]() -> task<void> {
        co_await with_retry(
            op, retry_policy{.max_attempts = 3, .base_delay = 10ms, .on_retry = [&callback_count](size_t attempt, const std::exception &) {
                                 callback_count++;
                             }});
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    EXPECT_EQ(callback_count, 2); // 2 retries
}

// =============================================================================
// TEST SUITE: Predefined Policies
// =============================================================================

class PredefinedPolicies : public ::testing::Test {
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

/**
 * @test Transient network policy
 * @brief Filters network errors
 */
TEST_F(PredefinedPolicies, TransientNetworkPolicy) {
    auto policy = transient_network_policy();

    // Should retry on timeout
    EXPECT_TRUE(policy.is_retryable(std::runtime_error("connection timeout")));

    // Should not retry on auth error
    EXPECT_FALSE(policy.is_retryable(std::runtime_error("auth failed")));
}

/**
 * @test Idempotent policy
 * @brief Retries all errors
 */
TEST_F(PredefinedPolicies, IdempotentPolicy) {
    auto policy = idempotent_policy();

    EXPECT_TRUE(policy.is_retryable(std::runtime_error("any error")));
    EXPECT_EQ(policy.max_attempts, 10);
}

// =============================================================================
// TEST SUITE: Advanced Retry APIs
// =============================================================================

class RetryAdvancedTests : public ::testing::Test {
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

TEST_F(RetryAdvancedTests, WithRetryUntilPredicateSuccess) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        int  call_count = 0;
        auto result     = co_await with_retry_until([&]() -> task<int> { co_return ++call_count; }, [](int v) { return v >= 3; },
                                                    retry_policy{.max_attempts = 10, .base_delay = 1ms, .strategy = backoff_strategy::fixed});
        EXPECT_EQ(result, 3);
        EXPECT_EQ(call_count, 3);
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(RetryAdvancedTests, WithRetryUntilExhaustsAttempts) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await with_retry_until([]() -> task<int> { co_return 0; }, [](int v) { return v > 0; },
                                      retry_policy{.max_attempts = 3, .base_delay = 1ms, .strategy = backoff_strategy::fixed});
            ADD_FAILURE() << "Should have thrown retry_exhausted";
        } catch (const retry_exhausted &e) {
            EXPECT_EQ(e.attempts(), 3u);
        }
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(RetryAdvancedTests, MakeRetryableWrapper) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        int  call_count   = 0;
        auto retryable_fn = make_retryable(
            [&]() -> task<int> {
                if (++call_count < 3)
                    throw std::runtime_error("fail");
                co_return 42;
            },
            retry_policy{.max_attempts = 5, .base_delay = 1ms, .strategy = backoff_strategy::fixed});
        auto result = co_await retryable_fn();
        EXPECT_EQ(result, 42);
        EXPECT_EQ(call_count, 3);
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(RetryAdvancedTests, RetryDefaultPolicy) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        int  call_count = 0;
        auto result     = co_await retry([&]() -> task<int> {
            if (++call_count < 2)
                throw std::runtime_error("transient");
            co_return 99;
        });
        EXPECT_EQ(result, 99);
        done = true;
    });
    run_for(2000ms);
    EXPECT_TRUE(done);
}

TEST_F(RetryAdvancedTests, AggressiveRetryPolicy) {
    auto policy = aggressive_retry_policy();
    EXPECT_EQ(policy.max_attempts, 20u);
    EXPECT_EQ(policy.strategy, backoff_strategy::linear);
    EXPECT_TRUE(policy.is_retryable(std::runtime_error("any")));
}

TEST_F(RetryAdvancedTests, RetryExhaustedAccessors) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await with_retry(
                []() -> task<int> {
                    throw std::runtime_error("test-err");
                    co_return 0;
                },
                retry_policy{.max_attempts = 2, .base_delay = 1ms, .strategy = backoff_strategy::fixed});
            ADD_FAILURE() << "Should have thrown retry_exhausted";
        } catch (const retry_exhausted &e) {
            EXPECT_EQ(e.attempts(), 2u);
            EXPECT_NE(e.last_error(), nullptr);
            try {
                e.rethrow_last();
                ADD_FAILURE() << "Should have thrown";
            } catch (const std::runtime_error &inner) {
                EXPECT_STREQ(inner.what(), "test-err");
            }
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
