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
 *   - every half-open admission is numbered (`trial()`), and `on_abandoned(now, trial)` releases only
 *     the trial still in flight -- never a newer one, never on behalf of a closed-state call (QB-297);
 *   - a negative cooldown is clamped to zero (must NOT wrap to ~1.8e19 ns and never recover);
 *   - a zero `failure_threshold` is clamped to 1;
 *   - a reading older than the one its cooldown runs from counts as no time elapsed (must NOT wrap past the
 *     cooldown and admit a trial at once: QB-1237);
 *   - a stamp older than the cooldown's start keeps that start (must NOT move it back and end the cooldown
 *     early: QB-1264);
 *   - a trial's admission is the cooldown's start too: a verdict or a release stamped before it re-arms from it
 *     (must NOT re-arm from the older stamp and admit the next trial early: QB-1264, sequence B).
 *
 * Strengthened over the original CircuitBreakerUnit suite: every transition now also asserts
 * `state()` AND `failure_count()` (not just one observable), and each `allow()` verdict is checked
 * at the exact boundary tick on both sides (cooldown-1 fails fast, cooldown succeeds).
 *
 * Promoted to unit/ from the former test-actor-coroutine-resilience.cpp (the engine half — the
 * `ask_retry` / `ask_guarded` combinators — lives in system/coroutine/coroutine-resilience.cpp).
 */

#include <cstdint>
#include <limits>

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
    cb.on_failure(0);                // open at t=0
    ASSERT_TRUE(cb.allow(100 * MS)); // -> half_open
    cb.on_failure(100 * MS);         // trial failed -> reopen at t=100ms (cooldown re-armed from here)
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
    EXPECT_TRUE(cb.allow(100 * MS)); // first call after cooldown -> the one trial
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
    ASSERT_TRUE(cb.allow(100 * MS)); // -> half_open (trial admitted)
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
// Numbered trials: a release names the trial it gives back (Huly QB-297)
// ---------------------------------------------------------------------------

TEST(CircuitBreaker, EveryHalfOpenAdmissionIsNumbered) {
    qb::CircuitBreaker cb(1, 100ms);
    EXPECT_EQ(cb.trial(), 0u) << "no trial before the first half-open admission";
    cb.on_failure(0);
    EXPECT_EQ(cb.trial(), 0u) << "opening is not an admission";
    ASSERT_TRUE(cb.allow(100 * MS));
    EXPECT_EQ(cb.trial(), 1u);
    EXPECT_FALSE(cb.allow(101 * MS));
    EXPECT_EQ(cb.trial(), 1u) << "a refused allow() admits nothing";
    cb.on_failure(150 * MS); // the trial failed: open again
    ASSERT_TRUE(cb.allow(250 * MS));
    EXPECT_EQ(cb.trial(), 2u) << "the next admission gets the next number";
    cb.on_success();
    EXPECT_TRUE(cb.allow(260 * MS));
    EXPECT_EQ(cb.trial(), 2u) << "a closed breaker admits without numbering";
}

TEST(CircuitBreaker, ReleaseTouchesOnlyTheTrialStillInFlight) {
    qb::CircuitBreaker cb(1, 100ms);
    cb.on_failure(0);
    ASSERT_TRUE(cb.allow(100 * MS)); // trial 1
    const auto first = cb.trial();
    cb.on_abandoned(120 * MS, first); // the holder gives it back: open, cooldown re-armed from 120ms
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(219 * MS));
    ASSERT_TRUE(cb.allow(220 * MS)); // trial 2 in flight
    const auto second = cb.trial();
    ASSERT_NE(second, first);

    cb.on_abandoned(230 * MS, first); // a late release of the superseded trial
    EXPECT_EQ(cb.state(), State::half_open) << "a release of an older trial must not touch the one in flight";
    cb.on_abandoned(230 * MS, 0u); // a call admitted while closed holds no trial
    EXPECT_EQ(cb.state(), State::half_open) << "a closed-state call releases nothing";
    EXPECT_FALSE(cb.allow(10'000 * MS)) << "still exactly one trial in flight";

    cb.on_abandoned(240 * MS, second); // its holder
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_TRUE(cb.allow(340 * MS)) << "released by its holder, the breaker admits a fresh trial after the cooldown";
}

TEST(CircuitBreaker, NumberedReleaseIsNoOpOutsideHalfOpen) {
    qb::CircuitBreaker cb(1, 100ms);
    cb.on_abandoned(0, 1u); // closed
    EXPECT_EQ(cb.state(), State::closed);
    cb.on_failure(0); // open at t=0
    cb.on_abandoned(50 * MS, cb.trial());
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(99 * MS)) << "an open breaker's cooldown is not re-armed by a release";
    EXPECT_TRUE(cb.allow(100 * MS));
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

// ---------------------------------------------------------------------------
// A reading older than the one its cooldown runs from (Huly QB-1237)
// ---------------------------------------------------------------------------

// One clock, read in another order: a reading taken before an `ask_guarded` whose timeout then opened the breaker
// is older than the one `on_failure` stamped. The cooldown was `now - opened_at` on unsigned nanoseconds, so such a
// reading wrapped to ~1.8e19 ns and admitted a half-open trial at once. A reading older than the one the cooldown runs
// from -- whatever stamped it: the opening failure, a trial's admission, a failed trial, an abandoned trial's release, a
// failure recorded while open -- counts as no time elapsed.
TEST(CircuitBreaker, AReadingOlderThanTheCooldownsStartCountsAsNoTimeElapsed) {
    constexpr std::uint64_t SEC = 1'000 * MS; // one second in nanoseconds
    qb::CircuitBreaker      cb(1, 10s);
    cb.on_failure(1'000 * SEC); // opened at t = 1000 s
    ASSERT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(1'000 * SEC - 1)) << "a reading one nanosecond older than the opening one wrapped past the cooldown";
    EXPECT_FALSE(cb.allow(0)) << "the oldest reading wrapped past the cooldown";
    EXPECT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(1'000 * SEC)) << "the opening reading itself: nothing elapsed";
    EXPECT_FALSE(cb.allow(1'010 * SEC - 1)) << "a nanosecond short of the cooldown";
    EXPECT_TRUE(cb.allow(1'010 * SEC)) << "a cooldown after the opening reading, the breaker admits its trial";
    EXPECT_EQ(cb.state(), State::half_open);

    // A failed trial re-arms the cooldown from its own reading: one older than it -- even one a whole cooldown
    // after the first opening -- is no time elapsed either.
    cb.on_failure(1'020 * SEC);
    ASSERT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(1'015 * SEC)) << "older than the re-opening reading, past the first cooldown";
    EXPECT_FALSE(cb.allow(1'030 * SEC - 1));
    EXPECT_TRUE(cb.allow(1'030 * SEC));

    // So does the release of an abandoned trial.
    cb.on_abandoned(1'040 * SEC, cb.trial());
    ASSERT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(1'035 * SEC)) << "older than the release's reading";
    EXPECT_FALSE(cb.allow(1'050 * SEC - 1));
    EXPECT_TRUE(cb.allow(1'050 * SEC));
    EXPECT_EQ(cb.state(), State::half_open);

    // A failure recorded while open -- a call admitted while closed whose verdict comes late -- re-arms it as well.
    qb::CircuitBreaker late(1, 10s);
    late.on_failure(100 * SEC);
    late.on_failure(200 * SEC);
    ASSERT_EQ(late.state(), State::open);
    EXPECT_FALSE(late.allow(150 * SEC)) << "older than the re-arming reading, though past the opening one";
    EXPECT_FALSE(late.allow(210 * SEC - 1));
    EXPECT_TRUE(late.allow(210 * SEC));
}

// The saturation is exact: an older reading is zero elapsed, not one nanosecond. The smallest cooldown refuses it
// and admits a nanosecond after the opening reading; a zero cooldown admits at once, whatever the reading. And the
// distance is unsigned over the whole range of readings, never a signed difference.
TEST(CircuitBreaker, AnOlderReadingIsZeroElapsedAtEveryEdge) {
    qb::CircuitBreaker one_ns(1, qb::duration{1});
    one_ns.on_failure(100 * MS);
    EXPECT_FALSE(one_ns.allow(100 * MS - 1)) << "an older reading under a 1 ns cooldown";
    EXPECT_FALSE(one_ns.allow(100 * MS)) << "the opening reading under a 1 ns cooldown";
    EXPECT_TRUE(one_ns.allow(100 * MS + 1));

    qb::CircuitBreaker zero(1, qb::duration::zero());
    zero.on_failure(100 * MS);
    EXPECT_TRUE(zero.allow(100 * MS - 1)) << "a zero cooldown has nothing to wait for, an older reading included";
    EXPECT_EQ(zero.state(), State::half_open);

    // Across the whole range: a distance of 2^63 ns or more is a distance, not a sign.
    constexpr std::uint64_t LAST = (std::numeric_limits<std::uint64_t>::max)();
    qb::CircuitBreaker      wide(1, 10s); // not `far`: <windows.h> defines it as an empty macro
    wide.on_failure(0);
    EXPECT_TRUE(wide.allow(LAST)) << "the newest reading after the oldest: the cooldown has passed";
    qb::CircuitBreaker back(1, qb::duration{1});
    back.on_failure(LAST);
    EXPECT_FALSE(back.allow(5)) << "the oldest reading after the newest: no time elapsed";
}

// ---------------------------------------------------------------------------
// A stamp older than the cooldown's start (Huly QB-1264)
// ---------------------------------------------------------------------------

// The calls that (re-)arm the cooldown stamp the reading they are given. One older than the cooldown's start -- one
// clock, read in another order: the late verdict of a call admitted while closed, a trial given back with a reading
// taken before its admission -- moved the start back and ended the cooldown early. Such a stamp keeps the breaker's
// clock: the cooldown still runs from its start. A newer reading re-arms it, as before.
TEST(CircuitBreaker, AFailureStampedBeforeTheCooldownsStartKeepsIt) {
    constexpr std::uint64_t SEC = 1'000 * MS; // one second in nanoseconds
    qb::CircuitBreaker      cb(1, 10s);
    cb.on_failure(1'000 * SEC); // opened at t = 1000 s
    cb.on_failure(990 * SEC);   // the late failure of a call admitted while closed, its reading older
    ASSERT_EQ(cb.state(), State::open);
    EXPECT_FALSE(cb.allow(1'000 * SEC)) << "the cooldown ran from the older stamp: a trial admitted at its start";
    EXPECT_FALSE(cb.allow(1'010 * SEC - 1)) << "a nanosecond short of a cooldown after its start";
    EXPECT_TRUE(cb.allow(1'010 * SEC)) << "a cooldown after its start, the breaker admits its trial";

    // A failed trial whose reading is older than the cooldown's start keeps it too.
    qb::CircuitBreaker failed(1, 10s);
    failed.on_failure(1'000 * SEC);
    ASSERT_TRUE(failed.allow(1'010 * SEC)); // the half-open trial
    failed.on_failure(990 * SEC);
    ASSERT_EQ(failed.state(), State::open);
    EXPECT_FALSE(failed.allow(1'010 * SEC - 1)) << "a failed trial stamped before the cooldown's start moved it back";
    EXPECT_FALSE(failed.allow(1'010 * SEC)) << "the trial's admission is the cooldown's start: no new trial at once";
    EXPECT_FALSE(failed.allow(1'020 * SEC - 1));
    EXPECT_TRUE(failed.allow(1'020 * SEC)) << "a cooldown after the trial's admission";

    // An opening from closed: a late success closed the open breaker, then a late failure, its reading older than the
    // previous start, opens it again. The previous start still holds.
    qb::CircuitBreaker reopened(1, 10s);
    reopened.on_failure(1'000 * SEC);
    reopened.on_success();
    ASSERT_EQ(reopened.state(), State::closed);
    reopened.on_failure(995 * SEC);
    ASSERT_EQ(reopened.state(), State::open);
    EXPECT_FALSE(reopened.allow(1'005 * SEC)) << "an opening from closed stamped before the previous start moved it back";
    EXPECT_FALSE(reopened.allow(1'010 * SEC - 1));
    EXPECT_TRUE(reopened.allow(1'010 * SEC));

    // Positive controls: a newer failure while open, or a newer opening from closed, re-arms the cooldown from its reading.
    qb::CircuitBreaker later(1, 10s);
    later.on_failure(1'000 * SEC);
    later.on_failure(1'005 * SEC);
    EXPECT_FALSE(later.allow(1'015 * SEC - 1)) << "a newer failure while open re-arms the cooldown from its reading";
    EXPECT_TRUE(later.allow(1'015 * SEC));

    qb::CircuitBreaker next_cycle(1, 10s);
    next_cycle.on_failure(1'000 * SEC);
    next_cycle.on_success();
    next_cycle.on_failure(1'005 * SEC);
    EXPECT_FALSE(next_cycle.allow(1'015 * SEC - 1)) << "a newer opening from closed arms the cooldown from its reading";
    EXPECT_TRUE(next_cycle.allow(1'015 * SEC));
}

TEST(CircuitBreaker, ATrialGivenBackWithAnOlderStampKeepsTheCooldownsStart) {
    constexpr std::uint64_t SEC = 1'000 * MS; // one second in nanoseconds

    // The plain release, then the numbered one: each given a reading older than the cooldown's start.
    qb::CircuitBreaker plain(1, 10s);
    plain.on_failure(1'000 * SEC);
    ASSERT_TRUE(plain.allow(1'010 * SEC));
    plain.on_abandoned(990 * SEC);
    ASSERT_EQ(plain.state(), State::open);
    EXPECT_FALSE(plain.allow(1'010 * SEC - 1)) << "on_abandoned(now) stamped before the cooldown's start moved it back";
    EXPECT_FALSE(plain.allow(1'010 * SEC)) << "the trial's admission is the cooldown's start: no new trial at once";
    EXPECT_TRUE(plain.allow(1'020 * SEC));

    qb::CircuitBreaker numbered(1, 10s);
    numbered.on_failure(1'000 * SEC);
    ASSERT_TRUE(numbered.allow(1'010 * SEC));
    numbered.on_abandoned(990 * SEC, numbered.trial());
    ASSERT_EQ(numbered.state(), State::open);
    EXPECT_FALSE(numbered.allow(1'010 * SEC - 1)) << "on_abandoned(now, trial) stamped before the cooldown's start moved it back";
    EXPECT_FALSE(numbered.allow(1'010 * SEC)) << "the trial's admission is the cooldown's start: no new trial at once";
    EXPECT_TRUE(numbered.allow(1'020 * SEC));

    // Positive control: a release with a newer reading re-arms the cooldown from it, in both forms.
    qb::CircuitBreaker plain_later(1, 10s);
    plain_later.on_failure(1'000 * SEC);
    ASSERT_TRUE(plain_later.allow(1'010 * SEC));
    plain_later.on_abandoned(1'020 * SEC);
    EXPECT_FALSE(plain_later.allow(1'030 * SEC - 1)) << "a newer release re-arms the cooldown from its reading";
    EXPECT_TRUE(plain_later.allow(1'030 * SEC));

    qb::CircuitBreaker numbered_later(1, 10s);
    numbered_later.on_failure(1'000 * SEC);
    ASSERT_TRUE(numbered_later.allow(1'010 * SEC));
    numbered_later.on_abandoned(1'020 * SEC, numbered_later.trial());
    EXPECT_FALSE(numbered_later.allow(1'030 * SEC - 1)) << "a newer numbered release re-arms the cooldown from its reading";
    EXPECT_TRUE(numbered_later.allow(1'030 * SEC));
}

// The trial's admission is the cooldown's start too (Huly QB-1264, sequence B): a verdict or a release stamped before the
// admission -- older than it, though newer or older than the opening -- re-arms the cooldown from the admission, not
// from the stamp. In 3.3.0 the next trial came a cooldown after the stamp, at once for a stamp a cooldown or more before
// the admission.
TEST(CircuitBreaker, AVerdictStampedBeforeItsTrialsAdmissionReArmsFromTheAdmission) {
    constexpr std::uint64_t SEC = 1'000 * MS; // one second in nanoseconds
    for (std::uint64_t const stamp : {995 * SEC, 1'005 * SEC}) {
        qb::CircuitBreaker released(1, 10s); // opened at 1000 s, the trial admitted at 1010 s, given back with `stamp`
        released.on_failure(1'000 * SEC);
        ASSERT_TRUE(released.allow(1'010 * SEC));
        released.on_abandoned(stamp, released.trial());
        ASSERT_EQ(released.state(), State::open);
        EXPECT_FALSE(released.allow(1'010 * SEC)) << "a release stamped " << stamp / SEC << " s re-armed from it or the opening";
        EXPECT_FALSE(released.allow(1'020 * SEC - 1)) << "a release stamped " << stamp / SEC << " s re-armed from it or the opening";
        EXPECT_TRUE(released.allow(1'020 * SEC)) << "a cooldown after the admission, a fresh trial";

        qb::CircuitBreaker failed(1, 10s); // the same trial failing, its verdict stamped `stamp`
        failed.on_failure(1'000 * SEC);
        ASSERT_TRUE(failed.allow(1'010 * SEC));
        failed.on_failure(stamp);
        ASSERT_EQ(failed.state(), State::open);
        EXPECT_FALSE(failed.allow(1'020 * SEC - 1)) << "a failure stamped " << stamp / SEC << " s re-armed from it or the opening";
        EXPECT_TRUE(failed.allow(1'020 * SEC));
    }

    // The admission holds through a close: a success, then a late failure stamped between the opening and the admission,
    // re-opens the breaker from the admission -- the start never moves back, `on_success` included.
    qb::CircuitBreaker closed_between(1, 10s);
    closed_between.on_failure(1'000 * SEC);
    ASSERT_TRUE(closed_between.allow(1'010 * SEC));
    closed_between.on_success();
    closed_between.on_failure(1'005 * SEC);
    ASSERT_EQ(closed_between.state(), State::open);
    EXPECT_FALSE(closed_between.allow(1'015 * SEC)) << "an opening from closed stamped before the last admission re-armed from the stamp";
    EXPECT_FALSE(closed_between.allow(1'020 * SEC - 1));
    EXPECT_TRUE(closed_between.allow(1'020 * SEC));

    // A release stamped with the admission itself -- what `ask_guarded`'s lease does from its destructor -- re-arms from it.
    qb::CircuitBreaker at_admission(1, 10s);
    at_admission.on_failure(1'000 * SEC);
    ASSERT_TRUE(at_admission.allow(1'010 * SEC));
    at_admission.on_abandoned(1'010 * SEC, at_admission.trial());
    ASSERT_EQ(at_admission.state(), State::open);
    EXPECT_FALSE(at_admission.allow(1'020 * SEC - 1));
    EXPECT_TRUE(at_admission.allow(1'020 * SEC));

    // Positive control: a verdict stamped after the admission re-arms from its own reading, as before.
    qb::CircuitBreaker after(1, 10s);
    after.on_failure(1'000 * SEC);
    ASSERT_TRUE(after.allow(1'010 * SEC));
    after.on_failure(1'012 * SEC);
    EXPECT_FALSE(after.allow(1'022 * SEC - 1));
    EXPECT_TRUE(after.allow(1'022 * SEC));
}

// The stated consequence (Huly QB-1264): the reading that admits a trial is a stamp like any other. One ahead of the
// breaker's clock -- the end of the range here, a time-of-day reading passed by hand in practice -- holds every later
// opening until the clock reaches it, `on_success` included; such a breaker must be replaced.
TEST(CircuitBreaker, AnAdmittingReadingAheadOfTheClockHoldsTheStart) {
    constexpr std::uint64_t SEC = 1'000 * MS; // one second in nanoseconds
    qb::CircuitBreaker      ahead(1, 10s);
    ahead.on_failure(1'000 * SEC);
    ASSERT_TRUE(ahead.allow(UINT64_MAX)); // admitted on a reading far ahead: the start
    ahead.on_failure(1'020 * SEC);
    ASSERT_EQ(ahead.state(), State::open);
    EXPECT_FALSE(ahead.allow(UINT64_MAX - 1)) << "a failure on the right clock moved the start back from the far admission";
    ahead.on_success();
    ahead.on_failure(2'000 * SEC);
    EXPECT_FALSE(ahead.allow(3'000 * SEC)) << "an opening from closed on the right clock replaced the far start";
}
