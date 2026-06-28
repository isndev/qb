/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/patterns/resilience-jitter.cpp
 * @brief Retry-backoff jitter math: `qb::detail::apply_retry_jitter` bounds and variance.
 *
 * `qb::ask_retry` desynchronizes a retry-storm by drawing the actual backoff wait uniformly from
 * `[d*(1-jitter), d]` (see retry_policy::jitter). This is pure arithmetic on a thread-local RNG — no
 * actor, no engine, no clock — so it lives in the unit tier (daemon-free, parallel-safe). The three
 * cases pin the documented contract exactly:
 *   - jitter == 0  → the value is returned UNCHANGED for every draw (no randomness leaks in);
 *   - jitter == 0.5 → every draw lands in [d/2, d] AND at least one draw is strictly below d
 *                     (proves the jitter is live, not a constant);
 *   - jitter == 1  → every draw lands in [0, d], and a jitter > 1 is CLAMPED to 1 (still in band,
 *                    never negative, never above d).
 *
 * Each band is exercised over thousands of draws so a single out-of-band result fails loudly; the
 * variance check (a strictly-below-d draw was seen) guards against a "always returns d" regression
 * that a band-only check would pass vacuously.
 */

#include <gtest/gtest.h>
#include <qb/core/patterns.h>
#include <qb/system/time.h> // qb::duration
#include <chrono>

using namespace std::chrono_literals;

namespace {
// Number of draws per band — large enough that an out-of-band value is overwhelmingly likely to be
// hit if the math is wrong, while staying instantaneous (pure arithmetic, no I/O).
constexpr int kDraws = 5000;
} // namespace

TEST(RetryJitter, ZeroJitterIsExactForEveryDraw) {
    const qb::duration d = 100ms;
    for (int i = 0; i < kDraws; ++i)
        EXPECT_EQ(qb::detail::apply_retry_jitter(d, 0.0), d)
            << "jitter == 0 must return the backoff unchanged (no randomness)";
}

TEST(RetryJitter, HalfJitterStaysInLowerHalfBandAndVaries) {
    const qb::duration d              = 100ms;
    bool               saw_below_full = false;
    for (int i = 0; i < kDraws; ++i) {
        const auto r = qb::detail::apply_retry_jitter(d, 0.5);
        ASSERT_GE(r.count(), d.count() / 2) << "draw below the [d*(1-0.5)] floor";
        ASSERT_LE(r.count(), d.count()) << "draw above the backoff ceiling d";
        if (r.count() < d.count())
            saw_below_full = true;
    }
    // Teeth: a "always returns d" stub would pass the band checks but never vary — this fails it.
    EXPECT_TRUE(saw_below_full) << "jitter must actually spread the value below d, not be constant";
}

TEST(RetryJitter, FullJitterSpansZeroToDAndClampsAboveOne) {
    const qb::duration d              = 200ms;
    bool               saw_below_full = false;
    for (int i = 0; i < kDraws; ++i) {
        const auto r = qb::detail::apply_retry_jitter(d, 1.0);
        ASSERT_GE(r.count(), 0) << "full jitter must never produce a negative wait";
        ASSERT_LE(r.count(), d.count()) << "full jitter must never exceed d";
        if (r.count() < d.count())
            saw_below_full = true;
    }
    EXPECT_TRUE(saw_below_full) << "full jitter must reach below d (the whole band is reachable)";

    // jitter > 1 is clamped to 1: the result stays in [0, d] — no negative, no out-of-band blow-up.
    for (int i = 0; i < kDraws; ++i) {
        const auto r = qb::detail::apply_retry_jitter(d, 4.0);
        ASSERT_GE(r.count(), 0) << "jitter > 1 must be clamped, never producing a negative wait";
        ASSERT_LE(r.count(), d.count()) << "jitter > 1 clamps to 1 — still bounded by d";
    }
}

TEST(RetryJitter, NegativeAndZeroDurationAreWellBehaved) {
    // A zero backoff with any jitter stays zero (factor * 0 == 0); a degenerate input must not throw
    // or wrap. This guards the `out < 0 ? 0 : out` floor in apply_retry_jitter.
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(qb::detail::apply_retry_jitter(qb::duration::zero(), 0.5).count(), 0);
        EXPECT_EQ(qb::detail::apply_retry_jitter(qb::duration::zero(), 1.0).count(), 0);
    }
}

// ---------------------------------------------------------------------------
// grow_backoff: one geometric step, clamped to max_backoff, overflow-safe.
// ---------------------------------------------------------------------------

TEST(RetryBackoff, GrowsGeometricallyThenClampsToMax) {
    using qb::detail::grow_backoff;
    EXPECT_EQ(grow_backoff(50ms, 2.0, 1s), 100ms);  // 50*2
    EXPECT_EQ(grow_backoff(100ms, 2.0, 1s), 200ms); // 100*2
    EXPECT_EQ(grow_backoff(800ms, 2.0, 1s), 1s);    // 1600ms clamped to the 1s cap
    EXPECT_EQ(grow_backoff(1s, 2.0, 1s), 1s);       // already at the cap
}

TEST(RetryBackoff, MultiplierOneIsConstant) {
    using qb::detail::grow_backoff;
    EXPECT_EQ(grow_backoff(50ms, 1.0, 1s), 50ms); // no growth, no shrink
}

TEST(RetryBackoff, DegenerateMultiplierFloorsToZeroNoUB) {
    using qb::detail::grow_backoff;
    // multiplier 0 / negative would, before the fix, cast a 0 / huge-negative double — now floored.
    EXPECT_EQ(grow_backoff(50ms, 0.0, 1s).count(), 0);
    EXPECT_EQ(grow_backoff(50ms, -5.0, 1s).count(), 0);
}

TEST(RetryBackoff, CapBelowCurrentReturnsCap) {
    using qb::detail::grow_backoff;
    EXPECT_EQ(grow_backoff(2s, 2.0, 1s), 1s); // current already past a small cap → the cap
}

// The defect this guards: `static_cast<rep>(current.count() * multiplier)` is UB when the product
// exceeds the int64 range. With a pathological multiplier / max_backoff the product overflows; the
// fix clamps in double space and returns the EXACT max_backoff rep, never casting the huge double.
// Built under the `sanitize` preset (ASan+UBSan), this test fails loudly if the cast UB returns.
TEST(RetryBackoff, HugeProductDoesNotOverflowTheCast) {
    using qb::detail::grow_backoff;
    const qb::duration cap = qb::duration::max(); // ~292 years in ns (INT64_MAX)

    // current * multiplier ≈ 1e29 ns ≫ INT64_MAX → must saturate to the cap, no UBSan trip.
    EXPECT_EQ(grow_backoff(std::chrono::seconds(100), 1e18, cap), cap);
    // half the range, ×4 → 2× the range → also saturates to the cap.
    EXPECT_EQ(grow_backoff(qb::duration{qb::duration::max().count() / 2}, 4.0, cap), cap);
    // a realistic-but-large config (1h backoff, ×100, 365-day cap) stays exact (no overflow,
    // 100h < the 8760h cap so it is NOT clamped).
    EXPECT_EQ(grow_backoff(std::chrono::hours(1), 100.0, std::chrono::hours(24 * 365)),
              std::chrono::hours(100));
}
