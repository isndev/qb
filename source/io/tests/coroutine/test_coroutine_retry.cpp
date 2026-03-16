/**
 * @file test_coroutine_retry.cpp
 * @brief Retry mechanism tests
 *
 * Tests for:
 * - with_retry: retry with backoff
 * - retry_policy: configuration
 * - backoff strategies
 *
 * @author qb - C++ Actor Framework
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
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
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
        auto result = co_await with_retry(op, retry_policy{
            .max_attempts = 5,
            .base_delay = 10ms
        });
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
            co_await with_retry(op, retry_policy{
                .max_attempts = 3,
                .base_delay = 10ms
            });
            EXPECT_FALSE(true);  // Should not reach
        } catch (const retry_exhausted& e) {
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
            co_await with_retry(op, retry_policy{
                .max_attempts = 5,
                .base_delay = 10ms,
                .is_retryable = [](const std::exception& e) {
                    return e.what() != std::string("fatal");
                }
            });
            EXPECT_FALSE(true);
        } catch (const std::runtime_error& e) {
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
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Fixed backoff
 * @brief Constant delay between retries
 */
TEST_F(RetryPolicyTests, FixedBackoff) {
    std::atomic<int> attempts{0};
    auto start = std::chrono::steady_clock::now();

    auto op = [&attempts]() -> task<int> {
        attempts++;
        if (attempts < 3) {
            throw std::runtime_error("fail");
        }
        co_return 42;
    };

    auto coro_fn = [&op, &start]() -> task<void> {
        co_await with_retry(op, retry_policy{
            .max_attempts = 3,
            .base_delay = 20ms,
            .strategy = backoff_strategy::fixed
        });
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(200ms);

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 40ms);  // 2 retries * 20ms
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
        co_await with_retry(op, retry_policy{
            .max_attempts = 4,
            .base_delay = 10ms,
            .strategy = backoff_strategy::exponential
        });
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
        co_await with_retry(op, retry_policy{
            .max_attempts = 3,
            .base_delay = 10ms,
            .on_retry = [&callback_count](size_t attempt, const std::exception&) {
                callback_count++;
            }
        });
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    EXPECT_EQ(callback_count, 2);  // 2 retries
}

// =============================================================================
// TEST SUITE: Predefined Policies
// =============================================================================

class PredefinedPolicies : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
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
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
