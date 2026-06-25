/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/patterns/circuit-breaker.cpp
 * @brief `qb::CircuitBreaker` three-state machine — pure logic, no engine.
 *
 * `CircuitBreaker` (qb/core/patterns/resilience.h) is a timer-less single-thread state machine that
 * the caller drives with an explicit nanosecond clock (`allow(now)` / `on_failure(now)` / ...).
 * Driving time by hand makes every transition deterministic, so these are unit tests: NO `qb::Main`,
 * NO wall clock, NO sleeps. We prove the full lifecycle and its hardening contracts:
 *
 *   closed --(failure_threshold consecutive failures)--> open
 *   open   --(cooldown elapsed, exactly ONE trial admitted)--> half_open
 *   half_open --(on_success)--> closed   |   half_open --(on_failure)--> open (cooldown re-armed)
 *
 * plus the edge contracts the production header added:
 *   - half-open admits *exactly one* trial (no thundering herd against a still-down dependency);
 *   - `on_abandoned` releases a trial whose caller was killed, re-arming cooldown (no wedge);
 *   - `on_abandoned` is a no-op outside half-open;
 *   - a negative cooldown is clamped to zero (must NOT wrap to ~1.8e19 ns and never recover);
 *   - a zero `failure_threshold` is clamped to 1.
 *
 * Strengthened over the original CircuitBreakerUnit suite: every transition now also asserts
 * `state()` AND `failure_count()` (not just one observable), and each `allow()` verdict is checked
 * at the exact boundary tick on both sides (cooldown-1 fails fast, cooldown succeeds).
 *
 * Promoted to unit/ from the former test-actor-coroutine-resilience.cpp (the engine half — the
 * `ask_retry` / `ask_guarded` combinators — lives in system/coroutine/coroutine-resilience.cpp).
 */

#include <cstdint>

#include <gtest/gtest.h>
#include <qb/core/patterns.h>
#include <qb/system/time.h> // qb::duration

using namespace std::chrono_literals;

namespace {
constexpr std::uint64_t MS = 1'000'000ull; // one millisecond in nanoseconds
using State                = qb::CircuitBreaker::State;
} // namespace

// ---------------------------------------------------------------------------
// Construction / closed steady state
// ---------------------------------------------------------------------------

TEST(CircuitBreaker, StartsClosedAndAllows) {
    qb::CircuitBreaker cb(3, 100ms);
    EXPECT_EQ(cb.state(), State::closed);
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_TRUE(cb.allow(0));
    // allow() in the closed state is a pure predicate — it must not mutate anything.
    EXPECT_EQ(cb.state(), State::closed);
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_TRUE(cb.allow(123 * MS)); // closed admits at any clock value
}

TEST(CircuitBreaker, StaysClosedBelowThreshold) {
    qb::CircuitBreaker cb(3, 100ms);
    cb.on_failure(0);
    EXPECT_EQ(cb.failure_count(), 1u);
    EXPECT_EQ(cb.state(), State::closed);
    cb.on_failure(0); // 2 < 3
    EXPECT_EQ(cb.failure_count(), 2u);
    EXPECT_EQ(cb.state(), State::closed);
    EXPECT_TRUE(cb.allow(0)); // still admits while below threshold
}

TEST(CircuitBreaker, OpensExactlyAtThreshold) {
    qb::CircuitBreaker cb(3, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);
    EXPECT_EQ(cb.state(), State::closed) << "must NOT open before the threshold is reached";
    cb.on_failure(0); // 3 == threshold -> open
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_EQ(cb.failure_count(), 3u);
    EXPECT_FALSE(cb.allow(0)) << "within cooldown -> fail fast";
    EXPECT_EQ(cb.state(), State::open) << "a rejected allow() must not transition";
}

TEST(CircuitBreaker, ThresholdOfOneOpensOnFirstFailure) {
    qb::CircuitBreaker cb(1, 100ms);
    cb.on_failure(0);
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(0));
}

TEST(CircuitBreaker, ZeroThresholdIsClampedToOne) {
    // Header clamps failure_threshold to >= 1; a single failure must therefore open the breaker.
    qb::CircuitBreaker cb(0u, 100ms);
    EXPECT_EQ(cb.state(), State::closed);
    cb.on_failure(0);
    EXPECT_EQ(cb.state(), State::open) << "zero threshold must clamp to 1, opening on the first failure";
}

// ---------------------------------------------------------------------------
// Success resets accumulated failures
// ---------------------------------------------------------------------------

TEST(CircuitBreaker, SuccessResetsFailures) {
    qb::CircuitBreaker cb(3, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);
    EXPECT_EQ(cb.failure_count(), 2u);
    cb.on_success();
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_EQ(cb.state(), State::closed);
    // After a reset it takes the full threshold again to re-open.
    cb.on_failure(0);
    cb.on_failure(0);
    EXPECT_EQ(cb.state(), State::closed) << "reset failure run must require the FULL threshold to re-open";
    cb.on_failure(0);
    EXPECT_EQ(cb.state(), State::open);
}

// ---------------------------------------------------------------------------
// open -> (cooldown) -> half_open boundary
// ---------------------------------------------------------------------------

TEST(CircuitBreaker, FailsFastDuringCooldown) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0); // open at t=0
    ASSERT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(50 * MS)) << "50ms < 100ms cooldown -> fail fast";
    EXPECT_FALSE(cb.allow(99 * MS)) << "one tick before cooldown end still fails fast";
    EXPECT_EQ(cb.state(), State::open) << "rejected allow() must not transition out of open";
}

TEST(CircuitBreaker, HalfOpensExactlyAtCooldown) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0); // open at t=0
    EXPECT_FALSE(cb.allow(99 * MS)) << "boundary-1 fails fast";
    EXPECT_TRUE(cb.allow(100 * MS)) << "cooldown elapsed -> admit exactly one trial";
    EXPECT_EQ(cb.state(), State::half_open);
}

// ---------------------------------------------------------------------------
// half_open verdicts
// ---------------------------------------------------------------------------

TEST(CircuitBreaker, HalfOpenSuccessCloses) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);
    ASSERT_TRUE(cb.allow(100 * MS)); // -> half_open
    ASSERT_EQ(cb.state(), State::half_open);
    cb.on_success(); // trial succeeded
    EXPECT_EQ(cb.state(), State::closed);
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_TRUE(cb.allow(110 * MS)) << "a recovered breaker admits freely again";
}

TEST(CircuitBreaker, HalfOpenFailureReopensAndReArmsCooldown) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);          // open at t=0
    ASSERT_TRUE(cb.allow(100 * MS)); // -> half_open
    cb.on_failure(100 * MS);   // trial failed -> reopen at t=100ms (cooldown re-armed from here)
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(150 * MS)) << "50ms into the NEW cooldown -> fail fast";
    EXPECT_TRUE(cb.allow(200 * MS)) << "100ms after reopen -> a fresh trial";
    EXPECT_EQ(cb.state(), State::half_open);
}

// ---------------------------------------------------------------------------
// Single-trial half-open + abandon-release (no-thundering-herd / no-wedge)
// ---------------------------------------------------------------------------

TEST(CircuitBreaker, HalfOpenAdmitsExactlyOneTrial) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);                // open at t=0
    EXPECT_TRUE(cb.allow(100 * MS));  // first call after cooldown -> the one trial
    EXPECT_EQ(cb.state(), State::half_open);
    EXPECT_FALSE(cb.allow(101 * MS)) << "a concurrent caller must fail fast (no thundering herd)";
    EXPECT_FALSE(cb.allow(199 * MS)) << "still exactly one trial in flight";
    EXPECT_FALSE(cb.allow(10'000 * MS)) << "and it never auto-resolves on its own — it must be verdicted";
    EXPECT_EQ(cb.state(), State::half_open);
}

TEST(CircuitBreaker, AbandonedHalfOpenTrialReArmsCooldownAndDoesNotWedge) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0);                // open at t=0
    ASSERT_TRUE(cb.allow(100 * MS));  // -> half_open (trial admitted)
    cb.on_abandoned(120 * MS);       // trial's caller was killed: release, re-arm cooldown from t=120ms
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(150 * MS)) << "30ms into the re-armed cooldown -> fail fast";
    EXPECT_FALSE(cb.allow(219 * MS)) << "boundary-1 still fails fast";
    EXPECT_TRUE(cb.allow(220 * MS)) << "100ms after abandon -> a fresh trial (not wedged)";
    EXPECT_EQ(cb.state(), State::half_open);
}

TEST(CircuitBreaker, OnAbandonedIsNoOpWhenClosed) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_abandoned(0); // closed -> no effect
    EXPECT_EQ(cb.state(), State::closed);
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_TRUE(cb.allow(0));
}

TEST(CircuitBreaker, OnAbandonedIsNoOpWhenOpen) {
    qb::CircuitBreaker cb(2, 100ms);
    cb.on_failure(0);
    cb.on_failure(0); // open at t=0
    ASSERT_EQ(cb.state(), State::open);
    cb.on_abandoned(50 * MS); // open (not half-open) -> no effect, original cooldown unchanged
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(50 * MS)) << "still cooling down from the ORIGINAL t=0 open, not re-armed";
    EXPECT_TRUE(cb.allow(100 * MS)) << "the original 100ms cooldown still governs recovery";
}

// ---------------------------------------------------------------------------
// Negative-cooldown clamp (must recover, must not wrap to ~1.8e19 ns)
// ---------------------------------------------------------------------------

TEST(CircuitBreaker, NegativeCooldownClampedSoBreakerRecovers) {
    // A negative cooldown must clamp to zero, not wrap to ~1.8e19 ns (which would never recover):
    // allow() compares an unsigned ns delta against _cooldown.count().
    qb::CircuitBreaker cb(1, qb::duration{-100});
    cb.on_failure(0); // open at t=0 (threshold 1)
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_TRUE(cb.allow(0)) << "cooldown == 0 -> immediately admits a trial";
    EXPECT_EQ(cb.state(), State::half_open);
}

// ---------------------------------------------------------------------------
// Full lifecycle round-trip in one go (closed -> open -> half_open -> closed -> open ...)
// ---------------------------------------------------------------------------

TEST(CircuitBreaker, FullLifecycleRoundTrip) {
    qb::CircuitBreaker cb(2, 100ms);

    // closed -> open
    EXPECT_EQ(cb.state(), State::closed);
    cb.on_failure(0);
    cb.on_failure(0);
    EXPECT_EQ(cb.state(), State::open);

    // open -> half_open (trial) -> closed (success)
    EXPECT_TRUE(cb.allow(100 * MS));
    EXPECT_EQ(cb.state(), State::half_open);
    cb.on_success();
    EXPECT_EQ(cb.state(), State::closed);
    EXPECT_EQ(cb.failure_count(), 0u);

    // closed -> open again (fresh failure run)
    cb.on_failure(200 * MS);
    cb.on_failure(200 * MS);
    EXPECT_EQ(cb.state(), State::open);

    // open -> half_open -> open (failed trial re-arms cooldown) -> half_open (recovered timing)
    EXPECT_TRUE(cb.allow(300 * MS));
    EXPECT_EQ(cb.state(), State::half_open);
    cb.on_failure(300 * MS);
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(399 * MS));
    EXPECT_TRUE(cb.allow(400 * MS));
    EXPECT_EQ(cb.state(), State::half_open);
}
