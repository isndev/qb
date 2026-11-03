/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/request-deadline.cpp
 * @brief Absolute-deadline budget propagation: `qb::deadline_in` / `qb::remaining` / `qb::ask_by`.
 *
 * A `qb::deadline` is an ABSOLUTE completion time threaded through a chain of `ask_by` calls so the
 * WHOLE chain is bounded end-to-end (unlike a relative per-`ask` timeout that resets at each hop).
 * Proven here against the running engine, with three exact oracles in one coroutine pass:
 *   - BUDGET: `remaining(deadline_in(ctx, 100ms), ctx)` reads back EXACTLY 100ms — the core's
 *     monotonic sample is taken once per loop pass (`time()`'s date was, until 3.4: Huly QB-1199), so
 *     building and querying the deadline in the same pass is exact to the nanosecond (`100'000'000 ns`);
 *   - FAIL-FAST: `ask_by` with an already-spent deadline (`deadline{0}`) throws `timeout_error`
 *     IMMEDIATELY without sending a request;
 *   - SUCCESS: `ask_by` with a future deadline completes within budget and returns the
 *     responder-computed value (seq 5 + 1 = 6).
 *
 * The three outcomes are mirrored to atomics behind a single "ran" flag asserted after join(), so a
 * never-scheduled coroutine cannot pass any of them vacuously. Shutdown is driven by the asker the
 * instant its coroutine finishes — no wall-clock stop offset.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <atomic>
#include <chrono>
#include "../../shared/ProbeResponders.h"

using namespace qb;
using namespace std::chrono_literals;
using qb::test::FastResponder;
using qb::test::Probe;

namespace {
std::atomic<long> g_dl_remaining_ns{-1};
std::atomic<bool> g_dl_past_timeout{false};
std::atomic<int>  g_dl_val{-1};
std::atomic<bool> g_dl_ran{false}; // the asker coroutine reached the end (ran-guard)
} // namespace

class DeadlineAsker : public qb::Actor {
    qb::ActorId _fast;

public:
    explicit DeadlineAsker(qb::ActorId fast)
        : _fast(fast) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Probe>(*this);
        auto fast = _fast;
        spawn([fast](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            // The core samples the monotonic clock once per loop pass, so deadline_in + remaining in the same pass is exact.
            const auto dl = qb::deadline_in(c, 100ms);
            g_dl_remaining_ns.store(static_cast<long>(qb::remaining(dl, c).count()));

            // Past deadline → fail fast (no request sent).
            try {
                (void) co_await qb::ask_by(c, fast, Probe{5}, qb::deadline{0});
            } catch (const qb::io::async::timeout_error &) {
                g_dl_past_timeout.store(true);
            }
            // Future deadline → succeeds within budget.
            try {
                auto r = co_await qb::ask_by(c, fast, Probe{5}, qb::deadline_in(c, 500ms));
                g_dl_val.store(r.response);
            } catch (...) {
            }
            g_dl_ran.store(true);
            qb::Main::stop(); // event-driven: stop the instant the coroutine finishes
        });
        co_return true;
    }
    void
    on(Probe &e) {
        resolve_ask(e);
    }
};

TEST(Deadline, BudgetPropagationAndFailFast) {
    g_dl_remaining_ns.store(-1);
    g_dl_past_timeout.store(false);
    g_dl_val.store(-1);
    g_dl_ran.store(false);
    qb::Main   main;
    const auto fast = main.addActor<FastResponder>(0);
    main.addActor<DeadlineAsker>(0, fast);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_dl_ran.load()) << "the deadline asker coroutine must have run";
    EXPECT_EQ(g_dl_remaining_ns.load(), 100'000'000L) << "exact within one loop pass: one monotonic sample per pass";
    EXPECT_TRUE(g_dl_past_timeout.load()) << "an already-spent budget must fail fast";
    EXPECT_EQ(g_dl_val.load(), 6) << "future deadline → reply (seq 5 + 1)";
}

TEST(Deadline, DeadlineAtIsTheMonotonicInstant) {
    // `deadline_at` is how a deadline is built from an instant (since 3.4, Huly QB-1199): `at_ns` is that instant of
    // `qb::mono_now()`'s clock in nanoseconds. Before its epoch is the past (0); the clock's end is a deadline never met.
    const qb::mono_time when = qb::mono_now() + 5s;
    EXPECT_EQ(qb::deadline_at(when).at_ns,
              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count()));
    EXPECT_EQ(qb::deadline_at(qb::mono_time{}).at_ns, 0u);
    EXPECT_EQ(qb::deadline_at(qb::mono_time::min()).at_ns, 0u);
    EXPECT_EQ(qb::deadline_at(qb::mono_time::max()).at_ns, static_cast<std::uint64_t>(qb::duration::max().count()));
}

namespace request_deadline_test {

/// What `remaining` reads of the two spellings of "no deadline", and of the deadline 3.3 let a caller build by hand.
std::atomic<long long> g_none_in{-1}, g_none_at{-1}, g_none_max{-1};

// Reads what is left of the two "no deadline" spellings -- `deadline_in(ctx, duration::max())` and
// `deadline_at(mono_time::max())` -- or, `dated`, of a deadline built by hand from the time of day, two years ahead: the
// way 3.3 allowed, an epoch value a debug build must name.
class NoDeadlineReader : public qb::Actor {
    bool _dated;

public:
    explicit NoDeadlineReader(bool dated)
        : _dated(dated) {}

    qb::io::async::task<bool>
    onInit() override {
        spawn([dated = _dated](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            if (dated) {
                constexpr std::uint64_t two_years = 2ull * 365 * 24 * 3600 * 1'000'000'000;
                (void) qb::remaining(qb::deadline{static_cast<std::uint64_t>(qb::unix_nanos(qb::wall_now())) + two_years}, c);
            } else {
                g_none_in  = qb::remaining(qb::deadline_in(c, qb::duration::max()), c).count();
                g_none_at  = qb::remaining(qb::deadline_at(qb::mono_time::max()), c).count();
                g_none_max = qb::remaining(qb::deadline{UINT64_MAX}, c).count(); // past qb::duration's range: clamped
            }
            qb::Main::stop();
            co_return;
        });
        co_return true;
    }
};

inline void
read_deadlines(bool const dated) {
    qb::Main main;
    main.addActor<NoDeadlineReader>(0, dated);
    main.start(false);
    main.join();
}

} // namespace request_deadline_test

TEST(Deadline, TheNoDeadlineSpellingsPassTheDateCheck) {
    // A deadline beyond reach is how "no deadline" is spelled, and both spellings land centuries past the time of day: the
    // debug check that names a date stays silent. Left: 292 years of `deadline_in`'s, 292 less the uptime of `deadline_at`'s.
    request_deadline_test::read_deadlines(false);
    constexpr long long kTwoHundredYears = 200ll * 365 * 24 * 3600 * 1'000'000'000;
    EXPECT_GT(request_deadline_test::g_none_in.load(), kTwoHundredYears);
    EXPECT_GT(request_deadline_test::g_none_at.load(), kTwoHundredYears);
    EXPECT_EQ(request_deadline_test::g_none_max.load(), qb::duration::max().count()) << "remaining() wrapped negative";
}

#ifndef NDEBUG
TEST(DeadlineDeathTest, ADeadlineBuiltFromTheTimeOfDayAssertsInADebugBuild) {
    // On the monotonic clock a deadline built from a date lies decades ahead: a release build waits for it as long as the
    // process lives; a debug build names the mistake where the deadline is read (`remaining`, so `ask_by`) ...
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(request_deadline_test::read_deadlines(true), "looks like a wall-clock epoch value");
    // ... and where it is built from an instant of the wrong clock.
    EXPECT_DEATH((void) qb::deadline_at(qb::mono_time{std::chrono::nanoseconds{qb::unix_nanos(qb::wall_now())}}),
                 "looks like a wall-clock epoch value");
}
#endif
