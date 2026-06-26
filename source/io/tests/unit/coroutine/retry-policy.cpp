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
#include <limits>
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

/**
 * @regression exponential overflow GUARD (not the max_delay clamp): when base_delay is so large
 * that base_ms * 2^shift would overflow int64 milliseconds, the guard `factor > max_rep/base_ms`
 * trips and the delay saturates to max_delay BEFORE the multiply. base_delay ≈ 1e10 ms makes
 * base_ms * 2^30 overflow (≈ 1e19 > int64 max), exercising the overflow branch — distinct from the
 * ordinary "product fits but exceeds max_delay" clamp the other tests hit.
 */
TEST(RetryBackoffMath, ExponentialOverflowGuardSaturatesToMaxDelay) {
    using namespace std::chrono;
    retry_policy policy{.base_delay = milliseconds(10'000'000'000LL), // 1e10 ms
                        .max_delay  = hours(24),
                        .strategy   = backoff_strategy::exponential};
    // retry_number 31+ -> shift clamps to 30 -> factor 2^30; base_ms*factor overflows -> max_delay.
    auto delay = detail::calculate_delay(40, policy);
    EXPECT_EQ(delay, hours(24)) << "an overflowing exponential step must saturate to max_delay, not wrap";
}

/**
 * @regression exponential_jitter shares the same pre-multiply overflow guard as exponential: a
 * huge base_delay must saturate to max_delay (the post-clamp jitter then stays within [max, max]
 * since the pre-jitter delay is already at max).
 */
TEST(RetryBackoffMath, ExponentialJitterOverflowGuardSaturatesToMaxDelay) {
    using namespace std::chrono;
    retry_policy policy{.base_delay = milliseconds(10'000'000'000LL),
                        .max_delay  = hours(24),
                        .strategy   = backoff_strategy::exponential_jitter};
    for (int i = 0; i < 32; ++i) {
        auto delay = detail::calculate_delay(40, policy);
        // Pre-jitter delay is clamped to max_delay; jitter on a value already == max cannot grow it.
        EXPECT_EQ(delay, hours(24)) << "an overflowing jitter step must saturate to max_delay";
    }
}

/**
 * @regression final negative clamp (`if (delay_ms < 0) delay_ms = 0;`): a negative max_delay makes
 * max_ms < 0, so the positive base delay is first clamped DOWN to the negative max, then floored to
 * 0 — calculate_delay must never return a negative duration regardless of policy fields.
 */
TEST(RetryBackoffMath, NegativeMaxDelayFloorsToZero) {
    using namespace std::chrono;
    retry_policy policy{.base_delay = 10ms, .max_delay = milliseconds(-500), .strategy = backoff_strategy::fixed};
    EXPECT_EQ(detail::calculate_delay(1, policy), 0ms) << "a negative max_delay must floor the result at 0, never negative";
    EXPECT_GE(detail::calculate_delay(3, policy), 0ms);
}

/**
 * @test linear multiply-then-clamp saturates to max_delay for a large representable base.
 *
 * NOTE on the retry.h:142 linear PRE-MULTIPLY overflow guard (`if (base_ms != 0 && mult >
 * max_rep/base_ms) delay_ms = max_ms`): that guard is UNREACHABLE through the public retry_policy
 * interface. policy fields are qb::duration == std::chrono::nanoseconds, so the LARGEST base_delay
 * storable without overflowing the int64 nanosecond field is INT64_MAX/1e6 ≈ 9.22e12 ms; with the
 * multiplier clamped to 10000 (retry.h:141), base_ms*mult tops out at ~9.22e16 — well under
 * INT64_MAX (9.22e18). So base_ms*mult never overflows for any representable base, and the guard's
 * `delay_ms = max_ms` branch can never be taken; saturation always comes from the trailing clamp.
 * The previous version fed base_delay = 1e18 ms, which overflows the nanosecond field on assignment
 * (1e18 ms = 1e24 ns > INT64_MAX) — UBSan flags the conversion multiply, and the policy is garbage.
 * We therefore test the REACHABLE contract: a large-but-representable base whose computed step
 * (base*mult, no overflow, no guard) overshoots a smaller max_delay and is pinned by the clamp.
 * Distinct from LinearHugeRetryNumberNoOverflowClampsToMax, which uses a tiny base + a retry number
 * large enough to hit the mult clamp; here the multiplier is small and the BASE is what's large.
 */
TEST(RetryBackoffMath, LinearLargeBaseMultiplyThenClampSaturates) {
    using namespace std::chrono;
    // base_ms = 9e9 (9e18 ns, representable). retry_number 20 -> mult 20 -> product 1.8e11 ms (fits
    // int64, no guard), which overshoots max_delay = 1e9 ms, so the trailing clamp saturates it.
    const milliseconds big_max{1'000'000'000LL};
    retry_policy       policy{.base_delay = milliseconds(9'000'000'000LL),
                              .max_delay  = big_max,
                              .strategy   = backoff_strategy::linear};
    EXPECT_EQ(detail::calculate_delay(20, policy), big_max)
        << "a linear step (base*mult, computed without overflow) above max_delay must clamp to max_delay";
    // Even mult=2 already overshoots max_delay (2 * 9e9 ms >> 1e9 ms), so it saturates too.
    EXPECT_EQ(detail::calculate_delay(2, policy), big_max);
}

/**
 * @test exponential growth without overflow but exceeding a modest max_delay still saturates at the
 * trailing clamp while exercising the exponential case's non-overflow multiply path for a mid-range
 * retry number (shift 1..29) distinct from the shift-clamped/overflow cases above. base_ms*2^shift
 * stays well within int64 here, so the overflow GUARD is NOT taken — the value is produced by the
 * real multiply and then clamped.
 */
TEST(RetryBackoffMath, ExponentialMidRangeMultiplyThenClamp) {
    using namespace std::chrono;
    retry_policy policy{.base_delay = 1ms, .max_delay = milliseconds(100), .strategy = backoff_strategy::exponential};
    EXPECT_EQ(detail::calculate_delay(5, policy), milliseconds(16));  // 1 * 2^4, fits and < max
    EXPECT_EQ(detail::calculate_delay(8, policy), milliseconds(100)); // 1 * 2^7 = 128 -> clamp to 100
}

/**
 * @test exponential_jitter saturates to max_delay (pre-jitter clamp + trailing clamp) and the
 * jitter multiply never wraps for any representable policy.
 *
 * NOTE on the retry.h:178 jitter-multiply overflow guard (`if (jitter_pct != 0 && delay_ms >
 * max/jitter_pct/2) delay_ms = max_ms`): that guard is UNREACHABLE through the public retry_policy
 * interface. policy fields are qb::duration == std::chrono::nanoseconds, so the LARGEST max_delay
 * that can be stored without overflowing the int64 nanosecond field is INT64_MAX/1e6 ≈ 9.22e12 ms.
 * After duration_cast, max_ms ≤ 9.22e12, and the smallest guard threshold (jitter_pct == 1) is
 * INT64_MAX/1/2 ≈ 4.6e18 ms — so delay_ms * jitter_pct (≤ 9.22e12 * 49 ≈ 4.5e14) can never overflow.
 * The previous version of this test fed base_delay = 1e15 ms, which itself overflows the nanosecond
 * field on assignment (1e15 ms = 1e24 ns > INT64_MAX), corrupting the policy so calculate_delay
 * returned 0ns — a false premise, not a real saturation. We therefore test the REACHABLE contract:
 * a large-but-representable max_delay with a base whose exponential factor blows past it, so every
 * draw lands on the pre-jitter clamp and the trailing clamp pins the result to exactly max_delay.
 */
TEST(RetryBackoffMath, ExponentialJitterSaturatesToMaxDelay) {
    using namespace std::chrono;
    // Largest comfortably-representable max_delay (9e12 ms = 9e21 ns < INT64_MAX) and a base whose
    // 2^shift factor overflows past it, so the pre-jitter exponential is clamped to max_ms first.
    const milliseconds big_max{9'000'000'000'000LL};
    retry_policy       policy{.base_delay = milliseconds(1'000'000'000LL),
                              .max_delay  = big_max,
                              .strategy   = backoff_strategy::exponential_jitter};
    for (int i = 0; i < 64; ++i) {
        auto delay = detail::calculate_delay(40, policy);
        // delay_ms is clamped to max_ms before jitter; jitter then adds 0-50% (which itself never
        // overflows here) and the trailing clamp pins it back to exactly max_delay.
        EXPECT_EQ(delay, big_max) << "exponential_jitter past max_delay must saturate to max_delay, never wrap";
        EXPECT_GE(delay, 0ms);
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
