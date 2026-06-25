/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/system/event-router.cpp
 * @brief The four compile-time event-routing primitives — pure logic, no engine.
 *
 * Exercises `qb/system/event/router.h` over *local mocks* (`ActorId`/`RawEvent`/`FakeActor`),
 * deliberately isolated from the real actor runtime so only the router's dispatch/dispose
 * logic is under test: NO `qb::Main`, NO event loop, NO daemon, fully deterministic.
 *
 *   sesh — single-event / single-handler          (route N → exactly N handler hits)
 *   semh — single-event / multi-handler            (subscribe/unsubscribe + broadcast fan-out)
 *   mesh — multi-event / single-handler            (type-id keyed resolver)
 *   memh — multi-event / multi-handler + onError   (per-route error callback on an unknown id)
 *
 * Every expected count is *derived from the routing topology* (subscriptions × iterations),
 * not echoed from a value the test set, so each is a real invariant. Two orthogonal oracles
 * are exercised per event flavour:
 *   - the handler-hit oracle: `TestEvent::_count` / `TestConstEvent::_count` count `on()` calls;
 *   - the disposal oracle: `TestDestroyEvent::_count` counts *destructor* runs (it is the only
 *     event whose dtor increments `_count`), so it proves the `_CleanEvent` dispose path —
 *     `_CleanEvent=true` destroys the `alive=false` payload on every route, `_CleanEvent=false`
 *     leaves it untouched (count stays 0). Both directions are now asserted on all four routers.
 *
 * Strengthened over the original: the two MEMH `_CleanEvent=false` cases (previously commented
 * out) are re-enabled, the `6144` destroy-count is derived in-line (was a fitted magic number),
 * and the MEMH `onError` callback — previously an empty lambda that was never observed — is now
 * asserted to actually fire for an unregistered event id.
 */

#include <gtest/gtest.h>
#include <qb/system/event/router.h>

struct ActorId {
    uint32_t _id;

    ActorId() = default;
    explicit ActorId(uint32_t id) noexcept
        : _id(id) {}

    explicit
    operator uint32_t() const noexcept {
        return _id;
    }

    [[nodiscard]] bool
    is_valid() const noexcept {
        return _id != 0;
    }
    [[nodiscard]] bool
    is_broadcast() const noexcept {
        return _id == std::numeric_limits<uint32_t>::max();
    }
    bool
    operator==(ActorId const &rhs) const noexcept {
        return _id == rhs._id;
    }

    static const ActorId broadcastId;
};

const ActorId ActorId::broadcastId = ActorId{std::numeric_limits<uint32_t>::max()};

namespace std {
template <>
struct hash<::ActorId> {
    std::size_t
    operator()(ActorId const &val) const noexcept {
        return static_cast<uint32_t>(val);
    }
};
} // namespace std

template <typename T>
struct type {
    constexpr static void
    id() {}
};

struct RawEvent {
    RawEvent()                 = default;
    RawEvent(RawEvent const &) = delete;
    using id_type              = const char *;
    using id_handler_type      = ActorId;

    //    template<typename T>
    //    constexpr static id_type type_to_id() { return
    //    static_cast<id_type>(reinterpret_cast<std::size_t>(&type<T>::id)); }

    template <typename T>
    constexpr static id_type
    type_to_id() {
        return typeid(T).name();
    }

    id_type         id = "RawEvent";
    id_handler_type dest{};
    id_handler_type source{};
    bool            alive = true;

    [[nodiscard]] id_type
    getID() const noexcept {
        return id;
    }
    [[nodiscard]] bool
    is_alive() const noexcept {
        return alive;
    }
    [[nodiscard]] id_handler_type
    getDestination() const noexcept {
        return dest;
    }
};

struct TestEvent : public RawEvent {
    static std::size_t _count;

    TestEvent() {
        id = RawEvent::type_to_id<TestEvent>();
    }
};

struct TestConstEvent : public RawEvent {
    static std::size_t _count;

    TestConstEvent() {
        id = RawEvent::type_to_id<TestConstEvent>();
    }
};

struct TestDestroyEvent : public RawEvent {
    static std::size_t _count;

    TestDestroyEvent() {
        alive = false;
        id    = RawEvent::type_to_id<TestDestroyEvent>();
    }

    ~TestDestroyEvent() {
        ++_count;
    }
};

std::size_t TestEvent::_count        = 0;
std::size_t TestConstEvent::_count   = 0;
std::size_t TestDestroyEvent::_count = 0;

void
reset_all_event_counts() {
    TestEvent::_count        = 0;
    TestConstEvent::_count   = 0;
    TestDestroyEvent::_count = 0;
}

struct FakeActor {
    ActorId _id;

    explicit FakeActor(uint32_t id)
        : _id(id) {}
    FakeActor(FakeActor const &) = delete;

    [[nodiscard]] ActorId
    id() const {
        return _id;
    }

    [[nodiscard]] bool
    is_alive() const noexcept {
        return true;
    }

    void
    on(TestEvent &event) {
        ++TestEvent::_count;
    }

    void
    on(TestConstEvent const &event) const {
        ++TestConstEvent::_count;
    }

    void
    on(TestDestroyEvent const &event) const {}
};

template <typename _Event, bool _CleanEvent = true>
void
Test_SESH(std::size_t expected_count) {
    _Event    event;
    FakeActor actor(1);

    _Event::_count = 0;
    for (std::size_t i = 0; i < 1024u; ++i) {
        qb::router::sesh<_Event, FakeActor>(actor).template route<_CleanEvent>(event);
    }
    EXPECT_EQ(_Event::_count, expected_count);
}

TEST(EventRouting, SESH) {
    Test_SESH<TestEvent>(1024);
    Test_SESH<const TestConstEvent>(1024);
    Test_SESH<TestDestroyEvent>(1024);
    Test_SESH<TestDestroyEvent, false>(0);
}

template <typename _Event, typename _Handler = void, bool _CleanEvent = true>
void
Test_SEMH(std::size_t expected_count) {
    std::remove_const_t<_Event> event;
    FakeActor                   actor1(1);
    FakeActor                   actor2(2);
    FakeActor                   actor3(3);

    qb::router::semh<_Event, _Handler> semhRouter;

    semhRouter.subscribe(actor1);
    semhRouter.subscribe(actor2);
    semhRouter.subscribe(actor3);
    semhRouter.unsubscribe(actor1.id());

    _Event::_count = 0;
    for (std::size_t i = 0; i < 1024u; ++i) {
        for (std::size_t j = 1; j < 4u; ++j) {
            event.dest = ActorId(j);
            semhRouter.template route<_CleanEvent>(event);
        }
        event.dest = ActorId::broadcastId;
        semhRouter.template route<_CleanEvent>(event);
    }
    EXPECT_EQ(_Event::_count, expected_count);
}

TEST(EventRouting, SEMH) {
    Test_SEMH<TestEvent>(4096);
    Test_SEMH<const TestConstEvent>(4096);
    Test_SEMH<TestDestroyEvent>(4096);
    Test_SEMH<TestDestroyEvent, void, false>(0);
    Test_SEMH<TestEvent, FakeActor>(4096);
    Test_SEMH<const TestConstEvent, FakeActor>(4096);
    Test_SEMH<TestDestroyEvent, FakeActor>(4096);
    Test_SEMH<TestDestroyEvent, FakeActor, false>(0);
}

template <typename _Event, bool _CleanEvent = true>
void
Test_MESH(std::size_t expected_count) {
    std::remove_const_t<_Event> event;
    FakeActor                   actor1(1);

    qb::router::mesh<RawEvent, FakeActor, _CleanEvent> meshRouter(actor1);

    meshRouter.template subscribe<_Event>();

    _Event::_count = 0;
    for (std::size_t i = 0; i < 1024u; ++i) {
        meshRouter.route(event);
    }
    EXPECT_EQ(_Event::_count, expected_count);
}

TEST(EventRouting, MESH) {
    Test_MESH<TestEvent>(1024);
    Test_MESH<const TestConstEvent>(1024);
    Test_MESH<TestDestroyEvent>(1024);
    Test_MESH<TestDestroyEvent, false>(0);
}

template <typename _Event, bool _CleanEvent = true, typename _Handler = void>
void
Test_MEMH(std::size_t expected_count) {
    std::remove_const_t<_Event>                       event;
    FakeActor                                         actor1(1);
    FakeActor                                         actor2(2);
    FakeActor                                         actor3(3);
    FakeActor                                         actor4(4);
    FakeActor                                         actor5(5);
    qb::router::memh<RawEvent, _CleanEvent, _Handler> memhRouter;

    memhRouter.template subscribe<_Event>(actor1);
    memhRouter.template subscribe<_Event>(actor2);
    memhRouter.template subscribe<_Event>(actor3);
    memhRouter.template subscribe<_Event>(actor4);
    memhRouter.template subscribe<_Event>(actor5);
    memhRouter.unsubscribe(actor1.id());
    memhRouter.unsubscribe(actor2);
    memhRouter.template unsubscribe<_Event>(actor3);

    _Event::_count     = 0;
    const auto onError = [](auto &) {
    };
    for (std::size_t i = 0; i < 1024u; ++i) {
        for (std::size_t j = 1; j < 6u; ++j) {
            event.dest = ActorId(j);
            memhRouter.route(event, onError);
        }
        event.dest = ActorId::broadcastId;
        memhRouter.route(event, onError);
    }
    EXPECT_EQ(_Event::_count, expected_count);
}

TEST(EventRouting, MEMH) {
    // --- handler-hit oracle (TestEvent / TestConstEvent count on() calls) ------
    // Per iteration the loop routes to 5 explicit dests (j=1..5) + 1 broadcast. Actors
    // 1,2,3 are unsubscribed (unsubscribe by id / by actor / by <Event>(actor)), so only
    // 4 and 5 remain subscribed. Targeted route to dest 4 or 5 hits its actor (2 of the 5
    // targeted routes hit), routes to 1/2/3 hit nobody, and each broadcast hits both
    // survivors (2 hits). Per iteration: 2 (targeted) + 2 (broadcast) = 4 → ×1024 = 4096.
    Test_MEMH<TestEvent>(4096);
    Test_MEMH<const TestConstEvent>(4096);

    // --- disposal oracle (TestDestroyEvent::_count counts DESTRUCTOR runs) ------
    // With _CleanEvent=true the router disposes the (alive=false) event after EVERY route,
    // regardless of whether a handler matched — FakeActor::on(TestDestroyEvent) never
    // touches _count, so _count is purely the dispose/dtor count. Routes per iteration:
    // 5 targeted + 1 broadcast = 6 → 6 × 1024 = 6144 destructor calls. (The dest-1/2/3
    // routes still dispose even though no live handler matches.)
    constexpr std::size_t kRoutesPerIter = 6;        // 5 targeted (j=1..5) + 1 broadcast
    constexpr std::size_t kDisposed      = kRoutesPerIter * 1024u; // = 6144
    static_assert(kDisposed == 6144u, "MEMH destroy-count derivation");
    Test_MEMH<TestDestroyEvent>(kDisposed);

    // _CleanEvent=false: dispose() is skipped on every route, so NO destructor runs during
    // routing. The assertion executes before the stack `event` leaves scope (whose own dtor
    // would be the only +1), so the disposal count is exactly 0. (Re-enabled: was commented
    // out, leaving the MEMH clean-event=false path silently untested — every other router
    // tests it.)
    Test_MEMH<TestDestroyEvent, false>(0);

    // Same matrix against the homogeneous (_Handler=FakeActor) MEMH specialization.
    Test_MEMH<TestEvent, true, FakeActor>(4096);
    Test_MEMH<const TestConstEvent, true, FakeActor>(4096);
    Test_MEMH<TestDestroyEvent, true, FakeActor>(kDisposed);
    Test_MEMH<TestDestroyEvent, false, FakeActor>(0);
}

// ---------------------------------------------------------------------------
// The MEMH `onError` callback: a route for an event id that was never subscribed
// must invoke the caller-supplied error handler (and must NOT dispatch/dispose).
// The original suite passed an empty lambda here, so the error path executed but
// was never observed; this proves it actually fires.
// ---------------------------------------------------------------------------

// An event type the router is never subscribed to. Distinct id_type (typeid name)
// ⇒ memh::route() takes the `onError` branch.
struct UnregisteredEvent : public RawEvent {
    UnregisteredEvent() {
        id = RawEvent::type_to_id<UnregisteredEvent>();
    }
};

TEST(EventRouting, MEMHonErrorFiresForUnregisteredEvent) {
    FakeActor                              actor1(1);
    qb::router::memh<RawEvent, true, void> memhRouter;
    // Subscribe TestEvent only; UnregisteredEvent is deliberately NOT registered.
    memhRouter.subscribe<TestEvent>(actor1);

    std::size_t       on_error_calls = 0;
    const ActorId    *seen_dest      = nullptr;
    UnregisteredEvent unknown;
    unknown.dest = ActorId(1);

    const auto onError = [&](RawEvent &e) {
        ++on_error_calls;
        seen_dest = &e.dest; // the unrouted event is handed verbatim to the callback
    };

    // A registered id routes normally (no onError); an unregistered id must call onError.
    TestEvent::_count = 0;
    TestEvent known;
    known.dest = ActorId(1);
    memhRouter.route(known, onError);
    EXPECT_EQ(on_error_calls, 0u) << "a registered event id must NOT take the error path";
    EXPECT_EQ(TestEvent::_count, 1u) << "registered event must be dispatched to its handler";

    memhRouter.route(unknown, onError);
    EXPECT_EQ(on_error_calls, 1u) << "an unregistered event id MUST invoke onError exactly once";
    EXPECT_EQ(seen_dest, &unknown.dest) << "onError receives the very event that could not be routed";

    // Fire it again to prove it is repeatable, not a one-shot.
    memhRouter.route(unknown, onError);
    EXPECT_EQ(on_error_calls, 2u);
    EXPECT_EQ(TestEvent::_count, 1u) << "the unknown event never reached a handler";
}