/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * See the License for the specific terms.
 */

/**
 * @file system/engine/no-default-events.cpp
 * @brief What a `qb::no_default_events` actor must register in order to be stoppable — and what
 *        it must NOT be told to register instead.
 *
 * `Main::stop()` does not send anything. It stores a signum and bumps a generation counter
 * (`Main.cpp:558-566`); each `VirtualCore` notices the bump on its next pass and synthesises a
 * **`qb::SignalEvent`** addressed to every actor it owns (`VirtualCore.cpp:677-681`). Nothing in
 * the engine ever constructs a `qb::KillEvent` — that type exists so a PEER can kill an actor by
 * pushing one. So the minimum subscription for a graceful shutdown is `SignalEvent`.
 *
 * Every shipped surface used to say `KillEvent`, including the comment in the tagged constructor
 * itself. Following that advice produces an engine that cannot be stopped: the actor's core never
 * empties, `Main::join()` never returns, and there is no diagnostic of any kind. Both halves are
 * asserted here, because the negative half is the whole point — a test that only showed
 * `SignalEvent` working would leave the wrong advice looking harmless.
 *
 * HOW A TEST ASSERTS A WEDGE WITHOUT BECOMING ONE. The obvious shape — start the engine, call
 * `stop()`, and see whether `join()` returns — cannot clean up after itself in the negative case:
 * the worker is inside a loop that will never end, so it can be neither joined nor cancelled, and
 * detaching it leaks a thread that is still running framework code at `exit()`. Under ASan and
 * TSan that is a finding of its own, and it would be this file's finding rather than qb's.
 *
 * So the subject is never the only actor on the core. A `Rescuer` shares it: also
 * `no_default_events`, also `KillEvent`-only (so `stop()` cannot reach IT either, in both
 * polarities symmetrically), plus a callback that watches one flag. The measurement is whether
 * `stop()` reached THE SUBJECT — observed in the subject's own destructor — not whether `join()`
 * returned, which the Rescuer's presence would confound. Once the verdict is in, the flag is set,
 * the Rescuer broadcasts a `KillEvent` that both actors DO handle, the core empties normally and
 * `join()` returns. Nothing is leaked and nothing is detached in either case.
 *
 * `Main::stop()` is process-wide state, so these cases are `serial`.
 */

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace no_default_events_test {

std::atomic<bool> g_up{false};           ///< the subject reached onInit
std::atomic<bool> g_subject_gone{false}; ///< the subject was destroyed — i.e. something killed it
std::atomic<bool> g_release{false};      ///< the verdict is in; the Rescuer may tear the core down

/// The shape eleven documentation surfaces used to prescribe: opts out, registers KillEvent only.
class KillOnly : public qb::Actor {
public:
    KillOnly()
        : qb::Actor(qb::no_default_events) {}
    ~KillOnly() override {
        g_subject_gone.store(true, std::memory_order_release);
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        g_up.store(true, std::memory_order_release);
        co_return true;
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

/// The shape that actually works.
class SignalOnly : public qb::Actor {
public:
    SignalOnly()
        : qb::Actor(qb::no_default_events) {}
    ~SignalOnly() override {
        g_subject_gone.store(true, std::memory_order_release);
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::SignalEvent>(*this);
        registerEvent<qb::KillEvent>(*this); // so the Rescuer's teardown reaches it too
        g_up.store(true, std::memory_order_release);
        co_return true;
    }
    // Both must be declared: any `on` in a derived class hides EVERY base overload, and dispatch
    // goes through the derived type (`router.h:286`), so the registrations above need these.
    void
    on(qb::SignalEvent const &) {
        kill();
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

/// Survives `stop()` exactly like the subject, then ends the run on request. Its own subscription
/// set is deliberately identical to `KillOnly`'s so it can never be what made a case pass.
class Rescuer
    : public qb::Actor
    , public qb::ICallback {
public:
    Rescuer()
        : qb::Actor(qb::no_default_events) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (g_release.load(std::memory_order_acquire))
            broadcast<qb::KillEvent>(); // reaches the subject and this actor alike
    }
    void
    on(qb::KillEvent const &) {
        kill();
    }
};

/// Runs `Actor` alongside a `Rescuer`, calls `Main::stop()`, and reports whether the SUBJECT died
/// within `budget`. Always tears the engine down cleanly afterwards.
template <typename Actor>
bool
stops_within(std::chrono::milliseconds budget) {
    g_up.store(false, std::memory_order_release);
    g_subject_gone.store(false, std::memory_order_release);
    g_release.store(false, std::memory_order_release);

    bool reached_subject = false;
    {
        qb::Main engine;
        engine.addActor<Actor>(0);
        engine.addActor<Rescuer>(0);

        std::thread runner([&engine] { engine.start(false); }); // engine on THAT thread

        // Signal only once the subject is up, so a pass cannot mean "stop() arrived first".
        const auto up_by = std::chrono::steady_clock::now() + 5s;
        while (!g_up.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < up_by)
            std::this_thread::sleep_for(1ms);
        EXPECT_TRUE(g_up.load(std::memory_order_acquire)) << "the subject never reached onInit";

        qb::Main::stop();

        const auto give_up = std::chrono::steady_clock::now() + budget;
        while (!g_subject_gone.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < give_up)
            std::this_thread::sleep_for(1ms);
        reached_subject = g_subject_gone.load(std::memory_order_acquire);

        // Verdict recorded — now end the run through the one channel both actors do handle.
        g_release.store(true, std::memory_order_release);
        runner.join();
    }
    return reached_subject;
}

TEST(NoDefaultEvents, RegisteringSignalEventMakesTheActorStoppable) {
    EXPECT_TRUE(stops_within<SignalOnly>(5s)) << "a no_default_events actor that registers qb::SignalEvent must honour Main::stop()";
}

TEST(NoDefaultEvents, RegisteringKillEventAloneDoesNotMakeTheActorStoppable) {
    // NOT a wish, a fact: it pins WHY the documentation had to change. If this ever starts
    // returning true the engine has grown a second shutdown channel, and every doc that now says
    // "register SignalEvent" needs revisiting — starting with the @warning on
    // qb::no_default_events_t and the comment in Actor::Actor(no_default_events_t).
    EXPECT_FALSE(stops_within<KillOnly>(1500ms))
        << "qb::KillEvent is never synthesised by the engine; if Main::stop() now reaches this actor, the shutdown path changed";
}

} // namespace no_default_events_test
