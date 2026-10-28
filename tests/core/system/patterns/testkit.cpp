/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/testkit.cpp
 * @brief The shipped actor test kit (`qb/testkit.h`, Huly QB-57) against a running engine.
 *
 * Every case is a scenario run by a `qb::test::TestKit` -- the kit's own shape, with no global atomics
 * and no hand-written driver -- and each asserts both what the scenario observed and how the run ended
 * (`RunResult`). Covered: the expectation of an event sent to the probe, its sender kept; the probe's
 * per-type FIFO and an event recorded before the expectation; a timeout (a lower bound only: the probe
 * answers at its deadline, never earlier); an event arriving after a timed-out expectation, kept for the
 * next; `expect_none` over a quiet window, broken by an arrival, consuming nothing; two waiters, oldest first;
 * a probe on another core; a type the probe does not record; the exchanges a scenario asks other actors
 * with, registered by `run<Asks...>` (and the timeout without them, the reason the parameter exists); a
 * scenario that throws; the watchdog; an engine that refuses to start; one run per kit.
 */

#include <gtest/gtest.h>
#include <qb/testkit.h>
#include <chrono>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace testkit_test {

struct Order : qb::Event {
    int qty = 0;
    explicit Order(int q)
        : qty(q) {}
};

struct Filled : qb::Event {
    int qty = 0;
    explicit Filled(int q)
        : qty(q) {}
};

struct Cancelled : qb::Event {
    int qty = 0;
    explicit Cancelled(int q)
        : qty(q) {}
};

/// The actor under test: fills every order, to the destination it was given.
class Desk : public qb::Actor {
    qb::ActorId _report;

public:
    explicit Desk(qb::ActorId report)
        : _report(report) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Order>(*this);
        co_return true;
    }
    void
    on(Order const &o) {
        if (o.qty < 0)
            push<Cancelled>(_report, -o.qty);
        else
            push<Filled>(_report, o.qty);
    }
};

/// A request another actor answers: what `run<Asks...>` exists for.
struct Quote : qb::Request<int> {
    int base = 0;
    Quote()  = default;
    explicit Quote(int b)
        : base(b) {}
};

class Quoter : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Quote>(*this);
        co_return true;
    }
    void
    on(Quote &q) {
        qb::answer(*this, q, [](Quote const &r) { return r.base * 2; });
    }
};

using steady = std::chrono::steady_clock;

/// The branch that wins a race against an expectation: it ends after `d`, with nothing.
qb::io::async::task<int>
pause(qb::ScopedCoroContext ctx, qb::duration d) {
    co_await ctx.sleep(d);
    co_return 0;
}

} // namespace testkit_test

using namespace testkit_test;

TEST(TestKit, ExpectYieldsTheEventSentToTheProbeWithItsSender) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run([desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        ctx.push_to<Order>(desk, 7);
        const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 2s);
        EXPECT_TRUE(filled.has_value());
        if (filled) {
            EXPECT_EQ(filled->qty, 7);
            EXPECT_EQ(filled->getSource(), desk) << "the recorded copy keeps its sender";
        }
    });
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(run.describe(), "ok");
}

TEST(TestKit, EachTypeIsAQueueInArrivalOrderAndARecordedEventIsYieldedAtOnce) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled, Cancelled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run([desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        ctx.push_to<Order>(desk, 1);
        ctx.push_to<Order>(desk, -5);
        ctx.push_to<Order>(desk, 2);
        ctx.push_to<Order>(desk, 3);
        const auto first = co_await qb::test::expect<Filled>(ctx, probe, 2s);
        // The three fills left the desk in one pass: the second and the third are recorded by the time
        // the first is answered, so a zero timeout takes them -- it never waits.
        const auto second    = co_await qb::test::expect<Filled>(ctx, probe, qb::duration::zero());
        const auto third     = co_await qb::test::expect<Filled>(ctx, probe, qb::duration::zero());
        const auto none      = co_await qb::test::expect<Filled>(ctx, probe, qb::duration::zero());
        const auto cancelled = co_await qb::test::expect<Cancelled>(ctx, probe, qb::duration::zero());
        EXPECT_EQ(first ? first->qty : -1, 1);
        EXPECT_EQ(second ? second->qty : -1, 2);
        EXPECT_EQ(third ? third->qty : -1, 3);
        EXPECT_FALSE(none.has_value()) << "three fills, three expectations: nothing left";
        EXPECT_EQ(cancelled ? cancelled->qty : -1, 5) << "the other type has its own queue";
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ATimeoutYieldsNothingNoEarlierThanItsDeadline) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        run   = kit.run([probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        const auto start  = steady::now();
        const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 100ms);
        const auto took   = steady::now() - start;
        EXPECT_FALSE(filled.has_value());
        EXPECT_GE(took, 100ms) << "the probe answers at its deadline, never earlier";
        // An absence is told apart from a probe that never answers (false): this true is the probe's own answer.
        const auto quiet_from = steady::now();
        EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 100ms));
        EXPECT_GE(steady::now() - quiet_from, 100ms);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnEventLentToAnAbandonedExpectationReturnsForTheNext) {
    // The losing branch of a when_any leaves its waiter on the probe; the event that reaches it is lent, never
    // acknowledged, and comes back after detail::lend_hold for the next expectation -- it was lost before.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto race = co_await qb::io::async::when_any(qb::test::expect<Filled>(ctx, probe, 5s), pause(ctx, 50ms));
            EXPECT_EQ(race.index, 1u) << "the pause wins: the expectation is abandoned, its waiter still on the probe";
            ctx.push_to<Order>(desk, 7); // answered to the abandoned waiter, lent
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 10s);
            EXPECT_TRUE(filled.has_value()) << "an event lent to an abandoned expectation is never lost";
            if (filled)
                EXPECT_EQ(filled->qty, 7);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnAcknowledgedEventDoesNotComeBack) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Order>(desk, 3);
            EXPECT_TRUE((co_await qb::test::expect<Filled>(ctx, probe, 5s)).has_value());
            EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, qb::test::detail::lend_hold + 500ms))
                << "the expectation acknowledged its event: the loan is over";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnEventArrivingAfterATimedOutExpectationIsKeptForTheNext) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run([desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        EXPECT_FALSE((co_await qb::test::expect<Filled>(ctx, probe, 30ms)).has_value());
        ctx.push_to<Order>(desk, 4);
        const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 2s);
        EXPECT_EQ(filled ? filled->qty : -1, 4) << "the expired waiter must not swallow a later arrival";
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ExpectNoneHoldsOverAQuietWindowBreaksOnAnArrivalAndConsumesNothing) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run([desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        const auto start = steady::now();
        EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 50ms));
        EXPECT_GE(steady::now() - start, 50ms) << "absence is proven at the end of the window";

        ctx.push_to<Order>(desk, 9);
        const auto broken = steady::now();
        EXPECT_FALSE(co_await qb::test::expect_none<Filled>(ctx, probe, 30s));
        EXPECT_LT(steady::now() - broken, 15s) << "an arrival answers at once, not at the end of the window";
        EXPECT_FALSE(co_await qb::test::expect_none<Filled>(ctx, probe, 30s)) << "already recorded: false at once";

        const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 2s);
        EXPECT_EQ(filled ? filled->qty : -1, 9) << "expect_none consumed nothing";
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, TwoWaitersAtOnceAreServedOldestFirst) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run([desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        auto a      = qb::test::expect<Filled>(ctx, probe, 5s);
        auto b      = qb::test::expect<Filled>(ctx, probe, 5s);
        auto orders = [desk](qb::ScopedCoroContext c) -> qb::io::async::task<int> {
            co_await c.sleep(20ms); // both waiters parked first
            c.push_to<Order>(desk, 1);
            c.push_to<Order>(desk, 2);
            co_return 2;
        };
        auto [first, second, sent] = co_await qb::io::async::when_all(std::move(a), std::move(b), orders(ctx));
        EXPECT_EQ(sent, 2);
        // when_all starts its tasks in order: a's expectation reaches the probe first and parks first.
        EXPECT_EQ(first ? first->qty : -1, 1) << "the oldest waiter takes the first arrival";
        EXPECT_EQ(second ? second->qty : -1, 2);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AProbeOnAnotherCoreIsExpectedAcrossTheMailbox) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(1);
    const auto        desk  = kit.add<Desk>(1, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Order>(desk, 11);
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 5s);
            EXPECT_EQ(filled ? filled->qty : -1, 11);
            EXPECT_EQ(filled ? filled->getSource() : qb::ActorId{}, desk);
        },
        30s, 0);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ExpectingATypeTheProbeDoesNotRecordIsALogicError) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        run   = kit.run([probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        bool threw = false;
        try {
            (void) co_await qb::test::expect<Cancelled>(ctx, probe, 2s);
        } catch (std::logic_error const &) {
            threw = true;
        }
        EXPECT_TRUE(threw) << "a test written against the wrong probe must fail loudly";
        threw = false;
        try {
            (void) co_await qb::test::expect_none<Cancelled>(ctx, probe, 2s);
        } catch (std::logic_error const &) {
            threw = true;
        }
        EXPECT_TRUE(threw);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, RunRegistersTheExchangesTheScenarioAsksOtherActorsWith) {
    {
        qb::test::TestKit kit;
        const auto        quoter = kit.add<Quoter>(0);
        const auto        run    = kit.run<Quote>([quoter](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto q = co_await qb::ask(ctx, quoter, Quote{21}, 2s);
            EXPECT_EQ(q.response, 42);
        });
        EXPECT_TRUE(run) << run.describe();
    }
    {
        // Without <Quote> the driver does not register the reply: the ask cannot resolve and times out.
        qb::test::TestKit kit;
        const auto        quoter = kit.add<Quoter>(0);
        const auto        run    = kit.run([quoter](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            bool timed_out = false;
            try {
                (void) co_await qb::ask(ctx, quoter, Quote{21}, 100ms);
            } catch (qb::io::async::timeout_error const &) {
                timed_out = true;
            }
            EXPECT_TRUE(timed_out) << "the reason run<Asks...> exists";
        });
        EXPECT_TRUE(run) << run.describe();
    }
}

TEST(TestKit, AScenarioThatThrowsIsReportedByTheRun) {
    qb::test::TestKit kit;
    (void) kit.probe<Filled>(0);
    const auto run = kit.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> {
        throw std::runtime_error("no fill");
        co_return;
    });
    EXPECT_FALSE(run);
    EXPECT_FALSE(run.completed);
    EXPECT_FALSE(run.timed_out);
    ASSERT_TRUE(run.error);
    EXPECT_NE(run.describe().find("the scenario threw: no fill"), std::string::npos) << run.describe();
}

TEST(TestKit, TheWatchdogStopsAScenarioThatNeverEnds) {
    qb::test::TestKit kit;
    const auto        start = steady::now();
    const auto run = kit.run([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await ctx.sleep(std::chrono::hours(1)); }, 200ms);
    EXPECT_FALSE(run);
    EXPECT_TRUE(run.timed_out);
    EXPECT_FALSE(run.completed);
    EXPECT_GE(steady::now() - start, 200ms);
    EXPECT_FALSE(run.error) << "the scenario is torn down with its core, never resumed: nothing thrown is reported";
    EXPECT_NE(run.describe().find("the watchdog stopped the engine"), std::string::npos) << run.describe();
}

TEST(TestKit, AnEngineThatFailsToStartIsReported) {
    qb::test::TestKit kit;
    (void) kit.engine().core(1); // a registered core with no actor: the engine refuses to start
    const auto run = kit.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; }, 300ms);
    EXPECT_FALSE(run);
    EXPECT_TRUE(run.engine_error) << run.describe();
    EXPECT_FALSE(run.completed);
}

TEST(TestKit, AKitRunsOnce) {
    qb::test::TestKit kit;
    const auto        run = kit.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; });
    EXPECT_TRUE(run) << run.describe();
    EXPECT_THROW((void) kit.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; }), std::logic_error);
}
