/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/dead-letters.cpp
 * @brief Every event that reaches no actor is a dead letter: counted, reported with its reason (Huly QB-163).
 *
 * Until 3.3 the engine logged ONE of the ways an event can reach no actor -- a unicast whose type no
 * actor of the core had registered -- and dropped every other one in silence: a unicast to a dead or
 * unknown id whose type SOME actor of the core handles (the common shape of a stale id), a default
 * event (`KillEvent`, `PingEvent`, ...) to an unknown id,
 * the stash of an actor whose asynchronous `onInit()` failed, the residue queued for a core that had
 * stopped. What this file pins, each through the per-core `DeadLetterHandler` and the core's
 * `CoreStats` counters:
 *
 *   - the reason is exact: `not_found` when no live actor holds the destination id, `unhandled` when
 *     a live one does but handles no event of that type -- whichever router path dropped it;
 *   - a removed actor is `not_found`, and a BROADCAST reports nothing (it is owed to nobody); an
 *     event reaching an actor that handles its type but was killed earlier in the SAME pass, not yet
 *     reaped, is skipped unreported -- the one window the dispatch does not cover, pinned so it
 *     stays a decision;
 *   - a failed asynchronous `onInit()` reports every stashed event as `init_failed`, and a full
 *     stash reports its overflow as `stash_overflow` -- each one disposed, none leaked;
 *   - the residue for a stopped core is `peer_stopped`;
 *   - a broadcast to a core the engine does not run reaches no actor and is `not_found` on the core it
 *     reached, the first one, whatever sent it (Huly QB-268);
 *   - a handler that throws is contained: the core carries on and the letter is still counted.
 *
 * The oversize drop's report is pinned with the drop itself (messaging/oversize-event-probe.cpp), and
 * the stash of an actor killed in the very pass its init completed (init/init-gate-disposal.cpp).
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace dead_letters_test {

using namespace std::chrono_literals;

std::mutex                  g_mu;
std::vector<qb::DeadLetter> g_letters;

void
record(qb::DeadLetter const &letter) {
    std::lock_guard lk(g_mu);
    g_letters.push_back(letter);
}
std::vector<qb::DeadLetter>
letters() {
    std::lock_guard lk(g_mu);
    return g_letters;
}
void
reset() {
    std::lock_guard lk(g_mu);
    g_letters.clear();
}
std::size_t
count(qb::DeadLetterReason const reason) {
    std::lock_guard lk(g_mu);
    std::size_t     n = 0;
    for (auto const &l : g_letters)
        n += l.reason == reason;
    return n;
}

// A payload that counts its live instances: a dropped event must still be destroyed.
std::atomic<int> g_live{0};
struct Counted {
    Counted() noexcept {
        g_live.fetch_add(1, std::memory_order_relaxed);
    }
    Counted(Counted const &) noexcept {
        g_live.fetch_add(1, std::memory_order_relaxed);
    }
    Counted &operator=(Counted const &) = default;
    ~Counted() {
        g_live.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct Ping : qb::Event {};
struct Orphan : qb::Event {}; // registered by no actor anywhere
struct Done : qb::Event {};
struct Loaded : qb::Event {
    Counted payload;
};

const qb::ActorId kNobody(9999u); // sid 9999 on core 0 (the uint32 form: sid low, core high) -- no actor ever has it

// --- 1. the reason, whichever router path dropped it -------------------------------------------

class Holder : public qb::Actor { // makes Ping a registered type on the core
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping const &) {}
};

class Bystander : public qb::Actor { // alive, handles neither Ping nor Orphan
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

qb::CoreStats g_stats{};

class Driver : public qb::Actor {
    qb::ActorId _holder, _bystander;

public:
    Driver(qb::ActorId holder, qb::ActorId bystander) noexcept
        : _holder(holder)
        , _bystander(bystander) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Done>(*this);
        push<Ping>(kNobody);            // registered type, no such id      -> not_found
        push<Orphan>(kNobody);          // unregistered type, no such id    -> not_found
        push<Ping>(_bystander);         // registered type, live non-holder -> unhandled
        push<Orphan>(_bystander);       // unregistered type, live actor    -> unhandled
        push<qb::KillEvent>(kNobody);   // a default event, no such id      -> not_found
        push<Ping>(qb::BroadcastId(0)); // a broadcast: the holder takes it, nobody else is owed it
        push<Done>(id());               // FIFO: handled after all of the above
        co_return true;
    }
    void
    on(Done const &) {
        g_stats = getCoreStats();
        push<qb::KillEvent>(_holder);
        push<qb::KillEvent>(_bystander);
        kill();
    }
};

TEST(DeadLetters, EveryUnicastThatReachesNoActorIsReportedWithItsReason) {
    reset();
    g_stats = {};
    qb::Main main;
    main.core(0).setDeadLetterHandler(&record);
    const auto holder    = main.addActor<Holder>(0);
    const auto bystander = main.addActor<Bystander>(0);
    const auto driver    = main.addActor<Driver>(0, holder, bystander);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    const auto got = letters();
    ASSERT_EQ(got.size(), 5u) << "five unicasts reached no actor; the broadcast reached the holder";
    struct Want {
        qb::EventId          type;
        qb::ActorId          dest;
        qb::DeadLetterReason reason;
    };
    const Want want[] = {
        {qb::Event::type_to_id<Ping>(), kNobody, qb::DeadLetterReason::not_found},
        {qb::Event::type_to_id<Orphan>(), kNobody, qb::DeadLetterReason::not_found},
        {qb::Event::type_to_id<Ping>(), bystander, qb::DeadLetterReason::unhandled},
        {qb::Event::type_to_id<Orphan>(), bystander, qb::DeadLetterReason::unhandled},
        {qb::Event::type_to_id<qb::KillEvent>(), kNobody, qb::DeadLetterReason::not_found}
    };
    for (std::size_t i = 0; i < 5; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(got[i].event_id, want[i].type);
        EXPECT_EQ(got[i].destination, want[i].dest);
        EXPECT_EQ(got[i].source, driver);
        EXPECT_EQ(got[i].core, 0);
        EXPECT_EQ(got[i].reason, want[i].reason) << qb::dead_letter_reason_name(got[i].reason);
    }
    EXPECT_EQ(g_stats.dead_letters, 5u);
    EXPECT_EQ(g_stats.dead_letters_by_reason[static_cast<std::size_t>(qb::DeadLetterReason::not_found)], 3u);
    EXPECT_EQ(g_stats.dead_letters_by_reason[static_cast<std::size_t>(qb::DeadLetterReason::unhandled)], 2u);
}

// --- 2. a removed actor is not_found; the same-pass window; a broadcast reports nothing --------

class Victim : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping const &) {
        ADD_FAILURE() << "a killed actor received an event";
    }
};

struct Next : qb::Event {};

class Executioner : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit Executioner(qb::ActorId victim) noexcept
        : _victim(victim) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Next>(*this);
        registerEvent<Done>(*this);
        push<qb::KillEvent>(_victim); // pass 1: the victim kills itself; its reap waits for the pass end
        push<Ping>(_victim);          // pass 1, same batch: skipped by the dispatch -- and NOT reported
        push<Next>(id());
        co_return true;
    }
    void
    on(Next const &) {
        push<Ping>(_victim);            // pass 2: the victim is gone -> not_found
        push<Ping>(qb::BroadcastId(0)); // a broadcast is owed to nobody: never a dead letter
        push<Done>(id());
    }
    void
    on(Done const &) {
        kill();
    }
};

TEST(DeadLetters, ARemovedActorIsNotFoundAndABroadcastReportsNothing) {
    reset();
    qb::Main main;
    main.core(0).setDeadLetterHandler(&record);
    const auto victim = main.addActor<Victim>(0);
    main.addActor<Executioner>(0, victim);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    // ONE letter: the Ping of pass 2. The Ping of pass 1 reached an actor killed earlier in the same
    // pass and not yet reaped; the dispatch skips it without a report -- the documented window (a
    // call in that branch, which every event runs through, cost +4 % on the one-core pass probe).
    const auto got = letters();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].reason, qb::DeadLetterReason::not_found);
    EXPECT_EQ(got[0].destination, victim);
    EXPECT_EQ(got[0].event_id, qb::Event::type_to_id<Ping>());
}

// --- 3. a failed asynchronous onInit; a full stash ------------------------------------------------

class FailsItsInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Loaded>(*this);
        co_await context().sleep(30ms); // Activating: what arrives meanwhile is stashed
        co_return false;
    }
    void
    on(Loaded const &) {
        ADD_FAILURE() << "an actor whose init failed received a stashed event";
    }
};

class Loader : public qb::Actor {
    qb::ActorId _target;
    int         _n;

public:
    Loader(qb::ActorId target, int n) noexcept
        : _target(target)
        , _n(n) {}
    qb::io::async::task<bool>
    onInit() override {
        for (int i = 0; i < _n; ++i)
            push<Loaded>(_target);
        kill();
        co_return true;
    }
};

TEST(DeadLetters, AFailedAsyncInitReportsEveryStashedEventAndDisposesIt) {
    reset();
    g_live.store(0);
    constexpr int kStashed = 7;
    {
        qb::Main main;
        main.core(0).setDeadLetterHandler(&record);
        const auto target = main.addActor<FailsItsInit>(0);
        main.addActor<Loader>(0, target, kStashed);
        main.start(false);
        main.join();
    }
    EXPECT_EQ(count(qb::DeadLetterReason::init_failed), static_cast<std::size_t>(kStashed));
    EXPECT_EQ(letters().size(), static_cast<std::size_t>(kStashed));
    EXPECT_EQ(g_live.load(), 0) << "every stashed event is destroyed";
}

TEST(DeadLetters, AFullStashOverflowsFailsTheActivationAndLosesNothingUnreported) {
    reset();
    g_live.store(0);
    constexpr int kSent = 5000; // past the per-actor stash cap (VirtualCore::kActivationStashCap, 4096)
    {
        qb::Main main;
        main.core(0).setDeadLetterHandler(&record);
        const auto target = main.addActor<FailsItsInit>(0);
        main.addActor<Loader>(0, target, kSent);
        main.start(false);
        main.join();
    }
    const auto overflow = count(qb::DeadLetterReason::stash_overflow);
    const auto failed   = count(qb::DeadLetterReason::init_failed);
    EXPECT_GT(overflow, 0u) << "a stash that full must overflow";
    EXPECT_GT(failed, 0u) << "the stash held before the overflow dies with the failed activation";
    EXPECT_EQ(overflow + failed, static_cast<std::size_t>(kSent)) << "every event sent is reported once, by one reason";
    EXPECT_EQ(g_live.load(), 0) << "every event is destroyed, overflowed or stashed";
}

// --- 4. the residue for a stopped core ------------------------------------------------------------

constexpr std::uint64_t kRing = qb::detail::max_deliverable_buckets;
std::atomic<int>        g_peer_stopped{0};

class Gone : public qb::Actor { // core 1's only actor: its core stops at once
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        kill();
        co_return true;
    }
    void
    on(Ping const &) {}
};

struct Burst : qb::Event {};

class Survivor : public qb::Actor {
    qb::ActorId _gone;

public:
    explicit Survivor(qb::ActorId gone) noexcept
        : _gone(gone) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Burst>(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(100ms); // core 1 has long stopped by then
            ctx.push<Burst>();
        });
        co_return true;
    }
    void
    on(Burst const &) {
        for (std::uint64_t i = 0; i < 4 * kRing; ++i) // four rings deep: the stopped core's ring takes one
            push<Ping>(_gone);
        kill(); // this core's shutdown drain finds the rest queued for a stopped core
    }
};

TEST(DeadLetters, TheResidueForAStoppedCoreIsPeerStopped) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the residue must target another core";
    reset();
    qb::Main main;
    main.core(0).setDeadLetterHandler(&record);
    const auto gone = main.addActor<Gone>(1);
    main.addActor<Survivor>(0, gone);
    main.start(false);
    main.join();
    ASSERT_FALSE(main.hasError());

    const auto stopped = count(qb::DeadLetterReason::peer_stopped);
    EXPECT_GE(stopped, 3 * kRing - 1) << "what the stopped core's ring could not take is reported at shutdown";
    EXPECT_LE(stopped, 4 * kRing);
    for (auto const &l : letters()) {
        EXPECT_EQ(l.reason, qb::DeadLetterReason::peer_stopped);
        EXPECT_EQ(l.destination, gone);
        EXPECT_EQ(l.core, 0);
    }
}

// --- 5. a handler that throws ---------------------------------------------------------------------

std::atomic<int> g_thrown{0};
qb::CoreStats    g_after_throw{};

class ThrowingDriver : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Done>(*this);
        push<Orphan>(kNobody);
        push<Orphan>(kNobody);
        push<Done>(id());
        co_return true;
    }
    void
    on(Done const &) {
        g_after_throw = getCoreStats();
        kill();
    }
};

// --- 6. a broadcast to a core the engine does not run (Huly QB-268) --------------------------------
// Every id outside the core set aliases the first core's pipe and mailbox (`CoreSet::resolve` gives it
// index 0), so an event to a core the engine does not run lands on the first core. A unicast found no
// actor there and was a `not_found` letter; a BROADCAST was fanned out to every actor of that core: on an
// engine of cores 1 and 3, a `Hello` to `BroadcastId(2)` reached core 1's actors, and a `KillEvent` so
// addressed killed them all. It now reaches no actor and is reported once, `not_found`, by the core it
// reached, its payload disposed -- whatever primitive sent it and whatever router path meets it.

struct Hello : qb::Event {
    Counted payload;
};
struct Sync : qb::Event {};

constexpr qb::CoreId kLowCore = 1, kHighCore = 3; // cores 0, 2 and 5 are absent, and 300 is past qb::MaxCores
std::atomic<int>     g_misaddressed{0};           // a Hello whose BroadcastId names another core than its receiver's
std::atomic<int>     g_hello_on[4]{};             // valid Hello receptions, by core
std::atomic<bool>    g_synced{false};

class Listener : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Hello>(*this);
        registerEvent<Ping>(*this);
        registerEvent<Sync>(*this);
        co_return true;
    }
    void
    on(Hello const &hello) {
        if (hello.getDestination().is_broadcast() && hello.getDestination().index() != getIndex())
            g_misaddressed.fetch_add(1);
        else
            g_hello_on[getIndex()].fetch_add(1);
    }
    void
    on(Ping const &) {}
    void
    on(Sync const &) { // behind everything the driver queued for this core: all of it has been received
        g_synced.store(true);
        qb::Main::stop();
    }
};

class MisaddressingDriver : public qb::Actor {
    qb::ActorId _low_listener;

public:
    explicit MisaddressingDriver(qb::ActorId low_listener) noexcept
        : _low_listener(low_listener) {}
    qb::io::async::task<bool>
    onInit() override {
        // A deadlock guard, never the oracle: a listener the defect kills never stops the engine.
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(10s);
            (void) ctx;
            qb::Main::stop();
        });
        broadcast<Hello>();                        // control: every listener, once
        push<Hello>(qb::BroadcastId(kHighCore));   // control: this core's listener, once
        push<Hello>(qb::BroadcastId(2));           // absent, between the two
        push<Hello>(qb::BroadcastId(0));           // absent, below
        push<Hello>(qb::BroadcastId(5));           // absent, above
        push<Hello>(qb::BroadcastId(300));         // past qb::MaxCores
        push<Orphan>(qb::BroadcastId(2));          // a type nobody subscribed: the router's other path
        push<qb::KillEvent>(qb::BroadcastId(2));   // a default event: kills nobody
        send<Ping>(qb::BroadcastId(2));            // send: straight into the first core's ring
        push<Ping>(qb::ActorId(2u << 16 | 9999u)); // control: a unicast to an absent core is not_found
        push<Sync>(_low_listener);                 // FIFO behind all of the above, to the first core
        co_return true;
    }
};

TEST(DeadLetters, ABroadcastToACoreTheEngineDoesNotRunReachesNoActor) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the misaddressed events cross from core 3 to core 1";
    reset();
    g_live.store(0);
    g_misaddressed.store(0);
    for (auto &n : g_hello_on)
        n.store(0);
    g_synced.store(false);
    {
        qb::Main main;
        main.core(kLowCore).setDeadLetterHandler(&record);
        main.core(kHighCore).setDeadLetterHandler(&record);
        const auto low = main.addActor<Listener>(kLowCore);
        main.addActor<Listener>(kHighCore);
        main.addActor<MisaddressingDriver>(kHighCore, low);
        main.start();
        main.join();
        ASSERT_FALSE(main.hasError());
    }
    EXPECT_TRUE(g_synced.load()) << "the first core's listener was killed by a KillEvent addressed to core 2";
    EXPECT_EQ(g_misaddressed.load(), 0) << "a broadcast to a core the engine does not run reached the first core's actors";
    EXPECT_EQ(g_hello_on[kLowCore].load(), 1) << "control: broadcast<Hello>() reaches core 1 once";
    EXPECT_EQ(g_hello_on[kHighCore].load(), 2) << "control: broadcast<Hello>() and BroadcastId(3) reach core 3";
    EXPECT_EQ(g_live.load(), 0) << "every Hello payload is destroyed, the dropped ones included";

    const auto  got        = letters();
    std::size_t broadcasts = 0, unicasts = 0;
    for (auto const &l : got) {
        SCOPED_TRACE(qb::event_type_name(l.event_id));
        EXPECT_EQ(l.reason, qb::DeadLetterReason::not_found);
        EXPECT_EQ(l.core, kLowCore) << "reported by the core the event reached";
        EXPECT_NE(l.destination.index(), kLowCore);
        EXPECT_NE(l.destination.index(), kHighCore);
        (l.destination.is_broadcast() ? broadcasts : unicasts) += 1;
    }
    EXPECT_EQ(broadcasts, 7u) << "four Hello, the Orphan, the KillEvent and the sent Ping, each reported once";
    EXPECT_EQ(unicasts, 1u) << "control: the unicast to core 2";
}

TEST(DeadLetters, AHandlerThatThrowsIsContainedAndTheLetterStillCounted) {
    g_thrown.store(0);
    g_after_throw = {};
    qb::Main main;
    main.core(0).setDeadLetterHandler([](qb::DeadLetter const &) {
        g_thrown.fetch_add(1);
        throw std::runtime_error("dead-letter handler failure");
    });
    main.addActor<ThrowingDriver>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError()) << "the throw stays in the dead-letter path";
    EXPECT_EQ(g_thrown.load(), 2);
    EXPECT_EQ(g_after_throw.dead_letters, 2u);
}

} // namespace dead_letters_test
