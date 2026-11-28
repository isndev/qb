/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/spawn-on.cpp
 * @brief `qb::spawn_on` / `qb::Spawner` (Huly QB-54): an actor created on another core while the engine runs.
 *
 * Each case is a `qb::test::TestKit` scenario (`run<qb::SpawnRequest>`: the driver resolves the answers). Covered:
 * the spawn on another core and on the caller's own, the new actor RUNNING there (it reports its core) and reachable
 * by its id; arguments moved across (a `std::string`, a `std::vector`); an async `onInit()` waited for (a lower
 * bound only); an init that fails before it suspends (`refused`: the core rolls its admission back), one that fails
 * after and one that outlasts the timeout (`not_ready`, the id valid); a service tag
 * already there (`refused`); a core with no spawner and a core that does not exist (`unreachable`, at once, no crash);
 * many spawns at once; the caller that does not register `qb::SpawnRequest`, whose answer never resolves; and the
 * `Spawner`'s answer as the only one -- a target core, or the caller's own, held past the timeout delays it and
 * changes nothing in it; a `Spawner` killed under a request (`not_ready`, the id valid), ahead of one, twice, behind
 * a backlog, beside a core that has stopped; one that ends at once -- its core stops on an error, its engine is
 * stopped -- read as gone, not timed (`not_ready` and the id when it had created the actor, `unreachable` when
 * nothing took the request); one built after a request was sent; a spawn from an `onInit()`. A case orders what it
 * needs by events -- a probe, a gate, a pass of the core; where it holds a thread, the hold is the subject, and
 * the case cannot go red for a host that stalls longer.
 */

#include <gtest/gtest.h>
#include <qb/core/patterns.h>
#include <qb/string.h>
#include <qb/testkit.h>

#include "../../shared/InitFixtures.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace spawn_on_test {

/// What a spawned actor reports from its `onInit()`: where it runs and what it was built with.
struct Hello : qb::Event {
    qb::CoreId     core = 0;
    qb::string<32> name;
    std::size_t    items = 0;
    Hello(qb::CoreId c, std::string const &n, std::size_t i)
        : core(c)
        , name(n.c_str())
        , items(i) {}
};

struct Ping : qb::Event {};
struct Pong : qb::Event {};

class Worker : public qb::Actor {
    std::string      _name;
    std::vector<int> _items;
    qb::ActorId      _report;

public:
    Worker(std::string name, std::vector<int> items, qb::ActorId report)
        : _name(std::move(name))
        , _items(std::move(items))
        , _report(report) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        push<Hello>(_report, getIndex(), _name, _items.size());
        co_return true;
    }
    void
    on(Ping const &) {
        push<Pong>(_report);
    }
};

class SlowInit : public qb::Actor {
    qb::duration _wait;

public:
    explicit SlowInit(qb::duration wait)
        : _wait(wait) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(_wait);
        co_return true;
    }
};

class FailingInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return false; // before any suspension: the core rolls the admission back
    }
};

class FailingAsyncInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(std::chrono::milliseconds(1));
        co_return false; // after a suspension: the actor exists, Activating, then fails
    }
};

class Throwing : public qb::Actor {
public:
    Throwing() {
        throw std::runtime_error("a constructor that refuses its configuration");
    }
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

struct Registry : qb::ServiceActor<Registry> {
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

class Idle : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

using steady = std::chrono::steady_clock;

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

/// Takes `_wait` to become active, and says so only then: its `Hello` leaves in the pass that ends its init.
class HelloOnceActive : public qb::Actor {
    qb::ActorId  _report;
    qb::duration _wait;

public:
    HelloOnceActive(qb::ActorId report, qb::duration wait)
        : _report(report)
        , _wait(wait) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(_wait);
        push<Hello>(_report, getIndex(), std::string("active"), std::size_t{0});
        co_return true;
    }
};

/// Says it exists, then takes `_wait` to become active: a request for it is waiting at its `Spawner` once its
/// `Hello` is out.
class SlowHello : public qb::Actor {
    qb::ActorId  _report;
    qb::duration _wait;

public:
    SlowHello(qb::ActorId report, qb::duration wait)
        : _report(report)
        , _wait(wait) {}
    qb::io::async::task<bool>
    onInit() override {
        push<Hello>(_report, getIndex(), std::string("slow"), std::size_t{0});
        co_await context().sleep(_wait);
        co_return true;
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

/// Slow to build: the actors added to its core after it exist that much later.
class SlowToBuild : public qb::Actor {
public:
    explicit SlowToBuild(qb::duration d) {
        std::this_thread::sleep_for(d);
    }
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

/// What a pool tells of the spawn its `onInit()` asked for.
struct Spawned : qb::Event {
    qb::spawn_status status;
    explicit Spawned(qb::spawn_status s)
        : status(s) {}
};

/// A pool whose `onInit()` -- it runs while the other cores still build their actors -- spawns on `core`, and tells
/// `told` how it ended.
class EarlyPool : public qb::Actor {
    qb::CoreId  _core;
    qb::ActorId _told;

public:
    EarlyPool(qb::CoreId core, qb::ActorId told)
        : _core(core)
        , _told(told) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SpawnRequest>(*this);
        auto       ctx  = context();
        const auto told = _told;
        const auto w    = co_await qb::spawn_on<Idle>(ctx, _core, 30s);
        ctx.push_to<Spawned>(told, w.status);
        co_return true;
    }
    void
    on(qb::SpawnRequest &e) {
        (void) resolve_ask(e);
    }
};

/// Says it exists, then becomes active once its gate opens: read from a timer, a millisecond apart -- by an event,
/// never by a time of its own.
class GatedHello : public qb::Actor {
    qb::ActorId                        _report;
    std::shared_ptr<std::atomic<bool>> _gate;

public:
    GatedHello(qb::ActorId report, std::shared_ptr<std::atomic<bool>> gate)
        : _report(report)
        , _gate(std::move(gate)) {}
    qb::io::async::task<bool>
    onInit() override {
        push<Hello>(_report, getIndex(), std::string("gated"), std::size_t{0});
        while (!_gate->load())
            co_await context().sleep(1ms);
        co_return true;
    }
};

/// Opens the gate, then holds the core 20 ms: the timer that reads the gate is due by the next pass, whose timers end
/// the gated actor's init before that pass reads anything. With `kill_next`, that next pass also sends it a kill, from
/// this core: read one pass later still, ahead of what the pass that sent it pushes afterwards.
struct OpenTheGate : qb::Event {
    std::shared_ptr<std::atomic<bool>> gate;
    qb::ActorId                        kill_next;

    explicit OpenTheGate(std::shared_ptr<std::atomic<bool>> g, qb::ActorId k = {})
        : gate(std::move(g))
        , kill_next(k) {}
};

struct KillFromHere : qb::Event {
    qb::ActorId target;

    explicit KillFromHere(qb::ActorId t)
        : target(t) {}
};

class GateKeeper : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<OpenTheGate>(*this);
        registerEvent<KillFromHere>(*this);
        co_return true;
    }
    void
    on(OpenTheGate const &o) {
        o.gate->store(true);
        std::this_thread::sleep_for(20ms);
        if (o.kill_next.is_valid())
            push<KillFromHere>(id(), o.kill_next);
    }
    void
    on(KillFromHere const &k) {
        push<qb::KillEvent>(k.target);
    }
};

/// A consumer slower than its producer: `per` for each `Flood`.
struct Flood : qb::Event {};

class SlowSink : public qb::Actor {
    qb::duration _per;

public:
    explicit SlowSink(qb::duration per)
        : _per(per) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Flood>(*this);
        co_return true;
    }
    void
    on(Flood const &) {
        std::this_thread::sleep_for(_per);
    }
};

/// A producer beside a `Spawner`: `n` events for the sink, in the pipe the `Spawner`'s answers take -- more than the
/// sink's mailbox holds, so what follows them waits on this core, unpublished, for as long as the sink takes.
struct Go : qb::Event {
    int n;
    explicit Go(int count)
        : n(count) {}
};

class Flooder : public qb::Actor {
    qb::ActorId _sink;

public:
    explicit Flooder(qb::ActorId sink)
        : _sink(sink) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Go>(*this);
        co_return true;
    }
    void
    on(Go const &g) {
        for (int i = 0; i < g.n; ++i)
            push<Flood>(_sink);
    }
};

/// An `onInit()` that waits by running its thread's loop -- what a module's blocking call does -- before the engine's
/// start barrier: every timer and every yield of the thread resumes under it.
class Pumper : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        qb::io::async::run_for(10ms);
        co_return true;
    }
};

/// Builds a second `Spawner` on its core when told, and tells `told` whether the core admitted it.
struct Rebuild : qb::Event {};

class Rebuilder : public qb::Actor {
    qb::ActorId _told;

public:
    explicit Rebuilder(qb::ActorId told)
        : _told(told) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Rebuild>(*this);
        co_return true;
    }
    void
    on(Rebuild const &) {
        push<Spawned>(_told, addRefActor<qb::Spawner>().id().is_valid() ? qb::spawn_status::spawned : qb::spawn_status::refused);
    }
};

/// What a caller that outlives the stop of its engine read, and what became of the actor it asked for.
struct StopReport {
    std::atomic<bool>          exists{false};
    std::atomic<bool>          stopped{false}; ///< set by the test once it has stopped the engine
    std::atomic<bool>          active{false};
    std::atomic<int>           status{-1};
    std::atomic<std::uint64_t> returned{0}; ///< the id `spawn_on` returned
    std::atomic<std::uint64_t> created{0};  ///< the id the actor has
};

/// Created by a `spawn_on`: says it exists, becomes active only once the engine has been stopped -- a flag the test
/// sets, read a millisecond apart -- and ignores that stop: it finishes its work, then ends itself.
class Stubborn : public qb::Actor {
    std::shared_ptr<StopReport> _r;

public:
    explicit Stubborn(std::shared_ptr<StopReport> r)
        : _r(std::move(r)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SignalEvent>(*this);
        registerEvent<Ping>(*this);
        push<Ping>(id()); // stashed while it activates, replayed once it is active
        _r->created = id().raw64();
        _r->exists  = true;
        while (!_r->stopped.load())
            co_await context().sleep(1ms);
        co_return true;
    }
    void
    on(Ping const &) {
        _r->active = true;
        kill();
    }
    void
    on(qb::SignalEvent const &) {}
};

/// The caller: it too ignores the stop, reads what its `spawn_on` returns, then ends itself -- as it does after ten
/// seconds without an answer, so that a `spawn_on` that never returns is a failure of the case and not a hang.
class Survivor : public qb::Actor {
    std::shared_ptr<StopReport> _r;

public:
    explicit Survivor(std::shared_ptr<StopReport> r)
        : _r(std::move(r)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SpawnRequest>(*this);
        registerEvent<qb::SignalEvent>(*this);
        spawn([r = _r](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto race = co_await qb::io::async::when_any(qb::spawn_on<Stubborn>(ctx, 1, std::chrono::hours(1), r), ctx.sleep(10s));
            if (race.index == 0 && !race.has_exception()) {
                const auto w = race.get<qb::spawn_result>();
                r->returned  = w.id.raw64();
                r->status    = static_cast<int>(w.status);
            }
            ctx.push<qb::KillEvent>();
        });
        co_return true;
    }
    void
    on(qb::SpawnRequest &e) {
        (void) resolve_ask(e);
    }
    void
    on(qb::SignalEvent const &) {}
};

/// Spawns a `Worker` on `core` from a coroutine of its own, tells `told` how it ended, then ends itself: the last
/// actor of its core, which stops with it.
class OneShotCaller : public qb::Actor {
    qb::CoreId  _core;
    qb::ActorId _told;

public:
    OneShotCaller(qb::CoreId core, qb::ActorId told)
        : _core(core)
        , _told(told) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SpawnRequest>(*this);
        spawn([core = _core, told = _told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto w = co_await qb::spawn_on<Worker>(ctx, core, 5s, std::string("for a core that stops"), std::vector<int>{}, told);
            ctx.push_to<Spawned>(told, w.status);
            ctx.push<qb::KillEvent>();
        });
        co_return true;
    }
    void
    on(qb::SpawnRequest &e) {
        (void) resolve_ask(e);
    }
};

/// Tells `told` -- a `Spawned{unreachable}`, the one status no spawn of these cases ends on -- once `watched` is
/// destroyed.
struct Mourn : qb::Event {
    qb::ActorId watched;
    explicit Mourn(qb::ActorId w)
        : watched(w) {}
};

class Mourner : public qb::Actor {
    qb::ActorId _told;

public:
    explicit Mourner(qb::ActorId told)
        : _told(told) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Mourn>(*this);
        registerEvent<qb::DownEvent>(*this);
        co_return true;
    }
    void
    on(Mourn const &m) {
        watch(m.watched);
    }
    void
    on(qb::DownEvent const &) {
        push<Spawned>(_told, qb::spawn_status::unreachable);
    }
};

/// Opens the gate, holds the core 20 ms -- the gated actor's init ends in the timers of the next pass -- and sends
/// `target` a terminal signal from this core: read in that same pass, after the timers.
struct OpenThenSignal : qb::Event {
    std::shared_ptr<std::atomic<bool>> gate;
    qb::ActorId                        target;

    OpenThenSignal(std::shared_ptr<std::atomic<bool>> g, qb::ActorId t)
        : gate(std::move(g))
        , target(t) {}
};

class Signaller : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<OpenThenSignal>(*this);
        co_return true;
    }
    void
    on(OpenThenSignal const &o) {
        o.gate->store(true);
        std::this_thread::sleep_for(20ms);
        push<qb::SignalEvent>(o.target).signum = SIGINT;
    }
};

/// Kills itself, then -- two passes on, the kill read -- asks for a spawn on a core nobody serves, from the coroutine
/// that outlives it: says how that `spawn_on` ended, 1 returned, 2 `cancelled_error`.
class KilledCaller : public qb::Actor {
    std::shared_ptr<std::atomic<int>> _ended;

public:
    explicit KilledCaller(std::shared_ptr<std::atomic<int>> ended)
        : _ended(std::move(ended)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SpawnRequest>(*this);
        spawn([ended = _ended](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push<qb::KillEvent>();                            // read by this core's next pass
            co_await qb::detail::core_wait{qb::duration::zero()}; // that pass: resumed ahead of its receive
            co_await qb::detail::core_wait{qb::duration::zero()}; // the one after: the actor is killed
            try {
                (void) co_await qb::spawn_on<Idle>(ctx, 7, 1s);
                ended->store(1);
            } catch (qb::io::async::cancelled_error const &) {
                ended->store(2);
            }
        });
        co_return true;
    }
    void
    on(qb::SpawnRequest &e) {
        (void) resolve_ask(e);
    }
};

/// A handler that runs its thread's loop for `d`: what a module's blocking call does, inside a pass's receive.
struct RunTheLoop : qb::Event {
    qb::duration d;
    explicit RunTheLoop(qb::duration x)
        : d(x) {}
};

class LoopRunner : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<RunTheLoop>(*this);
        co_return true;
    }
    void
    on(RunTheLoop const &r) {
        qb::io::async::run_for(r.d);
    }
};

/// Sends `target` a `RunTheLoop` from ITS core: the event reaches the target's core through this core's ring.
struct RelayTheRun : qb::Event {
    qb::ActorId  target;
    qb::duration d;
    RelayTheRun(qb::ActorId t, qb::duration x)
        : target(t)
        , d(x) {}
};

class Relay : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<RelayTheRun>(*this);
        co_return true;
    }
    void
    on(RelayTheRun const &r) {
        push<RunTheLoop>(r.target, r.d);
    }
};

/// Hears a `Hello` from an actor of its own core, lets passes of that core go by -- an event it sends itself, six
/// hops -- then ends something of that core: `target` gets `signum` when there is one (a terminal signal ends a
/// `Spawner` at once), a `Boom` otherwise (a `Thrower`, and the core with it). What the core had to publish when
/// the `Hello` was read -- the answer about the actor that said it -- has left by then. Passes, not time: a host
/// that stalls, or a thread that is preempted, changes nothing in that order.
struct Hop : qb::Event {
    int left;
    explicit Hop(int n)
        : left(n) {}
};

class EndAfterPasses : public qb::Actor {
    qb::ActorId _target;
    int         _signum;

public:
    explicit EndAfterPasses(qb::ActorId target, int signum = 0)
        : _target(target)
        , _signum(signum) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Hello>(*this);
        registerEvent<Hop>(*this);
        co_return true;
    }
    void
    on(Hello const &) {
        push<Hop>(id(), 6);
    }
    void
    on(Hop const &h) {
        if (h.left > 0)
            push<Hop>(id(), h.left - 1);
        else if (_signum != 0)
            push<qb::SignalEvent>(_target).signum = _signum;
        else
            push<Boom>(_target);
    }
};

/// Active at once, and gone by the first thing it reads: an event it pushed to itself from its `onInit()`.
class EndsItselfAtOnce : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        push<Ping>(id());
        co_return true;
    }
    void
    on(Ping const &) {
        kill();
    }
};

/// Watches the broadcast id of `core` when told -- the question a killed `Spawner` asks a core, answered at once by
/// a `DownEvent` whose source IS that id -- and forwards the answer to `_to`, source kept: a core's answer, in
/// everything but who asked. Tells `told` once it has.
struct WatchAndForward : qb::Event {
    qb::CoreId core;
    explicit WatchAndForward(qb::CoreId c)
        : core(c) {}
};

class Forwarder : public qb::Actor {
    qb::ActorId _to;
    qb::ActorId _told;

public:
    Forwarder(qb::ActorId to, qb::ActorId told)
        : _to(to)
        , _told(told) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<WatchAndForward>(*this);
        registerEvent<qb::DownEvent>(*this);
        co_return true;
    }
    void
    on(WatchAndForward const &w) {
        watch(qb::BroadcastId(w.core));
    }
    void
    on(qb::DownEvent &down) {
        forward(_to, down);
        push<Spawned>(_told, qb::spawn_status::refused);
    }
};

/// A caller of its own, which counts every `qb::SpawnRequest` its handler is given: one request, one answer.
struct Answers {
    std::atomic<int> given{0};
    std::atomic<int> status{-1};
};

class CountingCaller : public qb::Actor {
    std::shared_ptr<Answers>           _answers;
    qb::ActorId                        _probe;
    std::shared_ptr<std::atomic<bool>> _gate;

public:
    CountingCaller(std::shared_ptr<Answers> answers, qb::ActorId probe, std::shared_ptr<std::atomic<bool>> gate)
        : _answers(std::move(answers))
        , _probe(probe)
        , _gate(std::move(gate)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SpawnRequest>(*this);
        spawn([answers = _answers, probe = _probe, gate = _gate](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto w = co_await qb::spawn_on<GatedHello>(ctx, 1, std::chrono::hours(1), probe, gate);
            answers->status.store(static_cast<int>(w.status));
        });
        co_return true;
    }
    void
    on(qb::SpawnRequest &e) {
        ++_answers->given;
        (void) resolve_ask(e);
    }
};

/// Ends once no `Spawner` holds `spawner`: the killed one is destroyed, at the end of its last pass.
qb::io::async::task<void>
until_it_is_destroyed(qb::ScopedCoroContext ctx, qb::ActorId spawner) {
    while (qb::detail::spawners().seen(spawner) != 0)
        co_await ctx.sleep(1ms);
}

/// Once the actor a pending `spawn_on` creates has said it exists -- its request waits at the `Spawner` -- sends
/// `E` to `target`. True when the `Hello` was heard.
template <typename E>
qb::io::async::task<bool>
once_it_exists(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::ActorId target) {
    const bool exists = (co_await qb::test::expect<Hello>(ctx, probe, 10s)).has_value();
    ctx.push_to<E>(target);
    co_return exists;
}

/// Hears the new actor say it exists, then lets `d` pass: the branch that a `spawn_on` with nothing to return loses to.
qb::io::async::task<bool>
hello_then_pause(qb::ScopedCoroContext ctx, qb::ActorId probe, qb::duration d) {
    const bool exists = (co_await qb::test::expect<Hello>(ctx, probe, 10s)).has_value();
    co_await ctx.sleep(d);
    co_return exists;
}

/// A spawn with a timeout of 100 ms of an actor whose constructor holds the target core's thread for 600 ms: the
/// core is held from inside the request, past the timeout and the grace the caller's own deadline used to add.
void
an_answer_delayed_by_a_stall_is_the_spawners(qb::CoreId spawner_core) {
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(spawner_core);
    const auto run = kit.run<qb::SpawnRequest>(
        [spawner_core](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto w = co_await qb::spawn_on<SlowToBuild>(ctx, spawner_core, 100ms, qb::duration{600ms});
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "an answer delayed by a stall was read as unreachable";
            EXPECT_TRUE(w.id.is_valid());
            EXPECT_EQ(w.id.index(), spawner_core);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

} // namespace spawn_on_test

using namespace spawn_on_test;

TEST(SpawnOn, CreatesAnActiveActorOnAnotherCoreThatRunsThere) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello, Pong>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto w = co_await qb::spawn_on<Worker>(ctx, 1, 5s, std::string("shard-7"), std::vector<int>{1, 2, 3}, probe);
            EXPECT_TRUE(w) << static_cast<int>(w.status);
            EXPECT_EQ(w.status, qb::spawn_status::spawned);
            EXPECT_EQ(w.id.index(), 1) << "created by the spawner of core 1, on core 1";
            const auto hello = co_await qb::test::expect<Hello>(ctx, probe, 5s);
            EXPECT_TRUE(hello.has_value());
            if (hello) {
                EXPECT_EQ(hello->core, 1) << "its onInit ran on core 1";
                EXPECT_EQ(std::string(hello->name.c_str()), "shard-7") << "the string crossed by value";
                EXPECT_EQ(hello->items, 3u);
                EXPECT_EQ(hello->getSource(), w.id);
            }
            ctx.push_to<Ping>(w.id);
            EXPECT_TRUE((co_await qb::test::expect<Pong>(ctx, probe, 5s)).has_value()) << "reachable by its id";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, TheCallersOwnCoreWorksToo) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(0);
    const auto run = kit.run<qb::SpawnRequest>([probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        const auto w = co_await qb::spawn_on<Worker>(ctx, 0, 5s, std::string("local"), std::vector<int>{}, probe);
        EXPECT_EQ(w.status, qb::spawn_status::spawned);
        EXPECT_EQ(w.id.index(), 0);
        const auto hello = co_await qb::test::expect<Hello>(ctx, probe, 5s);
        EXPECT_EQ(hello ? hello->core : qb::CoreId{99}, 0);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, AnAsyncInitIsWaitedForBeforeTheAnswer) {
    // No deadline of the Spawner's and none of the core's: a host that stalls for seconds under this case changes
    // nothing of what it reads.
    qb::test::ScopedDeadline none{0u};
    qb::test::TestKit        kit;
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<Idle>(0);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto start = steady::now();
            const auto w     = co_await qb::spawn_on<SlowInit>(ctx, 1, qb::duration::zero(), qb::duration{std::chrono::milliseconds(50)});
            EXPECT_EQ(w.status, qb::spawn_status::spawned);
            EXPECT_GE(steady::now() - start, 50ms) << "answered once onInit() completed, not at its construction";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, AnInitThatFailsIsRefusedOrNotReadyByWhenItFails) {
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto sync_failed = co_await qb::spawn_on<FailingInit>(ctx, 1, 5s);
            EXPECT_EQ(sync_failed.status, qb::spawn_status::refused) << "failed before suspending: never admitted";
            EXPECT_FALSE(sync_failed.id.is_valid());
            const auto async_failed = co_await qb::spawn_on<FailingAsyncInit>(ctx, 1, 5s);
            EXPECT_EQ(async_failed.status, qb::spawn_status::not_ready) << "failed after suspending: it existed";
            EXPECT_TRUE(async_failed.id.is_valid());
            const auto slow = co_await qb::spawn_on<SlowInit>(ctx, 1, 100ms, qb::duration{std::chrono::seconds(30)});
            EXPECT_EQ(slow.status, qb::spawn_status::not_ready) << "still activating when the timeout passed";
            EXPECT_TRUE(slow.id.is_valid());
        },
        30s);
    // The slow actor is still Activating when the run ends: the engine's stop ends it. A failed init is logged and
    // its actor removed; it stops no core, so the run is clean.
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, AConstructorThatThrowsIsRefusedAndTheCoreKeepsServing) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto thrown = co_await qb::spawn_on<Throwing>(ctx, 1, 2s);
            EXPECT_EQ(thrown.status, qb::spawn_status::refused) << "the throw stopped core 1 before";
            EXPECT_FALSE(thrown.id.is_valid());
            const auto after = co_await qb::spawn_on<Worker>(ctx, 1, 5s, std::string("after"), std::vector<int>{}, probe);
            EXPECT_EQ(after.status, qb::spawn_status::spawned) << "the core and its spawner still serve";
        },
        30s);
    EXPECT_TRUE(run) << run.describe() << " (an engine error means the throw stopped a core)";
}

TEST(SpawnOn, ATimeoutOfZeroWaitsWithoutADeadline) {
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto start = steady::now();
            const auto w     = co_await qb::spawn_on<SlowInit>(ctx, 1, qb::duration::zero(), qb::duration{std::chrono::milliseconds(400)});
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "answered once the init completed";
            EXPECT_GE(steady::now() - start, 400ms);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, AServiceAlreadyOnTheCoreIsRefused) {
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<Registry>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto again = co_await qb::spawn_on<Registry>(ctx, 1, 2s);
            EXPECT_EQ(again.status, qb::spawn_status::refused);
            EXPECT_FALSE(again.id.is_valid());
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ACoreWithoutSpawnerOrThatDoesNotExistIsUnreachable) {
    // `unreachable` is a fact read before anything is sent, not the end of a wait: a timeout of zero -- no deadline at
    // all -- gets it too, where it used to wait until its caller was killed.
    std::atomic<int>  letters{0};
    qb::test::TestKit kit;
    (void) kit.add<Idle>(1);                                     // core 1 runs, but no spawner there
    for (const qb::CoreId core : {qb::CoreId{0}, qb::CoreId{1}}) // an event for a core the engine does not run is its sender's dead letter
        kit.engine().core(core).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto none = co_await qb::spawn_on<Idle>(ctx, 1, 100ms);
            EXPECT_EQ(none.status, qb::spawn_status::unreachable);
            EXPECT_FALSE(none.id.is_valid());
            const auto unbounded = co_await qb::spawn_on<Idle>(ctx, 1, qb::duration::zero());
            EXPECT_EQ(unbounded.status, qb::spawn_status::unreachable) << "no deadline, and nothing to wait for";
            const auto absent = co_await qb::spawn_on<Idle>(ctx, 7, 100ms);
            EXPECT_EQ(absent.status, qb::spawn_status::unreachable) << "a core the engine does not run";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(letters.load(), 0) << "a request was sent where no Spawner would read it";
}

TEST(SpawnOn, ManySpawnsInFlightEachGetTheirOwnActor) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            std::vector<qb::io::async::task<qb::spawn_result>> spawns;
            for (int i = 0; i < 8; ++i)
                spawns.push_back(qb::spawn_on<Worker>(ctx, 1, 5s, "w" + std::to_string(i), std::vector<int>(i), probe));
            const auto              results = co_await qb::io::async::when_all(std::move(spawns));
            std::set<std::uint64_t> ids;
            for (auto const &r : results) {
                EXPECT_EQ(r.status, qb::spawn_status::spawned);
                ids.insert(r.id.raw64());
            }
            EXPECT_EQ(ids.size(), 8u) << "eight distinct actors";
            std::set<std::size_t> sizes;
            for (int i = 0; i < 8; ++i) {
                const auto hello = co_await qb::test::expect<Hello>(ctx, probe, 5s);
                if (hello)
                    sizes.insert(hello->items);
            }
            EXPECT_EQ(sizes.size(), 8u) << "each built with its own arguments";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ACallerThatDoesNotRegisterSpawnRequestNeverSeesTheAnswer) {
    // The answer is a dead letter at the driver, so spawn_on has nothing to return -- and it returns nothing: it used
    // to call the core unreachable 350 ms later, with the actor alive on it.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto race = co_await qb::io::async::when_any(
                qb::spawn_on<Worker>(ctx, 1, 100ms, std::string("orphan"), std::vector<int>{}, probe), hello_then_pause(ctx, probe, 600ms));
            EXPECT_EQ(race.index, 1u) << "spawn_on returned a verdict it was never given";
            EXPECT_FALSE(race.has_exception());
            if (race.index == 1 && !race.has_exception()) {
                EXPECT_TRUE(race.get<bool>()) << "the actor exists all the same";
            }
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ATimeoutOfMaxIsNoDeadlineNotAnOverflow) {
    // The caller's own deadline, timeout + a grace, overflowed for qb::duration::max() (UBSan): it has none now.
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        const auto w = co_await qb::spawn_on<SlowInit>(ctx, 1, qb::duration::max(), qb::duration{std::chrono::milliseconds(50)});
        EXPECT_EQ(w.status, qb::spawn_status::spawned);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, TheCoresActivationDeadlineStillBoundsAZeroTimeout) {
    // A zero timeout sets no deadline of the Spawner's, and the caller has none; the target core still fails an onInit() that
    // outlasts its activation deadline, and the answer is not_ready (the second review: the docs said "no deadline").
    qb::test::ScopedDeadline deadline{300u * 1000u * 1000u}; // 300 ms, before start(), restored after the run
    qb::test::TestKit        kit;
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            // No deadline of the Spawner's, and an init that never ends by itself: a host that stalls past a shorter
            // one would let it complete in the pass that enforces the deadline.
            const auto w = co_await qb::spawn_on<SlowInit>(ctx, 1, qb::duration::zero(), qb::duration{std::chrono::hours(1)});
            EXPECT_EQ(w.status, qb::spawn_status::not_ready) << "ended by the init, not by the core's activation deadline";
            EXPECT_TRUE(w.id.is_valid());
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, AnAnswerDelayedByAStallOfTheTargetCoreIsNotReadAsUnreachable) {
    // spawn_on timed its own ask -- the timeout plus a grace of 250 ms -- and called the core unreachable at its
    // expiry. On a target core that stalled past it, the actor was created, alive, and its id never reached the caller:
    // an orphan the caller had been told did not exist. The Spawner's answer is the only one now.
    an_answer_delayed_by_a_stall_is_the_spawners(1);
}

TEST(SpawnOn, AnAnswerDelayedByAStallOfTheCallersOwnCoreIsNotReadAsUnreachable) {
    // The same on one core: after the stall the caller's deadline and the Spawner's answer came due in one pass, and a
    // core checks its request deadlines before it reads what it was sent.
    an_answer_delayed_by_a_stall_is_the_spawners(0);
}

TEST(SpawnOn, ASpawnerKilledUnderARequestAnswersNotReadyWithTheId) {
    // The actor exists and its init runs when the Spawner is killed: the Spawner's wait died with it, and the caller,
    // which times nothing, would have waited for ever. A killed Spawner answers what waits on it.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto [w, existed] = co_await qb::io::async::when_all(
                qb::spawn_on<SlowHello>(ctx, 1, std::chrono::hours(1), probe, qb::duration{std::chrono::hours(1)}),
                once_it_exists<qb::KillEvent>(ctx, probe, qb::Actor::getServiceId<qb::Spawner>(1)));
            EXPECT_TRUE(existed);
            EXPECT_EQ(w.status, qb::spawn_status::not_ready) << "created, and its activation no longer the Spawner's to report";
            EXPECT_TRUE(w.id.is_valid());
            const auto after = co_await qb::spawn_on<Idle>(ctx, 1, 5s);
            EXPECT_EQ(after.status, qb::spawn_status::unreachable) << "that core's Spawner is gone";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ARequestSentBehindAKillOfItsSpawnerIsUnreachable) {
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            ctx.push_to<qb::KillEvent>(qb::Actor::getServiceId<qb::Spawner>(1));
            const auto w = co_await qb::spawn_on<Idle>(ctx, 1, 5s);
            EXPECT_EQ(w.status, qb::spawn_status::unreachable) << "the kill was ahead of the request: nothing is created";
            EXPECT_FALSE(w.id.is_valid());
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ASpawnerWhoseCoreStopsOnAnErrorIsReadAsGone) {
    // The end that answers nobody: an exception out of a handler ends the Spawner's core, and what its pipes held is
    // not sent. The caller reads that the Spawner is gone -- a fact it samples, not a deadline -- and the request it
    // shares with the Spawner says an actor was created: `not_ready`, with the id. `unreachable` would lose it.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Hello>(0);
    const auto        thrower = kit.add<Thrower>(1);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, thrower](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto [w, existed] = co_await qb::io::async::when_all(
                qb::spawn_on<SlowHello>(ctx, 1, std::chrono::hours(1), probe, qb::duration{std::chrono::hours(1)}),
                once_it_exists<Boom>(ctx, probe, thrower));
            EXPECT_TRUE(existed);
            EXPECT_EQ(w.status, qb::spawn_status::not_ready) << "the Spawner's core stopped under the request, its actor created";
            EXPECT_TRUE(w.id.is_valid());
            EXPECT_EQ(w.id.index(), 1);
        },
        30s);
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out);
    EXPECT_TRUE(run.engine_error) << "a core stopped on an exception";
}

TEST(SpawnOn, ASpawnFromAnOnInitWaitsForTheTargetsSpawnerToBeBuilt) {
    // onInit() runs while the other cores still build their actors: the target's Spawner does not exist yet -- its
    // core is building, for 400 ms, the actor added ahead of it. spawn_on lets its core take a turn first.
    qb::test::TestKit kit;
    const auto        told = kit.probe<Spawned>(0);
    (void) kit.add<SlowToBuild>(1, 400ms);
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<EarlyPool>(0, 1, told);
    const auto run = kit.run(
        [told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto said = co_await qb::test::expect<Spawned>(ctx, told, 10s);
            EXPECT_TRUE(said.has_value());
            EXPECT_EQ(said ? said->status : qb::spawn_status::unreachable, qb::spawn_status::spawned);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, AnAnswerOfASpawnerEndedAtOnceIsReadBeforeItIsReadAsGone) {
    // The caller reads whether its Spawner still exists at the head of a pass, and what it was sent later in that
    // pass. A Spawner that is killed waits for the caller to have read; one ended at once does not. Here the caller's
    // thread is held INSIDE a pass, ahead of that reading -- by a wait that came due first -- while the Spawner
    // answers `spawned` and is ended by a terminal signal -- sent from its own core, six passes after the new actor
    // said it was active, the answer gone: the reading is the next thing the thread does, "gone" is what it reads,
    // and the answer sits unread in its mailbox.
    // The caller concludes one pass later, and the answer has won by then; concluding at the reading returned
    // `not_ready`, of an actor the Spawner had said was active. (An init that ends outside the hold -- 250 ms into a
    // hold from 150 to 550 -- proves less, and cannot fail for it.)
    qb::test::TestKit kit;
    const auto        staller = kit.add<Staller>(0);
    const auto        stopper = kit.add<EndAfterPasses>(1, qb::Actor::getServiceId<qb::Spawner>(1), SIGINT);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [staller, stopper](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto hold_ahead_of_the_reading = [](qb::ScopedCoroContext c, qb::ActorId staller) -> qb::io::async::task<bool> {
                co_await qb::detail::core_wait{qb::duration::zero()}; // the pass the request leaves in, and the watch starts
                c.push_to<Stall>(staller, 150ms);                     // it lasts 150 ms: the first reading is due by the next
                co_await qb::detail::core_wait{qb::duration::zero()}; // the next pass, ahead of that reading: it came due first
                std::this_thread::sleep_for(400ms);                   // the Spawner answers, and is ended
                co_return true;
            };
            auto [w, held] = co_await qb::io::async::when_all(qb::spawn_on<HelloOnceActive>(ctx, 1, 5s, stopper, qb::duration{250ms}),
                                                              hold_ahead_of_the_reading(ctx, staller));
            EXPECT_TRUE(held);
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "the Spawner had answered: it was read as gone first";
            EXPECT_TRUE(w.id.is_valid());
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ARequestThatReachesAKilledSpawnerCreatesNothing) {
    // A request in the same batch as the kill, behind it -- a raw ask, sent at once: the Spawner still stands, for one
    // pass, and must not create an actor it will never report. It answers `unreachable`.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            using factory_t    = qb::detail::spawn_factory<Worker, std::string, std::vector<int>, qb::ActorId>;
            const auto spawner = qb::Actor::getServiceId<qb::Spawner>(1);
            ctx.push_to<qb::KillEvent>(spawner);
            const auto answer = co_await qb::ask(
                ctx, spawner,
                qb::SpawnRequest{std::make_shared<factory_t>(std::string("too late"), std::vector<int>{}, qb::ActorId{probe}), 5s},
                qb::duration::zero());
            EXPECT_EQ(answer.response.status, qb::spawn_status::unreachable);
            EXPECT_FALSE(answer.response.id.is_valid());
            EXPECT_TRUE(co_await qb::test::expect_none<Hello>(ctx, probe, 100ms)) << "an actor was created by a Spawner that was killed";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, TheCensusKnowsEachSpawnerOfAnIdByItsBirth) {
    // An ActorId names a place in ONE engine: two engines of one process give the Spawner of a core the same id, and
    // a core gives it to the Spawner built once the first was killed. The census holds each under its birth, and a
    // request is stamped with the last birth it saw: the first Spawner to end does not take the other out, and one
    // born after the stamp stands for nobody the request was sent to.
    const auto id     = qb::Actor::getServiceId<qb::Spawner>(qb::CoreId{200}); // no engine of this binary runs a core 200
    auto      &census = qb::detail::spawners();
    ASSERT_EQ(census.seen(id), 0u);
    const auto first  = census.enter(id);
    const auto second = census.enter(id);
    EXPECT_GT(second, first);
    const auto stamp = census.seen(id);
    EXPECT_GE(stamp, second) << "a request sent now was sent to both";
    census.leave(first);
    EXPECT_TRUE(census.holds(id, stamp)) << "one Spawner ended, and the other left the census with it";
    const auto third = census.enter(id);
    census.leave(third);
    EXPECT_TRUE(census.holds(id, stamp)) << "the youngest Spawner ended, and an older one left the census for it";
    EXPECT_FALSE(census.holds(id, first)) << "a request stamped before the second was born is held by the first alone";
    census.leave(second);
    EXPECT_FALSE(census.holds(id, stamp));
    EXPECT_EQ(census.seen(id), 0u) << "the last Spawner ended, and its id is still held";
    const auto fourth = census.enter(id);
    EXPECT_FALSE(census.holds(id, stamp)) << "a Spawner born after the request was stamped stands for those it was sent to";
    EXPECT_TRUE(census.holds(id, census.seen(id)));
    census.leave(fourth);
    census.leave(fourth); // one leave too many is nobody's entry: it takes none away
    EXPECT_EQ(census.seen(id), 0u);
}

TEST(SpawnOn, AWaitThatEndsInAKilledSpawnersLastPassIsNoDeadLetter) {
    // The Spawner is on the caller's own core: killed, it has no other core to wait for, and ends a pass after its
    // kill. The kill answers the request, and the wait for its actor ends in that last pass -- the gate opens behind
    // the kill, in the batch that carries both. A settlement pushed then would be read once the Spawner is dead: a
    // dead letter of its core, counted, logged and handed to the user's handler, for an event no user sent.
    std::atomic<int>  letters{0};
    qb::test::TestKit kit;
    const auto        probe  = kit.probe<Hello>(0);
    const auto        keeper = kit.add<GateKeeper>(0);
    (void) kit.add<qb::Spawner>(0);
    kit.engine().core(0).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, keeper](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner        = qb::Actor::getServiceId<qb::Spawner>(0);
            const auto gate           = std::make_shared<std::atomic<bool>>(false);
            auto       kill_then_open = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId spawner, qb::ActorId keeper,
                                           std::shared_ptr<std::atomic<bool>> gate) -> qb::io::async::task<bool> {
                const bool exists = (co_await qb::test::expect<Hello>(c, probe, 10s)).has_value(); // the request waits at the Spawner
                c.push_to<qb::KillEvent>(spawner);
                c.push_to<OpenTheGate>(keeper, std::move(gate));
                co_return exists;
            };
            auto [w, existed] = co_await qb::io::async::when_all(qb::spawn_on<GatedHello>(ctx, 0, std::chrono::hours(1), probe, gate),
                                                                 kill_then_open(ctx, probe, spawner, keeper, gate));
            EXPECT_TRUE(existed);
            EXPECT_EQ(w.status, qb::spawn_status::not_ready) << "the kill's answer, and no other";
            EXPECT_TRUE(w.id.is_valid());
            co_await until_it_is_destroyed(ctx, spawner);
            co_await ctx.sleep(50ms); // the wait has resumed by now, a pass after the Spawner's end
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(letters.load(), 0) << "the Spawner's own settlement was left to its core as a dead letter";
}

TEST(SpawnOn, ASettlementReadBehindTheKillThatAnsweredItsRequestAnswersNobody) {
    // The other order: the actor's init ends in the pass that sends the Spawner a kill from its own core. The wait
    // ends while the Spawner still serves, and its settlement is pushed behind that kill: read after it, by a Spawner
    // that has answered the request and is still alive, it finds none, and answers nobody. The kill's answer says what
    // the actor is when it is written: active.
    std::atomic<int>  letters{0};
    qb::test::TestKit kit;
    const auto        probe  = kit.probe<Hello>(0);
    const auto        keeper = kit.add<GateKeeper>(1);
    (void) kit.add<qb::Spawner>(1);
    kit.engine().core(1).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, keeper](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner        = qb::Actor::getServiceId<qb::Spawner>(1);
            const auto gate           = std::make_shared<std::atomic<bool>>(false);
            auto       open_then_kill = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId spawner, qb::ActorId keeper,
                                           std::shared_ptr<std::atomic<bool>> gate) -> qb::io::async::task<bool> {
                const bool exists = (co_await qb::test::expect<Hello>(c, probe, 10s)).has_value();
                c.push_to<OpenTheGate>(keeper, std::move(gate), spawner);
                co_return exists;
            };
            auto [w, existed] = co_await qb::io::async::when_all(qb::spawn_on<GatedHello>(ctx, 1, std::chrono::hours(1), probe, gate),
                                                                 open_then_kill(ctx, probe, spawner, keeper, gate));
            EXPECT_TRUE(existed);
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "the kill was read first, and the actor was active by then";
            EXPECT_TRUE(w.id.is_valid());
            co_await until_it_is_destroyed(ctx, spawner);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(letters.load(), 0);
}

TEST(SpawnOn, ASpawnedAnswerBehindABacklogOutlivesTheKillOfItsSpawner) {
    // The Spawner answers `spawned` -- behind 6000 events its core has for the caller's, whose consumer takes 150 us
    // each: the answer waits on the Spawner's core, unpublished, for most of a second -- and is killed once the actor
    // has said it exists, to a probe on a third core, which the flood does not reach. A Spawner that left the census
    // a pass after its kill was read as gone long before its answer was: `unreachable`, of an actor that is active.
    // It leaves once the caller's core has read what it was sent.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Hello, Pong>(2);
    const auto        sink    = kit.add<SlowSink>(0, qb::duration{150us});
    const auto        flooder = kit.add<Flooder>(1, sink);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, flooder](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto kill_once_it_exists = [](qb::ScopedCoroContext c, qb::ActorId probe) -> qb::io::async::task<qb::ActorId> {
                const auto hello = co_await qb::test::expect<Hello>(c, probe, 20s);
                c.push_to<qb::KillEvent>(qb::Actor::getServiceId<qb::Spawner>(1));
                co_return hello ? hello->getSource() : qb::ActorId{};
            };
            ctx.push_to<Go>(flooder, 6000); // ahead of the request, in the one pipe to core 1
            auto [w, worker] = co_await qb::io::async::when_all(
                qb::spawn_on<Worker>(ctx, 1, 5s, std::string("behind"), std::vector<int>{}, probe), kill_once_it_exists(ctx, probe));
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "the actor is active and the Spawner said so: it was read as gone first";
            EXPECT_EQ(w.id, worker);
            ctx.push_to<Ping>(worker);
            EXPECT_TRUE((co_await qb::test::expect<Pong>(ctx, probe, 20s)).has_value()) << "the actor does not answer";
        },
        60s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ASpawnFromAnOnInitIsNotSentByAPumpOfItsCoresLoop) {
    // Beside the pool, an actor whose onInit() runs the thread's loop before the engine's start barrier: a spawn_on
    // that only yielded resumed under that pump, read a census the target core had not reached yet -- it is building,
    // for 400 ms, the actor added ahead of its Spawner -- and returned `unreachable` for good. A pass of the core is
    // what it waits for, and no pump stands in for one.
    qb::test::TestKit kit;
    const auto        told = kit.probe<Spawned>(0);
    (void) kit.add<SlowToBuild>(1, 400ms);
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<EarlyPool>(0, 1, told);
    (void) kit.add<Pumper>(0); // on the caller's core, its onInit() after the pool's
    const auto run = kit.run(
        [told](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto said = co_await qb::test::expect<Spawned>(ctx, told, 10s);
            EXPECT_TRUE(said.has_value());
            EXPECT_EQ(said ? said->status : qb::spawn_status::unreachable, qb::spawn_status::spawned)
                << "the census was read before core 1 had built its Spawner";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ASpawnerBuiltAfterARequestWasSentCreatesNothingForIt) {
    // A core's Spawner is killed and another built in its place. A request stamped before the second was born was
    // sent to the first: its caller reads the first gone and returns `unreachable`, whatever becomes of the request.
    // The second must create nothing for it -- an actor nobody would ever hear of -- and serves what is sent to it.
    qb::test::TestKit kit;
    const auto        probe     = kit.probe<Hello, Spawned>(0);
    const auto        rebuilder = kit.add<Rebuilder>(1, probe);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, rebuilder](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            using factory_t    = qb::detail::spawn_factory<Worker, std::string, std::vector<int>, qb::ActorId>;
            const auto spawner = qb::Actor::getServiceId<qb::Spawner>(1);
            const auto stamp   = qb::detail::spawners().seen(spawner); // what a request sent now carries
            EXPECT_NE(stamp, 0u);
            ctx.push_to<qb::KillEvent>(spawner);
            co_await until_it_is_destroyed(ctx, spawner);
            ctx.push_to<Rebuild>(rebuilder);
            const auto rebuilt = co_await qb::test::expect<Spawned>(ctx, probe, 10s);
            EXPECT_EQ(rebuilt ? rebuilt->status : qb::spawn_status::refused, qb::spawn_status::spawned)
                << "the core admitted no second Spawner";
            EXPECT_FALSE(qb::detail::spawners().holds(spawner, stamp)) << "the second Spawner stands for the one that request was sent to";
            auto late         = std::make_shared<factory_t>(std::string("late"), std::vector<int>{}, qb::ActorId{probe});
            late->stamp       = stamp;
            const auto answer = co_await qb::ask(ctx, spawner, qb::SpawnRequest{late, 5s}, qb::duration::zero());
            EXPECT_EQ(answer.response.status, qb::spawn_status::unreachable);
            EXPECT_FALSE(answer.response.id.is_valid());
            EXPECT_TRUE(co_await qb::test::expect_none<Hello>(ctx, probe, 100ms))
                << "an actor was created for a request sent to a Spawner that had ended";
            const auto fresh = co_await qb::spawn_on<Worker>(ctx, 1, 5s, std::string("fresh"), std::vector<int>{}, probe);
            EXPECT_EQ(fresh.status, qb::spawn_status::spawned) << "the second Spawner does not serve what is sent to it";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ASpawnerBornLaterAtTheSameIdDoesNotStandForTheOneThatWasAsked) {
    // The same between two engines of one process: another engine starts a Spawner on the same core index -- the same
    // id, a later birth -- while a request waits at this engine's, whose core then stops on an error. The request was
    // sent to the one Spawner that could take it: the caller reads that one gone, not an id that is held.
    std::atomic<bool> go{false};
    std::atomic<bool> built{false};
    std::atomic<bool> done{false};
    std::thread       other([&go, &built, &done] {
        while (!go.load())
            std::this_thread::sleep_for(1ms);
        qb::Main engine;
        engine.addActor<qb::Spawner>(1);
        engine.start(); // returns once its core has built the Spawner
        built = true;
        while (!done.load())
            std::this_thread::sleep_for(1ms);
        qb::Main::stop();
        engine.join();
    });
    {
        qb::test::TestKit kit;
        const auto        probe   = kit.probe<Hello>(0);
        const auto        thrower = kit.add<Thrower>(1);
        (void) kit.add<qb::Spawner>(1);
        const auto run = kit.run<qb::SpawnRequest>(
            [probe, thrower, &go, &built](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                auto stop_its_core_beside_another = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId thrower, std::atomic<bool> *go,
                                                       std::atomic<bool> *built) -> qb::io::async::task<bool> {
                    const bool exists = (co_await qb::test::expect<Hello>(c, probe, 10s)).has_value(); // the request waits at the Spawner
                    go->store(true);
                    while (!built->load())
                        co_await c.sleep(1ms);
                    c.push_to<Boom>(thrower);
                    co_return exists;
                };
                auto [w, existed] = co_await qb::io::async::when_all(
                    qb::spawn_on<SlowHello>(ctx, 1, std::chrono::hours(1), probe, qb::duration{std::chrono::hours(1)}),
                    stop_its_core_beside_another(ctx, probe, thrower, &go, &built));
                EXPECT_TRUE(existed);
                EXPECT_EQ(w.status, qb::spawn_status::not_ready);
                EXPECT_TRUE(w.id.is_valid());
            },
            30s);
        EXPECT_TRUE(run.completed) << run.describe();
        EXPECT_FALSE(run.timed_out) << "the caller waited on a Spawner that was never asked";
        EXPECT_TRUE(run.engine_error) << "a core stopped on an exception";
    }
    go   = true; // a run that never reached it
    done = true;
    other.join();
}

TEST(SpawnOn, AnActorCreatedByASpawnerThatEndsWithItsEngineIsNotReadAsUnreachable) {
    // A stop of the engine ends the Spawner at once, and it answers nobody. A caller that outlives the stop -- it
    // finishes its work first, as the actor it asked for does -- reads the Spawner gone: with an actor created for
    // its request, `not_ready` and the id. `unreachable` would say nothing was created, of an actor that lives on.
    auto     report = std::make_shared<StopReport>();
    qb::Main engine;
    engine.addActor<Survivor>(0, report);
    engine.addActor<qb::Spawner>(1);
    engine.start();
    for (const auto until = steady::now() + 10s; !report->exists.load() && steady::now() < until;)
        std::this_thread::sleep_for(1ms); // the request waits at the Spawner: its actor exists, and activates
    EXPECT_TRUE(report->exists.load()) << "no actor was created";
    qb::Main::stop();
    report->stopped = true; // the actor's init ends after the stop: no Spawner is left to say what it became
    engine.join();
    EXPECT_FALSE(engine.hasError());
    EXPECT_EQ(report->status.load(), static_cast<int>(qb::spawn_status::not_ready));
    EXPECT_NE(report->created.load(), 0u);
    EXPECT_EQ(report->returned.load(), report->created.load()) << "the id of the actor that was created";
    EXPECT_TRUE(report->active.load()) << "the actor lived on, and became active";
}

TEST(SpawnOn, ATargetCoreHeldPastTheSpawnersTimeoutDoesNotReportAnActiveActorNotReady) {
    // The Spawner's own wait: 300 ms for an init of 50, on a core held 600 ms once the actor exists. The pass after
    // the hold reads the wait's deadline before it runs the turn that ends the init: the wait says "not active" of
    // an actor that is, an instant later. The answer says what the actor is when it is written.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Hello>(0);
    const auto        staller = kit.add<Staller>(1);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, staller](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto hold_once_it_exists = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId staller) -> qb::io::async::task<bool> {
                const bool exists = (co_await qb::test::expect<Hello>(c, probe, 10s)).has_value();
                c.push_to<Stall>(staller, 600ms);
                co_return exists;
            };
            auto [w, existed] = co_await qb::io::async::when_all(qb::spawn_on<SlowHello>(ctx, 1, 300ms, probe, qb::duration{50ms}),
                                                                 hold_once_it_exists(ctx, probe, staller));
            EXPECT_TRUE(existed);
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "the stall stood in for the answer: the init took 50 ms of a timeout of 300";
            EXPECT_TRUE(w.id.is_valid());
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ARequestBehindTheEventThatStopsItsSpawnersCoreIsUnreachable) {
    // The other end that answers nobody, with nothing created: the request is stamped while the Spawner lives, and
    // leaves behind the event that stops its core -- both wait for this core's next pass, the event first. Nothing
    // took the request: `unreachable`, a fact.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Hello>(0);
    const auto        thrower = kit.add<Thrower>(1);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, thrower](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto boom_at_the_next_pass = [](qb::ScopedCoroContext c, qb::ActorId thrower) -> qb::io::async::task<bool> {
                co_await qb::detail::core_wait{qb::duration::zero()};
                c.push_to<Boom>(thrower);
                co_return true;
            };
            auto [sent, w] = co_await qb::io::async::when_all(
                boom_at_the_next_pass(ctx, thrower),
                qb::spawn_on<Worker>(ctx, 1, std::chrono::hours(1), std::string("never"), std::vector<int>{}, probe));
            EXPECT_TRUE(sent);
            EXPECT_EQ(w.status, qb::spawn_status::unreachable) << "nothing took the request";
            EXPECT_FALSE(w.id.is_valid());
            EXPECT_TRUE(co_await qb::test::expect_none<Hello>(ctx, probe, 100ms));
        },
        30s);
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out);
    EXPECT_TRUE(run.engine_error) << "a core stopped on an exception";
}

TEST(SpawnOn, ARequestCopiedToTwoSpawnersCreatesOneActor) {
    // `qb::SpawnRequest` is public and can be copied: two requests that share one factory, sent to the Spawners of two
    // cores. The arguments are moved out by the first construction; the factory builds once, whichever core asks
    // first, and the other Spawner answers `refused`.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<qb::Spawner>(2);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            using factory_t = qb::detail::spawn_factory<Worker, std::string, std::vector<int>, qb::ActorId>;
            auto shared     = std::make_shared<factory_t>(std::string("one"), std::vector<int>{1, 2}, qb::ActorId{probe});
            auto asked      = [](qb::ScopedCoroContext c, qb::CoreId core,
                                 std::shared_ptr<factory_t> factory) -> qb::io::async::task<qb::spawn_result> {
                const auto answer = co_await qb::ask(c, qb::Actor::getServiceId<qb::Spawner>(core), qb::SpawnRequest{std::move(factory), 5s},
                                                     qb::duration::zero());
                co_return answer.response;
            };
            auto [first, second] = co_await qb::io::async::when_all(asked(ctx, 1, shared), asked(ctx, 2, shared));
            const int spawned    = (first.status == qb::spawn_status::spawned ? 1 : 0) + (second.status == qb::spawn_status::spawned ? 1 : 0);
            const int refused    = (first.status == qb::spawn_status::refused ? 1 : 0) + (second.status == qb::spawn_status::refused ? 1 : 0);
            EXPECT_EQ(spawned, 1) << static_cast<int>(first.status) << " and " << static_cast<int>(second.status);
            EXPECT_EQ(refused, 1);
            const auto hello = co_await qb::test::expect<Hello>(ctx, probe, 5s);
            EXPECT_TRUE(hello.has_value());
            EXPECT_EQ(hello ? hello->items : 0u, 2u) << "built from arguments another construction had moved out";
            EXPECT_TRUE(co_await qb::test::expect_none<Hello>(ctx, probe, 100ms)) << "the factory built twice";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ASpawnerKilledTwiceWaitsForEveryCoreItAnsweredToHaveRead) {
    // The Spawner has answered two cores -- a pool on core 2, then this caller, whose answer waits behind 6000 events
    // for a consumer that takes 150 us each -- when it is killed, twice in one batch. It ends once, and only when
    // BOTH cores have read what it sent: core 2 says so at once, this one after most of a second. A second kill that
    // started a second ending, an ending at the first core to answer, or a `DownEvent` from anybody counted as a
    // core's answer -- one is pushed behind the kills -- left the census with this caller's answer unread.
    std::atomic<int>  letters{0};
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Hello, Pong, Spawned>(2);
    const auto        sink    = kit.add<SlowSink>(0, qb::duration{150us});
    const auto        flooder = kit.add<Flooder>(1, sink);
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<EarlyPool>(2, 1, probe);
    kit.engine().core(1).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, flooder](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner = qb::Actor::getServiceId<qb::Spawner>(1);
            const auto pooled  = co_await qb::test::expect<Spawned>(ctx, probe, 20s); // core 2 has been answered
            EXPECT_EQ(pooled ? pooled->status : qb::spawn_status::unreachable, qb::spawn_status::spawned);
            auto kill_twice_once_it_exists = [](qb::ScopedCoroContext c, qb::ActorId probe,
                                                qb::ActorId spawner) -> qb::io::async::task<qb::ActorId> {
                const auto hello = co_await qb::test::expect<Hello>(c, probe, 20s);
                c.push_to<qb::KillEvent>(spawner);
                c.push_to<qb::KillEvent>(spawner);
                c.push_to<qb::DownEvent>(spawner, qb::ActorId{}, qb::DownReason::killed); // a death it never asked about
                co_return hello ? hello->getSource() : qb::ActorId{};
            };
            ctx.push_to<Go>(flooder, 6000); // ahead of the request, in the one pipe to core 1
            auto [w, worker] =
                co_await qb::io::async::when_all(qb::spawn_on<Worker>(ctx, 1, 5s, std::string("behind"), std::vector<int>{}, probe),
                                                 kill_twice_once_it_exists(ctx, probe, spawner));
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "the Spawner left the census with its answer to this core unread";
            EXPECT_EQ(w.id, worker);
            co_await until_it_is_destroyed(ctx, spawner); // it ends: one ending, not two that wait on each other
        },
        60s);
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(letters.load(), 0) << "a farewell or a settlement of the Spawner was left to its core";
}

TEST(SpawnOn, ASpawnerKilledBesideACoreThatStoppedStillEnds) {
    // A caller on core 2 is answered, then ends: its core, which ran nothing else, stops. The Spawner's core has
    // 3000 events for it that will never leave. Killed, the Spawner asks core 2 whether it has read its answer, and
    // is told the core has stopped: it ends. A Spawner that waited for its own core to have nothing left to send
    // never did -- and an engine shut down by kills never ended.
    qb::test::TestKit kit;
    const auto        probe   = kit.probe<Hello, Spawned>(0);
    const auto        mourner = kit.add<Mourner>(1, probe);
    (void) kit.add<qb::Spawner>(1);
    const auto caller  = kit.add<OneShotCaller>(2, 1, probe);
    const auto flooder = kit.add<Flooder>(1, caller);
    const auto run     = kit.run<qb::SpawnRequest>(
        [probe, mourner, caller, flooder](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner  = qb::Actor::getServiceId<qb::Spawner>(1);
            const auto answered = co_await qb::test::expect<Spawned>(ctx, probe, 10s);
            EXPECT_EQ(answered ? answered->status : qb::spawn_status::unreachable, qb::spawn_status::spawned);
            ctx.push_to<Mourn>(mourner, caller); // only now: the two reports come from two cores, and would cross
            const auto mourned = co_await qb::test::expect<Spawned>(ctx, probe, 10s); // the caller is destroyed: core 2 stops
            EXPECT_EQ(mourned ? mourned->status : qb::spawn_status::spawned, qb::spawn_status::unreachable);
            ctx.push_to<Go>(flooder, 3000); // for an actor that is gone, on a core that reads nothing any more
            ctx.push_to<qb::KillEvent>(spawner);
            co_await until_it_is_destroyed(ctx, spawner);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ASpawnerEndedByASignalInThePassItsActorBecameActiveLeavesNoDeadLetter) {
    // A terminal signal ends a Spawner at once -- no farewell -- and here in the very pass whose timers ended its
    // actor's init: the wait for that actor resumes one pass later, the Spawner reaped. It settles nothing: a
    // settlement pushed then was a dead letter of the core. The caller reads the Spawner gone, and the actor created.
    std::atomic<int>  letters{0};
    qb::test::TestKit kit;
    const auto        probe     = kit.probe<Hello>(0);
    const auto        signaller = kit.add<Signaller>(1);
    (void) kit.add<qb::Spawner>(1);
    kit.engine().core(1).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, signaller](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner          = qb::Actor::getServiceId<qb::Spawner>(1);
            const auto gate             = std::make_shared<std::atomic<bool>>(false);
            auto       open_then_signal = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId spawner, qb::ActorId signaller,
                                             std::shared_ptr<std::atomic<bool>> gate) -> qb::io::async::task<bool> {
                const bool exists = (co_await qb::test::expect<Hello>(c, probe, 10s)).has_value(); // the request waits at the Spawner
                c.push_to<OpenThenSignal>(signaller, std::move(gate), spawner);
                co_return exists;
            };
            auto [w, existed] = co_await qb::io::async::when_all(qb::spawn_on<GatedHello>(ctx, 1, std::chrono::hours(1), probe, gate),
                                                                 open_then_signal(ctx, probe, spawner, signaller, gate));
            EXPECT_TRUE(existed);
            EXPECT_EQ(w.status, qb::spawn_status::not_ready) << "the Spawner ended at once, its actor created";
            EXPECT_TRUE(w.id.is_valid());
            co_await ctx.sleep(50ms); // the wait has resumed by now, a pass after the Spawner's end
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(letters.load(), 0) << "the wait of a Spawner that had ended settled, with nobody to read it";
}

TEST(SpawnOn, ACallerKilledBeforeItsRequestLeavesIsCancelledWhateverTheCensusSays) {
    // `spawn_on` first waits for a pass of its core, and that wait does not hear a kill. A caller killed meanwhile
    // must be told so when it resumes: with no Spawner on the target core it was told `unreachable` instead -- a
    // result, to a coroutine whose actor is gone, and a loop that retries on it never ended.
    auto              ended = std::make_shared<std::atomic<int>>(0);
    qb::test::TestKit kit;
    (void) kit.add<Idle>(1);
    (void) kit.add<KilledCaller>(1, ended);
    const auto run = kit.run(
        [ended](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            while (ended->load() == 0)
                co_await ctx.sleep(1ms);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(ended->load(), 2) << "a killed caller's spawn_on returned a result";
}

TEST(SpawnOn, ADeathNobodyAskedAboutDoesNotEndAServingSpawner) {
    // A killed Spawner ends on the answers of the cores it asked. One that serves asked nobody: a `DownEvent` pushed
    // to it, and one forwarded to it from a watch of its own core's broadcast id -- a core's answer in everything
    // but who asked -- are not its to act on. It ended at once, with nobody answered.
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello, Spawned>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto forwarder = kit.add<Forwarder>(2, qb::Actor::getServiceId<qb::Spawner>(1), probe);
    const auto run       = kit.run<qb::SpawnRequest>(
        [probe, forwarder](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner = qb::Actor::getServiceId<qb::Spawner>(1);
            ctx.push_to<qb::DownEvent>(spawner, qb::ActorId{}, qb::DownReason::killed);
            const auto after_a_push = co_await qb::spawn_on<Worker>(ctx, 1, 5s, std::string("first"), std::vector<int>{}, probe);
            EXPECT_EQ(after_a_push.status, qb::spawn_status::spawned) << "a death it never asked about ended the Spawner";
            ctx.push_to<WatchAndForward>(forwarder, qb::CoreId{1});
            const auto forwarded = co_await qb::test::expect<Spawned>(ctx, probe, 10s); // the forwarder's word: it is on its way
            EXPECT_EQ(forwarded ? forwarded->status : qb::spawn_status::spawned, qb::spawn_status::refused);
            const auto after_a_forward = co_await qb::spawn_on<Worker>(ctx, 1, 5s, std::string("second"), std::vector<int>{}, probe);
            EXPECT_EQ(after_a_forward.status, qb::spawn_status::spawned) << "the answer of a core to somebody else ended the Spawner";
            const auto still = co_await qb::spawn_on<Worker>(ctx, 1, 5s, std::string("third"), std::vector<int>{}, probe);
            EXPECT_EQ(still.status, qb::spawn_status::spawned);
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ARefusalLostWithItsSpawnersCoreIsStillReadAsARefusal) {
    // A Spawner takes the request and its core admits nothing -- a service of that tag is there already. The refusal
    // waits behind a backlog, and the core stops on an error a pass behind the request: the refusal never leaves.
    // `unreachable` would say nobody took the request; the request itself says one did, and refused.
    qb::test::TestKit kit;
    const auto        sink    = kit.add<SlowSink>(0, qb::duration{150us});
    const auto        flooder = kit.add<Flooder>(1, sink);
    const auto        thrower = kit.add<Thrower>(1);
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<Registry>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [flooder, thrower](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto boom_a_pass_behind = [](qb::ScopedCoroContext c, qb::ActorId thrower) -> qb::io::async::task<bool> {
                co_await qb::detail::core_wait{qb::duration::zero()}; // the pass the request leaves in
                co_await qb::detail::core_wait{qb::duration::zero()}; // the one after
                c.push_to<Boom>(thrower);
                co_return true;
            };
            ctx.push_to<Go>(flooder, 6000);
            co_await ctx.sleep(50ms); // the flood fills this core's mailbox; with room left the refusal goes straight in
            auto [again, sent] = co_await qb::io::async::when_all(qb::spawn_on<Registry>(ctx, 1, 5s), boom_a_pass_behind(ctx, thrower));
            EXPECT_TRUE(sent);
            EXPECT_EQ(again.status, qb::spawn_status::refused) << "a request a Spawner took and refused was read as taken by nobody";
            EXPECT_FALSE(again.id.is_valid());
            co_await until_it_is_destroyed(ctx, qb::Actor::getServiceId<qb::Spawner>(1)); // its core has stopped: the run's error is set
        },
        60s);
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out);
    EXPECT_TRUE(run.engine_error) << "a core stopped on an exception";
}

TEST(SpawnOn, ASpawnerWaitingForACoreThatStopsUnderItEnds) {
    // The other way a core stops: after the Spawner asked it. Core 2 is held when the Spawner is killed, and stops
    // on an error once the hold ends, in the batch that carried both -- it never reads the question. A core that
    // stops tells those that watch it: the Spawner ends.
    qb::test::TestKit kit;
    const auto        told    = kit.probe<Spawned>(0);
    const auto        staller = kit.add<Staller>(2);
    const auto        thrower = kit.add<Thrower>(2);
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<EarlyPool>(2, 1, told);
    const auto run = kit.run<qb::SpawnRequest>(
        [told, staller, thrower](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner = qb::Actor::getServiceId<qb::Spawner>(1);
            const auto pooled  = co_await qb::test::expect<Spawned>(ctx, told, 10s); // core 2 has been answered
            EXPECT_EQ(pooled ? pooled->status : qb::spawn_status::unreachable, qb::spawn_status::spawned);
            ctx.push_to<Stall>(staller, 200ms);
            ctx.push_to<Boom>(thrower); // behind the hold, in the one pipe to core 2
            ctx.push_to<qb::KillEvent>(spawner);
            co_await until_it_is_destroyed(ctx, spawner);
        },
        30s);
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out) << "the Spawner waited for a core that had stopped";
    EXPECT_TRUE(run.engine_error) << "a core stopped on an exception";
}

TEST(SpawnOn, AParkingCallerReadsAnAnswerThatLeftBeforeItsSpawnerEndedAtOnce) {
    // The same on a caller's core that parks, with the thread held from a timer due when the watch's first reading
    // is: a timer may fire while the core parks, behind that pass's receive -- a reading made there, and a verdict
    // at the head of the next pass, would have no receive between them. The readings are made at the head of a pass.
    qb::test::TestKit kit;
    const auto        stopper = kit.add<EndAfterPasses>(1, qb::Actor::getServiceId<qb::Spawner>(1), SIGINT);
    (void) kit.add<qb::Spawner>(1);
    kit.engine().core(0).setLatency(1s);
    const auto run = kit.run<qb::SpawnRequest>(
        [stopper](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto hold_from_a_timer = [](qb::ScopedCoroContext c) -> qb::io::async::task<bool> {
                co_await qb::detail::core_wait{qb::duration::zero()}; // the pass the request leaves in, and the watch starts
                co_await c.sleep(qb::detail::spawner_watch_period);   // due when the first reading is
                std::this_thread::sleep_for(400ms);                   // the Spawner answers (250 ms), and is ended
                co_return true;
            };
            auto [w, held] = co_await qb::io::async::when_all(qb::spawn_on<HelloOnceActive>(ctx, 1, 5s, stopper, qb::duration{250ms}),
                                                              hold_from_a_timer(ctx));
            EXPECT_TRUE(held);
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "the Spawner had answered: it was read as gone with no receive in between";
            EXPECT_TRUE(w.id.is_valid());
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ACallerWhoseLoopAHandlerRunsReadsAnAnswerThatLeftBeforeItsSpawnersCoreStopped) {
    // And under a pump: the caller's core is inside a handler that runs its loop for 700 ms -- asked to through a
    // third core, whose ring is read behind the Spawner's -- while the Spawner answers `spawned` and its core stops
    // on an error four passes later, the answer gone. A timer fires under that pump, inside the receive, after the
    // Spawner's ring was read: a reading made there would be followed by its verdict before the answer is read. (A
    // pump that ends before the actor's init does -- a host that stalls -- proves less, and cannot fail for it.)
    qb::test::TestKit kit;
    const auto        runner  = kit.add<LoopRunner>(0);
    const auto        thrower = kit.add<Thrower>(1);
    (void) kit.add<qb::Spawner>(1);
    const auto boomer = kit.add<EndAfterPasses>(1, thrower);
    const auto relay  = kit.add<Relay>(2);
    const auto run    = kit.run<qb::SpawnRequest>(
        [runner, relay, boomer](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            auto run_the_loop = [](qb::ScopedCoroContext c, qb::ActorId runner, qb::ActorId relay) -> qb::io::async::task<bool> {
                co_await qb::detail::core_wait{qb::duration::zero()}; // the pass the request leaves in
                c.push_to<RelayTheRun>(relay, runner, qb::duration{700ms});
                co_return true;
            };
            auto [w, sent] = co_await qb::io::async::when_all(qb::spawn_on<HelloOnceActive>(ctx, 1, 5s, boomer, qb::duration{150ms}),
                                                              run_the_loop(ctx, runner, relay));
            EXPECT_TRUE(sent);
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "the Spawner had answered: it was read as gone under a pump of the loop";
            EXPECT_TRUE(w.id.is_valid());
            co_await until_it_is_destroyed(ctx, qb::Actor::getServiceId<qb::Spawner>(1)); // its core has stopped: the run's error is set
        },
        30s);
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out);
    EXPECT_TRUE(run.engine_error) << "a core stopped on an exception";
}

TEST(SpawnOn, ASettlementNobodysWaitSentDoesNotAnswerARequest) {
    // A settlement is the Spawner's own wait telling it to look: one pushed by anybody else, for a request whose
    // wait still runs, would answer `not_ready` an actor that is about to be active. The gate opens behind it.
    qb::test::TestKit kit;
    const auto        probe  = kit.probe<Hello>(0);
    const auto        keeper = kit.add<GateKeeper>(1);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe, keeper](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto gate            = std::make_shared<std::atomic<bool>>(false);
            auto       forge_then_open = [](qb::ScopedCoroContext c, qb::ActorId probe, qb::ActorId keeper,
                                            std::shared_ptr<std::atomic<bool>> gate) -> qb::io::async::task<bool> {
                const bool exists = (co_await qb::test::expect<Hello>(c, probe, 10s)).has_value(); // the request waits at the Spawner
                c.push_to<qb::detail::SpawnSettled>(qb::Actor::getServiceId<qb::Spawner>(1), std::uint64_t{1}); // its first ticket
                c.push_to<OpenTheGate>(keeper, std::move(gate));
                co_return exists;
            };
            auto [w, existed] = co_await qb::io::async::when_all(qb::spawn_on<GatedHello>(ctx, 1, std::chrono::hours(1), probe, gate),
                                                                 forge_then_open(ctx, probe, keeper, gate));
            EXPECT_TRUE(existed);
            EXPECT_EQ(w.status, qb::spawn_status::spawned) << "a settlement the Spawner's wait did not send answered the request";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, AnActorThatEndsItselfBeforeItsAnswerIsWrittenIsNotSaidSpawned) {
    // The wait sees the actor active; the actor is gone by the first event it reads, which is ahead of the wait's
    // settlement. The answer says what the actor is when it is written: `not_ready`, with the id of an actor that
    // was. `spawned` would hand the caller an id nobody holds, as that of an actor to talk to.
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto w = co_await qb::spawn_on<EndsItselfAtOnce>(ctx, 1, 5s);
            EXPECT_EQ(w.status, qb::spawn_status::not_ready) << "said spawned of an actor that had ended";
            EXPECT_TRUE(w.id.is_valid());
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ASpawnerKilledThenEndedAtOnceInThatPassLeavesNoDeadLetter) {
    // A kill, and behind it in the same batch a terminal signal: the Spawner starts its ending, and is ended at once
    // in the pass of its kill. What the ending waits for must not be an event of the Spawner's own, read by the
    // next pass with nobody there -- a dead letter of the core, as at an engine stopped just after a kill.
    std::atomic<int>  letters{0};
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<Idle>(1); // core 1 runs on once its Spawner is gone
    kit.engine().core(1).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner = qb::Actor::getServiceId<qb::Spawner>(1);
            ctx.push_to<qb::KillEvent>(spawner);
            ctx.push_to<qb::SignalEvent>(spawner).signum = SIGINT;
            co_await until_it_is_destroyed(ctx, spawner);
            co_await ctx.sleep(50ms); // the pass after its end has run by now
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(letters.load(), 0) << "an event of the Spawner's own ending was left to its core";
}

TEST(SpawnOn, ARequestAnsweredByAKillIsNotAnsweredAgainByItsSettlement) {
    // One request, one answer. The settlement of a wait is read behind the kill that answered its request (the kill
    // comes from the Spawner's own core, in the pass the actor's init ends): the asker's handler is given one
    // `qb::SpawnRequest`, not two -- the second would be discarded by `resolve_ask`, and seen by nobody but it.
    auto              answers = std::make_shared<Answers>();
    auto              gate    = std::make_shared<std::atomic<bool>>(false);
    qb::test::TestKit kit;
    const auto        probe  = kit.probe<Hello>(0);
    const auto        keeper = kit.add<GateKeeper>(1);
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<CountingCaller>(2, answers, probe, gate);
    const auto run = kit.run(
        [probe, keeper, gate, answers](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner = qb::Actor::getServiceId<qb::Spawner>(1);
            EXPECT_TRUE((co_await qb::test::expect<Hello>(ctx, probe, 10s)).has_value()); // the request waits at the Spawner
            ctx.push_to<OpenTheGate>(keeper, gate, spawner);
            co_await until_it_is_destroyed(ctx, spawner);
            while (answers->status.load() < 0)
                co_await ctx.sleep(1ms);
            co_await ctx.sleep(50ms); // an answer too many has been handed over by now
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(answers->status.load(), static_cast<int>(qb::spawn_status::spawned));
    EXPECT_EQ(answers->given.load(), 1) << "the asker's handler was given more than one answer to one request";
}

TEST(SpawnOn, ARequestAnsweredByItsSettlementIsNotAnsweredAgainByAKill) {
    // The other order: the request is settled and answered, then the Spawner is killed. It answers at its kill what
    // still waits, and nothing that was answered.
    auto              answers = std::make_shared<Answers>();
    auto              gate    = std::make_shared<std::atomic<bool>>(true); // open: the init ends at its first reading
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<CountingCaller>(2, answers, probe, gate);
    const auto run = kit.run(
        [probe, answers](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto spawner = qb::Actor::getServiceId<qb::Spawner>(1);
            EXPECT_TRUE((co_await qb::test::expect<Hello>(ctx, probe, 10s)).has_value());
            while (answers->status.load() < 0)
                co_await ctx.sleep(1ms);
            ctx.push_to<qb::KillEvent>(spawner);
            co_await until_it_is_destroyed(ctx, spawner);
            co_await ctx.sleep(50ms); // an answer too many has been handed over by now
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
    EXPECT_EQ(answers->status.load(), static_cast<int>(qb::spawn_status::spawned));
    EXPECT_EQ(answers->given.load(), 1) << "the asker's handler was given more than one answer to one request";
}
