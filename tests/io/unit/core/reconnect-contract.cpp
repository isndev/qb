/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/reconnect-contract.cpp
 * @brief The reconnect contract's vocabulary (`qb/io/reconnect.h`): the names, and the backoff
 *        arithmetic of `qb::io::reconnect_policy`, the one reconnection policy every module aliases.
 *
 * Pure logic: no loop, no socket, no client. The policy is data plus one pure function,
 * `next_delay(attempt, rng)`, the wait after `attempt` failed attempts; every case calls it
 * directly, with a seeded generator where jitter is on, so the arithmetic the clients rely on is
 * pinned once, here, and each client's own suite only has to prove that it OBEYS it.
 *
 * What is pinned, and why each matters:
 *   - every enumerator has a distinct name and the counts match the enumerations -- a value added
 *     without its name is a "?" in a log line and a metrics label;
 *   - the vocabulary is byte-sized, as the death watch's `qb::DownReason` is;
 *   - the first attempt is immediate; the defaults (100 ms, x2, 30 s cap, jitter on, unlimited, the
 *     client's own connect timeout); the growth is geometric, capped, and STOPS at the cap;
 *   - jitter is +-25 % in integer milliseconds, never takes a positive wait below 1 ms, and a zero
 *     wait stays zero;
 *   - the pathological inputs read sanely: an initial delay above the cap is the cap, a multiplier
 *     below 1 is 1, a negative attempt is 0, a negative cap is 0;
 *   - `allows_attempt` is the bound a run checks before each attempt.
 *
 * The arithmetic cases moved here from qbm-http's `unit/retry-policy/retry-policy.cpp` with the
 * struct itself: `qb::http::RetryPolicy` and `qb::redis::RetryPolicy` are now aliases of it.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <random>
#include <set>
#include <string_view>
#include <type_traits>

#include <qb/io/reconnect.h>

using namespace std::chrono_literals;

namespace reconnect_contract_test {

[[nodiscard]] long long
ms(qb::duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

static_assert(sizeof(qb::io::connection_state) == 1);
static_assert(sizeof(qb::io::down_reason) == 1);
static_assert(sizeof(qb::io::gap_reason) == 1);
static_assert(std::is_trivially_copyable_v<qb::io::gap_info>);

// The names are constexpr: a metrics label can be a compile-time constant.
static_assert(qb::io::connection_state_name(qb::io::connection_state::ready) == "ready");
static_assert(qb::io::down_reason_name(qb::io::down_reason::handshake_refused) == "handshake_refused");
static_assert(qb::io::gap_reason_name(qb::io::gap_reason::overflow) == "overflow");

/// Every value below the count has its own name, and the value AT the count does not: a count that
/// lags an added enumerator, or an enumerator added without its name, fails here.
template <typename E, typename Name>
void
expect_names_cover(std::size_t count, Name name_of) {
    std::set<std::string_view> names;
    for (std::size_t i = 0; i < count; ++i) {
        const auto name = name_of(static_cast<E>(i));
        EXPECT_NE(name, "?") << "value " << i << " has no name";
        EXPECT_TRUE(names.insert(name).second) << "the name '" << name << "' is used twice";
    }
    EXPECT_EQ(name_of(static_cast<E>(count)), "?") << "a value past the count has a name: the count lags the enumeration";
}

TEST(ReconnectContract, EveryStateReasonAndGapHasItsOwnName) {
    expect_names_cover<qb::io::connection_state>(qb::io::connection_states, qb::io::connection_state_name);
    expect_names_cover<qb::io::down_reason>(qb::io::down_reasons, qb::io::down_reason_name);
    expect_names_cover<qb::io::gap_reason>(qb::io::gap_reasons, qb::io::gap_reason_name);
}

TEST(ReconnectContract, AGapInfoDefaultsToAReconnectThatCountedNothing) {
    const qb::io::gap_info gap;
    EXPECT_EQ(gap.reason, qb::io::gap_reason::reconnect);
    EXPECT_EQ(gap.lost, 0u) << "what the server sent while no connection was there cannot be counted";
}

TEST(ReconnectPolicy, FirstAttemptIsImmediate) {
    qb::io::reconnect_policy policy;
    EXPECT_EQ(policy.next_delay(0), qb::duration::zero());
    EXPECT_EQ(policy.next_delay(-1), qb::duration::zero()) << "a negative attempt count reads as none";
    std::mt19937 rng{7};
    EXPECT_EQ(policy.next_delay(0, &rng), qb::duration::zero()) << "jitter has nothing to add to a zero wait";
}

TEST(ReconnectPolicy, Defaults) {
    const qb::io::reconnect_policy policy;
    EXPECT_EQ(policy.max_attempts, -1);
    EXPECT_EQ(policy.initial_delay, 100ms);
    EXPECT_EQ(policy.max_delay, 30s);
    EXPECT_DOUBLE_EQ(policy.multiplier, 2.0);
    EXPECT_TRUE(policy.jitter);
    EXPECT_EQ(policy.connect_timeout, qb::duration::zero()) << "zero leaves each attempt's deadline to the client";
    EXPECT_FALSE(policy.on_retry);
    // Without a generator the jitter is not applied, so the defaults read exactly.
    EXPECT_EQ(ms(policy.next_delay(1)), 100);
    EXPECT_EQ(ms(policy.next_delay(2)), 200);
    EXPECT_EQ(ms(policy.next_delay(3)), 400);
    EXPECT_EQ(ms(policy.next_delay(9)), 25600);
    EXPECT_EQ(ms(policy.next_delay(10)), 30000) << "100 ms * 2^9 = 51.2 s is past the 30 s cap";
}

TEST(ReconnectPolicy, AllowsAttemptIsTheBoundOfARun) {
    const qb::io::reconnect_policy unlimited;
    EXPECT_TRUE(unlimited.allows_attempt(0));
    EXPECT_TRUE(unlimited.allows_attempt(1'000'000)) << "-1 is unlimited";

    const auto three = qb::io::reconnect_policy{}.with_max_attempts(3);
    EXPECT_TRUE(three.allows_attempt(0));
    EXPECT_TRUE(three.allows_attempt(2));
    EXPECT_FALSE(three.allows_attempt(3)) << "three attempts made: the run is spent";

    const auto none = qb::io::reconnect_policy{}.with_max_attempts(0);
    EXPECT_FALSE(none.allows_attempt(0)) << "zero attempts: a run that makes none";
}

TEST(ReconnectPolicy, GrowsGeometricallyAndStopsAtTheCap) {
    const auto policy = qb::io::reconnect_policy{}.with_initial_delay(30ms).with_multiplier(2.0).with_max_delay(500ms).with_jitter(false);
    EXPECT_EQ(ms(policy.next_delay(1)), 30);
    EXPECT_EQ(ms(policy.next_delay(2)), 60);
    EXPECT_EQ(ms(policy.next_delay(3)), 120);
    EXPECT_EQ(ms(policy.next_delay(4)), 240);
    EXPECT_EQ(ms(policy.next_delay(5)), 480);
    EXPECT_EQ(ms(policy.next_delay(6)), 500) << "960 would be past the cap";
    EXPECT_EQ(ms(policy.next_delay(7)), 500);
    EXPECT_EQ(ms(policy.next_delay(1000)), 500) << "the growth stops at the cap: no overflow, whatever the attempt";
}

TEST(ReconnectPolicy, HugeMultiplierAndAttemptDoNotOverflow) {
    const auto policy = qb::io::reconnect_policy{}.with_initial_delay(1ms).with_multiplier(1e9).with_max_delay(2s).with_jitter(false);
    EXPECT_EQ(ms(policy.next_delay(2)), 2000);
    EXPECT_EQ(ms(policy.next_delay(1000)), 2000);
}

TEST(ReconnectPolicy, NonIntegralMultiplierTruncatesInMilliseconds) {
    const auto policy = qb::io::reconnect_policy{}.with_initial_delay(100ms).with_multiplier(1.5).with_max_delay(10s).with_jitter(false);
    EXPECT_EQ(ms(policy.next_delay(1)), 100);
    EXPECT_EQ(ms(policy.next_delay(2)), 150);
    EXPECT_EQ(ms(policy.next_delay(3)), 225);
    EXPECT_EQ(ms(policy.next_delay(4)), 337) << "337.5 truncates: the arithmetic is integer milliseconds";
}

TEST(ReconnectPolicy, JitterStaysWithinAQuarterEitherWay) {
    const auto   policy = qb::io::reconnect_policy{}.with_initial_delay(100ms).with_jitter(true);
    std::mt19937 rng{20260909};
    long long    lo = 1'000'000, hi = 0;
    for (int i = 0; i < 2000; ++i) {
        const auto d = ms(policy.next_delay(1, &rng));
        lo           = std::min(lo, d);
        hi           = std::max(hi, d);
        ASSERT_GE(d, 75);
        ASSERT_LE(d, 125);
    }
    EXPECT_LT(lo, 85) << "2000 draws of a uniform +-25 must reach the low quarter";
    EXPECT_GT(hi, 115) << "and the high one";
    // The cap bounds the base, not the jittered result: a jittered wait may exceed max_delay by the quarter.
    const auto capped = qb::io::reconnect_policy{}.with_initial_delay(1s).with_max_delay(1s).with_jitter(true);
    for (int i = 0; i < 200; ++i) {
        const auto d = ms(capped.next_delay(5, &rng));
        ASSERT_GE(d, 750);
        ASSERT_LE(d, 1250);
    }
}

TEST(ReconnectPolicy, JitterNeverTakesAPositiveWaitBelowOneMillisecond) {
    std::mt19937 rng{3};
    const auto   tiny = qb::io::reconnect_policy{}.with_initial_delay(1ms).with_jitter(true);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(ms(tiny.next_delay(1, &rng)), 1) << "a quarter of 1 ms is 0 ms: nothing to add, nothing to remove";
    }
    const auto four = qb::io::reconnect_policy{}.with_initial_delay(4ms).with_jitter(true);
    for (int i = 0; i < 200; ++i) {
        const auto d = ms(four.next_delay(1, &rng));
        ASSERT_GE(d, 3);
        ASSERT_LE(d, 5);
    }
}

TEST(ReconnectPolicy, ZeroInitialDelayStaysZeroEvenJittered) {
    std::mt19937 rng{11};
    const auto   policy = qb::io::reconnect_policy{}.with_initial_delay(0ms).with_jitter(true);
    EXPECT_EQ(policy.next_delay(1, &rng), qb::duration::zero());
    EXPECT_EQ(policy.next_delay(5, &rng), qb::duration::zero()) << "zero times any multiplier is zero: immediate retries were asked for";
}

TEST(ReconnectPolicy, InitialDelayAboveTheCapIsTheCap) {
    const auto policy = qb::io::reconnect_policy{}.with_initial_delay(5s).with_max_delay(1s).with_jitter(false);
    EXPECT_EQ(ms(policy.next_delay(1)), 1000);
    EXPECT_EQ(ms(policy.next_delay(3)), 1000);
}

TEST(ReconnectPolicy, MultiplierBelowOneReadsAsOne) {
    const auto shrinking = qb::io::reconnect_policy{}.with_initial_delay(200ms).with_multiplier(0.5).with_jitter(false);
    EXPECT_EQ(ms(shrinking.next_delay(1)), 200);
    EXPECT_EQ(ms(shrinking.next_delay(4)), 200) << "the wait never shrinks";
    const auto negative = qb::io::reconnect_policy{}.with_initial_delay(200ms).with_multiplier(-3.0).with_jitter(false);
    EXPECT_EQ(ms(negative.next_delay(3)), 200);
}

TEST(ReconnectPolicy, NegativeMaxDelayReadsAsZero) {
    const auto policy = qb::io::reconnect_policy{}.with_initial_delay(100ms).with_max_delay(-1s).with_jitter(false);
    EXPECT_EQ(policy.next_delay(1), qb::duration::zero());
    EXPECT_EQ(policy.next_delay(4), qb::duration::zero());
}

TEST(ReconnectPolicy, BuildersChainAndStoreEveryField) {
    int          seen_attempt = 0;
    qb::duration seen_delay{};
    auto         policy = qb::io::reconnect_policy{}
                              .with_max_attempts(3)
                              .with_initial_delay(10ms)
                              .with_max_delay(80ms)
                              .with_multiplier(3.0)
                              .with_jitter(false)
                              .with_connect_timeout(250ms)
                              .with_on_retry([&](int attempt, qb::duration delay) {
                          seen_attempt = attempt;
                          seen_delay   = delay;
                              });
    EXPECT_EQ(policy.max_attempts, 3);
    EXPECT_EQ(policy.initial_delay, 10ms);
    EXPECT_EQ(policy.max_delay, 80ms);
    EXPECT_DOUBLE_EQ(policy.multiplier, 3.0);
    EXPECT_FALSE(policy.jitter);
    EXPECT_EQ(policy.connect_timeout, 250ms);
    ASSERT_TRUE(policy.on_retry);
    policy.on_retry(2, policy.next_delay(2));
    EXPECT_EQ(seen_attempt, 2);
    EXPECT_EQ(ms(seen_delay), 30);
    EXPECT_EQ(ms(policy.next_delay(3)), 80) << "90 is past the cap";
}

} // namespace reconnect_contract_test
