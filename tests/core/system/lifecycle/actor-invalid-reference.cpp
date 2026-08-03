/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/lifecycle/actor-invalid-reference.cpp
 * @brief Sending to an invalid / dead actor id is dropped gracefully — the sender survives — and a
 *        throwing `onInit` fails the core's init (it never silently swallows the throw).
 *
 * The original test-actor-error-handling.cpp had two tautological cases (ShouldRecoverFromErrors /
 * ShouldTerminateOnUnrecoverableErrors) that asserted ONLY on globals the actor itself flipped
 * straight from the test's input flag — they could never fail. They are deleted. What remains is the
 * one real, framework-observable contract plus a real failure path:
 *
 *   - SendToNeverExistedActorSenderSurvives — a push to an id that was never registered is dropped,
 *     and the sender keeps processing its mailbox (a follow-up self-tick still fires).
 *   - SendToDeadActorSenderSurvives — same, but to an id that WAS live and has since been killed.
 *   - ThrowingOnInitFailsCoreInit — a `onInit` that throws fails the init and aborts `start()` with
 *     `hasError()`. (Per the engine: an `onInit` throw is caught by `__drive_init__` and surfaced as
 *     a false-return init failure, i.e. the BadActorInit-class outcome — `hasError()` is the only
 *     per-engine observable; there is no public per-core error-code getter.)
 *
 * Every in-actor side-effect is mirrored to a post-join() atomic so a never-scheduled actor cannot
 * pass vacuously.
 */

#include <atomic>
#include <gtest/gtest.h>
#include <stdexcept>

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/main.h>

using namespace std::chrono_literals;

// A payload that nobody on the dead/never-existed target could ever handle.
struct Poke : public qb::Event {};
// Sender self-tick proving its mailbox keeps advancing after a bad push.
struct SelfTick : public qb::Event {};

namespace {
std::atomic<bool> g_sender_survived{false}; // the sender processed a tick AFTER the bad push
std::atomic<bool> g_sender_oninit_ran{false};
} // namespace

// Pushes a Poke to `_target` (an invalid or dead id), then schedules a self-tick. If the bad push
// had torn down the sender or its core, the self-tick would never fire.
class Sender : public qb::Actor {
    qb::ActorId _target;

public:
    explicit Sender(qb::ActorId target)
        : _target(target) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SelfTick>(*this);
        g_sender_oninit_ran.store(true);
        push<Poke>(_target);  // dropped: the target is invalid / dead
        push<SelfTick>(id()); // mailbox-ordered after the bad push
        co_return true;
    }

    void
    on(const SelfTick &) {
        g_sender_survived.store(true); // we are alive and draining our mailbox after the bad push
        qb::Main::stop();
        kill();
    }
};

TEST(InvalidReference, SendToNeverExistedActorSenderSurvives) {
    g_sender_survived.store(false);
    g_sender_oninit_ran.store(false);

    qb::Main main;
    // An id that was never registered on core 0: the uint32 layout is {sid:low16, core:high16},
    // so 4242 == {sid 4242, core 0} — a high sid the low-first allocator never hands out.
    main.addActor<Sender>(0, qb::ActorId(static_cast<uint32_t>(4242)));
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError()) << "a push to a never-existed id must not kill the core";
    EXPECT_TRUE(g_sender_oninit_ran.load());
    EXPECT_TRUE(g_sender_survived.load()) << "the sender must keep processing its mailbox";
}

// ---------------------------------------------------------------------------
// Push to an id that WAS live and has since been killed.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_victim_died{false};
std::atomic<bool> g_dead_sender_survived{false};
} // namespace

struct Die : public qb::Event {};
struct VictimGone : public qb::Event {
    qb::ActorId who;
    explicit VictimGone(qb::ActorId w)
        : who(w) {}
};

class Victim : public qb::Actor {
    qb::ActorId _notify;

public:
    explicit Victim(qb::ActorId notify)
        : _notify(notify) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Die>(*this);
        co_return true;
    }
    void
    on(const Die &) {
        g_victim_died.store(true);
        push<VictimGone>(_notify, id());
        kill();
    }
};

// Kills a victim, then — once notified it is gone — pushes to the now-dead id and self-ticks.
class DeadIdSender : public qb::Actor {
    qb::ActorId _victim;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<VictimGone>(*this);
        registerEvent<SelfTick>(*this);
        _victim = addRefActor<Victim>(id()).id();
        to(_victim).push<Die>();
        co_return true;
    }
    void
    on(const VictimGone &e) {
        push<Poke>(e.who);    // push to the now-dead id — must be dropped, not crash
        push<SelfTick>(id()); // mailbox-ordered after the bad push
    }
    void
    on(const SelfTick &) {
        g_dead_sender_survived.store(true);
        qb::Main::stop();
        kill();
    }
};

TEST(InvalidReference, SendToDeadActorSenderSurvives) {
    g_victim_died.store(false);
    g_dead_sender_survived.store(false);

    qb::Main main;
    main.addActor<DeadIdSender>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError()) << "a push to a dead id must not kill the core";
    EXPECT_TRUE(g_victim_died.load()) << "the victim must actually have been killed first";
    EXPECT_TRUE(g_dead_sender_survived.load()) << "the sender must survive pushing to a dead id";
}

// ---------------------------------------------------------------------------
// A throwing onInit fails the core init (the throw is never silently swallowed).
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_throwinit_ran{false};
std::atomic<bool> g_throwinit_destroyed{false};
} // namespace

class ThrowingInitActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_throwinit_ran.store(true);
        if (id().is_valid()) // always true; defeats unreachable-code analysis on the co_return
            throw std::runtime_error("onInit blew up");
        co_return true;
    }
    ~ThrowingInitActor() override {
        g_throwinit_destroyed.store(true);
    }
};

TEST(InvalidReference, ThrowingOnInitFailsCoreInit) {
    g_throwinit_ran.store(false);
    g_throwinit_destroyed.store(false);

    qb::Main main;
    main.addActor<ThrowingInitActor>(0);
    main.start(false);
    main.join();

    // The throw is surfaced as an init failure (BadActorInit-class) and aborts start().
    EXPECT_TRUE(main.hasError()) << "a throwing onInit must fail the core init, not be swallowed";
    EXPECT_TRUE(g_throwinit_ran.load()) << "onInit must actually have run";
    EXPECT_TRUE(g_throwinit_destroyed.load()) << "a failed-init actor must still be destroyed";
}
