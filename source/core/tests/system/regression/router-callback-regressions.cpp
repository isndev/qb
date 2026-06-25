/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/regression/router-callback-regressions.cpp
 * @brief Two regression pins from the engine audit, plus a positive-control delivery case.
 *
 *   - Finding #4: an event type NO actor subscribes to must be dropped+warned, NOT crash the
 *     destination core (the bug threw std::out_of_range in router::memh::route's onError disposer
 *     path; start_thread caught it and flagged ExceptionThrown). The assertion is preserved VERBATIM.
 *   - Finding #C: an actor killed during the callback dispatch pass (by another actor's tick) must
 *     NOT receive another on(LoopEvent) tick in that same pass. The assertion is preserved VERBATIM.
 *   - Positive control: a registered event IS delivered — so a green run of the two pins cannot be
 *     "nothing ran". If the engine silently dropped everything, this control would fail.
 *
 * DE-FLAKED: the original stopped each engine with a detached `std::thread` that slept a fixed
 * `300ms` then called `qb::Main::stop()`. Here every run stops as soon as its observable is in hand
 * (a follow-up registered event delivered, or a bounded number of callback passes elapsed) — no
 * wall-clock stopper, no detached thread. A LOUD ctest TIMEOUT remains the only backstop.
 */

#include <atomic>
#include <gtest/gtest.h>

#include <qb/actor.h>
#include <qb/icallback.h>
#include <qb/main.h>

// ===========================================================================
// Finding #4 repro: an event type that NO actor anywhere subscribes to.
// Sending it must NOT kill the destination core — it should be dropped+warned.
// A follow-up *registered* event (Probe) is delivered after it, proving the core survived AND
// giving the run a completion-driven stop (no 300ms thread).
// ===========================================================================
struct NeverSubscribedEvent : qb::Event {};
struct Probe : qb::Event {}; // registered on the receiver; its delivery proves liveness + stops

static std::atomic<bool> g_probe_delivered{false};

struct ReproReceiver : qb::Actor {
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        registerEvent<Probe>(*this);
        co_return true;
    }
    void
    on(Probe const &) {
        // If the unregistered event had killed this core, we would never get here.
        g_probe_delivered.store(true);
        qb::Main::stop();
        kill();
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

struct ReproSender : qb::Actor {
    qb::ActorId target;
    explicit ReproSender(qb::ActorId t)
        : target(t) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        push<NeverSubscribedEvent>(target); // nobody handles this type — must be dropped, not crash
        push<Probe>(target);                // registered: mailbox-ordered after the bad event
        co_return true;
    }
};

TEST(AuditRepro, UnregisteredEventTypeMustNotKillCore) {
    g_probe_delivered.store(false);
    qb::Main engine;
    auto     rid = engine.addActor<ReproReceiver>(0);
    engine.addActor<ReproSender>(0, rid);
    engine.start(false); // this thread becomes the worker; returns on shutdown
    engine.join();
    // BUG: the unregistered event throws std::out_of_range in router::memh::route's
    // onError disposer path; start_thread catches it and flags ExceptionThrown, so
    // hasError() becomes true (the core died). It must be false (event dropped).
    EXPECT_FALSE(engine.hasError()) << "destination core died after receiving an unregistered event type";
    // Positive control: the registered Probe, sent AFTER the unregistered event, was delivered —
    // the core kept processing its mailbox.
    EXPECT_TRUE(g_probe_delivered.load()) << "a registered event after the bad one must still be delivered";
}

// ===========================================================================
// Finding #C: an actor killed during the callback dispatch pass (by another
// actor's tick) must NOT receive another on(LoopEvent) tick in that same pass.
// ===========================================================================
static std::atomic<int> g_victim_ticks{0};

struct CbVictim
    : qb::Actor
    , qb::ICallback {
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        g_victim_ticks.fetch_add(1, std::memory_order_relaxed);
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

struct CbKiller
    : qb::Actor
    , qb::ICallback {
    qb::RefActorHandle<CbVictim> victim;
    int                          _passes = 0;
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this);           // killer registers first => earlier in list
        victim = addRefHandle<CbVictim>(); // victim registers its callback after
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        // First tick: synchronously kill the victim (which is later in the
        // callback snapshot). get() returns nullptr once it is dead, so this
        // fires exactly once.
        if (auto *v = victim.get())
            v->kill();
        // Completion-driven stop: a few more callback passes give a *buggy* engine ample time to
        // re-tick the just-killed victim within a pass; once they have elapsed clean, stop.
        if (++_passes >= 4) {
            qb::Main::stop();
            kill();
        }
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

TEST(AuditRepro, KilledActorGetsNoFurtherCallbackInSamePass) {
    g_victim_ticks = 0;
    qb::Main engine;
    engine.addActor<CbKiller>(0);
    engine.start(false);
    engine.join();
    // BUG: victim's tick ran once after it was killed (it was still in the
    // already-copied snapshot). Fixed: a killed actor is skipped in the loop.
    EXPECT_EQ(g_victim_ticks.load(), 0) << "victim ticked after being killed in the same callback pass";
}
