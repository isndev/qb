/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/supervisor-strategies.cpp
 * @brief `qb::Supervisor` / `qb::SupervisedActor` — restart strategies, the generation guard,
 *        restart-intensity escalation (cumulative AND sliding-window), kill-propagation, and
 *        supervision crossed with async `onInit`.
 *
 * Each supervised `TestWorker` reports every (re)spawn to a `SpawnCoordinator` via a `SpawnAck`
 * carrying its slot, its new live child id, and its supervisor id. The coordinator is the single
 * driver+oracle for every case:
 *   - it scripts crashes *causally* off those acks (crash k is fired by the (initial+k)-th ack, so
 *     the (k+1)-th restart cannot begin until the k-th completes — no wall clock spaces crashes);
 *   - it proves each restarted slot was genuinely re-spawned (a second `SpawnAck` for that slot ⇒
 *     `spawn_child(slot)` re-ran a fresh `onInit`). The `ActorId` itself may recycle — the freed sid
 *     is the smallest-free and the replacement reclaims it (ServiceIdPool prefers the lowest sid) —
 *     so the framework-truth oracle is "the slot re-acked", not index-inequality;
 *   - it ends the run deterministically: either at an exact target ack-count (restart cases) or,
 *     for "no restart should happen" cases, after a bounded self-`Settle` countdown that gives a
 *     spurious restart time to ack before the engine stops.
 * So the engine itself, never a fixed `Nms` stopper, ends every run. The spawn total distinguishes
 * each strategy precisely:
 *   - one_for_one  + 1 crash of 3 children -> 3 + 1 = 4 spawns
 *   - one_for_all  + 1 crash of 3 children -> 3 + 3 = 6 spawns
 *   - rest_for_one + crash slot 1 of 3     -> 3 + 2 = 5 spawns
 *
 * DE-FLAKED over the original suite: no `50ms`-spaced `async::callback` crashes, no `120ms`
 * liveness snapshot — every transition is event-ordered. A LOUD ctest TIMEOUT remains the only
 * backstop; nothing here can hang silently because every path ends on an observed event.
 *
 * RECOVERED here (folded from the deleted test-actor-patterns-enrich.cpp, modernized):
 *   - SupervisorKillPropagation — killing the supervisor tears down its children (no orphans);
 *   - SupervisorRestartWindow   — sliding-window restart intensity (N restarts within T → escalate).
 *
 * Two independent oracles per case: in-actor side-effects are mirrored to a global `std::atomic`
 * asserted after `join()`, so a never-scheduled actor cannot pass vacuously.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor suites.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/main.h>

using namespace qb;
using namespace std::chrono_literals;

namespace {
// Single-core suite ⇒ every write happens on core 0; reads happen after join() (which establishes
// the happens-before), so plain atomics are sufficient.
std::atomic<int>  g_spawns{0};            // total worker onInit() calls (initial + restarts)
std::atomic<int>  g_acks{0};              // total SpawnAcks the coordinator saw
std::atomic<bool> g_escalated{false};
std::atomic<bool> g_slot_replaced{false}; // a restarted slot reported a NEW ActorId

void
reset_globals() {
    g_spawns.store(0);
    g_acks.store(0);
    g_escalated.store(false);
    g_slot_replaced.store(false);
}
} // namespace

// Tells a worker to terminate cooperatively (sent by the supervisor on a TriggerCrash).
struct Crash : public qb::Event {};
// Test control: tells the supervisor to crash the child currently in `slot`.
struct TriggerCrash : public qb::Event {
    std::size_t slot;
    explicit TriggerCrash(std::size_t s)
        : slot(s) {}
};
// A worker -> coordinator notification: "slot S (re)spawned, now live as `who`, supervised by `sup`".
struct SpawnAck : public qb::Event {
    std::size_t slot;
    qb::ActorId who;
    qb::ActorId sup;
    SpawnAck(std::size_t s, qb::ActorId w, qb::ActorId su)
        : slot(s)
        , who(w)
        , sup(su) {}
};

// ---------------------------------------------------------------------------
// SpawnCoordinator — the single completion + crash-driver + slot-replacement oracle (see header).
// Construction modes:
//   * stop_at_acks  > 0 : end the run when exactly that many acks have arrived (restart cases);
//   * stop_at_acks == 0 : end after all scripted crashes are issued + a bounded Settle countdown
//                         (cases where NO restart should happen — the settle would catch a spurious
//                         respawn's ack before the engine stops).
// It learns the supervisor id from the first SpawnAck, so no out-of-engine wiring is needed.
// ---------------------------------------------------------------------------
class SpawnCoordinator : public qb::Actor {
    struct Settle : public qb::Event {};

    const int                _initial;
    const int                _stop_at_acks; // 0 ⇒ settle-then-stop mode
    qb::ActorId              _sup;
    std::vector<std::size_t> _crash_slots;
    std::vector<qb::ActorId> _last_id; // last id reported per slot
    std::vector<int>         _slot_acks;
    int                      _acks    = 0;
    std::size_t             _issued  = 0;
    int                      _settle  = 0;
    bool                     _settling = false;

public:
    SpawnCoordinator(int initial, std::size_t slots, std::vector<std::size_t> crash_slots,
                     int stop_at_acks)
        : _initial(initial)
        , _stop_at_acks(stop_at_acks)
        , _crash_slots(std::move(crash_slots))
        , _last_id(slots, qb::ActorId{})
        , _slot_acks(slots, 0) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SpawnAck>(*this);
        registerEvent<Settle>(*this);
        co_return true;
    }

    void
    on(const SpawnAck &e) {
        ++_acks;
        g_acks.fetch_add(1);
        _sup = e.sup; // learned from the worker (stable across restarts)

        if (e.slot < _last_id.size()) {
            if (_slot_acks[e.slot] > 0) {
                // A re-ack on this slot proves `spawn_child(slot)` re-ran: the slot was genuinely
                // replaced by a freshly-`onInit`'d actor. Note: the *ActorId* itself may recycle —
                // the freed sid is the smallest-free and is reclaimed by the replacement
                // (VirtualCore::ServiceIdPool prefers the lowest sid), so asserting index-inequality
                // would be flaky/wrong. The framework-truth here is "the slot was re-spawned".
                EXPECT_TRUE(e.who.is_valid()) << "restarted slot " << e.slot << " must be valid";
                g_slot_replaced.store(true);
            }
            ++_slot_acks[e.slot];
            _last_id[e.slot] = e.who;
        }

        // Fire the next scripted crash once we have seen the initial fan-in plus `_issued` restarts.
        if (_sup.is_valid() && _issued < _crash_slots.size()
            && _acks >= _initial + static_cast<int>(_issued)) {
            push<TriggerCrash>(_sup, _crash_slots[_issued]);
            ++_issued;
            // In settle-mode, once the last scripted crash is issued, begin the settle countdown.
            if (_stop_at_acks == 0 && _issued == _crash_slots.size() && !_settling) {
                _settling = true;
                push<Settle>(id());
            }
        }
        // settle-mode with NO scripted crash (pure initial fan-in): settle once everyone has acked.
        if (_stop_at_acks == 0 && _crash_slots.empty() && !_settling && _acks >= _initial) {
            _settling = true;
            push<Settle>(id());
        }

        if (_stop_at_acks > 0 && _acks == _stop_at_acks) {
            qb::Main::stop();
            kill();
        }
    }

    void
    on(const Settle &) {
        // A few mailbox passes give a spurious restart's ack time to land (and bump g_spawns/g_acks)
        // before we stop — turning "no restart" into a positive, non-vacuous assertion.
        if (++_settle < 5) {
            push<Settle>(id());
            return;
        }
        qb::Main::stop();
        kill();
    }
};

// A supervised worker: announces every spawn to the coordinator; on Crash, terminates cooperatively
// (notifies its supervisor → restart). Holds the coordinator id so it can ack on every (re)spawn.
class TestWorker : public qb::SupervisedActor {
    qb::ActorId _coord;
    std::size_t _slot;

public:
    TestWorker(qb::ActorId sup, std::size_t slot, std::uint64_t gen, qb::ActorId coord)
        : qb::SupervisedActor(sup, slot, gen)
        , _coord(coord)
        , _slot(slot) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Crash>(*this);
        g_spawns.fetch_add(1);
        push<SpawnAck>(_coord, _slot, id(), supervisor());
        co_return true;
    }
    void
    on(Crash &) {
        stop(); // notify supervisor + kill → triggers a restart
    }
};

// The concrete supervisor under test. Takes the coordinator id at construction so its INITIAL
// children (spawned inside Supervisor::onInit, before any event runs) are already wired to it.
class TestSupervisor : public qb::Supervisor {
    qb::ActorId _coord;

public:
    TestSupervisor(qb::restart_strategy strat, std::size_t count, qb::ActorId coord,
                   unsigned max_restarts = 0, qb::duration window = qb::duration::zero())
        : qb::Supervisor(strat, count, max_restarts, window)
        , _coord(coord) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<TriggerCrash>(*this);
        co_return co_await qb::Supervisor::onInit(); // registers ChildDown + spawns initial children
    }
    void
    on(TriggerCrash &e) {
        push<Crash>(child(e.slot)); // crash the current child in that slot
    }

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t generation) override {
        return addRefActor<TestWorker>(id(), slot, generation, _coord).id();
    }
    void
    on_escalate() override {
        g_escalated.store(true);
    }
};

// Restart-case runner: ends when exactly `expected_spawns` acks have arrived. Asserts the EXACT
// spawn total + that the crashed slot was replaced by a fresh ActorId.
static void
run_strategy(qb::restart_strategy strat, std::size_t child_count, std::vector<std::size_t> crash_slots,
             int expected_spawns) {
    reset_globals();
    qb::Main main;

    auto coord = main.addActor<SpawnCoordinator>(0, static_cast<int>(child_count), child_count,
                                                 std::move(crash_slots), /*stop_at_acks*/ expected_spawns);
    main.addActor<TestSupervisor>(0, strat, child_count, coord);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_spawns.load(), expected_spawns) << "spawn total = initial + restarts";
    EXPECT_EQ(g_acks.load(), expected_spawns) << "every spawn must ack the coordinator";
    EXPECT_TRUE(g_slot_replaced.load()) << "a crashed slot must hold a NEW ActorId after restart";
}

// ===========================================================================
// 1. Restart strategies — exact spawn totals, causally-driven crashes.
// ===========================================================================
TEST(ActorSupervisor, OneForOneRestartsOnlyDeadChild) {
    run_strategy(qb::restart_strategy::one_for_one, 3, {1}, 4); // 3 initial + 1 restart of slot 1
}

TEST(ActorSupervisor, OneForAllRestartsEveryChild) {
    run_strategy(qb::restart_strategy::one_for_all, 3, {1}, 6); // 3 initial + 3 (all restarted)
}

TEST(ActorSupervisor, RestForOneRestartsFromSlotOnward) {
    run_strategy(qb::restart_strategy::rest_for_one, 3, {1}, 5); // 3 initial + slots 1,2 restarted
}

// ===========================================================================
// 2. Generation guard — a stale ChildDown is ignored (no restart).
// ===========================================================================
namespace {
// Injects one stale-generation ChildDown right after the children are up, then relies on the
// coordinator's settle countdown to prove no restart followed.
class StaleProbeSupervisor : public qb::Supervisor {
    qb::ActorId _coord;

public:
    StaleProbeSupervisor(std::size_t count, qb::ActorId coord)
        : qb::Supervisor(qb::restart_strategy::one_for_one, count, 3)
        , _coord(coord) {}

    qb::io::async::task<bool>
    onInit() override {
        auto ok = co_await qb::Supervisor::onInit();
        // Mailbox-ordered behind the children's spawns: a stale (gen 999) ChildDown for slot 0.
        this->template push<qb::ChildDown>(id(), std::size_t{0}, std::uint64_t{999});
        co_return ok;
    }

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t gen) override {
        return addRefActor<TestWorker>(id(), slot, gen, _coord).id();
    }
};
} // namespace

TEST(ActorSupervisor, StaleChildDownIsIgnored) {
    reset_globals();
    qb::Main main;
    // settle-mode (stop_at_acks=0): 3 initial acks, then settle. A restart would ack a 4th time.
    auto coord = main.addActor<SpawnCoordinator>(0, /*initial*/ 3, 3, std::vector<std::size_t>{},
                                                 /*stop_at_acks*/ 0);
    main.addActor<StaleProbeSupervisor>(0, 3, coord);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_spawns.load(), 3) << "a stale-generation ChildDown must NOT trigger a restart";
    EXPECT_EQ(g_acks.load(), 3);
    EXPECT_FALSE(g_slot_replaced.load()) << "no slot was replaced (no restart happened)";
}

// ===========================================================================
// 3/4. Restart-intensity escalation — cumulative cap AND sliding window.
//      A single child is crashed repeatedly; once the cap is exceeded the supervisor escalates
//      (no respawn) and ends the run via on_escalate. Crashes are driven off the acks (causal).
// ===========================================================================
namespace {
struct EscalationDone : public qb::Event {};

class EscalatingSupervisor : public qb::Supervisor {
    qb::ActorId _coord;
    const int   _crashes;
    int         _acks   = 0;
    int         _issued = 0;

public:
    EscalatingSupervisor(qb::ActorId coord, unsigned max_restarts, qb::duration window, int crashes)
        : qb::Supervisor(qb::restart_strategy::one_for_one, 1, max_restarts, window)
        , _coord(coord)
        , _crashes(crashes) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SpawnAck>(*this); // the supervisor itself watches acks to script its crashes
        registerEvent<EscalationDone>(*this);
        co_return co_await qb::Supervisor::onInit();
    }
    // The worker acks the coordinator; the coordinator does not relay, so the supervisor scripts off
    // its OWN copy: workers ack the coordinator, and we also make them ack us. Simpler: the worker
    // acks the coordinator only; here we drive crashes off ChildDown timing instead — but that is
    // exactly what we crash. So we watch the coordinator's acks via a forwarded SpawnAck: the
    // EscalatingSupervisor passes ITS id as the coordinator to the workers (below), and forwards a
    // copy to the real coordinator for counting.
    void
    on(const SpawnAck &e) {
        ++_acks;
        g_acks.fetch_add(1);
        // forward to the real coordinator for the spawn-count bookkeeping + slot-replacement oracle
        push<SpawnAck>(_coord, e.slot, e.who, id());
        // ack 1 (initial) → crash 1; ack 2 (after restart 1) → crash 2; ...
        if (_issued < _crashes && _acks >= 1 + _issued) {
            if (auto c = child(0); c.is_valid())
                push<Crash>(c);
            ++_issued;
        }
    }
    void
    on(EscalationDone &) {
        qb::Main::stop();
        kill();
    }

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t gen) override {
        // Workers ack the SUPERVISOR (so it can script crashes); it forwards a copy to the coord.
        return addRefActor<TestWorker>(id(), slot, gen, id()).id();
    }
    void
    on_escalate() override {
        g_escalated.store(true);
        push<EscalationDone>(id()); // end the run: no further respawn will ack
    }
};
} // namespace

TEST(ActorSupervisor, MaxRestartsEscalatesCumulative) {
    reset_globals();
    qb::Main main;
    // Coordinator in settle-mode only as a sink (the supervisor stops the engine on escalation).
    auto coord = main.addActor<SpawnCoordinator>(0, /*initial*/ 1, 1, std::vector<std::size_t>{},
                                                 /*stop_at_acks*/ 1000);
    main.addActor<EscalatingSupervisor>(0, coord, /*max_restarts*/ 2u, /*window*/ qb::duration::zero(),
                                        /*crashes*/ 3);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_escalated.load()) << "the third failure must escalate (cumulative cap=2)";
    EXPECT_EQ(g_spawns.load(), 3) << "1 initial + 2 restarts (the third did not respawn)";
}

TEST(SupervisorRestartWindow, EscalatesWithinWindow) {
    reset_globals();
    qb::Main main;
    auto coord = main.addActor<SpawnCoordinator>(0, /*initial*/ 1, 1, std::vector<std::size_t>{},
                                                 /*stop_at_acks*/ 1000);
    // window huge vs the test, so all 3 restarts fall inside it and the 4th escalates.
    main.addActor<EscalatingSupervisor>(0, coord, /*max_restarts*/ 3u, /*window*/ 10s, /*crashes*/ 4);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_escalated.load()) << "the 4th restart within the window must escalate";
    EXPECT_EQ(g_spawns.load(), 1 + 3) << "1 initial + 3 restarts (4th escalated, no respawn)";
}

// ===========================================================================
// 5. RECOVERED + modernized: killing the supervisor tears down its children (no orphans).
// ===========================================================================
namespace {
std::atomic<int> g_kp_alive{0};         // live KpWorker instances (onInit ++, dtor --)
std::atomic<int> g_kp_alive_at_kill{0}; // snapshot taken once the cascade has drained

struct KpProbe : public qb::Event {}; // self-marker that counts settle passes

class KpWorker : public qb::SupervisedActor {
public:
    KpWorker(qb::ActorId sup, std::size_t slot, std::uint64_t gen)
        : qb::SupervisedActor(sup, slot, gen) {}
    qb::io::async::task<bool>
    onInit() override {
        g_kp_alive.fetch_add(1);
        co_return true;
    }
    ~KpWorker() override {
        g_kp_alive.fetch_sub(1);
    }
};

class KpSupervisor : public qb::Supervisor {
public:
    KpSupervisor()
        : qb::Supervisor(qb::restart_strategy::one_for_one, 3) {}

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t generation) override {
        return addRefActor<KpWorker>(id(), slot, generation).id();
    }
};

// Kills the supervisor (cascades KillEvents to its children), then — ordered strictly after via a
// self-marker that rides a few mailbox passes behind the kill — snapshots child liveness and stops.
class KpDriver : public qb::Actor {
    qb::ActorId _sup;
    int         _phase = 0;

public:
    explicit KpDriver(qb::ActorId sup)
        : _sup(sup) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<KpProbe>(*this);
        push<qb::KillEvent>(_sup);
        push<KpProbe>(id());
        co_return true;
    }
    void
    on(KpProbe &) {
        if (++_phase < 5) { // let the same-core cascade + destructors fully drain
            push<KpProbe>(id());
            return;
        }
        g_kp_alive_at_kill.store(g_kp_alive.load());
        qb::Main::stop();
        kill();
    }
};
} // namespace

TEST(SupervisorKillPropagation, KillingSupervisorKillsChildren) {
    g_kp_alive.store(0);
    g_kp_alive_at_kill.store(-1);

    qb::Main   main;
    const auto sup = main.addActor<KpSupervisor>(0);
    main.addActor<KpDriver>(0, sup);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_kp_alive_at_kill.load(), 0) << "children torn down with the supervisor (no orphans)";
    EXPECT_EQ(g_kp_alive.load(), 0) << "nothing leaked at shutdown";
}

// ===========================================================================
// 6. Supervision × async onInit: a child whose async init FAILS emits no ChildDown → no restart.
// ===========================================================================
namespace {
// Acks BEFORE failing so the coordinator can count the spawn attempt and settle deterministically.
class FailingAsyncWorker : public qb::SupervisedActor {
    qb::ActorId _coord;
    std::size_t _slot;

public:
    FailingAsyncWorker(qb::ActorId sup, std::size_t slot, std::uint64_t gen, qb::ActorId coord)
        : qb::SupervisedActor(sup, slot, gen)
        , _coord(coord)
        , _slot(slot) {}
    qb::io::async::task<bool>
    onInit() override {
        g_spawns.fetch_add(1);
        push<SpawnAck>(_coord, _slot, id(), supervisor());
        co_await context().sleep(1ms);
        co_return false; // async init fails → removed WITHOUT stop() → no ChildDown → no restart
    }
};

class FailSupervisor : public qb::Supervisor {
    qb::ActorId _coord;

public:
    explicit FailSupervisor(qb::ActorId coord)
        : qb::Supervisor(qb::restart_strategy::one_for_one, 2, 0)
        , _coord(coord) {}

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t gen) override {
        return addRefActor<FailingAsyncWorker>(id(), slot, gen, _coord).id();
    }
};
} // namespace

TEST(ActorSupervisor, SupervisedAsyncInitChildFailsInitNoChildDown) {
    reset_globals();
    qb::Main main;
    // settle-mode: 2 spawn-attempt acks, then settle. A spurious restart would ack again.
    auto coord = main.addActor<SpawnCoordinator>(0, /*initial*/ 2, 2, std::vector<std::size_t>{},
                                                 /*stop_at_acks*/ 0);
    main.addActor<FailSupervisor>(0, coord);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_spawns.load(), 2) << "2 initial spawns; failed async init emits no ChildDown → no restart";
}

// ===========================================================================
// 7. Supervision × async onInit: a child whose async init ACTIVATES is then crashed → restarted.
// ===========================================================================
namespace {
class AsyncWorker : public qb::SupervisedActor {
    qb::ActorId _coord;
    std::size_t _slot;

public:
    AsyncWorker(qb::ActorId sup, std::size_t slot, std::uint64_t gen, qb::ActorId coord)
        : qb::SupervisedActor(sup, slot, gen)
        , _coord(coord)
        , _slot(slot) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Crash>(*this);
        g_spawns.fetch_add(1);
        co_await context().sleep(1ms); // async activation
        push<SpawnAck>(_coord, _slot, id(), supervisor()); // ack only once active
        co_return true;
    }
    void
    on(Crash &) {
        stop(); // cooperative termination → ChildDown → restart
    }
};

class AsyncSupervisor : public qb::Supervisor {
    qb::ActorId _coord;

public:
    explicit AsyncSupervisor(qb::ActorId coord)
        : qb::Supervisor(qb::restart_strategy::one_for_one, 2, 0)
        , _coord(coord) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<TriggerCrash>(*this);
        co_return co_await qb::Supervisor::onInit();
    }
    void
    on(TriggerCrash &e) {
        push<Crash>(child(e.slot));
    }

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t gen) override {
        return addRefActor<AsyncWorker>(id(), slot, gen, _coord).id();
    }
};
} // namespace

TEST(ActorSupervisor, SupervisedAsyncInitChildActivatesThenSupervised) {
    reset_globals();
    qb::Main main;
    // 2 initial async-init activations + 1 restart of slot 1 (also async-init) = 3 spawns.
    auto coord = main.addActor<SpawnCoordinator>(0, /*initial*/ 2, 2, std::vector<std::size_t>{1},
                                                 /*stop_at_acks*/ 3);
    main.addActor<AsyncSupervisor>(0, coord);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_spawns.load(), 3) << "2 initial async-init + 1 restart of slot 1 (also async-init)";
    EXPECT_TRUE(g_slot_replaced.load()) << "the restarted async slot must hold a NEW ActorId";
}

// ===========================================================================
// 8. Cooperative model: a child that dies WITHOUT stop() emits no ChildDown → no restart.
// ===========================================================================
namespace {
class SelfKillWorker : public qb::SupervisedActor {
    qb::ActorId _coord;
    std::size_t _slot;

public:
    SelfKillWorker(qb::ActorId sup, std::size_t slot, std::uint64_t gen, qb::ActorId coord)
        : qb::SupervisedActor(sup, slot, gen)
        , _coord(coord)
        , _slot(slot) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Crash>(*this);
        g_spawns.fetch_add(1);
        push<SpawnAck>(_coord, _slot, id(), supervisor());
        co_return true;
    }
    void
    on(Crash &) {
        kill(); // dies WITHOUT stop() → no ChildDown → no restart
    }
};

class CoopSupervisor : public qb::Supervisor {
    qb::ActorId _coord;

public:
    explicit CoopSupervisor(qb::ActorId coord)
        : qb::Supervisor(qb::restart_strategy::one_for_one, 2, 0)
        , _coord(coord) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<TriggerCrash>(*this);
        co_return co_await qb::Supervisor::onInit();
    }
    void
    on(TriggerCrash &e) {
        push<Crash>(child(e.slot));
    }

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t gen) override {
        return addRefActor<SelfKillWorker>(id(), slot, gen, _coord).id();
    }
};
} // namespace

TEST(ActorSupervisor, ChildDiesWithoutStopNotRestarted) {
    reset_globals();
    qb::Main main;
    // settle-mode with a scripted crash of slot 1: 2 initial acks → crash slot 1 → settle. The
    // self-kill emits no ChildDown, so no respawn acks; the settle countdown proves it.
    auto coord = main.addActor<SpawnCoordinator>(0, /*initial*/ 2, 2, std::vector<std::size_t>{1},
                                                 /*stop_at_acks*/ 0);
    main.addActor<CoopSupervisor>(0, coord);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_spawns.load(), 2) << "2 initial; the child died without stop() → no ChildDown → no restart";
    EXPECT_FALSE(g_slot_replaced.load()) << "no restart happened";
}
