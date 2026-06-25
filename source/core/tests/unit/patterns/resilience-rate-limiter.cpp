/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/patterns/resilience-rate-limiter.cpp
 * @brief Token-bucket math of `qb::rate_limiter::try_acquire` / `tokens` (pure, clock-injected).
 *
 * The rate limiter is a plain single-thread state machine with NO timer of its own: the caller
 * feeds it a `VirtualCore` timestamp (`now_ns`). That makes its accounting fully deterministic and
 * unit-testable WITHOUT an engine — we inject a synthetic, monotonically-advanced `now_ns` so the
 * refill arithmetic is exact and there is no wall-clock flake (the engine-driven `acquire()` wait is
 * covered separately under system/patterns/resilience-rate-limiter.cpp).
 *
 * Proven here, against the documented contract:
 *   - the bucket starts FULL (`capacity` tokens) and the first `now_ns` only PRIMES the clock
 *     (no windfall on the very first observation);
 *   - tokens regenerate at exactly one per `per_token`, with fractional credit visible via `tokens()`;
 *   - after a long idle the bucket CAPS at `capacity` (no unbounded accumulation);
 *   - `capacity < 1` clamps to 1 and `per_token <= 0` clamps to 1 ns (degenerate inputs are safe).
 */

#include <gtest/gtest.h>
#include <qb/core/patterns.h>
#include <chrono>
#include <cstdint>

using namespace std::chrono_literals;

namespace {
constexpr std::uint64_t MS = 1'000'000ull; // ns per ms — synthetic-clock unit
} // namespace

TEST(RateLimiterMath, StartsFullThenRefillsOnePerWindowAndCaps) {
    qb::rate_limiter rl(2.0, 10ms); // capacity 2, one token / 10ms

    // First observation primes the clock without a windfall, but the bucket already starts full.
    EXPECT_DOUBLE_EQ(rl.tokens(0), 2.0);
    EXPECT_TRUE(rl.try_acquire(0));  // consume #1 (was full)
    EXPECT_TRUE(rl.try_acquire(0));  // consume #2 → empty
    EXPECT_FALSE(rl.try_acquire(0)); // empty: nothing to take at the same instant

    // 5ms later only half a token has regenerated → still below 1, refused.
    EXPECT_NEAR(rl.tokens(5 * MS), 0.5, 1e-9);
    EXPECT_FALSE(rl.try_acquire(5 * MS));

    // At 10ms exactly one full token is available → granted, then exhausted again.
    EXPECT_TRUE(rl.try_acquire(10 * MS));
    EXPECT_FALSE(rl.try_acquire(10 * MS));

    // After a long idle the bucket refills but caps at capacity (2), not unbounded accumulation.
    EXPECT_DOUBLE_EQ(rl.tokens(1000 * MS), 2.0); // capped, not 100+
    EXPECT_TRUE(rl.try_acquire(1000 * MS));
    EXPECT_TRUE(rl.try_acquire(1000 * MS));
    EXPECT_FALSE(rl.try_acquire(1000 * MS)); // the cap held — only 2 were ever available
}

TEST(RateLimiterMath, NonMonotonicClockDoesNotGrantTokens) {
    qb::rate_limiter rl(1.0, 10ms);
    EXPECT_TRUE(rl.try_acquire(100 * MS)); // primes + consumes the starting token
    EXPECT_FALSE(rl.try_acquire(100 * MS));
    // A now_ns that does not advance past _last_ns must not regenerate anything (guards `now<=last`).
    EXPECT_FALSE(rl.try_acquire(50 * MS)); // earlier timestamp → no refill, still empty
    EXPECT_DOUBLE_EQ(rl.tokens(100 * MS), 0.0);
}

TEST(RateLimiterMath, DegenerateConstructionClamps) {
    // capacity < 1 clamps to 1 token of burst.
    qb::rate_limiter small(0.0, 10ms);
    EXPECT_DOUBLE_EQ(small.tokens(0), 1.0);
    EXPECT_TRUE(small.try_acquire(0));
    EXPECT_FALSE(small.try_acquire(0)); // only the single clamped token existed

    // per_token <= 0 clamps to 1 ns, so a token regenerates almost immediately (no div-by-zero).
    qb::rate_limiter fast(1.0, qb::duration::zero());
    EXPECT_TRUE(fast.try_acquire(0));
    EXPECT_FALSE(fast.try_acquire(0));
    EXPECT_TRUE(fast.try_acquire(2)) << "per_token clamped to 1ns → a token is back within a couple ns";
}
