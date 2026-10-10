/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/death-watch.cpp
 * @brief `Actor::watch()`: one `qb::DownEvent` when a watched actor is really gone (Huly QB-51).
 *
 * Before 3.3 an actor could learn that another one had died only if the dying one said so (the
 * `Supervisor`'s cooperative `ChildDown`) or by a health check with a timeout. What this file pins:
 *
 *   - a same-core and a cross-core watch each deliver ONE `DownEvent{watched, killed}`, after the
 *     watched actor's destructor ran;
 *   - watching an id that holds no actor answers at once (`unknown`), on this core, on another,
 *     and on a core the engine does not run;
 *   - an `onInit()` that fails or throws is reported as `init_failed` / `init_threw`;
 *   - watching twice delivers once; `unwatch()` delivers nothing -- not even an answer already on
 *     its way when it is called, on the same core and across cores;
 *   - a watcher that dies first is never notified -- its watch is withdrawn, so the watched
 *     actor's core reports no dead letter when it later dies -- on the same core and across cores;
 *   - a watch reaches a target still in its `onInit()`, on another core, and is answered when that
 *     init fails; an answer reaching a watcher still in its own `onInit()` waits for its activation,
 *     and dies with it, unreported, when that init fails;
 *   - a recycled id can be watched again after its `DownEvent`; and unwatched, then watched again
 *     as its next holder's while the answer about its previous one is still on its way, it waits
 *     for the answer about the next one;
 *   - an id whose core has stopped answers `core_stopped`, and so does a core that ends on an
 *     exception, after its actors' destructors; a watch opened during a blocked exceptional
 *     destructor gets no early answer; every watch is answered while its target's core stops,
 *     whichever side of the stop it lands on;
 *   - the engine shuts down cleanly, without a dead letter, with watches in place across cores.
 *
 * Results are written on the cores' threads and read after `join()`, which orders them.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace death_watch_test {

using namespace std::chrono_literals;

/// The `ActorId` of service id `sid` on core `core` (the bit layout `ActorId(uint32_t)` reads).
qb::ActorId
id_on(qb::CoreId const core, qb::ServiceId const sid) {
    return qb::ActorId(static_cast<std::uint32_t>(core) << 16 | sid);
}

struct Seen {
    qb::ActorId    watched{};
    qb::DownReason reason                 = qb::DownReason::killed;
    bool           target_destroyed_first = false;
};

std::atomic<bool> g_target_destroyed{false};
std::vector<Seen> g_seen; // written by the watcher's core only
std::atomic<int>  g_downs{0};

void
reset() {
    g_target_destroyed = false;
    g_seen.clear();
    g_downs = 0;
}

struct Go : qb::Event {};

// A target whose destructor flags itself, so a watcher can tell it ran BEFORE the DownEvent.
class Target : public qb::Actor {
public:
    ~Target() override {
        g_target_destroyed = true;
    }
};

// Watches `target`, then kills it; records every DownEvent; leaves after the first.
class Watcher : public qb::Actor {
    const qb::ActorId _target;
    const int         _watch_times;

public:
    explicit Watcher(qb::ActorId target, int watch_times = 1)
        : _target(target)
        , _watch_times(watch_times) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        registerEvent<Go>(*this);
        for (int i = 0; i < _watch_times; ++i)
            watch(_target);
        push<qb::KillEvent>(_target);
        co_return true;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, g_target_destroyed.load()});
        ++g_downs;
        if (g_downs == 1)
            push<Go>(id()); // a few passes for a duplicate to show, then stop
    }
    void
    on(Go const &) {
        if (++_settle < 20) {
            push<Go>(id());
            return;
        }
        qb::Main::stop();
        kill();
    }

private:
    int _settle = 0;
};

TEST(DeathWatch, SameCoreDownArrivesOnceAfterTheDestructor) {
    reset();
    qb::Main   engine;
    const auto target = engine.addActor<Target>(0);
    engine.addActor<Watcher>(0, target);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    ASSERT_EQ(g_seen.size(), 1u);
    EXPECT_EQ(g_seen[0].watched, target);
    EXPECT_EQ(g_seen[0].reason, qb::DownReason::killed);
    EXPECT_TRUE(g_seen[0].target_destroyed_first) << "the DownEvent comes after the destructor";
}

TEST(DeathWatch, CrossCoreDownArrivesOnceAfterTheDestructor) {
    reset();
    qb::Main   engine;
    const auto target = engine.addActor<Target>(1);
    engine.addActor<Watcher>(0, target);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    ASSERT_EQ(g_seen.size(), 1u);
    EXPECT_EQ(g_seen[0].watched, target);
    EXPECT_EQ(g_seen[0].reason, qb::DownReason::killed);
    EXPECT_TRUE(g_seen[0].target_destroyed_first);
}

TEST(DeathWatch, WatchingTwiceDeliversOnce) {
    reset();
    qb::Main   engine;
    const auto target = engine.addActor<Target>(1);
    engine.addActor<Watcher>(0, target, /*watch_times*/ 3);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    EXPECT_EQ(g_seen.size(), 1u) << "three watches, one registration, one DownEvent";
}

// --- an id that holds no actor --------------------------------------------------------------------

class Prober : public qb::Actor {
    const std::vector<qb::ActorId> _ids;

public:
    explicit Prober(std::vector<qb::ActorId> ids)
        : _ids(std::move(ids)) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        for (auto const id : _ids)
            watch(id);
        co_return true;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, false});
        if (g_seen.size() == _ids.size()) {
            qb::Main::stop();
            kill();
        }
    }
};

class Idle : public qb::Actor {}; // keeps core 1 running

TEST(DeathWatch, AnIdWithNoActorAnswersUnknownAtOnce) {
    reset();
    qb::Main engine;
    engine.addActor<Idle>(1);
    const auto here    = id_on(0, 900);
    const auto there   = id_on(1, 900);
    const auto nowhere = id_on(7, 900); // core 7 is not one of the engine's
    engine.addActor<Prober>(0, std::vector<qb::ActorId>{here, there, nowhere});
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    ASSERT_EQ(g_seen.size(), 3u);
    for (auto const &s : g_seen)
        EXPECT_EQ(s.reason, qb::DownReason::unknown) << "watched " << s.watched;
}

// --- init failures --------------------------------------------------------------------------------

class FailsInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(1ms);
        co_return false;
    }
};

class ThrowsInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(1ms);
        throw std::runtime_error("init threw on purpose");
    }
};

class InitWatcher : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        watch(addRefActor<FailsInit>().id());
        watch(addRefActor<ThrowsInit>().id());
        co_return true;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, false});
        if (g_seen.size() == 2) {
            qb::Main::stop();
            kill();
        }
    }
};

TEST(DeathWatch, AnInitThatFailsOrThrowsIsReportedAsSuch) {
    reset();
    qb::Main engine;
    engine.addActor<InitWatcher>(0);
    engine.start();
    engine.join();
    ASSERT_EQ(g_seen.size(), 2u);
    std::vector<qb::DownReason> reasons{g_seen[0].reason, g_seen[1].reason};
    EXPECT_NE(std::find(reasons.begin(), reasons.end(), qb::DownReason::init_failed), reasons.end());
    EXPECT_NE(std::find(reasons.begin(), reasons.end(), qb::DownReason::init_threw), reasons.end());
}

// --- unwatch, and a watcher that dies first -------------------------------------------------------

qb::CoreStats g_target_core_stats{};
qb::CoreStats g_watcher_core_stats{};

// On the target's core: kills the target 50 passes after it is told -- long after a withdrawal sent
// in the same breath has arrived -- then reads its core's counters 20 passes later and stops.
class Executioner : public qb::Actor {
    const qb::ActorId _target;
    int               _pass = 0;

public:
    explicit Executioner(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Go>(*this);
        co_return true;
    }
    void
    on(Go const &) {
        ++_pass;
        if (_pass == 50)
            push<qb::KillEvent>(_target);
        if (_pass < 70) {
            push<Go>(id());
            return;
        }
        g_target_core_stats = getCoreStats();
        qb::Main::stop();
        kill();
    }
};

// Keeps the watcher's core alive and reads its counters at shutdown: a DownEvent sent to a dead
// watcher would be a dead letter HERE.
class CoreWitness : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SignalEvent>(*this);
        co_return true;
    }
    void
    on(qb::SignalEvent const &) {
        g_watcher_core_stats = getCoreStats();
        kill();
    }
};

// Watches the target, then either unwatches it or dies, then starts the executioner.
class Quitter : public qb::Actor {
    const qb::ActorId _target;
    const qb::ActorId _executioner;
    const bool        _die;

public:
    Quitter(qb::ActorId target, qb::ActorId executioner, bool die)
        : _target(target)
        , _executioner(executioner)
        , _die(die) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        watch(_target);
        if (_die) {
            push<Go>(_executioner);
            kill();
        } else {
            unwatch(_target);
            push<Go>(_executioner);
        }
        co_return true;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, false});
    }
};

void
run_quitter(qb::CoreId const watcher_core, bool const die) {
    reset();
    g_target_core_stats  = {};
    g_watcher_core_stats = {};
    qb::Main   engine;
    const auto target      = engine.addActor<Target>(1);
    const auto executioner = engine.addActor<Executioner>(1, target);
    engine.addActor<Quitter>(watcher_core, target, executioner, die);
    if (watcher_core != 1)
        engine.addActor<CoreWitness>(watcher_core);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    EXPECT_TRUE(g_target_destroyed.load()) << "the target did die";
    EXPECT_TRUE(g_seen.empty()) << "no DownEvent after unwatch";
    EXPECT_EQ(g_target_core_stats.dead_letters, 0u) << "nothing sent to a dead watcher on the target's core";
    EXPECT_EQ(g_watcher_core_stats.dead_letters, 0u) << "nothing sent to a dead watcher on its own core";
}

TEST(DeathWatch, UnwatchSameCoreDeliversNothing) {
    run_quitter(1, false);
}
TEST(DeathWatch, UnwatchCrossCoreDeliversNothing) {
    run_quitter(0, false);
}
TEST(DeathWatch, AWatcherThatDiesFirstSameCoreIsNeverNotified) {
    run_quitter(1, true);
}
TEST(DeathWatch, AWatcherThatDiesFirstCrossCoreIsNeverNotified) {
    run_quitter(0, true);
}

struct Die : qb::Event {};

// Dies on `Die`.
class Dier : public Target {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Die>(*this);
        co_return true;
    }
    void
    on(Die const &) {
        kill();
    }
};

// Dies on `Die`, telling its sender first: that `Go` precedes the answer to the sender's watch,
// which leaves only once the target is reaped, at the end of the pass.
class TellingTarget : public Target {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Die>(*this);
        co_return true;
    }
    void
    on(Die const &e) {
        push<Go>(e.getSource());
        kill();
    }
};

// Watches the target and has it die; unwatches on the target's `Go`, with the answer behind it.
class LateQuitter : public qb::Actor {
    const qb::ActorId _target;
    bool              _unwatched = false;
    int               _settle    = 0;

public:
    explicit LateQuitter(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        registerEvent<Go>(*this);
        watch(_target);
        push<Die>(_target);
        co_return true;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, false});
    }
    void
    on(Go const &) {
        if (!_unwatched) {
            _unwatched = true;
            unwatch(_target);
        }
        if (++_settle < 20) {
            push<Go>(id());
            return;
        }
        qb::Main::stop();
        kill();
    }
};

void
run_late_quitter(qb::CoreId const watcher_core) {
    reset();
    std::atomic<int> letters{0};
    qb::Main         engine;
    for (qb::CoreId const core : {watcher_core, qb::CoreId{1}})
        engine.core(core).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    const auto target = engine.addActor<TellingTarget>(1);
    engine.addActor<LateQuitter>(watcher_core, target);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    EXPECT_TRUE(g_target_destroyed.load()) << "the target did die";
    EXPECT_TRUE(g_seen.empty()) << "the answer, on its way when unwatch() was called, found the watch closed";
    EXPECT_EQ(letters.load(), 0) << "and was no letter";
}

TEST(DeathWatch, UnwatchDropsAnAnswerAlreadyOnItsWaySameCore) {
    run_late_quitter(1);
}
TEST(DeathWatch, UnwatchDropsAnAnswerAlreadyOnItsWayCrossCore) {
    run_late_quitter(0);
}

// Watches a child that tells it before dying; on that word -- the answer to its watch behind it --
// unwatches it, spawns a successor, which gets the reused id, and watches that one. The answer about
// the first must not close the watch of the second: the only DownEvent comes once the second dies.
std::atomic<bool> g_id_reused{false};

class Successor : public qb::Actor {
    qb::ActorId _first{};
    qb::ActorId _second{};
    int         _settle = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        registerEvent<Go>(*this);
        _first = addRefActor<TellingTarget>().id();
        watch(_first);
        push<Die>(_first);
        co_return true;
    }
    void
    on(Go const &e) {
        if (!_second.is_valid() && e.getSource() == _first) {
            unwatch(_first);
            _second     = addRefActor<Dier>().id();
            g_id_reused = _second == _first;
            watch(_second);
            push<Go>(id());
            return;
        }
        if (++_settle < 20)
            push<Go>(id()); // passes enough for the first's answer to arrive, and be dropped
        else if (_settle == 20)
            push<Die>(_second);
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, _settle >= 20});
        qb::Main::stop();
        kill();
    }
};

TEST(DeathWatch, AWatchOfAReusedIdWaitsForItsNewHolder) {
    reset();
    g_id_reused = false;
    qb::Main engine;
    engine.addActor<Successor>(0);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    ASSERT_TRUE(g_id_reused.load()) << "the case needs the successor to reuse the id";
    ASSERT_EQ(g_seen.size(), 1u);
    EXPECT_TRUE(g_seen[0].target_destroyed_first) << "the answer is the second's, sent once it was told to die";
    EXPECT_EQ(g_seen[0].reason, qb::DownReason::killed);
}

// --- activation, on either side --------------------------------------------------------------------

class SlowFailsInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(50ms); // still activating when the watch arrives
        co_return false;
    }
};

// Watches `target`, with a deadline: an unanswered watch fails the test instead of hanging it.
class PatientWatcher : public qb::Actor {
    const qb::ActorId _target;

public:
    explicit PatientWatcher(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        registerEvent<Go>(*this);
        watch(_target);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(5s);
            ctx.push<Go>();
        });
        co_return true;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, false});
        qb::Main::stop();
        kill();
    }
    void
    on(Go const &) { // the deadline: no answer
        qb::Main::stop();
        kill();
    }
};

TEST(DeathWatch, AWatchReachesATargetStillInItsInitAcrossCores) {
    reset();
    qb::Main   engine;
    const auto target = engine.addActor<SlowFailsInit>(1);
    engine.addActor<PatientWatcher>(0, target);
    engine.start();
    engine.join();
    ASSERT_EQ(g_seen.size(), 1u) << "the watch reached the activating target, and its failure answered it";
    EXPECT_EQ(g_seen[0].watched, target);
    EXPECT_EQ(g_seen[0].reason, qb::DownReason::init_failed);
}

std::atomic<bool> g_active_when_down{false};

// Watches a target that dies at once, then stays in its own onInit(): the answer arrives while it
// is activating, waits in its stash, and is delivered once it is active.
class SlowWatcher : public qb::Actor {
    const qb::ActorId _target;
    bool              _active = false;

public:
    explicit SlowWatcher(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        watch(_target);
        push<Die>(_target);
        co_await context().sleep(20ms);
        _active = true;
        co_return true;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, g_target_destroyed.load()});
        g_active_when_down = _active;
        qb::Main::stop();
        kill();
    }
};

TEST(DeathWatch, AnAnswerToAnActivatingWatcherWaitsForItsActivation) {
    reset();
    g_active_when_down = false;
    qb::Main   engine;
    const auto target = engine.addActor<Dier>(0);
    engine.addActor<SlowWatcher>(0, target);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    ASSERT_EQ(g_seen.size(), 1u);
    EXPECT_EQ(g_seen[0].reason, qb::DownReason::killed);
    EXPECT_TRUE(g_seen[0].target_destroyed_first);
    EXPECT_TRUE(g_active_when_down.load()) << "delivered once the watcher is active, not before";
}

// --- a recycled id, and a stopped core ------------------------------------------------------------

// Spawns a child, watches it, kills it; on its DownEvent spawns another (the pool hands the same id
// back) and watches again: the second watch must reach the second actor.
class Recycler : public qb::Actor {
    int _round = 0;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        next();
        co_return true;
    }
    void
    next() {
        const auto child = addRefActor<Target>().id();
        watch(child);
        push<qb::KillEvent>(child);
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, false});
        if (++_round < 3) {
            next();
            return;
        }
        qb::Main::stop();
        kill();
    }
};

TEST(DeathWatch, ARecycledIdCanBeWatchedAgain) {
    reset();
    qb::Main engine;
    engine.addActor<Recycler>(0);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    ASSERT_EQ(g_seen.size(), 3u);
    for (auto const &s : g_seen)
        EXPECT_EQ(s.reason, qb::DownReason::killed);
}

class Ephemeral : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        kill(); // core 1's only actor: the core stops once it is reaped
        co_return true;
    }
};

// Watches the dead id until the answer is `core_stopped`: first `unknown` (or `killed`, if the
// watch beat the reap), then -- once core 1 has left its loop -- `core_stopped`, answered here.
class StoppedCoreProber : public qb::Actor {
    const qb::ActorId _gone;

public:
    explicit StoppedCoreProber(qb::ActorId gone)
        : _gone(gone) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        registerEvent<Go>(*this);
        watch(_gone);
        co_return true;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, false});
        if (e.reason == qb::DownReason::core_stopped || g_seen.size() > 2000) {
            qb::Main::stop();
            kill();
            return;
        }
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(1ms);
            ctx.push<Go>();
        });
    }
    void
    on(Go const &) {
        watch(_gone);
    }
};

TEST(DeathWatch, AnIdOnAStoppedCoreAnswersCoreStopped) {
    reset();
    qb::Main   engine;
    const auto gone = engine.addActor<Ephemeral>(1);
    engine.addActor<StoppedCoreProber>(0, gone);
    engine.start();
    engine.join();
    ASSERT_FALSE(engine.hasError());
    ASSERT_FALSE(g_seen.empty());
    EXPECT_EQ(g_seen.back().reason, qb::DownReason::core_stopped);
    for (std::size_t i = 0; i + 1 < g_seen.size(); ++i)
        EXPECT_TRUE(g_seen[i].reason == qb::DownReason::unknown || g_seen[i].reason == qb::DownReason::killed);
}

// Like SlowWatcher, but its init fails: the answer waits in its stash, and is dropped with it -- the
// watch dies with its watcher, so no DownEvent, and no letter for the drop.
class FailingWatcher : public qb::Actor {
    const qb::ActorId _target;

public:
    explicit FailingWatcher(qb::ActorId target)
        : _target(target) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        watch(_target);
        push<Die>(_target);
        co_await context().sleep(20ms);
        co_return false;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, false});
    }
};

TEST(DeathWatch, AnAnswerToAWatcherWhoseInitFailsDiesWithIt) {
    reset();
    std::atomic<int> letters{0};
    qb::Main         engine;
    engine.core(0).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    const auto target = engine.addActor<Dier>(0);
    engine.addActor<FailingWatcher>(0, target);
    engine.start();
    engine.join();
    EXPECT_TRUE(g_target_destroyed.load()) << "the target did die";
    EXPECT_TRUE(g_seen.empty());
    EXPECT_EQ(letters.load(), 0) << "the stashed answer is the death watch's own: no letter when the stash is dropped";
}

// --- a core that ends on an exception --------------------------------------------------------------

struct Boom : qb::Event {};

// Throws from its per-pass callback once told to: the exception unwinds its core's thread, which
// takes it along. A throwing routed event handler also stops its VirtualCore; a throw from
// event construction inside noexcept push/send still terminates the process.
// Its destructor takes its time, so an answer sent before it ran would be seen to be.
class Bomb
    : public Target
    , public qb::ICallback {
public:
    ~Bomb() override {
        std::this_thread::sleep_for(20ms);
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Boom>(*this);
        co_return true;
    }
    void
    on(Boom const &) {
        registerCallback(*this);
    }
    void
    on(qb::LoopEvent const &) override {
        throw std::runtime_error("boom, on purpose");
    }
};

// Watches the bomb from another core and sets it off, with a deadline.
class BombWatcher : public qb::Actor {
    const qb::ActorId _bomb;

public:
    explicit BombWatcher(qb::ActorId bomb)
        : _bomb(bomb) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        registerEvent<Go>(*this);
        watch(_bomb);
        push<Boom>(_bomb);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(5s);
            ctx.push<Go>();
        });
        co_return true;
    }
    void
    on(qb::DownEvent const &e) {
        g_seen.push_back({e.watched, e.reason, g_target_destroyed.load()});
        qb::Main::stop();
        kill();
    }
    void
    on(Go const &) { // the deadline: no answer
        qb::Main::stop();
        kill();
    }
};

TEST(DeathWatch, ACoreThatEndsOnAnExceptionAnswersCoreStopped) {
    reset();
    qb::Main   engine;
    const auto bomb = engine.addActor<Bomb>(1);
    engine.addActor<BombWatcher>(0, bomb);
    engine.start();
    engine.join();
    EXPECT_TRUE(engine.hasError()) << "core 1 ended on the exception";
    ASSERT_EQ(g_seen.size(), 1u) << "the watch was answered all the same";
    EXPECT_EQ(g_seen[0].watched, bomb);
    EXPECT_EQ(g_seen[0].reason, qb::DownReason::core_stopped);
    EXPECT_TRUE(g_seen[0].target_destroyed_first) << "after the destructor, as for any end";
}

// A watch opened while the target's exceptional teardown is inside its destructor must not
// observe core_stopped until that destructor completes. Each stage is acknowledged by the
// owning thread; timeouts below are only deadlock guards, never the ordering oracle.
struct LateWatchGate {
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    destructor_entered = false;
    bool                    release_destructor = false;
    bool                    pass_after_watch   = false;
    bool                    settled_after_down = false;
    int                     downs              = 0;
    qb::ActorId             watched{};
    qb::DownReason          reason                 = qb::DownReason::killed;
    bool                    target_destroyed_first = false;
};

class GatedBomb
    : public Target
    , public qb::ICallback {
    LateWatchGate *_gate;

public:
    explicit GatedBomb(LateWatchGate *gate)
        : _gate(gate) {}

    ~GatedBomb() override {
        std::unique_lock lock(_gate->mutex);
        _gate->destructor_entered = true;
        _gate->cv.notify_all();
        _gate->cv.wait(lock, [this] { return _gate->release_destructor; });
        // Target::~Target() sets g_target_destroyed only after this returns.
    }

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Boom>(*this);
        co_return true;
    }
    void
    on(Boom const &) {
        registerCallback(*this);
    }
    void
    on(qb::LoopEvent const &) override {
        throw std::runtime_error("boom, on purpose");
    }
};

class LateWatchProbe
    : public qb::Actor
    , public qb::ICallback {
    qb::ActorId    _target;
    LateWatchGate *_gate;
    bool           _watch_opened      = false;
    bool           _pass_seen         = false;
    int            _passes_after_down = 0;

public:
    LateWatchProbe(qb::ActorId target, LateWatchGate *gate)
        : _target(target)
        , _gate(gate) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        registerCallback(*this);
        push<Boom>(_target);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (!_watch_opened) {
            {
                std::lock_guard lock(_gate->mutex);
                if (!_gate->destructor_entered)
                    return;
            }
            watch(_target);
            _watch_opened = true;
            return;
        }
        if (!_pass_seen) {
            _pass_seen = true;
            {
                std::lock_guard lock(_gate->mutex);
                _gate->pass_after_watch = true;
            }
            _gate->cv.notify_all();
        }
        // Reception runs before callbacks on each pass. Two more passes after the first
        // answer let any duplicate answer already in the self pipe surface before join.
        bool has_down = false;
        {
            std::lock_guard lock(_gate->mutex);
            has_down = _gate->downs != 0;
        }
        if (has_down && ++_passes_after_down == 2) {
            {
                std::lock_guard lock(_gate->mutex);
                _gate->settled_after_down = true;
            }
            _gate->cv.notify_all();
        }
    }
    void
    on(qb::DownEvent const &event) {
        {
            std::lock_guard lock(_gate->mutex);
            ++_gate->downs;
            _gate->watched                = event.watched;
            _gate->reason                 = event.reason;
            _gate->target_destroyed_first = g_target_destroyed.load();
        }
        _gate->cv.notify_all();
    }
};

TEST(DeathWatch, AWatchOpenedDuringExceptionalDestructorWaitsForIt) {
    reset();
    LateWatchGate gate;
    qb::Main      engine;
    const auto    target = engine.addActor<GatedBomb>(1, &gate);
    engine.addActor<LateWatchProbe>(0, target, &gate);
    engine.start();

    // The second observer tick follows one complete receive phase after watch(). On the old
    // ordering, __watch__ saw stopped and queued a local WatchDown, delivered in that phase.
    bool ready                    = false;
    bool early_down               = false;
    bool destroyed_before_release = false;
    {
        std::unique_lock lock(gate.mutex);
        ready                    = gate.cv.wait_for(lock, 10s, [&] { return gate.pass_after_watch; });
        early_down               = gate.downs != 0;
        destroyed_before_release = g_target_destroyed.load();
        gate.release_destructor  = true; // also release on timeout, before stop/join
    }
    gate.cv.notify_all();

    bool settled = false;
    if (ready && !early_down) {
        std::unique_lock lock(gate.mutex);
        settled = gate.cv.wait_for(lock, 10s, [&] { return gate.settled_after_down; });
    }
    qb::Main::stop();
    engine.join();

    ASSERT_TRUE(ready) << "the watcher did not finish a pass after opening the watch";
    EXPECT_FALSE(early_down) << "DownEvent arrived while the target destructor was blocked";
    EXPECT_FALSE(destroyed_before_release) << "the target destructor passed its barrier before release";
    if (!early_down)
        EXPECT_TRUE(settled) << "the watch was not answered after destructor release";
    EXPECT_TRUE(engine.hasError()) << "the target core ended on its callback exception";
    EXPECT_TRUE(g_target_destroyed.load());
    std::lock_guard lock(gate.mutex);
    EXPECT_EQ(gate.downs, 1);
    EXPECT_EQ(gate.watched, target);
    EXPECT_EQ(gate.reason, qb::DownReason::core_stopped);
    EXPECT_TRUE(gate.target_destroyed_first);
}

// Core 1's only actor dies in its onInit, so core 1 stops at once. Round after round, the watch from
// core 0 lands on every side of that stop: registered before the reap (`killed`), answered by core 1
// after it (`unknown`), or seen stopped here or announced by core 1 (`core_stopped`) -- any of them
// right, no answer the defect.
TEST(DeathWatch, EveryWatchIsAnsweredWhileItsTargetCoreStops) {
    int by_reason[qb::DownReasons] = {};
    for (int round = 0; round < 200; ++round) {
        reset();
        qb::Main   engine;
        const auto gone = engine.addActor<Ephemeral>(1);
        engine.addActor<PatientWatcher>(0, gone);
        engine.start();
        engine.join();
        ASSERT_EQ(g_seen.size(), 1u) << "round " << round << ": the watch went unanswered";
        const auto reason = g_seen[0].reason;
        ASSERT_TRUE(reason == qb::DownReason::killed || reason == qb::DownReason::unknown || reason == qb::DownReason::core_stopped)
            << "round " << round << ": " << qb::down_reason_name(reason);
        ++by_reason[static_cast<std::size_t>(reason)];
    }
    std::printf("answers over 200 rounds: killed %d, unknown %d, core_stopped %d\n",
                by_reason[static_cast<std::size_t>(qb::DownReason::killed)], by_reason[static_cast<std::size_t>(qb::DownReason::unknown)],
                by_reason[static_cast<std::size_t>(qb::DownReason::core_stopped)]);
}

// --- shutdown with watches in place ---------------------------------------------------------------

std::atomic<int> g_mutual_up{0};

class Mutual : public qb::Actor {
    std::vector<qb::ActorId> _peers;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::DownEvent>(*this);
        ++g_mutual_up;
        co_return true;
    }
    void
    watch_all(std::vector<qb::ActorId> const &peers) {
        for (auto const p : peers)
            if (p != id())
                watch(p);
    }
    void
    on(qb::DownEvent const &) {}
};

struct Wire : qb::Event {
    std::vector<qb::ActorId> peers;
    explicit Wire(std::vector<qb::ActorId> p)
        : peers(std::move(p)) {}
};

class WiredMutual : public Mutual {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Wire>(*this);
        co_return co_await Mutual::onInit();
    }
    void
    on(Wire const &w) {
        watch_all(w.peers);
    }
};

class Conductor : public qb::Actor {
    const std::vector<qb::ActorId> _all;

public:
    explicit Conductor(std::vector<qb::ActorId> all)
        : _all(std::move(all)) {}
    qb::io::async::task<bool>
    onInit() override {
        for (auto const a : _all)
            push<Wire>(a, _all);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(20ms); // the watches are registered on every core by then
            (void) ctx;
            qb::Main::stop();
        });
        co_return true;
    }
};

TEST(DeathWatch, TheEngineShutsDownCleanlyWithWatchesInPlace) {
    g_mutual_up = 0;
    std::atomic<int>         letters{0};
    qb::Main                 engine;
    std::vector<qb::ActorId> all;
    for (qb::CoreId core = 0; core < 2; ++core)
        engine.core(core).setDeadLetterHandler([&letters](qb::DeadLetter const &) { ++letters; });
    for (qb::CoreId c = 0; c < 2; ++c)
        for (int i = 0; i < 4; ++i)
            all.push_back(engine.addActor<WiredMutual>(c));
    engine.addActor<Conductor>(0, all);
    engine.start();
    engine.join();
    EXPECT_FALSE(engine.hasError());
    EXPECT_EQ(g_mutual_up.load(), 8);
    EXPECT_EQ(letters.load(), 0) << "the death watch's own traffic is never a letter, and every DownEvent is handled";
}

} // namespace death_watch_test
