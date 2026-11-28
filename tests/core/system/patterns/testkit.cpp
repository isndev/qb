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
 * and no driver written by hand -- and each asserts both what the scenario observed and how the run ended
 * (`RunResult`). Covered: the expectation of an event sent to the probe, its sender kept; the probe's
 * per-type FIFO and an event recorded before the expectation; a timeout (a lower bound only: the probe
 * answers at its deadline, never earlier); an event arriving after a timed-out expectation, kept for the
 * next; `expect_none` over a quiet window, broken by an arrival, consuming nothing; two waiters, oldest first;
 * a probe on another core; a type the probe does not record; the exchanges a scenario asks other actors
 * with, registered by `run<Asks...>` (and the timeout without them, the reason the parameter exists); a
 * scenario that throws; the watchdog; an engine that refuses to start; one run per kit; the loan of an answered
 * event -- abandoned expectations withdrawn, loans given back at their rank, a loan kept through a stall -- and the
 * probe's watch of its askers -- a killed asker's waiters and loans let go of, on its core and across cores, its
 * loans in arrival order -- with a probe of `qb::DownEvent` still recording the ones it is sent; and the probe's clock
 * as the only one -- a host that stalls past a window or a timeout changes no answer -- with a probe that is gone
 * never read as an answer: killed, it tells the expectations waiting and the ones on their way; ended, or never a
 * probe, it is refused before anything is sent; stopped with its engine, it says nothing; stopped with its core on an
 * error, the kit's driver hears it and its scenario's expectations throw, another actor's wait. One probe holds an
 * id: a second, of another kit or engine alive at once, is refused loudly. A case orders its steps by events -- a
 * death heard, an expectation sent, a turn of a core -- not by a sleep; where a clock is the subject (a window that a
 * hold must outlast, a stall) the wait is longer than what it must cover, never a bound from above.
 */

#include <gtest/gtest.h>
#include <qb/testkit.h>
#include "../../shared/InitFixtures.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

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

/// Whether `qb::test::TestProbe<Events...>` is a type: the kit refuses some at compile time.
template <typename... Events>
concept a_probe_records = requires { typename qb::test::TestProbe<Events...>; };

static_assert(a_probe_records<Filled>);
static_assert(a_probe_records<Filled, Cancelled, qb::DownEvent, qb::KillEvent>, "the engine's events a probe may record");
// A probe that recorded `qb::SignalEvent` registered its own handler in place of the one that ends an actor when its
// engine stops: it recorded the stop and stayed, and `TestKit::run()` never returned from its join.
static_assert(!a_probe_records<qb::SignalEvent>, "a probe that records qb::SignalEvent never stops with its engine");
static_assert(!a_probe_records<Filled, qb::SignalEvent>);
static_assert(!a_probe_records<qb::test::detail::Expectation>, "the kit's own exchange is not a recorded type");

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

/// Tells `told` -- a probe of `Cancelled` -- `what`. Started as the LAST branch of a `when_all`, it follows what the
/// branches before it sent: when `told` sits beside their destination, one pipe carries both, in that order.
qb::io::async::task<void>
tell_now(qb::ScopedCoroContext ctx, qb::ActorId told, int what) {
    ctx.push_to<Cancelled>(told, what);
    co_return;
}

/// An asker killed while two expectations of it wait on the probe: a killed asker withdraws nothing. It tells `told`
/// (1) once both are sent.
class Abandoner : public qb::Actor {
    qb::ActorId _probe;
    qb::ActorId _told;

public:
    Abandoner(qb::ActorId probe, qb::ActorId told)
        : _probe(probe)
        , _told(told) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        spawn([probe = _probe, told = _told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            (void) co_await qb::io::async::when_all(qb::test::expect<Filled>(ctx, probe, 30s), qb::test::expect<Filled>(ctx, probe, 30s),
                                                    tell_now(ctx, told, 1));
        });
        co_return true;
    }
    void
    on(qb::test::detail::Expectation &e) {
        (void) resolve_ask(e);
    }
};

/// A raw expectation of `Filled`, never acknowledged, that tells `told` (1) once it is sent and (2) once it is answered.
qb::io::async::task<void>
hoard_one(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::ActorId told) {
    (void) co_await qb::io::async::when_all(qb::ask(ctx, probe, qb::test::detail::Expectation{qb::type_id<Filled>(), 30s, false, 0}, 30s),
                                            tell_now(ctx, told, 1));
    ctx.push_to<Cancelled>(told, 2);
}

/// An asker that takes the probe's answer and never acknowledges it -- a raw ask, as an asker killed between the two.
class Hoarder : public qb::Actor {
    qb::ActorId _probe;
    qb::ActorId _told;

public:
    Hoarder(qb::ActorId probe, qb::ActorId told)
        : _probe(probe)
        , _told(told) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        spawn([probe = _probe, told = _told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await hoard_one(ctx, probe, told); });
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
    qb::ActorId _told;

public:
    HoarderOnCommand(qb::ActorId probe, qb::ActorId told)
        : _probe(probe)
        , _told(told) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        registerEvent<AskNow>(*this);
        co_return true;
    }
    void
    on(AskNow const &) {
        spawn([probe = _probe, told = _told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await hoard_one(ctx, probe, told); });
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

/// An asker that expects one fill through the kit -- so acknowledges it -- and tells `taken` its quantity.
class Taker : public qb::Actor {
    qb::ActorId _probe;
    qb::ActorId _taken;

public:
    Taker(qb::ActorId probe, qb::ActorId taken)
        : _probe(probe)
        , _taken(taken) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        spawn([probe = _probe, taken = _taken](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 30s);
            ctx.push_to<Cancelled>(taken, filled ? filled->qty : -1);
        });
        co_return true;
    }
    void
    on(qb::test::detail::Expectation &e) {
        (void) resolve_ask(e);
    }
};

/// Holds its core for the time it names: no pass runs there meanwhile -- a host that stalls, on demand.
struct Stall : qb::Event {
    qb::duration held;
    explicit Stall(qb::duration h)
        : held(h) {}
};

class Staller : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Stall>(*this);
        co_return true;
    }
    void
    on(Stall const &s) {
        std::this_thread::sleep_for(s.held);
    }
};

/// Asks a `Feeder` to hold `staller`'s core for `held`, then to have `desk` fill one order.
struct Feed : qb::Event {
    qb::ActorId  desk;
    qb::ActorId  staller;
    qb::duration held;

    Feed(qb::ActorId d, qb::ActorId s, qb::duration h)
        : desk(d)
        , staller(s)
        , held(h) {}
};

/// Runs beside a probe: the stall it sends another core is ahead, in this core's pipe to it, of every answer the probe
/// gives to the fill that follows -- that core reads the answer only once the stall is over, whatever the timing.
class Feeder : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Feed>(*this);
        co_return true;
    }
    void
    on(Feed const &f) {
        push<Stall>(f.staller, f.held);
        push<Order>(f.desk, 1);
    }
};

/// The quantity an expectation of a fill yielded (-1: none), and how long after `start` its asker read it.
qb::io::async::task<std::pair<int, qb::duration>>
fill_and_when(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::duration timeout, steady::time_point start) {
    const auto filled = co_await qb::test::expect<Filled>(ctx, probe, timeout);
    co_return std::pair<int, qb::duration>{filled ? filled->qty : -1, steady::now() - start};
}

/// Has `feeder` stall `staller`'s core and fill one order, once the branches started before it have sent theirs.
qb::io::async::task<void>
feed_under_a_stall(qb::ScopedCoroContext ctx, qb::ActorId feeder, qb::ActorId desk, qb::ActorId staller, qb::duration held) {
    ctx.push_to<Feed>(feeder, desk, staller, held);
    co_return;
}

/// Stalls `staller`'s core once the branch started before it has sent its expectation: one ordered pipe carries both.
qb::io::async::task<void>
stall_now(qb::ScopedCoroContext ctx, qb::ActorId staller, qb::duration held) {
    ctx.push_to<Stall>(staller, held);
    co_return;
}

/// Stalls `staller`'s core once `probe` has turned a pass since the expectation sent before this branch reached it:
/// an expectation of zero is answered at once, and the stall sent on its answer is handled in a later pass of that
/// core than the one that parked the first expectation -- the coroutine that times it ran in between: the probe's
/// window is running when the stall begins.
qb::io::async::task<void>
stall_once_the_window_runs(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::ActorId staller, qb::duration held) {
    (void) co_await qb::test::expect<Filled>(ctx, probe, 0s);
    ctx.push_to<Stall>(staller, held);
}

/// A hold of a core that the scenario begins and ends: `held` once the core is in it, until `open` is set.
struct Latch {
    std::atomic<bool> held{false};
    std::atomic<bool> open{false};
};

struct Hold : qb::Event {};

/// A hold on a latch of its own. `later`: not in the handler that reads it but at the head of the holder's next
/// receive -- the holder sends it back to itself, and a core reads what it sent itself after its loop has run and
/// before its mailbox: the coroutines spawned in the pass that read the first one have started, and no pass of the
/// core ran in between.
struct HoldOn : qb::Event {
    std::shared_ptr<Latch> latch;
    bool                   later = false;

    explicit HoldOn(std::shared_ptr<Latch> l, bool at_the_next_receive = false)
        : latch(std::move(l))
        , later(at_the_next_receive) {}
};

/// Holds its core from a `Hold` until the latch opens: a stall whose end the scenario decides, not a clock.
class Holder : public qb::Actor {
    std::shared_ptr<Latch> _latch;

    static void
    hold(Latch &latch) {
        latch.held.store(true);
        // No scenario leaves it shut: the bound only keeps a broken one from holding the engine's join for ever.
        for (const auto give_up = steady::now() + 60s; !latch.open.load() && steady::now() < give_up;)
            std::this_thread::sleep_for(1ms);
    }

public:
    explicit Holder(std::shared_ptr<Latch> latch)
        : _latch(std::move(latch)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Hold>(*this);
        registerEvent<HoldOn>(*this);
        co_return true;
    }
    void
    on(Hold const &) {
        hold(*_latch);
    }
    void
    on(HoldOn const &h) {
        if (h.later)
            push<HoldOn>(id(), h.latch);
        else
            hold(*h.latch);
    }
};

/// Opens the latch when the engine is told to stop: what the open lets through follows the stop's decision.
class Releaser : public qb::Actor {
    std::shared_ptr<Latch> _latch;

public:
    explicit Releaser(std::shared_ptr<Latch> latch)
        : _latch(std::move(latch)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SignalEvent>(*this);
        co_return true;
    }
    void
    on(qb::SignalEvent const &) {
        _latch->open.store(true);
        kill();
    }
};

/// Fills `probe` -- on another core -- then holds `holder`'s, the caller's own: the fill is answered while the hold lasts.
qb::io::async::task<void>
fill_then_hold(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::ActorId holder) {
    ctx.push_to<Filled>(probe, 7);
    ctx.push_to<Hold>(holder);
    co_return;
}

/// Holds the core of `probe` -- `holder` runs beside it -- once the probe's windows run, sends an order to the desk
/// beside the probe -- the fill is then caused on that core, once it runs again -- lets the windows end, and opens the
/// latch. Every step follows the one before it by an event or a flag, the last by a sleep longer than the windows. A
/// host that stalls between the answer below and the hold ends the windows first: the case that uses this takes either
/// account of the fill.
qb::io::async::task<void>
order_from_a_held_core_and_release(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::ActorId holder, qb::ActorId local,
                                   std::shared_ptr<Latch> latch, qb::duration windows, qb::ActorId desk) {
    (void) co_await qb::test::expect<Filled>(ctx, probe, 0s); // answered: the hold is read a pass later, the windows' timers armed
    ctx.push_to<Hold>(holder);
    while (!latch->held.load())
        co_await ctx.sleep(1ms); // the probe's core is in the hold: what is sent now waits in its mailbox
    ctx.push_to<Order>(desk, 7);
    (void) co_await qb::test::expect<Cancelled>(ctx, local, 0s); // a turn of this core: the order has left it
    co_await ctx.sleep(windows + 100ms);                         // the windows have ended, by any clock
    latch->open.store(true);
}

/// Whether the calling core holds a request deadline -- what `qb::ask` arms for a timeout of its own.
qb::io::async::task<bool>
a_request_deadline_is_armed() {
    co_return qb::detail::deadlines_armed();
}

/// A quiet window of 100 ms on a probe of `probe_core` whose core stalls 600 ms while the expectation waits on it. No
/// bound on the time the case takes: the window's answer is all it asserts, and a host that stalls elsewhere -- before
/// the stall is sent, the window over already -- makes the case prove less, never fail.
void
a_quiet_window_holds_through_a_stall_of_the_probes_core(qb::CoreId probe_core) {
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(probe_core);
    const auto        staller = kit.add<Staller>(probe_core);
    const auto        run     = kit.run(
        [probe, staller](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto [quiet, stalled] = co_await qb::io::async::when_all(qb::test::expect_none<Filled>(ctx, probe, 100ms),
                                                                     stall_once_the_window_runs(ctx, probe, staller, 600ms));
            (void) stalled;
            EXPECT_TRUE(quiet) << "a quiet window answered late by a live probe was read as an arrival";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

/// A turn of the caller's core, by a round trip to `local`, a probe of `Cancelled` beside it: what the caller pushed
/// to actors of this core before it -- and what they pushed in answer, once -- has been handled when it returns. One
/// pipe carries all of it, in order.
qb::io::async::task<void>
a_turn(qb::ScopedCoroContext ctx, qb::ActorId local) {
    (void) co_await qb::test::expect<Cancelled>(ctx, local, 0s);
}

/// Waits until the holder is in the hold of `latch`: a flag it sets, read a millisecond at a time.
qb::io::async::task<void>
until_held(qb::ScopedCoroContext ctx, std::shared_ptr<Latch> latch) {
    while (!latch->held.load())
        co_await ctx.sleep(1ms);
}

/// Has a fill wait in the mailbox of `probe`'s core while the two windows sent before this branch end, and the
/// reminders of their deadlines come due -- by the order of that core's passes, and by no clock. `holder` runs beside
/// the probe and is in the hold of `gate` when this starts: nothing this core sends is read until it opens.
///  - Behind the two expectations goes a hold `later`: the pass that parks them reads it, and the next one -- its loop
///    has started the coroutines that time the windows -- holds the core at the head of its receive (`armed`). The
///    windows run from there, and the core turns no pass: no timer of theirs fires.
///  - A second hold (`fed`) is sent while the first lasts: the same pass reads it from its mailbox. The fill is sent
///    while it lasts, behind the batch that pass has taken: it waits for the next.
///  - The windows end, by any clock, and `fed` opens. The next pass fires both timers, reads their reminders from its
///    own pipe, and the fill, in its mailbox since before either came due, behind them.
/// A host that stalls anywhere only lengthens a step: the core never turns a pass in which a window could end before
/// the fill is in its mailbox.
qb::io::async::task<void>
fill_behind_the_reminders(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::ActorId holder, qb::ActorId local, std::shared_ptr<Latch> gate,
                          qb::duration windows) {
    const auto armed = std::make_shared<Latch>();
    const auto fed   = std::make_shared<Latch>();
    ctx.push_to<HoldOn>(holder, armed, true);
    co_await a_turn(ctx, local); // the expectations and the hold have left this core
    gate->open.store(true);
    co_await until_held(ctx, armed);
    ctx.push_to<HoldOn>(holder, fed);
    co_await a_turn(ctx, local);
    armed->open.store(true);
    co_await until_held(ctx, fed);
    ctx.push_to<Filled>(probe, 7);
    co_await a_turn(ctx, local);
    co_await ctx.sleep(windows + 100ms);
    fed->open.store(true);
}

/// What `told` -- a probe of `Cancelled` -- was told next, or -1.
qb::io::async::task<int>
next_told(qb::ScopedCoroContext ctx, qb::ActorId told) {
    const auto said = co_await qb::test::expect<Cancelled>(ctx, told, 10s);
    co_return said ? said->qty : -1;
}

/// Asks a `Mourner` to watch `who`.
struct Mourn : qb::Event {
    qb::ActorId who;

    explicit Mourn(qb::ActorId w)
        : who(w) {}
};

/// Watches the actor a `Mourn` names and tells `report` -- a probe of `Cancelled` -- once it is gone: a scenario
/// waits for a death on that probe, not on a sleep.
class Mourner : public qb::Actor {
    qb::ActorId _report;

public:
    explicit Mourner(qb::ActorId report)
        : _report(report) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Mourn>(*this);
        registerEvent<qb::DownEvent>(*this);
        co_return true;
    }
    void
    on(Mourn const &m) {
        watch(m.who);
    }
    void
    on(qb::DownEvent const &) {
        push<Cancelled>(_report, 1);
    }
};

/// Waits until `asker` has told `told` that its expectations are sent -- `told` sits beside the probe, so they wait on
/// it, and the probe watches the asker -- kills it, and waits until `mourner` has told `deaths` it is gone. The mourner
/// sits beside the probe too and watches after it: a core answers its watchers in the order they came, and one pipe
/// carries both answers, so the probe has heard the death when this returns. True when both were heard.
qb::io::async::task<bool>
kill_once_it_waits(qb::ScopedCoroContext ctx, qb::ActorId asker, qb::ActorId told, qb::ActorId mourner, qb::ActorId deaths) {
    if (co_await next_told(ctx, told) != 1)
        co_return false;
    ctx.push_to<Mourn>(mourner, asker);
    ctx.push_to<qb::KillEvent>(asker);
    co_return (co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value();
}

/// What an expectation said: the probe's answer, or that the probe was gone.
enum class verdict { none = -1, quiet, arrival, gone };

template <typename E>
qb::io::async::task<verdict>
quiet_verdict_of(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::duration window) {
    try {
        co_return (co_await qb::test::expect_none<E>(ctx, probe, window)) ? verdict::quiet : verdict::arrival;
    } catch (qb::test::probe_gone_error const &) {
        co_return verdict::gone;
    }
}

qb::io::async::task<verdict>
quiet_verdict(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::duration window) {
    return quiet_verdict_of<Filled>(std::move(ctx), probe, window);
}

qb::io::async::task<verdict>
fill_verdict(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::duration timeout) {
    try {
        co_return (co_await qb::test::expect<Filled>(ctx, probe, timeout)) ? verdict::arrival : verdict::quiet;
    } catch (qb::test::probe_gone_error const &) {
        co_return verdict::gone;
    }
}

/// Kills `target` once the branches started before it have sent their expectations: one ordered pipe carries all.
qb::io::async::task<void>
kill_now(qb::ScopedCoroContext ctx, qb::ActorId target) {
    ctx.push_to<qb::KillEvent>(target);
    co_return;
}

/// An asker whose coroutine outlives its kill long enough to expect once more: it tells `ready` it waits, catches the
/// cancellation, and expects again on a scope that is cancelled -- nothing departs from an actor that is being killed.
class Persister : public qb::Actor {
    qb::ActorId _probe;
    qb::ActorId _ready;

public:
    Persister(qb::ActorId probe, qb::ActorId ready)
        : _probe(probe)
        , _ready(ready) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        spawn([probe = _probe, ready = _ready](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Cancelled>(ready, 1);
            try {
                co_await ctx.until_cancelled();
            } catch (qb::io::async::cancelled_error const &) {
            }
            try {
                (void) co_await qb::test::expect<Filled>(ctx, probe, 30s);
            } catch (qb::io::async::cancelled_error const &) {
            }
        });
        co_return true;
    }
    void
    on(qb::test::detail::Expectation &e) {
        (void) resolve_ask(e);
    }
};

/// Throws from its constructor: the core that would build it refuses to start, and builds nothing after it.
class Unbuildable : public qb::Actor {
public:
    Unbuildable() {
        throw std::runtime_error("an actor that cannot be built");
    }
};

/// Ends its core: an exception out of a handler is the core's last pass.
struct Boom : qb::Event {};

class Thrower : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Boom>(*this);
        co_return true;
    }
    void
    on(Boom const &) {
        throw std::runtime_error("a handler that throws ends its core");
    }
};

/// Ends `thrower`'s core once the branches started before it have sent their expectations.
qb::io::async::task<void>
boom_now(qb::ScopedCoroContext ctx, qb::ActorId thrower) {
    ctx.push_to<Boom>(thrower);
    co_return;
}

/// What an expectation threw: `what()` of the `probe_gone_error`, or nothing.
qb::io::async::task<std::string>
gone_because(qb::ScopedCoroContext ctx, qb::ActorId probe) {
    try {
        (void) co_await qb::test::expect<Filled>(ctx, probe, std::chrono::hours(1));
    } catch (qb::test::probe_gone_error const &e) {
        co_return std::string(e.what());
    }
    co_return std::string();
}

/// An asker that is not the kit's driver, killed while its expectation waits: it records what the expectation threw
/// (1: `cancelled_error`, 2: `probe_gone_error`, 3: anything else).
class KilledWhileItWaits : public qb::Actor {
    qb::ActorId                       _probe;
    qb::ActorId                       _told;
    std::shared_ptr<std::atomic<int>> _threw;

public:
    KilledWhileItWaits(qb::ActorId probe, qb::ActorId told, std::shared_ptr<std::atomic<int>> threw)
        : _probe(probe)
        , _told(told)
        , _threw(std::move(threw)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        spawn([probe = _probe, told = _told, threw = _threw](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                (void) co_await qb::io::async::when_all(qb::test::expect<Filled>(ctx, probe, std::chrono::hours(1)), tell_now(ctx, told, 1));
            } catch (qb::io::async::cancelled_error const &) {
                threw->store(1);
            } catch (qb::test::probe_gone_error const &) {
                threw->store(2);
            } catch (...) {
                threw->store(3);
            }
        });
        co_return true;
    }
    void
    on(qb::test::detail::Expectation &e) {
        (void) resolve_ask(e);
    }
};

/// Sends `to` a `qb::DownEvent` that says `about`'s core stopped: the shape of a watch's answer, from the wrong sender.
struct Forge : qb::Event {
    qb::ActorId to;
    qb::ActorId about;

    Forge(qb::ActorId t, qb::ActorId a)
        : to(t)
        , about(a) {}
};

class Forger : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Forge>(*this);
        co_return true;
    }
    void
    on(Forge const &f) {
        push<qb::DownEvent>(f.to, f.about, qb::DownReason::core_stopped);
    }
};

/// Asks the census, from ITS core, whether a kit's driver lives at the id it is given, and tells `told` (1: yes).
struct Peek : qb::Event {
    qb::ActorId who;

    explicit Peek(qb::ActorId w)
        : who(w) {}
};

class Peeker : public qb::Actor {
    qb::ActorId _told;

public:
    explicit Peeker(qb::ActorId told)
        : _told(told) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Peek>(*this);
        co_return true;
    }
    void
    on(Peek const &p) {
        push<Cancelled>(_told, qb::test::detail::probes().driver(p.who) != nullptr ? 1 : 0);
    }
};

/// Stops the engine when told: a stop that is neither the scenario's nor the watchdog's.
struct StopNow : qb::Event {};

class Stopper : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<StopNow>(*this);
        co_return true;
    }
    void
    on(StopNow const &) {
        qb::Main::stop();
    }
};

/// Has `stopper` stop the engine and holds `staller`'s core -- the caller's own -- for `held`, once the branches
/// started before it have sent their expectations.
qb::io::async::task<void>
stop_and_stall(qb::ScopedCoroContext ctx, qb::ActorId stopper, qb::ActorId staller, qb::duration held) {
    ctx.push_to<StopNow>(stopper);
    ctx.push_to<Stall>(staller, held);
    co_return;
}

/// An asker that is NOT the kit's driver: it waits for a quiet window on `probe`, tells `sent` -- a probe beside
/// `probe` -- once its expectation has left, and `heard` the verdict if one ever comes.
class Outsider : public qb::Actor {
    qb::ActorId _probe;
    qb::ActorId _sent;
    qb::ActorId _heard;

public:
    Outsider(qb::ActorId probe, qb::ActorId sent, qb::ActorId heard)
        : _probe(probe)
        , _sent(sent)
        , _heard(heard) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        spawn([probe = _probe, sent = _sent, heard = _heard](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto [said, told] = co_await qb::io::async::when_all(quiet_verdict(ctx, probe, std::chrono::hours(1)), tell_now(ctx, sent, 1));
            (void) told;
            ctx.push_to<Cancelled>(heard, static_cast<int>(said));
        });
        co_return true;
    }
    void
    on(qb::test::detail::Expectation &e) {
        (void) resolve_ask(e);
    }
};

/// Counts one expectation as sent to `probe`, and ends its core: what an expectation leaves behind that was still in
/// its asker's pipe when an exception stopped that core -- a count, and nothing that will ever arrive.
class Doomed : public qb::Actor {
    qb::ActorId                        _probe;
    std::shared_ptr<std::atomic<bool>> _counted;

public:
    Doomed(qb::ActorId probe, std::shared_ptr<std::atomic<bool>> counted)
        : _probe(probe)
        , _counted(std::move(counted)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Boom>(*this);
        co_return true;
    }
    void
    on(Boom const &) {
        _counted->store(qb::test::detail::probes().departs(_probe, id()));
        throw std::runtime_error("a core that stops with an expectation in its pipe");
    }
};

/// Slow to build: the actors added to its core after it exist that much later.
class SlowToBuild : public qb::Actor {
public:
    explicit SlowToBuild(qb::duration d) {
        std::this_thread::sleep_for(d);
    }
};

/// An asker whose `onInit()` -- it runs while the other cores still build their actors -- expects a quiet window on
/// `probe`, and tells `told` the verdict.
class Early : public qb::Actor {
    qb::ActorId  _probe;
    qb::ActorId  _told;
    qb::duration _window;

public:
    Early(qb::ActorId probe, qb::ActorId told, qb::duration window = 50ms)
        : _probe(probe)
        , _told(told)
        , _window(window) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::test::detail::Expectation>(*this);
        auto       ctx   = context();
        const auto told  = _told;
        const auto heard = co_await quiet_verdict(ctx, _probe, _window);
        ctx.push_to<Cancelled>(told, static_cast<int>(heard));
        co_return true;
    }
    void
    on(qb::test::detail::Expectation &e) {
        (void) resolve_ask(e);
    }
};

/// The activation deadline of a case whose actors expect in their `onInit()`. A core fails an actor whose `onInit()`
/// has not returned when that deadline passes -- five seconds by default, counted from the moment the actor is built --
/// and an expectation asked there ends with it: a host that stalls for those seconds before the engine has started
/// made such a case fail on a kit that was right. Ten minutes: more than any stall the case has to outlast, and still
/// an end for a case that is broken.
struct PatientActivation : qb::test::ScopedDeadline {
    PatientActivation()
        : qb::test::ScopedDeadline(std::uint64_t{600} * 1000u * 1000u * 1000u) {}
};

/// A probe on `probe_core` killed BEFORE two expectations are sent to it from core 0 -- the kill is ahead of them in
/// the pipe, and the census took them as sent to a live probe: both are answered, then the probe dies.
void
expectations_on_their_way_to_a_killed_probe_are_answered(qb::CoreId probe_core) {
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(probe_core);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    const auto        run     = kit.run(
        [probe, deaths, mourner](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Mourn>(mourner, probe);
            ctx.push_to<qb::KillEvent>(probe);
            ctx.push_to<qb::KillEvent>(probe); // killed again while it waits for the two below: nothing more to do
            auto [none, fill] = co_await qb::io::async::when_all(quiet_verdict(ctx, probe, 30s), fill_verdict(ctx, probe, 30s));
            EXPECT_EQ(none, verdict::gone) << "sent to a live probe, received by one already killed: left without an answer";
            EXPECT_EQ(fill, verdict::gone);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value()) << "the last one answered, the probe dies";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
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
        // This true is the probe's own answer, at the end of its window: the asker's clock decides nothing.
        const auto quiet_from = steady::now();
        EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 100ms));
        EXPECT_GE(steady::now() - quiet_from, 100ms);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnEventLentToAnAbandonedExpectationReturnsForTheNext) {
    // The losing branch of a when_any is destroyed: its expectation withdraws itself, so the next one gets the event
    // (it was lost before the loan; lent to a frame that is gone and never withdrawn, it would stay lent for good).
    // AbandonedExpectationsAreWithdrawnSoTheNextGetsTheEventAtOnce pins the same with three of them.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto race = co_await qb::io::async::when_any(qb::test::expect<Filled>(ctx, probe, 5s), pause(ctx, 50ms));
            EXPECT_EQ(race.index, 1u) << "the pause wins: the expectation is abandoned";
            ctx.push_to<Order>(desk, 7); // behind the withdrawal: the abandoned waiter is gone, the fill is recorded
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 10s);
            EXPECT_TRUE(filled.has_value()) << "an event lent to an abandoned expectation is never lost";
            if (filled) {
                EXPECT_EQ(filled->qty, 7);
            }
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnAcknowledgedEventDoesNotComeBack) {
    // The acknowledgement ends the loan: what could still give the event back -- its asker's death, since no clock
    // does -- finds nothing to give. An asker that expects through the kit (it acknowledges) takes the fill, then dies.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(0);
    const auto        desk    = kit.add<Desk>(0, probe);
    const auto        taken   = kit.probe<Cancelled>(0);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    const auto        taker   = kit.add<Taker>(0, probe, taken);
    const auto        run     = kit.run(
        [desk, probe, taken, deaths, mourner, taker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Order>(desk, 3);
            const auto took = co_await qb::test::expect<Cancelled>(ctx, taken, 10s);
            EXPECT_EQ(took ? took->qty : -1, 3) << "the taker's expectation yielded the fill, and acknowledged it";
            ctx.push_to<Mourn>(mourner, taker);
            ctx.push_to<qb::KillEvent>(taker);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value()) << "the taker is gone: the probe heard it first";
            EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 100ms))
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
            c.push_to<Order>(desk, 1); // behind both expectations, in the one pipe: both waiters are parked first
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
    // Waiters abandoned by when_any losers stayed on the probe: each was lent the event in turn -- for two seconds,
    // while a loan still timed out -- so a live expectation behind three of them waited 6 s, and expect_none saw
    // "absent" while the event sat with a dead waiter (the second review).
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        turn  = kit.probe<Cancelled>(0);
    const auto        run   = kit.run(
        [desk, probe, turn](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (int i = 0; i < 3; ++i) {
                const auto race = co_await qb::io::async::when_any(qb::test::expect<Filled>(ctx, probe, 30s), pause(ctx, 20ms));
                EXPECT_EQ(race.index, 1u);
            }
            ctx.push_to<Order>(desk, 7);
            co_await a_turn(ctx, turn); // the fill has reached the probe: before the withdrawals it sat lent to a dead waiter
            EXPECT_FALSE(co_await qb::test::expect_none<Filled>(ctx, probe, 300ms)) << "present, not hidden with a dead waiter";
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 0s);
            EXPECT_EQ(filled ? filled->qty : -1, 7) << "served at once -- an expectation that waits for nothing yields it";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AKilledAskersWaitersAreLetGoOfAtOnce) {
    // A killed asker withdraws nothing (its scope is cancelled first): its two waiters stayed on the probe, each next
    // event lent to them -- the starvation of abandoned expectations, on the kill path (the third
    // review). The probe watches its askers: at the asker's death its waiters are dropped.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(0);
    const auto        desk    = kit.add<Desk>(0, probe);
    const auto        told    = kit.probe<Cancelled>(0);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    const auto        asker   = kit.add<Abandoner>(0, probe, told);
    const auto        run     = kit.run(
        [desk, probe, told, deaths, mourner, asker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_TRUE(co_await kill_once_it_waits(ctx, asker, told, mourner, deaths)) << "its two expectations waited, and it is gone";
            for (int qty : {1, 2, 3})
                ctx.push_to<Order>(desk, qty);
            const auto first = co_await qb::test::expect<Filled>(ctx, probe, 10s);
            EXPECT_EQ(first ? first->qty : -1, 1) << "in arrival order, none lent to the dead asker";
            for (int qty : {2, 3}) { // recorded in the pass that served the first: an expectation that waits for nothing yields them
                const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 0s);
                EXPECT_EQ(filled ? filled->qty : -1, qty) << "none left with the dead asker's waiters";
            }
            EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 100ms));
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, WhatADeadAskerWasLentComesBackAtOnce) {
    // The asker took the answer and died before it acknowledged it: the event came back two seconds later, when its
    // loan timed out (the third review). Its DownEvent gives the loan back, at its rank -- and nothing else would.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(0);
    const auto        desk    = kit.add<Desk>(0, probe);
    const auto        told    = kit.probe<Cancelled>(0);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    const auto        asker   = kit.add<Hoarder>(0, probe, told);
    const auto        run     = kit.run(
        [desk, probe, told, deaths, mourner, asker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_EQ(co_await next_told(ctx, told), 1) << "its expectation waits on the probe";
            ctx.push_to<Order>(desk, 1);
            EXPECT_EQ(co_await next_told(ctx, told), 2) << "it took the answer";
            EXPECT_TRUE(co_await qb::test::expect_none<Filled>(ctx, probe, 100ms)) << "1 is lent to the asker";
            ctx.push_to<Order>(desk, 2);
            ctx.push_to<Mourn>(mourner, asker);
            ctx.push_to<qb::KillEvent>(asker);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value()) << "it is gone, and the probe heard it first";
            for (int qty : {1, 2}) { // both are recorded by now: an expectation that waits for nothing yields them
                const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 0s);
                EXPECT_EQ(filled ? filled->qty : -1, qty) << "the dead asker's loan back at its rank, at its death";
            }
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
    const auto        turn  = kit.probe<Cancelled>(0);
    const auto        run   = kit.run(
        [desk, probe, turn](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (int qty : {1, 2, 3})
                ctx.push_to<Order>(desk, qty);
            co_await a_turn(ctx, turn); // all three recorded
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

TEST(TestKit, TheAnswerToTheProbesOwnWatchIsNotRecorded) {
    // A probe that records qb::DownEvent kept the core's answer to its own watch of a killed asker out of the queue
    // only by accident of its branch: nothing pinned it (the fifth review).
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled, qb::DownEvent>(0);
    const auto        told    = kit.probe<Cancelled>(0);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    const auto        asker   = kit.add<Abandoner>(0, probe, told);
    const auto        run     = kit.run([probe, told, deaths, mourner, asker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        // Its expectations wait on the probe, which watches it; killed, the core's answer reaches the probe first.
        EXPECT_TRUE(co_await kill_once_it_waits(ctx, asker, told, mourner, deaths));
        EXPECT_TRUE(co_await qb::test::expect_none<qb::DownEvent>(ctx, probe, 100ms)) << "the watch's answer was recorded";
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ADownEventSentToTheProbeLetsGoOfNoLiveAsker) {
    qb::test::TestKit kit;
    const auto        probe    = kit.probe<Filled, qb::DownEvent>(0);
    const auto        desk     = kit.add<Desk>(0, probe);
    const auto        reporter = kit.add<Reporter>(0, probe);
    const auto        run      = kit.run(
        [probe, desk, reporter](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto report_then_fill = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId reporter,
                                       qb::ActorId desk) -> qb::io::async::task<void> {
                c.push_to<Report>(reporter,
                                  c.id()); // behind the driver's expectation: "the driver is gone" -- wrong, and not the probe's watch
                EXPECT_FALSE(co_await qb::test::expect_none<qb::DownEvent>(c, probe, 30s)) << "the report has reached the probe";
                c.push_to<Order>(desk, 7);
            };
            auto [filled, done] =
                co_await qb::io::async::when_all(qb::test::expect<Filled>(ctx, probe, 30s), report_then_fill(ctx, probe, reporter, desk));
            (void) done;
            EXPECT_EQ(filled ? filled->qty : -1, 7) << "the live driver's waiter was dropped by a DownEvent it was sent";
            EXPECT_TRUE(co_await qb::test::expect<qb::DownEvent>(ctx, probe, 0s)) << "the report is recorded";
        },
        60s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AKilledAskerOnAnotherCoreIsLetGoOfAtOnce) {
    // The death watch across cores: the probe on core 1, the killed asker on core 2, the driver on core 0. Two sleeps
    // ordered the death once: a stall of core 2 before its kill, and the fills were served 3, 1, 2.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(1);
    const auto        desk    = kit.add<Desk>(1, probe);
    const auto        told    = kit.probe<Cancelled>(1);
    const auto        deaths  = kit.probe<Cancelled>(1);
    const auto        mourner = kit.add<Mourner>(1, deaths);
    const auto        asker   = kit.add<Abandoner>(2, probe, told);
    const auto        run     = kit.run(
        [desk, probe, told, deaths, mourner, asker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_TRUE(co_await kill_once_it_waits(ctx, asker, told, mourner, deaths)) << "its two expectations waited, and it is gone";
            for (int qty : {1, 2, 3})
                ctx.push_to<Order>(desk, qty);
            for (int qty : {1, 2, 3}) {
                const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 10s);
                EXPECT_EQ(filled ? filled->qty : -1, qty) << "none left with the dead asker's waiters";
            }
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
    const auto        told    = kit.probe<Cancelled>(0);
    const auto        hoarder = kit.add<HoarderOnCommand>(0, probe, told);
    const auto        run     = kit.run(
        [desk, probe, told, hoarder](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto first_waiter = [](qb::ScopedCoroContext c, qb::ActorId probe) -> qb::io::async::task<void> {
                (void) co_await qb::ask(c, probe, qb::test::detail::Expectation{qb::type_id<Filled>(), 30s, false, 777}, 30s);
            };
            auto choreography = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId desk, qb::ActorId hoarder,
                                   qb::ActorId told) -> qb::io::async::task<void> {
                c.push_to<AskNow>(hoarder); // behind the driver's raw expectation (token 777), sent by the branch before
                EXPECT_EQ(co_await next_told(c, told), 1) << "the hoarder's first waits";
                c.push_to<Order>(desk, 1); // 1 lent to the driver's
                c.push_to<Order>(desk, 2); // 2 lent to the hoarder's
                EXPECT_EQ(co_await next_told(c, told), 2) << "the hoarder took 2";
                c.push_to<AskNow>(hoarder);
                EXPECT_EQ(co_await next_told(c, told), 1) << "the hoarder's second waits";
                c.push_to<qb::test::detail::ExpectationWithdrawn>(probe, 777); // 1 back, lent to the hoarder's second
                EXPECT_EQ(co_await next_told(c, told), 2) << "the hoarder took 1: it holds 2, then 1";
                // The expectation below waits on the probe when the kill, behind it in the pipe, reaches the hoarder.
                auto [parked, killed] = co_await qb::io::async::when_all(qb::test::expect<Filled>(c, probe, 10s), kill_now(c, hoarder));
                (void) killed;
                EXPECT_EQ(parked ? parked->qty : -1, 1) << "the parked waiter got a later event first";
                const auto next = co_await qb::test::expect<Filled>(c, probe, 0s);
                EXPECT_EQ(next ? next->qty : -1, 2);
            };
            co_await qb::io::async::when_all(first_waiter(ctx, probe), choreography(ctx, probe, desk, hoarder, told));
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ALoanOutlastsAStallOfItsAsker) {
    // A loan timed out after two seconds: under an asker whose core stalled past them the event went back to its queue
    // while that asker still read its answer, and the next expectation was served it too -- one fill, two expectations
    // yielding it. A loan ends by an event only. The probe, the desk and the feeder run on core 1; the feeder holds
    // core 0 -- the asker's -- for 2.5 s, then has the desk fill: the stall is ahead of the answer in core 1's pipe.
    // The first expectation sets no deadline: the fill is its answer whenever a stalled host lets it be recorded. The
    // second's four seconds are a wait, not a bound -- long enough to be parked still when a loan of two seconds would
    // have run out -- and "none" is its answer whenever they end.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(1);
    const auto        desk    = kit.add<Desk>(1, probe);
    const auto        feeder  = kit.add<Feeder>(1);
    const auto        staller = kit.add<Staller>(0);
    const auto        run     = kit.run(
        [probe, desk, feeder, staller](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto start          = steady::now();
            auto [first, second, fed] = co_await qb::io::async::when_all(fill_and_when(ctx, probe, qb::duration::max(), start),
                                                                         qb::test::expect<Filled>(ctx, probe, 4s),
                                                                         feed_under_a_stall(ctx, feeder, desk, staller, 2500ms));
            (void) fed;
            EXPECT_EQ(first.first, 1) << "the oldest expectation takes the one fill";
            EXPECT_GE(first.second, 2500ms) << "its asker read the answer once the stall was over: two seconds and more after the loan";
            EXPECT_FALSE(second.has_value()) << "ONE fill was sent: the second expectation was served qty " << (second ? second->qty : -1);
            EXPECT_FALSE((co_await qb::test::expect<Filled>(ctx, probe, 0s)).has_value()) << "and none is recorded again";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ATimeoutOfMaxIsNoDeadlineNotAnOverflow) {
    // The asker's own timeout, wait + a grace, overflowed for qb::duration::max() (UBSan): it has none now.
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
    // The probe answered (the event lent) before the withdrawal reached it: the withdrawal gives the loan back -- and
    // nothing else would, while its asker lives.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        turn  = kit.probe<Cancelled>(0);
    const auto        run   = kit.run(
        [desk, probe, turn](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Order>(desk, 8);
            co_await a_turn(ctx, turn); // recorded: the expectation below is answered at once, the event lent
            const auto race = co_await qb::io::async::when_any(qb::test::expect<Filled>(ctx, probe, 5s), at_once());
            EXPECT_EQ(race.index, 1u);
            // The withdrawal left before this expectation, in one pipe: it waits for nothing, and yields the event.
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 0s);
            EXPECT_EQ(filled ? filled->qty : -1, 8) << "given back by the withdrawal, at once";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AQuietWindowAnsweredLateByALiveProbeIsNotReadAsAnArrival) {
    // The asker timed its own ask -- the window plus a grace of 250 ms -- and read its timeout as "an event arrived".
    // On a host that stalled past it the probe's window and that timeout came due in one pass of the core, the timeout
    // ran first, and expect_none returned false for a window nothing broke (Windows CI, run 45985436624: 994 ms for a
    // 300 ms case). The probe, the stall and the driver share core 0, and the window runs when the stall begins.
    a_quiet_window_holds_through_a_stall_of_the_probes_core(0);
}

TEST(TestKit, AQuietWindowAnsweredLateByAProbeOnAStalledCoreIsNotReadAsAnArrival) {
    // The same across the mailbox: only the probe's core stalls, the asker's runs on and its own clock passed first.
    a_quiet_window_holds_through_a_stall_of_the_probes_core(1);
}

TEST(TestKit, AnExpectationArmsNoDeadlineOnItsAskersCore) {
    // The rule itself, not one stall that beats one timeout: while an expectation waits, its asker's core holds no
    // request deadline -- the asker times nothing.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        run   = kit.run([probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        auto [quiet, armed] = co_await qb::io::async::when_all(qb::test::expect_none<Filled>(ctx, probe, 50ms), a_request_deadline_is_armed());
        EXPECT_FALSE(armed) << "the asker set a deadline of its own on the probe's answer";
        EXPECT_TRUE(quiet);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnEventThatReachedTheProbesCoreBeforeItsDeadlineIsNotAnsweredAsNone) {
    // The probe's deadline is a reminder it sends itself, and a core reads what it sent itself before what the other
    // cores sent it: after a stall of the probe's core the reminder was handled AHEAD of a fill that had waited in its
    // mailbox since long before the window ended -- expect_none said "none", and expect came back empty with the fill
    // recorded behind it. The probe's core is held from the pass that arms the windows until after both have ended and
    // the fill is in its mailbox: it turns no pass in between, so no stall of the host can end a window first (a
    // correct "none" then, which this case would have taken for the defect).
    qb::test::TestKit kit;
    const auto        gate   = std::make_shared<Latch>();
    const auto        probe  = kit.probe<Filled>(1);
    const auto        holder = kit.add<Holder>(1, gate);
    const auto        local  = kit.probe<Cancelled>(0);
    const auto        run    = kit.run(
        [probe, holder, local, gate](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Hold>(holder);
            co_await until_held(ctx, gate); // the probe's core reads nothing more until the expectations are all sent
            auto [none, fill, released] = co_await qb::io::async::when_all(quiet_verdict(ctx, probe, 300ms), fill_verdict(ctx, probe, 300ms),
                                                                           fill_behind_the_reminders(ctx, probe, holder, local, gate, 300ms));
            (void) released;
            EXPECT_EQ(none, verdict::arrival) << "a fill in the probe's mailbox before the window ended was answered \"none\"";
            EXPECT_EQ(fill, verdict::arrival) << "and an expectation of it came back empty";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AFillCausedOnTheProbesCoreAsItsWindowsEndIsYieldedOnce) {
    // The bound of the case above. A window is wall time on the probe's core, and what the probe reads before it
    // answers is what was ADDRESSED TO IT there when the window ended. Here an ORDER waits in that core's mailbox
    // through the hold, for the desk beside the probe: the fill exists only once the core runs again, after both
    // windows. Whether the probe then reads it ahead of its answers is the core's order of dispatch, not the kit's to
    // promise: "none" and "the fill" are both true accounts of a fill a stall kept from being sent. What holds either
    // way: nothing is made up, and the one fill is yielded once -- to the expectation, or to the next.
    qb::test::TestKit kit;
    const auto        latch  = std::make_shared<Latch>();
    const auto        probe  = kit.probe<Filled>(1);
    const auto        desk   = kit.add<Desk>(1, probe);
    const auto        holder = kit.add<Holder>(1, latch);
    const auto        local  = kit.probe<Cancelled>(0);
    const auto        run    = kit.run(
        [probe, desk, holder, local, latch](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto [none, fill, released] =
                co_await qb::io::async::when_all(quiet_verdict(ctx, probe, 300ms), fill_verdict(ctx, probe, 300ms),
                                                 order_from_a_held_core_and_release(ctx, probe, holder, local, latch, 300ms, desk));
            (void) released;
            EXPECT_NE(none, verdict::gone);
            EXPECT_NE(fill, verdict::gone);
            const bool yielded = fill == verdict::arrival;
            const auto next    = co_await qb::test::expect<Filled>(ctx, probe, yielded ? qb::duration{0s} : qb::duration{10s});
            EXPECT_NE(next.has_value(), yielded) << "one fill was caused: " << (yielded ? "it was yielded twice" : "it was lost");
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnEventRecordedBeforeTheExpectationIsYieldedHoweverLongTheHostStalls) {
    // expect lost the same race: its timeout returned "nothing" for an event the probe held before the expectation was
    // even sent -- the answer was in the mailbox, read after the timeout of the same pass.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(0);
    const auto        desk    = kit.add<Desk>(0, probe);
    const auto        staller = kit.add<Staller>(0);
    const auto        run     = kit.run(
        [desk, probe, staller](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Order>(desk, 7);
            EXPECT_FALSE(co_await qb::test::expect_none<Filled>(ctx, probe, 30s))
                << "the fill is recorded: the probe says so the moment it arrives";
            const auto start = steady::now();
            ctx.push_to<Stall>(staller, 600ms); // handled before the expectation below: one ordered pipe
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 100ms);
            EXPECT_GE(steady::now() - start, 600ms) << "the stall held the core past the timeout: the case tests nothing otherwise";
            EXPECT_EQ(filled ? filled->qty : -1, 7) << "an event the probe held was reported as none";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AProbeKilledWhileExpectationsWaitOnItAnswersEachThatItIsGone) {
    // A probe that died took its waiters with it, and the asker's own timeout turned the silence into an answer:
    // expect_none false -- "an event arrived" -- and expect empty -- "none did". Neither is true of a probe that is
    // gone, and a test asserts one or the other: the probe says it is gone as it ends, and the asker throws.
    // Each type the probe records has its queue and its waiters: all of them are told, on the probe's core and across.
    for (const qb::CoreId probe_core : {qb::CoreId{0}, qb::CoreId{1}}) {
        qb::test::TestKit kit;
        const auto        probe = kit.probe<Filled, Cancelled>(probe_core);
        const auto        run   = kit.run(
            [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                auto [none, fill, other, killed] =
                    co_await qb::io::async::when_all(quiet_verdict(ctx, probe, 30s), fill_verdict(ctx, probe, 30s),
                                                     quiet_verdict_of<Cancelled>(ctx, probe, 30s), kill_now(ctx, probe));
                (void) killed;
                EXPECT_EQ(none, verdict::gone) << "a window on a probe that is gone neither held nor broke";
                EXPECT_EQ(fill, verdict::gone) << "nor did a fill arrive or fail to";
                EXPECT_EQ(other, verdict::gone) << "the waiters of the probe's other type are told too";
            },
            30s);
        EXPECT_TRUE(run) << "probe on core " << probe_core << ": " << run.describe();
    }
}

TEST(TestKit, AScenarioThatMeetsAProbeGoneEndsItsRunWithThatError) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        run   = kit.run(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto [none, killed] = co_await qb::io::async::when_all(qb::test::expect_none<Filled>(ctx, probe, 30s), kill_now(ctx, probe));
            (void) killed;
            ADD_FAILURE() << "expect_none returned " << none << " for a probe that is gone";
        },
        30s);
    EXPECT_FALSE(run);
    EXPECT_FALSE(run.completed);
    EXPECT_FALSE(run.timed_out);
    ASSERT_TRUE(run.error);
    EXPECT_NE(run.describe().find("the probe was killed while this expectation waited on it"), std::string::npos) << run.describe();
    EXPECT_THROW(std::rethrow_exception(run.error), qb::test::probe_gone_error);
}

TEST(TestKit, ExpectationsOnTheirWayToAKilledProbeAreAnsweredThatItIsGone) {
    // An expectation the kill overtook reached a dead id -- a dead letter -- and its asker, which times nothing, would
    // wait for ever. The probe knows what was sent to it: it stays until the last has arrived, then dies.
    expectations_on_their_way_to_a_killed_probe_are_answered(0);
}

TEST(TestKit, ExpectationsOnTheirWayToAKilledProbeOnAnotherCoreAreAnsweredThatItIsGone) {
    expectations_on_their_way_to_a_killed_probe_are_answered(1);
}

TEST(TestKit, AKilledProbeTellsTheExpectationsWaitingWithoutWaitingForTheOnesOnTheirWay) {
    // The census counts one expectation as sent that has not arrived -- still in its asker's pipe, say -- so the probe,
    // killed, stays up for it. The ones that wait are told at once all the same: they do not wait with it.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        run   = kit.run(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_TRUE(qb::test::detail::probes().departs(probe, ctx.id())) << "one on its way, and nothing sends it";
            auto [none, killed] = co_await qb::io::async::when_all(quiet_verdict(ctx, probe, 30s), kill_now(ctx, probe));
            (void) killed;
            EXPECT_EQ(none, verdict::gone);
            EXPECT_FALSE(qb::test::detail::probes().departs(probe, ctx.id())) << "killed, it takes no expectation more, however long it stays";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AKilledProbeDoesNotOutliveAnAskerWhoseExpectationWasLost) {
    // An expectation counted as sent and lost with its asker's core -- an exception there drops what its pipes held --
    // never arrives: a killed probe that waited for it stayed up for ever, its own watchers never told. The probe
    // watches the askers it waits for: their core's stop is their death, and the count goes with them.
    qb::test::TestKit kit;
    const auto        counted = std::make_shared<std::atomic<bool>>(false);
    const auto        probe   = kit.probe<Filled>(1);
    const auto        doomed  = kit.add<Doomed>(2, probe, counted);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    const auto        run     = kit.run(
        [probe, doomed, deaths, mourner](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Mourn>(mourner, doomed);
            ctx.push_to<Boom>(doomed);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value())
                << "the asker's core stopped: its watcher said so";
            ctx.push_to<Mourn>(mourner, probe);
            ctx.push_to<qb::KillEvent>(probe);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value())
                << "killed, the probe waited for an expectation that went with its asker's core";
        },
        30s);
    EXPECT_TRUE(counted->load()) << "the expectation was not counted on its way: the case tests nothing";
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out);
    EXPECT_TRUE(run.engine_error) << "a core stopped on an exception";
}

TEST(TestKit, AnExpectationOfAnAskerThatIsBeingKilledCountsNothingOnItsWay) {
    // expect on a cancelled scope sends nothing (qb::ask does not), so it counts nothing as sent. A count that nothing
    // follows would stay until a probe that was killed heard its asker gone: read here as the kill handler reads it.
    qb::test::TestKit kit;
    const auto        probe     = kit.probe<Filled>(0);
    const auto        ready     = kit.probe<Cancelled>(0);
    const auto        deaths    = kit.probe<Cancelled>(0);
    const auto        mourner   = kit.add<Mourner>(0, deaths);
    const auto        persister = kit.add<Persister>(0, probe, ready);
    const auto        run       = kit.run(
        [probe, ready, deaths, mourner, persister](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, ready, 10s)).has_value()) << "the persister waits to be killed";
            ctx.push_to<Mourn>(mourner, persister);
            ctx.push_to<qb::KillEvent>(persister);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value()) << "killed: it expected once more on its way out";
            EXPECT_TRUE(qb::test::detail::probes().ends(probe).empty()) << "an expectation that was never sent is counted on its way";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnExpectationOfAProbeThatEndedThrowsAtOnce) {
    // The probe is dead before the expectation: nothing would answer, so nothing is sent. The asker's own clock used
    // to turn that silence into an answer 250 ms after the window -- "an event arrived" for expect_none, "none did" for
    // expect.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(0);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    const auto        run     = kit.run(
        [probe, deaths, mourner](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Mourn>(mourner, probe);
            ctx.push_to<qb::KillEvent>(probe);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value()) << "the probe is gone: its watcher said so";
            EXPECT_EQ(co_await quiet_verdict(ctx, probe, 30s), verdict::gone) << "a window on a probe that is not there got a verdict";
            EXPECT_EQ(co_await fill_verdict(ctx, probe, 30s), verdict::gone) << "an expectation of a probe that is not there got a verdict";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ARaceWonByAnExpectationThatThrowsCarriesItsError) {
    // when_any keeps the exception of the branch that wins in its result: the index alone reads a probe that is gone
    // as "the expectation resolved". get<T>() rethrows it.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(0);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    const auto        run     = kit.run(
        [probe, deaths, mourner](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Mourn>(mourner, probe);
            ctx.push_to<qb::KillEvent>(probe);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value());
            const auto race = co_await qb::io::async::when_any(qb::test::expect_none<Filled>(ctx, probe, 30s), pause(ctx, 10s));
            EXPECT_EQ(race.index, 0u) << "the expectation ends the race at once: it throws";
            EXPECT_TRUE(race.has_exception());
            EXPECT_THROW((void) race.get<bool>(), qb::test::probe_gone_error);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnIdThatNeverWasAProbeThrowsAtOnce) {
    // The desk's id -- an actor, no probe -- and the null id: the same error as a probe that ended, the kit cannot tell
    // them apart, and what() says so.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    const auto        desk  = kit.add<Desk>(0, probe);
    const auto        run   = kit.run([desk](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        EXPECT_EQ(co_await quiet_verdict(ctx, desk, 30s), verdict::gone);
        EXPECT_EQ(co_await fill_verdict(ctx, desk, 30s), verdict::gone);
        EXPECT_EQ(co_await quiet_verdict(ctx, qb::ActorId{}, 30s), verdict::gone);
        EXPECT_EQ(co_await quiet_verdict(ctx, qb::BroadcastId(0), 30s), verdict::gone)
            << "a broadcast is no probe: every actor of the core would get it";
        std::string met;
        try {
            (void) co_await qb::test::expect<Filled>(ctx, qb::ActorId{}, 30s);
        } catch (qb::test::probe_gone_error const &e) {
            met = e.what();
        }
        EXPECT_NE(met.find("no probe takes expectations at this id"), std::string::npos) << met;
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnExpectationSentBeforeItsProbeIsBuiltIsAnswered) {
    // onInit() is a coroutine, and it runs while the other cores still build their actors: the probe below does not
    // exist yet when the asker expects on it -- its core is building, for 400 ms, the actor added ahead of it. The
    // asker's own clock (the window and 250 ms) had run out when the engine started, and the window read as broken; a
    // census of the probes that are BUILT refuses the id instead. A kit announces a probe when it is added.
    const PatientActivation patient;
    qb::test::TestKit       kit;
    (void) kit.add<SlowToBuild>(1, 400ms);
    const auto probe = kit.probe<Filled>(1);
    const auto told  = kit.probe<Cancelled>(0);
    (void) kit.add<Early>(0, probe, told);
    const auto run = kit.run(
        [told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_EQ(co_await next_told(ctx, told), static_cast<int>(verdict::quiet)) << "1: an arrival, 2: no probe -- nothing was sent it";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnExpectationAskedInAnOnInitEndsAtItsCoresActivationDeadline) {
    // The bound of the case above, and it is the core's, not the kit's: an asker sets no deadline, but a core fails an
    // actor whose `onInit()` has not returned when its activation deadline passes -- five seconds by default, 200 ms
    // here. The actor's scope is cancelled, the expectation throws `cancelled_error` out of the `onInit()`, and the
    // actor is removed: its window of an hour on a quiet probe never gets its answer. Nothing else ends it -- the run
    // stops only once its death is heard.
    const qb::test::ScopedDeadline deadline{std::uint64_t{200} * 1000u * 1000u};
    qb::test::TestKit              kit;
    const auto                     probe   = kit.probe<Filled>(1);
    const auto                     told    = kit.probe<Cancelled>(0);
    const auto                     deaths  = kit.probe<Cancelled>(0);
    const auto                     mourner = kit.add<Mourner>(0, deaths);
    const auto                     early   = kit.add<Early>(0, probe, told, std::chrono::hours(1));
    const auto                     run     = kit.run(
        [told, deaths, mourner, early](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Mourn>(mourner, early);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 30s)).has_value()) << "the core ended the actor at its deadline";
            EXPECT_FALSE((co_await qb::test::expect<Cancelled>(ctx, told, 0s)).has_value()) << "it told a verdict: its window was answered";
        },
        60s);
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out) << run.describe();
}

TEST(TestKit, AProbeAddedThroughTheEngineIsKnownOnceItIsBuilt) {
    // A probe the kit did not add -- `engine().addActor` -- enters the census in its constructor: a spawned coroutine,
    // which runs once every core has built its actors, expects on it like on any other.
    qb::test::TestKit kit;
    const auto        probe = kit.engine().addActor<qb::test::TestProbe<Filled>>(1);
    const auto        desk  = kit.add<Desk>(1, probe);
    const auto        run   = kit.run(
        [desk, probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Order>(desk, 7);
            const auto filled = co_await qb::test::expect<Filled>(ctx, probe, 10s);
            EXPECT_EQ(filled ? filled->qty : -1, 7);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AProbeAtAnIdAnotherKitsProbeHoldsIsALogicErrorAndTakesNothingFromIt) {
    // A probe's id is its engine's own, and nothing an expectation can read names its engine: the kit holds one probe
    // per id. A second kit's probe at an id the first holds was taken for it -- and when either ended, the other's
    // expectations were refused, on a probe that was alive. It is refused where it is added, and the first kit's
    // probe is still the one an `onInit()` expects on before it is built.
    const PatientActivation patient;
    qb::test::TestKit       first;
    (void) first.add<SlowToBuild>(1, 100ms);
    const auto probe = first.probe<Filled>(1);
    const auto told  = first.probe<Cancelled>(0);
    (void) first.add<Early>(0, probe, told);
    {
        qb::test::TestKit second;
        (void) second.add<SlowToBuild>(1, 1ms);
        EXPECT_THROW((void) second.probe<Filled>(1), std::logic_error) << "a second probe at an id a live kit's probe holds";
        // Run all the same, the kit that was refused builds that probe: it is not the one announced, and its core says so.
        const auto refused = second.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; }, 30s);
        EXPECT_TRUE(refused.engine_error) << "the probe a kit was refused was built in the first kit's name: " << refused.describe();
        EXPECT_FALSE(refused.completed);
    }
    const auto run = first.run(
        [told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_EQ(co_await next_told(ctx, told), static_cast<int>(verdict::quiet))
                << "2: the second kit took the first one's probe with it";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AKitThatEndsLeavesAnotherKitsAnnouncementAlone) {
    // A kit that ended withdrew every id it had announced -- its probes long gone, and the same ids announced since by
    // the kit built after it, whose `onInit()` expectation was then refused. It withdraws what it announced and no
    // core ever built, nothing else.
    const PatientActivation patient;
    auto                    before = std::make_unique<qb::test::TestKit>();
    (void) before->add<SlowToBuild>(1, 1ms);
    (void) before->probe<Filled>(1);
    const auto ran = before->run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; }, 30s);
    EXPECT_TRUE(ran) << ran.describe();
    qb::test::TestKit kit;
    (void) kit.add<SlowToBuild>(1, 100ms);
    const auto probe = kit.probe<Filled>(1); // the id the first kit's probe had
    const auto told  = kit.probe<Cancelled>(0);
    (void) kit.add<Early>(0, probe, told);
    before.reset(); // it ends between this kit's add and its run
    const auto run = kit.run(
        [told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_EQ(co_await next_told(ctx, told), static_cast<int>(verdict::quiet)) << "2: the kit that ended took this one's probe with it";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AProbeBuiltAtAnIdAKitAnnouncedIsNotTakenForIt) {
    // A kit's announcement stands for a probe that is not built yet. A probe ANOTHER engine built at that id -- added
    // through another kit's engine, or to an engine with no kit -- took it: built in its name, and gone with it when
    // that engine ended, the announcing kit's `onInit()` expectation refused. A probe says who adds it: the
    // announcement is its kit's alone, and the other engine does not start.
    const PatientActivation patient;
    qb::test::TestKit       kit;
    (void) kit.add<SlowToBuild>(1, 100ms);
    const auto probe = kit.probe<Filled>(1);
    const auto told  = kit.probe<Cancelled>(0);
    (void) kit.add<Early>(0, probe, told);
    {
        qb::test::TestKit other;
        (void) other.add<SlowToBuild>(1, 1ms);
        EXPECT_EQ(other.engine().addActor<qb::test::TestProbe<Filled>>(1), probe) << "the same id, in another kit's engine";
        const auto refused = other.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; }, 30s);
        EXPECT_TRUE(refused.engine_error) << "a probe added by hand was built at an id a kit announced: " << refused.describe();
    }
    {
        qb::Main plain;
        (void) plain.addActor<SlowToBuild>(1, qb::duration{1ms});
        EXPECT_EQ(plain.addActor<qb::test::TestProbe<Filled>>(1), probe) << "the same id, in an engine with no kit";
        plain.start(); // a start that is refused has joined its cores when it returns
        const bool refused = plain.hasError();
        qb::Main::stop();
        plain.join();
        EXPECT_TRUE(refused) << "a plain engine's probe was built at an id a kit announced";
    }
    const auto run = kit.run(
        [told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_EQ(co_await next_told(ctx, told), static_cast<int>(verdict::quiet)) << "2: another engine's probe took the announcement";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ADriverIsItsThreadsAloneAndASecondAtItsIdIsALogicError) {
    // The census knows a kit's driver by its id AND its thread: an actor of another engine may hold the same id, on
    // another thread, and was taken for the driver -- made to watch its probe, asked in a scope that was not its own.
    // Asked from another core, the census knows no driver at that id.
    qb::test::TestKit kit;
    const auto        told   = kit.probe<Cancelled>(0);
    const auto        peeker = kit.add<Peeker>(1, told);
    const auto        run    = kit.run([told, peeker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        EXPECT_NE(qb::test::detail::probes().driver(ctx.id()), nullptr) << "the scenario's own actor is its kit's driver";
        ctx.push_to<Peek>(peeker, ctx.id());
        EXPECT_EQ(co_await next_told(ctx, told), 0) << "another thread was given the driver that lives at this id";
        EXPECT_THROW(qb::test::detail::probes().drives(ctx.id(), nullptr), std::logic_error) << "two drivers at one id: two kits run at once";
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AProbeBuiltAtAnIdAnotherEnginesProbeHoldsStopsItsEngine) {
    // The same between two engines with no kit: the second probe's constructor throws, and its engine does not start.
    qb::Main   first;
    const auto id = first.addActor<qb::test::TestProbe<Filled>>(0);
    first.start(); // returns once its core has built the probe
    {
        qb::test::TestKit kit;
        EXPECT_THROW((void) kit.probe<Filled>(0), std::logic_error) << "a kit's probe at an id another engine's live probe holds";
    }
    qb::Main second;
    EXPECT_EQ(second.addActor<qb::test::TestProbe<Filled>>(0), id) << "the same id, in another engine";
    second.start(); // a start that is refused has joined its cores when it returns
    const bool refused = second.hasError();
    qb::Main::stop(); // both engines: the second runs, and would run on, if its probe was built
    second.join();
    first.join();
    EXPECT_TRUE(refused) << "a second probe was built at an id a live one holds";
    EXPECT_FALSE(first.hasError());
}

TEST(TestKit, AProbeThatStopsWithItsEngineSaysNothing) {
    // A probe that ends with its engine answers nobody: its askers stop with it. An answer from its destructor --
    // tried, and withdrawn -- reached a driver reaped in the same pass: a dead letter of the kit's own exchange in
    // every run a watchdog ended, handed to the test's dead-letter handler, and a scenario whose core read that answer
    // before the stop was reported as having thrown it.
    std::atomic<int>  letters{0};
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Filled>(0);
    kit.engine().core(0).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    const auto run = kit.run(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            (void) co_await qb::test::expect<Filled>(ctx, probe, std::chrono::hours(1));
        },
        200ms);
    EXPECT_FALSE(run);
    EXPECT_TRUE(run.timed_out);
    EXPECT_FALSE(run.completed);
    EXPECT_FALSE(run.error) << run.describe();
    EXPECT_EQ(letters.load(), 0) << "a run the watchdog ended left dead letters the test did not send";
}

TEST(TestKit, AProbeWhoseCoreStopsOnAnErrorTellsTheScenarioThatExpectsOnIt) {
    // The end that can answer nobody: an exception out of a handler ends the probe's core, and what its pipes held is
    // not sent. An expectation with no deadline of its own then waited until the watchdog -- ten seconds by default,
    // for ever with none. The kit's driver watches the probes its scenario expects on, and a watch is answered when a
    // core stops, behind everything that core had sent: both expectations throw, and say why.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(1);
    const auto        thrower = kit.add<Thrower>(1);
    const auto        run     = kit.run(
        [probe, thrower](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto [none, why, thrown] = co_await qb::io::async::when_all(quiet_verdict(ctx, probe, std::chrono::hours(1)),
                                                                        gone_because(ctx, probe), boom_now(ctx, thrower));
            (void) thrown;
            EXPECT_EQ(none, verdict::gone) << "a window on a probe whose core stopped neither held nor broke";
            EXPECT_NE(why.find("the probe's core stopped on an error"), std::string::npos) << why;
        },
        60s);
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out);
    EXPECT_TRUE(run.engine_error) << "a core stopped on an exception: the run says so";
    EXPECT_FALSE(run);
}

TEST(TestKit, AnExpectationOfAnotherActorOnAProbeWhoseCoreStopsGetsNoVerdict) {
    // The same end, seen by an asker that is not the kit's driver: nobody watches for it, and it registers no
    // `qb::DownEvent` -- the kit asks that of no actor. Its expectation gets no verdict, never a false one, and waits.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(1);
    const auto        sent    = kit.probe<Cancelled>(1);
    const auto        thrower = kit.add<Thrower>(1);
    const auto        heard   = kit.probe<Cancelled>(0);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    (void) kit.add<Outsider>(2, probe, sent, heard);
    const auto run = kit.run(
        [sent, thrower, heard, deaths, mourner](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_EQ(co_await next_told(ctx, sent), 1) << "the outsider's expectation waits on the probe";
            ctx.push_to<Mourn>(mourner, thrower);
            ctx.push_to<Boom>(thrower);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value())
                << "the probe's core stopped: its watcher said so";
            EXPECT_TRUE(co_await qb::test::expect_none<Cancelled>(ctx, heard, 200ms))
                << "it was given a verdict by a probe that could send none";
        },
        30s);
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_TRUE(run.engine_error) << "a core stopped on an exception";
}

TEST(TestKit, ADownEventSentToTheDriverEndsNoExpectation) {
    // The driver ends its scenario's expectations on a probe when its WATCH of that probe is answered -- the core's
    // answer, whose source is the probe. A `qb::DownEvent` of the same reason from any other actor ended them too, on
    // a probe that was alive, for the rest of the run.
    qb::test::TestKit kit;
    const auto        probe  = kit.probe<Filled>(0);
    const auto        desk   = kit.add<Desk>(0, probe);
    const auto        turn   = kit.probe<Cancelled>(0);
    const auto        forger = kit.add<Forger>(0);
    const auto        run    = kit.run(
        [probe, desk, turn, forger](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto forge_then_fill = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId desk, qb::ActorId turn,
                                      qb::ActorId forger) -> qb::io::async::task<void> {
                c.push_to<Forge>(forger, c.id(), probe); // behind the expectation: "the probe's core stopped", from the wrong sender
                co_await a_turn(c, turn);                // the driver has read it
                c.push_to<Order>(desk, 7);
            };
            auto [filled, done] =
                co_await qb::io::async::when_all(qb::test::expect<Filled>(ctx, probe, 30s), forge_then_fill(ctx, probe, desk, turn, forger));
            (void) done;
            EXPECT_EQ(filled ? filled->qty : -1, 7);
        },
        60s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AnAskerKilledWhileItsExpectationWaitsIsToldItWasCancelled) {
    // The asker's own end is `cancelled_error`, whatever scope the expectation was asked in: read as the end of the
    // probe, a killed asker was told "the probe's core stopped on an error".
    for (const qb::CoreId asker_core : {qb::CoreId{0}, qb::CoreId{1}}) {
        qb::test::TestKit kit;
        const auto        threw   = std::make_shared<std::atomic<int>>(0);
        const auto        probe   = kit.probe<Filled>(0);
        const auto        told    = kit.probe<Cancelled>(0);
        const auto        deaths  = kit.probe<Cancelled>(0);
        const auto        mourner = kit.add<Mourner>(0, deaths);
        const auto        asker   = kit.add<KilledWhileItWaits>(asker_core, probe, told, threw);
        (void) kit.add<Staller>(asker_core); // its core outlives it: the pass that unwinds its coroutine runs
        const auto run = kit.run(
            [told, deaths, mourner, asker](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                EXPECT_TRUE(co_await kill_once_it_waits(ctx, asker, told, mourner, deaths)) << "it waited, and it is gone";
            },
            30s);
        EXPECT_TRUE(run) << run.describe();
        EXPECT_EQ(threw->load(), 1) << "asker on core " << asker_core << ": 2 is probe_gone_error, 0 a frame that was never unwound";
    }
}

TEST(TestKit, AScenarioWhoseEngineIsStoppedByAnotherActorIsNotToldItsProbesAreGone) {
    // An engine that another actor stops takes the probes and the scenario together. The driver's watch of its probes
    // is answered then too -- "killed" -- and its core, held meanwhile, reads that answer before it reads the stop:
    // acted on, it ended the scenario's expectation with an error, and the run reported it or not by which of the two
    // its core read first. Only a core that stops on an error ends an expectation that waits. (One the scenario sends
    // AFTER the stop finds the probe gone, and throws, as after any end of a probe.)
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(1);
    const auto        stopper = kit.add<Stopper>(1);
    const auto        staller = kit.add<Staller>(0);
    const auto        run     = kit.run(
        [probe, stopper, staller](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            (void) co_await qb::io::async::when_all(qb::test::expect<Filled>(ctx, probe, std::chrono::hours(1)),
                                                    stop_and_stall(ctx, stopper, staller, 600ms));
        },
        30s);
    EXPECT_FALSE(run);
    EXPECT_FALSE(run.completed);
    EXPECT_FALSE(run.timed_out) << run.describe();
    EXPECT_FALSE(run.engine_error) << run.describe();
    EXPECT_FALSE(run.error) << run.describe();
}

TEST(TestKit, AnExpectationOfAProbeWhoseCoreStoppedOnAnErrorThrowsAtOnce) {
    // Once that core is gone its probes are too: the next expectation is refused, nothing sent.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Filled>(1);
    const auto        thrower = kit.add<Thrower>(1);
    const auto        deaths  = kit.probe<Cancelled>(0);
    const auto        mourner = kit.add<Mourner>(0, deaths);
    const auto        run     = kit.run(
        [probe, thrower, deaths, mourner](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<Mourn>(mourner, probe);
            ctx.push_to<Boom>(thrower);
            EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value())
                << "the probe's core stopped: its watcher said so";
            EXPECT_EQ(co_await quiet_verdict(ctx, probe, 30s), verdict::gone);
            EXPECT_EQ(co_await fill_verdict(ctx, probe, 30s), verdict::gone);
        },
        30s);
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out);
    EXPECT_TRUE(run.engine_error) << "a core stopped on an exception";
}

TEST(TestKit, AProbeThatRecordsKillEventRecordsTheOnesItIsSent) {
    // The probe's own end is a qb::KillEvent -- unless that is a type it records: the destination an actor under test
    // kills is a probe too, and it records the kill like any event it is sent.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<qb::KillEvent>(0);
    const auto        run   = kit.run([probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        ctx.push_to<qb::KillEvent>(probe);
        const auto kill = co_await qb::test::expect<qb::KillEvent>(ctx, probe, 10s);
        EXPECT_TRUE(kill.has_value());
        EXPECT_EQ(kill ? kill->getSource() : qb::ActorId{}, ctx.id());
        EXPECT_TRUE(co_await qb::test::expect_none<qb::KillEvent>(ctx, probe, 50ms)) << "recorded once, and the probe still answers";
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, ACoreThatStopsOnAnErrorRightAfterTheStartDoesNotEndTheRun) {
    // run() read hasError() right after the start and took any error for a start that was refused: a core that threw
    // in its first passes -- before the test thread had looked -- ended the wait at once, and a scenario that ran on
    // was reported not run to its end. Which thread looked first decided the report -- about one run in four here, so
    // the case runs twenty. The scenario hears core 1 stop (a watch answers a core's stop) and runs on.
    for (int round = 0; round < 20; ++round) {
        qb::test::TestKit kit;
        const auto        thrower = kit.add<Thrower>(1);
        const auto        deaths  = kit.probe<Cancelled>(0);
        const auto        mourner = kit.add<Mourner>(0, deaths);
        const auto        run     = kit.run(
            [thrower, deaths, mourner](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                ctx.push_to<Mourn>(mourner, thrower);
                ctx.push_to<Boom>(thrower); // core 1 stops on it, in its first passes
                EXPECT_TRUE((co_await qb::test::expect<Cancelled>(ctx, deaths, 10s)).has_value()) << "core 1 stopped: its watcher said so";
                co_await ctx.sleep(50ms); // the scenario runs on, on core 0
            },
            30s);
        EXPECT_TRUE(run.completed) << "round " << round << ": " << run.describe();
        EXPECT_FALSE(run.timed_out) << "round " << round;
        EXPECT_TRUE(run.engine_error) << "round " << round << ": a core stopped on an exception, the run says so";
        EXPECT_FALSE(run);
    }
}

TEST(TestKit, AStartRefusedBeforeTheDriverIsBuiltEndsTheRunAtOnce) {
    // The other half of the rule: an actor added before the driver throws from its constructor, its core builds
    // nothing after it, and no driver exists to end the wait -- run() ends it itself, without the watchdog.
    qb::test::TestKit kit;
    (void) kit.add<Unbuildable>(0);
    const auto run = kit.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; }, 30s);
    EXPECT_FALSE(run);
    EXPECT_TRUE(run.engine_error) << run.describe();
    EXPECT_FALSE(run.completed);
    EXPECT_FALSE(run.timed_out) << "nobody was left to end the wait but run() itself";
}

TEST(TestKit, AStartThatWasRefusedLeavesNoAnnouncementBehind) {
    // A kit announces a probe as it adds it, and a core that refuses to start builds none of what follows the actor it
    // could not build: the announcement stayed in the census until the kit itself was destroyed -- a run that was
    // over, and an id nothing would ever answer from, still held against the next kit. A run ends its kit's
    // announcements, as a run that built its probes ends them.
    const PatientActivation patient;
    qb::test::TestKit       refused;
    (void) refused.add<Unbuildable>(1);
    const auto never_built = refused.probe<Filled>(1); // behind the actor that throws: announced, never built
    const auto ran         = refused.run([](qb::ScopedCoroContext) -> qb::io::async::task<void> { co_return; }, 30s);
    EXPECT_TRUE(ran.engine_error) << ran.describe();
    EXPECT_FALSE(ran.completed);
    // `refused` is still alive: its run is over, and that is all the next kit needs.
    qb::test::TestKit kit;
    (void) kit.add<SlowToBuild>(1, 100ms);
    qb::ActorId probe;
    EXPECT_NO_THROW(probe = kit.probe<Filled>(1)) << "an announcement of a run that is over was held against the next kit";
    EXPECT_EQ(probe, never_built) << "the case is about ONE id: the second kit's probe must be given the first one's";
    const auto told = kit.probe<Cancelled>(0);
    (void) kit.add<Early>(0, probe, told);
    const auto run = kit.run(
        [told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            EXPECT_EQ(co_await next_told(ctx, told), static_cast<int>(verdict::quiet)) << "2: the id was not this kit's probe's";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(TestKit, AScenarioThatEndsAfterTheWatchdogDecidedIsNotReportedCompleted) {
    // The watchdog fires while the scenario's core is held in a handler, and the answer to its expectation is in that
    // core's mailbox, ahead of the stop: the scenario resumes, returns, and a run that had `timed_out` was reported
    // `completed` too -- by which of the two its core read first. The watchdog's verdict is the run's. The hold ends
    // when the engine is told to stop, heard on the probe's core: after the answer left it, never before the verdict.
    qb::test::TestKit kit;
    const auto        latch  = std::make_shared<Latch>();
    const auto        probe  = kit.probe<Filled>(1);
    const auto        holder = kit.add<Holder>(0, latch);
    (void) kit.add<Releaser>(1, latch);
    const auto run = kit.run(
        [probe, holder](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto [filled, held] = co_await qb::io::async::when_all(qb::test::expect<Filled>(ctx, probe, std::chrono::hours(1)),
                                                                   fill_then_hold(ctx, probe, holder));
            (void) held;
            EXPECT_EQ(filled ? filled->qty : -1, 7);
        },
        200ms);
    EXPECT_FALSE(run);
    EXPECT_TRUE(run.timed_out) << run.describe();
    EXPECT_FALSE(run.completed) << "a scenario that returned after the watchdog had decided was reported as run to its end";
    EXPECT_FALSE(run.error) << run.describe();
}
