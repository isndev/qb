/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * See the License for the specific terms.
 */

/**
 * @file system/engine/default-event-registry.cpp
 * @brief The five default events (`qb::default_events_t`) dispatch through the ACTOR REGISTRY,
 *        not through a per-type handler table — and every observable contract of the table
 *        survives the move.
 *
 * Until 3.2 an actor's constructor inserted five `key_table` slots (one per default event) and
 * its removal walked every resolver on the core to erase them: about 40 % of the 200 ns an actor
 * lifetime costs on `savina/fib`. Now `registerEvent<E>` for a default `E` stores a dispatch
 * pointer in the actor itself (`Actor::_default_on[k]`), and `VirtualCore::DefaultEventResolver<E>`
 * — installed once per core — answers a unicast from `__actor_slot__(dest)` and a broadcast from
 * a snapshot of the registry. What a user could observe before must still hold, and each case
 * below pins one such contract:
 *
 *  - a pushed `KillEvent` reaches the BASE handler of an actor that never mentioned it;
 *  - a derived class that re-registers a default event REPLACES the base handler (the
 *    supervisor pattern relies on this);
 *  - `unregisterEvent<E>` and `qb::no_default_events` both make the actor unreachable by `E`,
 *    and the event is dropped silently rather than delivered somewhere else;
 *  - a broadcast reaches every live actor, and an actor spawned BY a handler during that
 *    broadcast is not reached by it — the registry may reallocate under the walk, which is what
 *    the snapshot exists for, and ASan is the instrument that would see a walk over freed slots;
 *  - a `ServiceActor` (fixed sid, resolved through the same slot) is reached like any other.
 *
 * Every case ends by itself: the subject either dies of the event under test or kills itself
 * from a watchdog callback after a bounded wait, so a broken registry is a failed assertion,
 * never a wedged engine.
 */

#include <atomic>
#include <chrono>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace default_event_registry_test {

// ---- compile-time surface --------------------------------------------------------------------

static_assert(std::tuple_size_v<qb::default_events_t> == 5);
static_assert(qb::default_event_index<qb::KillEvent> == 0);
static_assert(qb::default_event_index<qb::SignalEvent> == 1);
static_assert(qb::default_event_index<qb::UnregisterCallbackEvent> == 2);
static_assert(qb::default_event_index<qb::PingEvent> == 3);
static_assert(qb::default_event_index<qb::RequireEvent> == 4);
static_assert(qb::default_event_index<qb::KillEvent const &> == 0, "cvref is stripped");
static_assert(qb::is_default_event<qb::KillEvent>);
static_assert(!qb::is_default_event<qb::Event>);
struct DerivedFromKill : qb::KillEvent {};
static_assert(!qb::is_default_event<DerivedFromKill>, "a type derived from a default event is its own event type, with its own table");
static_assert(qb::default_event_index<DerivedFromKill> == -1);

// ---- shared state ----------------------------------------------------------------------------

constexpr auto kWatchdog = 5s; ///< a registry that never delivers fails here, loudly

std::atomic<bool> g_timeout{false};
std::atomic<int>  g_target_gone{0};
std::atomic<int>  g_derived_calls{0};
std::atomic<bool> g_pushed{false};
std::atomic<int>  g_survived{0};
std::atomic<int>  g_members_gone{0};
std::atomic<int>  g_late_gone{0};
std::atomic<bool> g_late_got_kill{false};

void
reset() {
    g_timeout       = false;
    g_target_gone   = 0;
    g_derived_calls = 0;
    g_pushed        = false;
    g_survived      = 0;
    g_members_gone  = 0;
    g_late_gone     = 0;
    g_late_got_kill = false;
}

/// Callback mixin: `kill()` after `kWatchdog` and record that the deadline, not the event, ended us.
class Watchdog
    : public qb::Actor
    , public qb::ICallback {
    std::chrono::steady_clock::time_point _since;

protected:
    Watchdog()
        : _since(std::chrono::steady_clock::now()) {}
    explicit Watchdog(qb::no_default_events_t tag)
        : qb::Actor(tag)
        , _since(std::chrono::steady_clock::now()) {}

    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    [[nodiscard]] bool
    expired() const noexcept {
        return std::chrono::steady_clock::now() - _since > kWatchdog;
    }
    void
    on(qb::LoopEvent const &) override {
        if (expired()) {
            g_timeout = true;
            kill();
        }
    }
};

/// Never mentions KillEvent: only the base `Actor::on(KillEvent const &)` can end it early.
class Target : public Watchdog {
public:
    ~Target() override {
        ++g_target_gone;
    }
};

/// Pushes one `KillEvent` to `_to` from its first loop, then leaves.
class Killer
    : public qb::Actor
    , public qb::ICallback {
    qb::ActorId _to;

public:
    explicit Killer(qb::ActorId to)
        : _to(to) {}
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        push<qb::KillEvent>(_to);
        g_pushed = true;
        kill();
    }
};

TEST(DefaultEventRegistry, UnicastKillEventReachesTheBaseHandler) {
    reset();
    qb::Main   engine;
    const auto target = engine.addActor<Target>(0);
    ASSERT_TRUE(target.is_valid());
    engine.addActor<Killer>(0, target);
    engine.start(false);
    EXPECT_FALSE(engine.hasError());
    EXPECT_FALSE(g_timeout) << "the KillEvent was pushed but the base handler never ran";
    EXPECT_EQ(g_target_gone, 1);
}

/// Re-registers `KillEvent` in its constructor, as `qb/core/patterns/supervisor.h` does.
class Overrider : public Watchdog {
public:
    Overrider() {
        registerEvent<qb::KillEvent>(*this);
    }
    void
    on(qb::KillEvent const &) {
        ++g_derived_calls;
        kill();
    }
    using Watchdog::on;
};

TEST(DefaultEventRegistry, DerivedReregistrationReplacesTheBaseHandler) {
    reset();
    qb::Main   engine;
    const auto target = engine.addActor<Overrider>(0);
    engine.addActor<Killer>(0, target);
    engine.start(false);
    EXPECT_FALSE(engine.hasError());
    EXPECT_FALSE(g_timeout);
    // Had the base handler still been the subscription, it would have killed the actor and the
    // derived trampoline's `is_alive()` check would have kept this at 0.
    EXPECT_EQ(g_derived_calls, 1);
}

/// Waits for the push, then a bounded settle, then reports whether it is still alive.
class Survivor : public Watchdog {
    std::chrono::steady_clock::time_point _seen{};

protected:
    using Watchdog::Watchdog;

public:
    void
    on(qb::LoopEvent const &e) override {
        Watchdog::on(e);
        if (!g_pushed || !is_alive())
            return;
        const auto now = std::chrono::steady_clock::now();
        if (_seen == std::chrono::steady_clock::time_point{})
            _seen = now;
        else if (now - _seen > 100ms) {
            ++g_survived; // still alive 100 ms after the push: the KillEvent never reached us
            kill();
        }
    }
};

class Deaf : public Survivor {
public:
    qb::io::async::task<bool>
    onInit() override {
        unregisterEvent<qb::KillEvent>(*this);
        registerCallback(*this);
        co_return true;
    }
};

TEST(DefaultEventRegistry, UnregisterEventDropsAPushedDefault) {
    reset();
    qb::Main   engine;
    const auto target = engine.addActor<Deaf>(0);
    engine.addActor<Killer>(0, target);
    engine.start(false);
    EXPECT_FALSE(engine.hasError());
    EXPECT_FALSE(g_timeout);
    EXPECT_EQ(g_survived, 1) << "unregisterEvent<KillEvent> did not make the actor unreachable";
}

class OptedOut : public Survivor {
public:
    OptedOut()
        : Survivor(qb::no_default_events) {}
};

TEST(DefaultEventRegistry, NoDefaultEventsActorIsNotReachedByAPushedDefault) {
    reset();
    qb::Main   engine;
    const auto target = engine.addActor<OptedOut>(0);
    engine.addActor<Killer>(0, target);
    engine.start(false);
    EXPECT_FALSE(engine.hasError());
    EXPECT_FALSE(g_timeout);
    EXPECT_EQ(g_survived, 1) << "a no_default_events actor was reached by a KillEvent";
}

// ---- broadcast -------------------------------------------------------------------------------

constexpr int kMembers = 64;
constexpr int kLate    = 200; ///< enough to reallocate `_actors` under the broadcast walk

class Member : public Watchdog {
public:
    ~Member() override {
        ++g_members_gone;
    }
};

/// Spawned by a handler DURING the broadcast: must not receive that broadcast; dies on its own.
class Late
    : public qb::Actor
    , public qb::ICallback {
public:
    Late() {
        registerEvent<qb::KillEvent>(*this);
    }
    ~Late() override {
        ++g_late_gone;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::KillEvent const &) {
        g_late_got_kill = true;
        kill();
    }
    void
    on(qb::LoopEvent const &) override {
        kill();
    }
};

/// Its KillEvent handler grows the registry by `kLate` actors while the walk is in progress.
class Spawner : public Watchdog {
public:
    Spawner() {
        registerEvent<qb::KillEvent>(*this);
    }
    void
    on(qb::KillEvent const &) {
        for (int i = 0; i < kLate; ++i)
            addRefActor<Late>();
        kill();
    }
    using Watchdog::on;
};

class Trigger
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        broadcast<qb::KillEvent>(); // reaches this actor too, through the base handler
        unregisterCallback();
    }
};

TEST(DefaultEventRegistry, BroadcastReachesEveryActorAndSkipsOnesSpawnedByAHandler) {
    reset();
    qb::Main engine;
    // The spawner takes the lowest sid so the reallocation happens BEFORE the remaining members
    // are dispatched: those dispatches then read the snapshot, not the moved vector.
    engine.addActor<Spawner>(0);
    for (int i = 0; i < kMembers; ++i)
        engine.addActor<Member>(0);
    engine.addActor<Trigger>(0);
    engine.start(false);
    EXPECT_FALSE(engine.hasError());
    EXPECT_FALSE(g_timeout) << "at least one member was not reached by the broadcast";
    EXPECT_EQ(g_members_gone, kMembers);
    EXPECT_EQ(g_late_gone, kLate);
    EXPECT_FALSE(g_late_got_kill) << "an actor spawned during the broadcast received it";
}

// ---- service actor ---------------------------------------------------------------------------

struct RegistrySvcTag {};

class Svc
    : public qb::ServiceActor<RegistrySvcTag>
    , public qb::ICallback {
    std::chrono::steady_clock::time_point _since{std::chrono::steady_clock::now()};

public:
    ~Svc() override {
        ++g_target_gone;
    }
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (std::chrono::steady_clock::now() - _since > kWatchdog) {
            g_timeout = true;
            kill();
        }
    }
};

TEST(DefaultEventRegistry, ServiceActorIsReachedThroughTheSameSlot) {
    reset();
    qb::Main   engine;
    const auto svc = engine.addActor<Svc>(0);
    ASSERT_TRUE(svc.is_valid());
    engine.addActor<Killer>(0, svc);
    engine.start(false);
    EXPECT_FALSE(engine.hasError());
    EXPECT_FALSE(g_timeout);
    EXPECT_EQ(g_target_gone, 1);
}

} // namespace default_event_registry_test
