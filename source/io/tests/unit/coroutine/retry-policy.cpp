/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/retry-policy.cpp
 * @brief Retry policy fields + `detail::calculate_delay` backoff math — pure logic, no event loop.
 *
 * `retry.h` cleanly separates *what to retry* (the `retry_policy` struct: `max_attempts`,
 * `base_delay`, `max_delay`, `strategy`, `is_retryable`, `on_retry`) and *how long to wait between
 * tries* (`detail::calculate_delay(retry_number, policy)`) from the loop-driven *execution*
 * (`with_retry` / `with_retry_until` / `make_retryable` / `retry`, tested in
 * system/async/retry-runner.cpp). Everything in THIS file is a deterministic, allocation-only
 * computation — no scheduler, no `sleep`, no `run_for` — hence true UNIT tier.
 *
 * The `calculate_delay` contract (1-based `retry_number`, Findings 2.C.2 / 2.C.13):
 *   - fixed:       base_delay (constant)
 *   - linear:      base_delay * retry_number  (retry #1 = base_delay, NOT 0ms)
 *   - exponential: base_delay * 2^(retry_number-1)
 *   - exponential_jitter: exponential + 0..50% jitter
 * with every strategy hard-clamped to `[0, max_delay]` and 64-bit-overflow-safe for huge inputs.
 *
 * The predefined policies (`transient_network_policy`, `idempotent_policy`,
 * `aggressive_retry_policy`) are asserted on their exact fields and `is_retryable` decisions.
 *
 * Merged here from coroutine/test-coroutine-retry.cpp (the predefined-policy unit half) and the
 * backoff-math regressions formerly in coroutine/test-coroutine-regression.cpp (overflow / large
 * attempt / linear-first-retry). Bug docstrings are preserved as regression markers.
 */

#include <chrono>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

// ===========================================================================
// detail::calculate_delay — strategy math (1-based retry_number)
// ===========================================================================

TEST(RetryBackoffMath, FixedIsConstant) {
    retry_policy policy{.base_delay = 20ms, .max_delay = 1s, .strategy = backoff_strategy::fixed};
    EXPECT_EQ(detail::calculate_delay(1, policy), 20ms);
    EXPECT_EQ(detail::calculate_delay(2, policy), 20ms);
    EXPECT_EQ(detail::calculate_delay(5, policy), 20ms);
}

TEST(RetryBackoffMath, LinearGrowsByRetryNumber) {
    retry_policy policy{.base_delay = 10ms, .max_delay = 10s, .strategy = backoff_strategy::linear};
    EXPECT_EQ(detail::calculate_delay(1, policy), 10ms);
    EXPECT_EQ(detail::calculate_delay(2, policy), 20ms);
    EXPECT_EQ(detail::calculate_delay(3, policy), 30ms);
}

/**
 * @regression Finding 2.C.2: linear backoff's first retry must wait base_delay (1-based
 * retry_number), not 0ms — a 0ms first delay tight-spins.
 */
TEST(RetryBackoffMath, LinearFirstRetryUsesBaseDelayNotZero) {
    retry_policy policy{.base_delay = 25ms, .max_delay = 1s, .strategy = backoff_strategy::linear};
    EXPECT_EQ(detail::calculate_delay(1, policy), 25ms) << "first retry must sleep at least base_delay";
    EXPECT_EQ(detail::calculate_delay(2, policy), 50ms);
}

TEST(RetryBackoffMath, ExponentialDoublesEachRetry) {
    retry_policy policy{.base_delay = 10ms, .max_delay = 10s, .strategy = backoff_strategy::exponential};
    EXPECT_EQ(detail::calculate_delay(1, policy), 10ms); // 10 * 2^0
    EXPECT_EQ(detail::calculate_delay(2, policy), 20ms); // 10 * 2^1
    EXPECT_EQ(detail::calculate_delay(3, policy), 40ms); // 10 * 2^2
    EXPECT_EQ(detail::calculate_delay(4, policy), 80ms); // 10 * 2^3
}

TEST(RetryBackoffMath, DelayIsClampedToMaxDelay) {
    retry_policy policy{.base_delay = 10ms, .max_delay = 35ms, .strategy = backoff_strategy::exponential};
    // 10, 20, 40→clamp 35, 80→clamp 35
    EXPECT_EQ(detail::calculate_delay(1, policy), 10ms);
    EXPECT_EQ(detail::calculate_delay(2, policy), 20ms);
    EXPECT_EQ(detail::calculate_delay(3, policy), 35ms);
    EXPECT_EQ(detail::calculate_delay(8, policy), 35ms);
}

TEST(RetryBackoffMath, RetryNumberBelowOneIsClampedToOne) {
    retry_policy policy{.base_delay = 10ms, .max_delay = 1s, .strategy = backoff_strategy::exponential};
    EXPECT_EQ(detail::calculate_delay(0, policy), detail::calculate_delay(1, policy));
}

/**
 * @regression exponential `1 << attempt` was UB for attempt >= 31; now the shift is clamped to 30
 * and computed in 64-bit before the max_delay clamp.
 */
TEST(RetryBackoffMath, ExponentialHugeRetryNumberNoOverflowClampsToMax) {
    auto delay = detail::calculate_delay(
        50, retry_policy{.base_delay = 10ms, .max_delay = std::chrono::hours(24), .strategy = backoff_strategy::exponential});
    EXPECT_LE(delay, std::chrono::hours(24));
    EXPECT_GT(delay, 0ms);
}

/**
 * @regression exponential_jitter must not overflow on a huge retry number; jitter keeps the delay
 * inside (0, max_delay].
 */
TEST(RetryBackoffMath, ExponentialJitterHugeRetryNumberNoOverflowWithinBounds) {
    retry_policy policy{.base_delay = 1ms, .max_delay = 60s, .strategy = backoff_strategy::exponential_jitter};
    for (int i = 0; i < 64; ++i) {
        auto delay = detail::calculate_delay(100, policy);
        EXPECT_GT(delay, 0ms);
        EXPECT_LE(delay, 60s);
    }
}

/**
 * @regression linear `static_cast<int>(attempt)` truncated large values; now clamps in 64-bit.
 */
TEST(RetryBackoffMath, LinearHugeRetryNumberNoOverflowClampsToMax) {
    auto delay = detail::calculate_delay(
        100000, retry_policy{.base_delay = 1ms, .max_delay = std::chrono::seconds(30), .strategy = backoff_strategy::linear});
    EXPECT_LE(delay, std::chrono::seconds(30));
    EXPECT_GT(delay, 0ms);
}

TEST(RetryBackoffMath, JitterStaysWithinExponentialToOnePointFiveBand) {
    // For a single un-clamped step, jitter is 0..50% of the exponential base. Sample many draws and
    // assert the band [base, base*1.5] holds every time (deterministic envelope, random within it).
    retry_policy policy{.base_delay = 100ms, .max_delay = 10s, .strategy = backoff_strategy::exponential_jitter};
    for (int i = 0; i < 256; ++i) {
        auto delay = detail::calculate_delay(1, policy); // exponential base = 100ms
        EXPECT_GE(delay, 100ms);
        EXPECT_LE(delay, 150ms);
    }
}

// ===========================================================================
// Predefined policies — exact fields + is_retryable decisions
// ===========================================================================

TEST(RetryPredefinedPolicy, TransientNetworkFiltersByMessage) {
    auto policy = transient_network_policy();

    EXPECT_EQ(policy.max_attempts, 5u);
    EXPECT_EQ(policy.strategy, backoff_strategy::exponential_jitter);

    // Retryable: substrings the predicate whitelists.
    EXPECT_TRUE(policy.is_retryable(std::runtime_error("connection timeout")));
    EXPECT_TRUE(policy.is_retryable(std::runtime_error("connection reset by peer")));
    EXPECT_TRUE(policy.is_retryable(std::runtime_error("service temporarily unavailable")));

    // Not retryable: an auth error is none of the network substrings.
    EXPECT_FALSE(policy.is_retryable(std::runtime_error("auth failed")));
}

TEST(RetryPredefinedPolicy, IdempotentRetriesEverything) {
    auto policy = idempotent_policy();

    EXPECT_EQ(policy.max_attempts, 10u);
    EXPECT_EQ(policy.strategy, backoff_strategy::exponential_jitter);
    EXPECT_TRUE(policy.is_retryable(std::runtime_error("any error")));
    EXPECT_TRUE(policy.is_retryable(std::runtime_error("auth failed")));
}

TEST(RetryPredefinedPolicy, AggressiveIsLinearAndRetriesEverything) {
    auto policy = aggressive_retry_policy();

    EXPECT_EQ(policy.max_attempts, 20u);
    EXPECT_EQ(policy.base_delay, 10ms);
    EXPECT_EQ(policy.strategy, backoff_strategy::linear);
    EXPECT_TRUE(policy.is_retryable(std::runtime_error("any")));
}

TEST(RetryPredefinedPolicy, DefaultPolicyFields) {
    retry_policy policy{};
    EXPECT_EQ(policy.max_attempts, 3u);
    EXPECT_EQ(policy.strategy, backoff_strategy::exponential);
    EXPECT_TRUE(policy.is_retryable(std::runtime_error("default retries by default")));
}
