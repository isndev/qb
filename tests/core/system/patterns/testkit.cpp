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
 * scenario that throws; the watchdog; an engine that refuses to start; one run per kit; the loan of an answered
 * event -- abandoned expectations withdrawn, loans given back at their rank, a late acknowledgement -- and the
 * probe's watch of its askers -- a killed asker's waiters and loans let go of, on its core and across cores, its
 * loans in arrival order -- with a probe of `qb::DownEvent` still recording the ones it is sent.
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

/// An asker killed while two expectations of it wait on the probe: a killed asker withdraws nothing.
class Abandoner : public qb::Actor {
    qb::ActorId _probe;

public:
    explicit Abandoner(qb::ActorId probe)
        : _probe(probe) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        spawn([probe = _probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            (void) co_await qb::io::async::when_all(qb::test::expect<Filled>(ctx, probe, 30s), qb::test::expect<Filled>(ctx, probe, 30s));
        });
        co_return true;
    }
    void
    on(qb::test::detail::Expectation &e) {
        (void) resolve_ask(e);
    }
};

/// An asker that takes the probe's answer and never acknowledges it -- a raw ask, as an asker killed between the two.
class Hoarder : public qb::Actor {
    qb::ActorId _probe;

public:
    explicit Hoarder(qb::ActorId probe)
        : _probe(probe) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        spawn([probe = _probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            (void) co_await qb::ask(ctx, probe, qb::test::detail::Expectation{qb::type_id<Filled>(), 30s, false, 0}, 30s);
        });
        co_return true;
    }
    void
    on(qb::test::detail::Expectation &e) {
        (void) resolve_ask(e);
    }
};

/// A death report about `who` -- what a supervisor sends -- for the actor under test below.
struct Report : qb::Event {
    qb::ActorId who;

    explicit Report(qb::ActorId w)
        : who(w) {}
};

/// An actor under test that sends the probe a `qb::DownEvent` about the actor a `Report` names.
class Reporter : public qb::Actor {
    qb::ActorId _probe;

public:
    explicit Reporter(qb::ActorId probe)
        : _probe(probe) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Report>(*this);
        co_return true;
    }
    void
    on(Report const &r) {
        push<qb::DownEvent>(_probe, r.who, qb::DownReason::killed);
    }
};

/// Asks the hoarder for one more raw expectation of `Filled`.
struct AskNow : qb::Event {};

/// A `Hoarder` that asks on command: each `AskNow` is one more answer taken and never acknowledged.
class HoarderOnCommand : public qb::Actor {
    qb::ActorId _probe;

public:
    explicit HoarderOnCommand(qb::ActorId probe)
        : _probe(probe) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        registerEvent<AskNow>(*this);
        co_return true;
    }
    void
    on(AskNow const &) {
        spawn([probe = _probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            (void) co_await qb::ask(ctx, probe, qb::test::detail::Expectation{qb::type_id<Filled>(), 30s, false, 0}, 30s);
        });
    }
    void
    on(qb::test::detail::Expectation &e) {
        (void) resolve_ask(e);
    }
};

/// The branch that wins a race against an expectation: it ends after `d`, with nothing.
qb::io::async::task<int>
pause(qb::ScopedCoroContext ctx, qb::duration d) {
    co_await ctx.sleep(d);
    co_return 0;
}

/// The branch that wins a race at once, before the expectation's answer can come back.
qb::io::async::task<int>
at_once() {
    co_return 1;
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
    // The losing branch of a when_any is destroyed: its expectation withdraws itself, so the next one gets the event
    // (it was lost before the loan). The loan alone also passes this case, in lend_hold: the timing is pinned by
    // AbandonedExpectationsAreWithdrawnSoTheNextGetsTheEventAtOnce.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto race = co_await qb::io::async::when_any(qb::test::expect<Filled>(ctx, probe, 5s), pause(ctx, 50ms));
            EXPECT_EQ(race.index, 1u) << "the pause wins: the expectation is abandoned";
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

TEST(TestKit, AbandonedExpectationsAreWithdrawnSoTheNextGetsTheEventAtOnce) {
    // Waiters abandoned by when_any losers stayed on the probe: each was lent the event in turn for lend_hold, so a
    // live expectation behind three of them waited 6 s, and expect_none saw "absent" while the event sat with a
    // dead waiter (the second review).
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (int i = 0; i < 3; ++i) {
                const auto race = co_await qb::io::async::when_any(qb::test::expect<Filled>(ctx, probe, 30s), pause(ctx, 20ms));
                EXPECT_EQ(race.index, 1u);
            }
            ctx.push_to<Order>(desk, 7);
            co_await ctx.sleep(50ms); // the fill has reached the probe: before the withdrawals it sat lent to a dead waiter
            EXPECT_FALSE(co_await qb::test::expect_none<Filled>(ctx, probe, 300ms)) << "present, not hidden with a dead waiter";
            const auto start  = steady::now();
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 1s);
            EXPECT_EQ(filled ? filled->qty : -1, 7);
            EXPECT_LT(steady::now() - start, 500ms) << "served at once, not after a loan to a dead waiter";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AKilledAskersWaitersAreLetGoOfAtOnce) {
    // A killed asker withdraws nothing (its scope is cancelled first): its two waiters stayed on the probe, each next
    // event lent to them for lend_hold -- the starvation of abandoned expectations, on the kill path (the third
    // review). The probe watches its askers: at the asker's death its waiters are dropped.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        asker = kit.add<Abandoner>(0, probe);
    const auto        run   = kit.run(
        [desk, probe, asker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(100ms); // both its expectations wait on the probe
            ctx.push_to<qb::KillEvent>(asker);
            co_await ctx.sleep(100ms); // its DownEvent has reached the probe
            for (int qty : {1, 2, 3})
                ctx.push_to<Order>(desk, qty);
            const auto start = steady::now();
            for (int qty : {1, 2, 3}) {
                const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 1s);
                EXPECT_EQ(filled ? filled->qty : -1, qty) << "in arrival order, none lent to the dead asker";
            }
            EXPECT_LT(steady::now() - start, 1s) << "not after a loan to the dead asker's waiters";
            EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 100ms));
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, WhatADeadAskerWasLentComesBackAtOnce) {
    // The asker took the answer and died before it acknowledged it: the event came back only after lend_hold (the
    // third review). Its DownEvent gives the loan back at once, at its rank.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        asker = kit.add<Hoarder>(0, probe);
    const auto        run   = kit.run(
        [desk, probe, asker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(100ms); // its expectation waits on the probe
            ctx.push_to<Order>(desk, 1);
            co_await ctx.sleep(100ms);
            EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 100ms)) << "1 is lent to the asker";
            ctx.push_to<Order>(desk, 2);
            ctx.push_to<qb::KillEvent>(asker);
            co_await ctx.sleep(100ms); // its DownEvent has reached the probe
            const auto start = steady::now();
            for (int qty : {1, 2}) {
                const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 1s);
                EXPECT_EQ(filled ? filled->qty : -1, qty) << "the dead asker's loan back at its rank";
            }
            EXPECT_LT(steady::now() - start, 1s) << "not after lend_hold";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, LoansGivenBackTogetherComeBackInArrivalOrder) {
    // Two expectations answered (1 and 2 lent) then withdrawn together, 3 recorded behind them: each loan returned
    // to the head of the queue put them back as [2, 1, 3] (the second review). Each goes back to its rank.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (int qty : {1, 2, 3})
                ctx.push_to<Order>(desk, qty);
            co_await ctx.sleep(50ms); // all three recorded
            // Each branch's expectation reaches the probe before at_once wins (a branch nested in a when_all would not
            // be started); the losers are withdrawn in branch order, the order of their loans.
            const auto race =
                co_await qb::io::async::when_any(qb::test::expect<Filled>(ctx, probe, 5s), qb::test::expect<Filled>(ctx, probe, 5s), at_once());
            EXPECT_EQ(race.index, 2u);
            for (int qty : {1, 2, 3}) {
                const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 1s);
                EXPECT_EQ(filled ? filled->qty : -1, qty) << "arrival order, the loans given back included";
            }
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AProbeThatRecordsDownEventRecordsTheOnesItIsSent) {
    // A probe of qb::DownEvent -- the destination of a supervisor's reports -- recorded none: the watch of its
    // askers took every DownEvent, and one naming a live asker dropped its waiters (the fourth review). Only the
    // core's answer to the probe's own watch is the probe's.
    qb::test::TestKit kit;
    const auto        probe    = kit.probe<qb::DownEvent>(0);
    const auto        reporter = kit.add<Reporter>(0, probe);
    const auto        run      = kit.run([probe, reporter](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        ctx.push_to<Report>(reporter, ctx.id()); // a report about the driver, an asker of the probe
        const auto about_the_driver = co_await qb::test::expect<qb::DownEvent>(ctx, probe, 1s);
        EXPECT_EQ(about_the_driver ? about_the_driver->watched : qb::ActorId{}, ctx.id());
        ctx.push_to<Report>(reporter, reporter); // about itself: its source is the actor it names
        const auto about_itself = co_await qb::test::expect<qb::DownEvent>(ctx, probe, 1s);
        EXPECT_EQ(about_itself ? about_itself->watched : qb::ActorId{}, reporter);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ADownEventSentToTheProbeLetsGoOfNoLiveAsker) {
    qb::test::TestKit kit;
    const auto        probe    = kit.probe<Filled, qb::DownEvent>(0);
    const auto        desk     = kit.add<Desk>(0, probe);
    const auto        reporter = kit.add<Reporter>(0, probe);
    const auto        run      = kit.run([probe, desk, reporter](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        auto report_then_fill = [](qb::ScopedCoroContext c, qb::ActorId reporter, qb::ActorId desk) -> qb::io::async::task<void> {
            co_await c.sleep(50ms);              // the driver's expectation waits on the probe
            c.push_to<Report>(reporter, c.id()); // "the driver is gone": wrong, and not the probe's watch
            co_await c.sleep(50ms);
            c.push_to<Order>(desk, 7);
        };
        auto [filled, done] = co_await qb::io::async::when_all(qb::test::expect<Filled>(ctx, probe, 1s), report_then_fill(ctx, reporter, desk));
        (void) done;
        EXPECT_EQ(filled ? filled->qty : -1, 7) << "the live driver's waiter was dropped by a DownEvent it was sent";
        EXPECT_TRUE(co_await qb::test::expect<qb::DownEvent>(ctx, probe, 0s)) << "the report is recorded";
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AKilledAskerOnAnotherCoreIsLetGoOfAtOnce) {
    // The death watch across cores: the probe on core 1, the killed asker on core 2, the driver on core 0.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(1);
    const auto        desk  = kit.add<Desk>(1, probe);
    const auto        asker = kit.add<Abandoner>(2, probe);
    const auto        run   = kit.run(
        [desk, probe, asker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(100ms); // both its expectations wait on the probe
            ctx.push_to<qb::KillEvent>(asker);
            co_await ctx.sleep(100ms); // its DownEvent has reached the probe
            for (int qty : {1, 2, 3})
                ctx.push_to<Order>(desk, qty);
            const auto start = steady::now();
            for (int qty : {1, 2, 3}) {
                const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 1s);
                EXPECT_EQ(filled ? filled->qty : -1, qty);
            }
            EXPECT_LT(steady::now() - start, 1s) << "not after a loan to the dead asker's waiters";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ADeadAskersLoansReachAParkedWaiterInArrivalOrder) {
    // The hoarder holds 2, then 1 (given back by a withdrawal and lent to it again) when it dies, and the driver
    // waits on the probe: the loans went back in the order they were lent, 2 first (the fourth review).
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(0);
    const auto        desk    = kit.add<Desk>(0, probe);
    const auto        hoarder = kit.add<HoarderOnCommand>(0, probe);
    const auto        run     = kit.run(
        [desk, probe, hoarder](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto first_waiter = [](qb::ScopedCoroContext c, qb::ActorId probe) -> qb::io::async::task<void> {
                (void) co_await qb::ask(c, probe, qb::test::detail::Expectation{qb::type_id<Filled>(), 5s, false, 777}, 5s);
            };
            auto choreography = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId desk,
                                   qb::ActorId hoarder) -> qb::io::async::task<void> {
                co_await c.sleep(20ms); // the driver's raw expectation (token 777) waits
                c.push_to<AskNow>(hoarder);
                co_await c.sleep(20ms);    // and the hoarder's first
                c.push_to<Order>(desk, 1); // 1 lent to the driver's
                c.push_to<Order>(desk, 2); // 2 lent to the hoarder's
                co_await c.sleep(20ms);
                c.push_to<AskNow>(hoarder); // the hoarder's second waits
                co_await c.sleep(20ms);
                c.push_to<qb::test::detail::ExpectationWithdrawn>(probe, 777); // 1 back, lent to it: it holds 2, then 1
                co_await c.sleep(20ms);
                auto kill_later = [](qb::ScopedCoroContext k, qb::ActorId hoarder) -> qb::io::async::task<void> {
                    co_await k.sleep(30ms); // the expectation below waits on the probe first
                    k.push_to<qb::KillEvent>(hoarder);
                };
                auto [parked, killed] = co_await qb::io::async::when_all(qb::test::expect<Filled>(c, probe, 1s), kill_later(c, hoarder));
                (void) killed;
                EXPECT_EQ(parked ? parked->qty : -1, 1) << "the parked waiter got a later event first";
                const auto next = co_await qb::test::expect<Filled>(c, probe, 200ms);
                EXPECT_EQ(next ? next->qty : -1, 2);
            };
            co_await qb::io::async::when_all(first_waiter(ctx, probe), choreography(ctx, probe, desk, hoarder));
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnAcknowledgementAfterItsLoanExpiredTakesTheCopyGivenBack) {
    // An answer the asker took after the loan expired -- its core stalled past lend_hold -- was served twice: the
    // probe had given it back already (the second review). A raw ask stands for the late asker.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Order>(desk, 5);
            co_await ctx.sleep(100ms);
            const auto answer = co_await qb::ask(ctx, probe, qb::test::detail::Expectation{qb::type_id<Filled>(), {}, false, 0}, 1s);
            EXPECT_NE(answer.lend, 0u);
            co_await ctx.sleep(qb::test::detail::lend_hold + 300ms); // expired: given back
            ctx.push_to<qb::test::detail::ExpectationTaken>(probe, answer.lend);
            EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 200ms)) << "the late taker has it: not served again";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ATimeoutOfMaxIsNoDeadlineNotAnOverflow) {
    // wait + reply_grace overflowed for qb::duration::max() (signed overflow; UBSan reports it).
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run([desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        ctx.push_to<Order>(desk, 9);
        const auto filled = co_await qb::test::expect<Filled>(ctx, probe, qb::duration::max());
        EXPECT_EQ(filled ? filled->qty : -1, 9);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnExpectationWithdrawnAfterItsAnswerGivesTheEventBackAtOnce) {
    // The probe answered (the event lent) before the withdrawal reached it: the withdrawal gives the loan back at once,
    // not after lend_hold.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Order>(desk, 8);
            co_await ctx.sleep(50ms); // recorded: the expectation below is answered at once, the event lent
            const auto race = co_await qb::io::async::when_any(qb::test::expect<Filled>(ctx, probe, 5s), at_once());
            EXPECT_EQ(race.index, 1u);
            const auto start  = steady::now();
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 1s);
            EXPECT_EQ(filled ? filled->qty : -1, 8);
            EXPECT_LT(steady::now() - start, 500ms) << "given back by the withdrawal, not by the loan's expiry";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}
