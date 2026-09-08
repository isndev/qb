/**
 * @file qb/tests/core/system/engine/core-io-poll-interval.cpp
 * @brief `CoreInitializer::setIoPollInterval` reaches the core's listener (Huly QB-191).
 *
 * The io poll cadence lives in `qb::io::async::listener`, whose own default is "poll on every
 * pass"; a `VirtualCore` opts its listener in at thread start, with the interval the initializer
 * holds (1 µs by default). These cases pin the plumbing from both ends: the default engine sets a
 * positive interval on its core's listener, an explicit zero sets none, and a value chosen by the
 * user arrives in the listener's own ticks in the right proportion.
 *
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#include <atomic>
#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/main.h>

namespace core_io_poll_interval_test {

using namespace std::chrono_literals;

std::atomic<std::uint64_t> g_ticks_seen{~std::uint64_t{0}};

/// Reads the listener's interval from inside the core's thread and leaves.
class Prober : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_ticks_seen = qb::io::async::listener::current.io_poll_interval_ticks();
        kill();
        co_return true;
    }
};

static std::uint64_t
ticks_with(qb::duration const *interval) {
    g_ticks_seen = ~std::uint64_t{0};
    qb::Main main;
    if (interval)
        main.core(0).setIoPollInterval(*interval);
    main.addActor<Prober>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    return g_ticks_seen.load();
}

TEST(CoreIoPollInterval, TheDefaultEngineOptsItsCoreIn) {
    const auto ticks = ticks_with(nullptr);
    EXPECT_NE(ticks, ~std::uint64_t{0}) << "the actor ran";
    EXPECT_GT(ticks, 0u) << "a default core polls a quiet socket on a cadence, not on every pass";
}

TEST(CoreIoPollInterval, ZeroMeansEveryPass) {
    const qb::duration zero = qb::duration::zero();
    EXPECT_EQ(ticks_with(&zero), 0u) << "setIoPollInterval(0) is the 3.1 contract: one pass, one poll";
}

TEST(CoreIoPollInterval, AChosenIntervalArrivesInProportion) {
    const qb::duration one_us = 1us, one_ms = 1ms;
    const auto         a = ticks_with(&one_us);
    const auto         b = ticks_with(&one_ms);
    EXPECT_GT(a, 0u);
    EXPECT_GT(b, a * 500) << "a millisecond is at least 500x a microsecond in the counter's ticks";
    EXPECT_LT(b, a * 2000) << "and at most 2000x: the calibration is a calibration, not noise";
}

} // namespace core_io_poll_interval_test
