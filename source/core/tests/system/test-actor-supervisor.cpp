/**
 * @file test-actor-supervisor.cpp
 * @brief Tests for the supervision pattern: `qb::Supervisor` + `qb::SupervisedActor` and the
 *        `restart_strategy` family (one_for_one / one_for_all / rest_for_one), the generation
 *        guard (stale `ChildDown` ignored), and restart-intensity escalation.
 *
 * A supervised `TestWorker` increments a global spawn counter in `onInit`, so the number of
 * (re)spawns distinguishes the strategies precisely:
 *   - one_for_one  + 1 crash of 3 children -> 3 + 1 = 4 spawns
 *   - one_for_all  + 1 crash of 3 children -> 3 + 3 = 6 spawns
 *   - rest_for_one + crash slot 1 of 3     -> 3 + 2 = 5 spawns
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

using namespace qb;
using namespace std::chrono_literals;

namespace {
std::atomic<int>  g_spawns{0};     // total TestWorker onInit() calls (initial + restarts)
std::atomic<bool> g_escalated{false};
} // namespace

// Tells a worker to terminate (sent by the supervisor).
struct Crash : public qb::Event {};
// Test control: tells the supervisor to crash the child in `slot` (sent by the driver).
struct TriggerCrash : public qb::Event {
    std::size_t slot;
    explicit TriggerCrash(std::size_t s)
        : slot(s) {}
};

// A supervised worker: counts its spawns; on Crash, terminates cooperatively (notifies supervisor).
class TestWorker : public qb::SupervisedActor {
public:
    TestWorker(qb::ActorId sup, std::size_t slot, std::uint64_t gen)
        : qb::SupervisedActor(sup, slot, gen) {}
    bool
    onInit() override {
        registerEvent<Crash>(*this);
        g_spawns.fetch_add(1);
        return true;
    }
    void
    on(Crash &) {
        stop(); // notify supervisor + kill -> triggers a restart
    }
};

// A concrete supervisor: spawns TestWorkers and exposes a test-control crash trigger.
class TestSupervisor : public qb::Supervisor {
public:
    TestSupervisor(qb::restart_strategy strat, std::size_t count, unsigned max_restarts = 0)
        : qb::Supervisor(strat, count, max_restarts) {}

    bool
    onInit() override {
        registerEvent<TriggerCrash>(*this);
        return qb::Supervisor::onInit(); // registers ChildDown + spawns the initial children
    }
    void
    on(TriggerCrash &e) {
        push<Crash>(child(e.slot)); // crash the current child in that slot
    }

protected:
    qb::ActorId
    spawn_child(std::size_t slot, std::uint64_t generation) override {
        auto *w = addRefActor<TestWorker>(id(), slot, generation);
        return w->id();
    }
    void
    on_escalate() override {
        g_escalated = true;
    }
};

// Sends one TriggerCrash to the supervisor, then stops the engine.
class CrashDriver : public qb::Actor {
    qb::ActorId _sup;
    std::size_t _slot;

public:
    CrashDriver(qb::ActorId sup, std::size_t slot)
        : _sup(sup)
        , _slot(slot) {}
    bool
    onInit() override {
        auto sup  = _sup;
        auto slot = _slot;
        qb::io::async::callback([this, sup, slot] { push<TriggerCrash>(sup, slot); }, 20ms);
        qb::io::async::callback([] { qb::Main::stop(); }, 90ms);
        return true;
    }
};

TEST(ActorSupervisor, OneForOneRestartsOnlyDeadChild) {
    g_spawns     = 0;
    g_escalated  = false;
    qb::Main main;
    auto     sup = main.addActor<TestSupervisor>(0, qb::restart_strategy::one_for_one, 3);
    main.addActor<CrashDriver>(0, sup, 1);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_spawns.load(), 4); // 3 initial + 1 restart of the dead child only
}

TEST(ActorSupervisor, OneForAllRestartsEveryChild) {
    g_spawns    = 0;
    g_escalated = false;
    qb::Main main;
    auto     sup = main.addActor<TestSupervisor>(0, qb::restart_strategy::one_for_all, 3);
    main.addActor<CrashDriver>(0, sup, 1);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_spawns.load(), 6); // 3 initial + 3 (all restarted)
}

TEST(ActorSupervisor, RestForOneRestartsFromSlotOnward) {
    g_spawns    = 0;
    g_escalated = false;
    qb::Main main;
    auto     sup = main.addActor<TestSupervisor>(0, qb::restart_strategy::rest_for_one, 3);
    main.addActor<CrashDriver>(0, sup, 1); // crash slot 1 -> restart slots 1 and 2
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_spawns.load(), 5); // 3 initial + 2 (slots 1,2)
}

// Sends a ChildDown with a stale generation straight to the supervisor: it must be ignored.
class StaleDriver : public qb::Actor {
    qb::ActorId _sup;

public:
    explicit StaleDriver(qb::ActorId sup)
        : _sup(sup) {}
    bool
    onInit() override {
        auto sup = _sup;
        qb::io::async::callback([this, sup] { push<qb::ChildDown>(sup, std::size_t{0}, std::uint64_t{999}); },
                                20ms);
        qb::io::async::callback([] { qb::Main::stop(); }, 80ms);
        return true;
    }
};

TEST(ActorSupervisor, StaleChildDownIsIgnored) {
    g_spawns    = 0;
    g_escalated = false;
    qb::Main main;
    auto     sup = main.addActor<TestSupervisor>(0, qb::restart_strategy::one_for_one, 3);
    main.addActor<StaleDriver>(0, sup);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_spawns.load(), 3); // no restart happened
}

// Crashes the single child three times; with max_restarts=2 the third escalates.
class RepeatCrashDriver : public qb::Actor {
    qb::ActorId _sup;

public:
    explicit RepeatCrashDriver(qb::ActorId sup)
        : _sup(sup) {}
    bool
    onInit() override {
        auto sup = _sup;
        qb::io::async::callback([this, sup] { push<TriggerCrash>(sup, std::size_t{0}); }, 20ms);
        qb::io::async::callback([this, sup] { push<TriggerCrash>(sup, std::size_t{0}); }, 50ms);
        qb::io::async::callback([this, sup] { push<TriggerCrash>(sup, std::size_t{0}); }, 80ms);
        qb::io::async::callback([] { qb::Main::stop(); }, 130ms);
        return true;
    }
};

TEST(ActorSupervisor, MaxRestartsEscalates) {
    g_spawns    = 0;
    g_escalated = false;
    qb::Main main;
    auto     sup = main.addActor<TestSupervisor>(0, qb::restart_strategy::one_for_one, 1, 2);
    main.addActor<RepeatCrashDriver>(0, sup);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_escalated.load());   // the third failure escalated
    EXPECT_EQ(g_spawns.load(), 3);     // 1 initial + 2 restarts (third did not restart)
}
