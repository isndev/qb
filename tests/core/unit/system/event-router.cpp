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

#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>
#include <qb/core/ActorId.h>
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

// The router ends the lifetime of disposable events. Keep storage, reconstruct
// before the next route, and only destroy a lifetime still owned by the caller.
template <typename Event, bool Clean>
class routed_event_storage {
    using value_type = std::remove_const_t<Event>;
    alignas(value_type) unsigned char bytes[sizeof(value_type)];
    value_type *value = new (bytes) value_type{};

public:
    ~routed_event_storage() {
        if (value)
            std::destroy_at(value);
    }
    template <typename Route>
    void
    route(Route &&route, ActorId destination = ActorId{}) {
        if (!value)
            value = new (bytes) value_type{};
        value->dest   = destination;
        auto *current = value;
        if constexpr (Clean)
            if (!current->is_alive())
                value = nullptr;
        route(*current);
    }
};

template <typename _Event, bool _CleanEvent = true>
void
Test_SESH(std::size_t expected_count) {
    routed_event_storage<_Event, _CleanEvent> event;
    FakeActor                                 actor(1);

    _Event::_count = 0;
    for (std::size_t i = 0; i < 1024u; ++i) {
        event.route([&](auto &current) { qb::router::sesh<_Event, FakeActor>(actor).template route<_CleanEvent>(current); });
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
    routed_event_storage<_Event, _CleanEvent> event;
    FakeActor                                 actor1(1);
    FakeActor                                 actor2(2);
    FakeActor                                 actor3(3);

    qb::router::semh<_Event, _Handler> semhRouter;

    semhRouter.subscribe(actor1);
    semhRouter.subscribe(actor2);
    semhRouter.subscribe(actor3);
    semhRouter.unsubscribe(actor1.id());

    _Event::_count = 0;
    for (std::size_t i = 0; i < 1024u; ++i) {
        for (std::size_t j = 1; j < 4u; ++j) {
            event.route([&](auto &current) { semhRouter.template route<_CleanEvent>(current); }, ActorId(j));
        }
        event.route([&](auto &current) { semhRouter.template route<_CleanEvent>(current); }, ActorId::broadcastId);
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
    routed_event_storage<_Event, _CleanEvent> event;
    FakeActor                                 actor1(1);

    qb::router::mesh<RawEvent, FakeActor, _CleanEvent> meshRouter(actor1);

    meshRouter.template subscribe<_Event>();

    _Event::_count = 0;
    for (std::size_t i = 0; i < 1024u; ++i) {
        event.route([&](auto &current) { meshRouter.route(current); });
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
    routed_event_storage<_Event, _CleanEvent>         event;
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
            event.route([&](auto &current) { memhRouter.route(current, onError); }, ActorId(j));
        }
        event.route([&](auto &current) { memhRouter.route(current, onError); }, ActorId::broadcastId);
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
    constexpr std::size_t kRoutesPerIter = 6;                      // 5 targeted (j=1..5) + 1 broadcast
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
// =============================================================================
// memh — the per-router disposer memo.
//
// `_disposers` is `static`: one map for the whole process, and every read of it used to take a
// process-global mutex. That read sits on a path reached at full event rate — an event whose type
// has no resolver on this core takes `route()`'s `else` branch, which is exactly what
// `broadcast<E>()` produces on every core where no actor subscribed to `E` (the engine treats it
// as normal: `VirtualCore::__receive_events__`'s onError only logs for non-broadcast). So a
// broadcasting system serialised all of its cores through one lock. Measured on this very router,
// per event: 10.1 ns at 1 thread, 35.1 at 2, 125.9 at 4, 256.6 at 8 — throughput going DOWN as
// cores were added (98.7 -> 31.2 M events/s). With the memo: 5.4-5.8 ns flat, 178 -> 1379 M/s.
//
// The memo's one subtle obligation is below: a MISS must not be memoised. Disposers are registered
// lazily (`ensure_disposer<>()` at the enqueue funnels, `subscribe<E>()`), so a router can very
// well see a type before its disposer exists — and if it remembered that absence it would go on
// leaking that type's payload forever.
// =============================================================================

namespace {

struct LateDisposeEvent : public RawEvent {
    static std::size_t _count;
    LateDisposeEvent() {
        alive = false; // route() only disposes a non-alive payload
        id    = RawEvent::type_to_id<LateDisposeEvent>();
    }
    ~LateDisposeEvent() {
        ++_count;
    }
};
std::size_t LateDisposeEvent::_count = 0;

} // namespace

TEST(EventRouting, MemhDisposerMemoDoesNotCacheAMiss) {
    // The disposer registry is process-wide and has no removal: once this case has registered
    // LateDisposeEvent's disposer, a later repetition in the same process (--gtest_repeat) cannot
    // see the type unregistered again, and says so instead of failing its precondition. No other
    // case names LateDisposeEvent, so a shuffled order does not matter -- only a second pass does.
    static bool registered = false;
    if (std::exchange(registered, true))
        GTEST_SKIP() << "LateDisposeEvent's disposer was registered in an earlier repetition of this process: its first "
                        "registration happens once";
    qb::router::memh<RawEvent, true, void> router;
    const auto                             noop = [](auto &) {
    };

    // Before any disposer exists for this type, the router must find none — and must not remember
    // that. Nothing is registered on the router either, so this takes the `else` branch.
    LateDisposeEvent::_count = 0;
    {
        LateDisposeEvent early;
        router.route(early, noop);
    }
    // Exactly one destructor ran: `early`'s own, at end of scope. The router disposed nothing.
    ASSERT_EQ(LateDisposeEvent::_count, 1u) << "precondition: with no disposer registered, route() must dispose nothing";

    // The disposer appears later — the ordinary lifecycle, since enqueue funnels register on first
    // use and actors subscribe at runtime.
    qb::router::ensure_disposer<RawEvent, LateDisposeEvent>();

    LateDisposeEvent::_count = 0;
    {
        alignas(LateDisposeEvent) unsigned char storage[sizeof(LateDisposeEvent)];
        auto                                   *late = new (storage) LateDisposeEvent{};
        router.route(*late, noop);
        EXPECT_EQ(LateDisposeEvent::_count, 1u) << "the router did not pick up a disposer registered after its first lookup missed: a "
                                                   "negative result was memoised, so every event of this type leaks its payload for the "
                                                   "life of the process";
    }
    EXPECT_EQ(LateDisposeEvent::_count, 1u) << "route() must end exactly one payload lifetime";
}

TEST(EventRouting, MemhDisposerMemoStaysCorrectAcrossRepeatedRoutes) {
    qb::router::ensure_disposer<RawEvent, TestDestroyEvent>();

    qb::router::memh<RawEvent, true, void> router;
    const auto                             noop = [](auto &) {
    };

    // Every route must dispose exactly once — the memo must not skip, double-dispose, or go stale
    // once it is warm. Counting destructor runs is the oracle; the objects here are constructed in
    // place and never destroyed by the scope, so the count is attributable to route() alone.
    constexpr std::size_t kRoutes = 4096u;
    TestDestroyEvent::_count      = 0;

    alignas(TestDestroyEvent) unsigned char storage[sizeof(TestDestroyEvent)];
    for (std::size_t i = 0; i < kRoutes; ++i) {
        auto *ev = new (storage) TestDestroyEvent{};
        router.route(*ev, noop);
    }

    EXPECT_EQ(TestDestroyEvent::_count, kRoutes) << "each unrouted event must have its payload disposed exactly once";
}

// The dense table grows GEOMETRICALLY under one-id-at-a-time subscription -- a core spawning
// actors subscribes one new sid per spawn, and the previous growth rule reserved
// `max(idx + 1, size() * 2)` on every insert: one past what the last doubling left, so every
// new id reallocated and copied the whole table. O(n) per subscribe, O(n^2) per core, one table
// per event type; savina/fib (57 313 actors born and dead in one window) measured 43 s where
// CAF measures 86 ms. The oracle is the address of slot 0's value: it moves on every
// reallocation, so 60 000 sequential inserts must move it O(log n) times, never O(n).
TEST(EventRouting, DenseKeyTableGrowsGeometricallyUnderSequentialInserts) {
    qb::router::internal::key_table<std::uint16_t, int> table;
    table.insert_or_assign(std::uint16_t{0}, 1);
    const int              *anchor   = table.find(std::uint16_t{0});
    std::size_t             moves    = 0;
    constexpr std::uint32_t kInserts = 60000u;
    for (std::uint32_t i = 1; i < kInserts; ++i) {
        table.insert_or_assign(static_cast<std::uint16_t>(i), static_cast<int>(i));
        const int *now = table.find(std::uint16_t{0});
        ASSERT_NE(now, nullptr);
        if (now != anchor) {
            ++moves;
            anchor = now;
        }
    }
    EXPECT_LE(moves, 32u) << "the dense table reallocated " << moves << " times for " << kInserts
                          << " sequential inserts: growth is by size, not by capacity, and every subscription "
                             "of a new id copies the whole table";
    for (std::uint32_t i = 0; i < kInserts; i += 997) {
        const int *v = table.find(static_cast<std::uint16_t>(i));
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(*v, static_cast<int>(i == 0 ? 1 : i));
    }
}

namespace router_throw_test {

struct TrivialEvent : RawEvent {
    TrivialEvent() {
        dest = ActorId::broadcastId;
    }
};
static_assert(std::is_trivially_destructible_v<TrivialEvent>);

struct TrivialHandler {
    ActorId target{1};
    bool    throw_now = true;
    int     calls     = 0;
    ActorId
    id() const noexcept {
        return target;
    }
    bool
    is_alive() const noexcept {
        return true;
    }
    void
    on(TrivialEvent const &) {
        ++calls;
        if (throw_now)
            throw std::runtime_error("trivial handler failed");
    }
};

template <typename Router>
void
check_trivial_route_after_throw() {
    Router         router;
    TrivialHandler handler;
    router.subscribe(handler);

    TrivialEvent event;
    EXPECT_THROW(router.template route<true>(event), std::runtime_error);
    EXPECT_EQ(handler.calls, 1);

    handler.throw_now = false;
    router.template route<true>(event);
    EXPECT_EQ(handler.calls, 2);
}

TEST(EventRouting, GenericSemhPropagatesTrivialHandlerThrow) {
    check_trivial_route_after_throw<qb::router::semh<TrivialEvent, TrivialHandler>>();
}

TEST(EventRouting, HeterogeneousSemhPropagatesTrivialHandlerThrow) {
    check_trivial_route_after_throw<qb::router::semh<TrivialEvent, void>>();
}

struct OwnedEvent : RawEvent {
    static int           destroyed;
    std::unique_ptr<int> payload{std::make_unique<int>(7)};
    OwnedEvent() {
        alive = false;
        id    = type_to_id<OwnedEvent>();
    }
    ~OwnedEvent() {
        ++destroyed;
    }
};
int OwnedEvent::destroyed = 0;

struct Handler {
    ActorId target{1};
    bool    throw_now = true;
    int     calls     = 0;
    ActorId
    id() const noexcept {
        return target;
    }
    bool
    is_alive() const noexcept {
        return true;
    }
    void
    on(OwnedEvent const &) {
        ++calls;
        if (throw_now)
            throw std::runtime_error("router handler failed");
    }
};

template <typename Router>
void
check_clean_route_after_throw() {
    Router  router;
    Handler handler;
    router.subscribe(handler);
    OwnedEvent::destroyed = 0;
    alignas(OwnedEvent) unsigned char storage[sizeof(OwnedEvent)];

    auto *fault = new (storage) OwnedEvent;
    fault->dest = ActorId::broadcastId;
    bool threw  = false;
    try {
        router.template route<true>(*fault);
    } catch (std::runtime_error const &) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(OwnedEvent::destroyed, 1) << "route<true> must dispose the faulting payload";
    if (OwnedEvent::destroyed != 1) {
        fault->~OwnedEvent(); // release the payload on the red baseline
        return;
    }

    handler.throw_now = false;
    auto *next        = new (storage) OwnedEvent;
    next->dest        = ActorId::broadcastId;
    router.template route<true>(*next); // stale snapshot entries must not survive the throw
    EXPECT_EQ(handler.calls, 2);
    EXPECT_EQ(OwnedEvent::destroyed, 2);

    handler.throw_now = true;
    auto *unclean     = new (storage) OwnedEvent;
    unclean->dest     = ActorId{1};
    EXPECT_THROW(router.template route<false>(*unclean), std::runtime_error);
    EXPECT_EQ(OwnedEvent::destroyed, 2) << "route<false> leaves disposal with the caller";
    unclean->~OwnedEvent();
    EXPECT_EQ(OwnedEvent::destroyed, 3);

    auto *unicast_fault = new (storage) OwnedEvent;
    unicast_fault->dest = ActorId{1};
    EXPECT_THROW(router.template route<true>(*unicast_fault), std::runtime_error);
    EXPECT_EQ(OwnedEvent::destroyed, 4) << "route<true> must dispose a throwing unicast too";
    if (OwnedEvent::destroyed != 4)
        unicast_fault->~OwnedEvent(); // release the payload on the red baseline
}

TEST(EventRouting, GenericSemhDisposesAfterThrowWhenCleanRequested) {
    check_clean_route_after_throw<qb::router::semh<OwnedEvent, Handler>>();
}

TEST(EventRouting, HeterogeneousSemhDisposesAfterThrowWhenCleanRequested) {
    check_clean_route_after_throw<qb::router::semh<OwnedEvent, void>>();
}

TEST(EventRouting, MemhOnErrorThrowDisposesOwnedEvent) {
    qb::router::ensure_disposer<RawEvent, OwnedEvent>();
    qb::router::memh<RawEvent, true, void> router;
    OwnedEvent::destroyed = 0;
    alignas(OwnedEvent) unsigned char storage[sizeof(OwnedEvent)];
    auto                             *event = new (storage) OwnedEvent;
    event->dest                             = ActorId{1};
    bool threw                              = false;
    try {
        router.route(*event, [](RawEvent &) { throw std::runtime_error("unhandled event callback failed"); });
    } catch (std::runtime_error const &) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(OwnedEvent::destroyed, 1);
    if (OwnedEvent::destroyed != 1)
        event->~OwnedEvent(); // release the payload on the red baseline
}

TEST(EventRouting, MemhOnErrorThrowPreservesTransferredOwnership) {
    qb::router::ensure_disposer<RawEvent, OwnedEvent>();
    qb::router::memh<RawEvent, true, void> router;
    OwnedEvent::destroyed = 0;
    alignas(OwnedEvent) unsigned char storage[sizeof(OwnedEvent)];
    auto                             *event = new (storage) OwnedEvent;
    event->dest                             = ActorId{1};
    EXPECT_THROW(router.route(*event,
                              [](RawEvent &e) {
                                  e.alive = true; // models reply/forward handing ownership to another pipe
                                  throw std::runtime_error("error callback failed after transfer");
                              }),
                 std::runtime_error);
    EXPECT_EQ(OwnedEvent::destroyed, 0);
    event->~OwnedEvent(); // the caller owns the transferred payload in this isolated unit test
    EXPECT_EQ(OwnedEvent::destroyed, 1);
}

TEST(EventRouting, MemhOnErrorReturnPreservesTransferredOwnership) {
    qb::router::ensure_disposer<RawEvent, OwnedEvent>();
    qb::router::memh<RawEvent, true, void> router;
    OwnedEvent::destroyed = 0;
    alignas(OwnedEvent) unsigned char storage[sizeof(OwnedEvent)];
    auto                             *event = new (storage) OwnedEvent;
    event->dest                             = ActorId{1};
    router.route(*event, [](RawEvent &e) { e.alive = true; }); // reply/forward took ownership
    EXPECT_EQ(OwnedEvent::destroyed, 0);
    event->~OwnedEvent(); // the transferred owner releases it in this isolated unit test
    EXPECT_EQ(OwnedEvent::destroyed, 1);
}

} // namespace router_throw_test

TEST(EventRouting, ActorIdentityBanksSeparateOrdinaryAndServiceSlots) {
    qb::router::internal::key_table<qb::ActorId, int> table;
    const auto                                        ordinary = qb::ActorId::ordinary(7, 2, 9);
    const auto                                        service  = qb::ActorId::service_instance(7, 2, 9);
    table.insert_or_assign(ordinary, 17);
    table.insert_or_assign(service, 29);
    ASSERT_NE(table.find(ordinary), nullptr);
    ASSERT_NE(table.find(service), nullptr);
    EXPECT_EQ(*table.find(ordinary), 17);
    EXPECT_EQ(*table.find(service), 29);
    EXPECT_EQ(table.find(qb::ActorId::service_address(7, 2)), nullptr);
    std::size_t visited = 0;
    int         sum     = 0;
    table.for_each([&](qb::ActorId key, int value) {
        EXPECT_TRUE(key == ordinary || key == service);
        if (visited == 0) {
            EXPECT_EQ(key, service) << "preserve the former service-first traversal";
        }
        ++visited;
        sum += value;
    });
    EXPECT_EQ(visited, 2u);
    EXPECT_EQ(sum, 46);
    table.erase(qb::ActorId::ordinary(8, 2, 9)); // exactly beyond the primary bank's end
    table.erase(service);
    EXPECT_EQ(table.find(service), nullptr);
    ASSERT_NE(table.find(ordinary), nullptr);
    EXPECT_EQ(*table.find(ordinary), 17);
    table.erase(ordinary);
    EXPECT_EQ(table.find(ordinary), nullptr);
}

TEST(EventRouting, ActorIdentityLookupAndEraseRejectStaleIncarnations) {
    qb::router::internal::key_table<qb::ActorId, int> table;
    for (bool service : {false, true}) {
        const auto old  = service ? qb::ActorId::service_instance(42, 3, 0x12345678) : qb::ActorId::ordinary(42, 3, 0x12345678);
        const auto next = service ? qb::ActorId::service_instance(42, 3, 0x12345679) : qb::ActorId::ordinary(42, 3, 0x12345679);
        table.insert_or_assign(old, 1);
        EXPECT_EQ(table.find(next), nullptr);
        table.erase(old);
        table.insert_or_assign(next, 2);
        EXPECT_EQ(table.find(old), nullptr);
        table.erase(old);
        const auto foreign = service ? qb::ActorId::service_instance(42, 4, 0x12345679) : qb::ActorId::ordinary(42, 4, 0x12345679);
        table.erase(foreign);
        if (service)
            table.erase(qb::ActorId::service_address(42, 3));
        ASSERT_NE(table.find(next), nullptr);
        EXPECT_EQ(*table.find(next), 2);
        table.erase(next);
        EXPECT_EQ(table.find(next), nullptr);
    }
}

TEST(EventRouting, ActorIdentityHighSlotAndServiceTagRemainIndependent) {
    qb::router::internal::key_table<qb::ActorId, int> table;
    const auto                                        high    = qb::ActorId::ordinary(200000, 255, 0x89abcdef);
    const auto                                        service = qb::ActorId::service_instance(65534, 255, 0xfedcba98);
    table.insert_or_assign(high, 1);
    table.insert_or_assign(service, 2);
    ASSERT_NE(table.find(high), nullptr);
    ASSERT_NE(table.find(service), nullptr);
    EXPECT_EQ(*table.find(high), 1);
    EXPECT_EQ(*table.find(service), 2);
    EXPECT_EQ(table.find(qb::ActorId::ordinary(200000, 254, 0x89abcdef)), nullptr);
    EXPECT_EQ(table.find(qb::ActorId::service_instance(65534, 254, 0xfedcba98)), nullptr);
    table.erase(qb::ActorId::ordinary(200000, 254, 0x89abcdef));
    table.erase(qb::ActorId::service_instance(65534, 254, 0xfedcba98));
    ASSERT_NE(table.find(high), nullptr);
    ASSERT_NE(table.find(service), nullptr);
    EXPECT_EQ(*table.find(high), 1);
    EXPECT_EQ(*table.find(service), 2);
}

TEST(EventRouting, ActorIdentityEraseReachesServiceWhenPrimaryBankIsEmpty) {
    qb::router::internal::key_table<qb::ActorId, int> table;
    const auto                                        service = qb::ActorId::service_instance(7, 255, 0x12345678);
    table.insert_or_assign(service, 29);
    table.erase(qb::ActorId::ordinary(7, 255, 0x12345678));
    table.erase(qb::ActorId::service_address(7, 255));
    table.erase(qb::BroadcastId(255));
    ASSERT_NE(table.find(service), nullptr);
    EXPECT_EQ(*table.find(service), 29);
    table.erase(service);
    EXPECT_EQ(table.find(service), nullptr);
}

namespace event_router_partitioned_erase_test {

struct Key {
    std::uint16_t slot       = 0;
    std::uint16_t generation = 0;
    bool          secondary  = false;

    bool operator==(Key const &) const noexcept = default;
};

struct Value {
    static inline unsigned resets = 0;
    int                    number = 0;

    Value() = default;
    explicit Value(int value) noexcept
        : number(value) {}
    Value(Value const &) = default;
    Value &
    operator=(Value const &other) noexcept {
        resets += other.number == 0;
        number = other.number;
        return *this;
    }
};

} // namespace event_router_partitioned_erase_test

namespace qb::router {

template <>
struct dense_index<event_router_partitioned_erase_test::Key> {
    using Key                                        = event_router_partitioned_erase_test::Key;
    static constexpr bool        enabled             = true;
    static constexpr bool        partitioned         = true;
    static constexpr std::size_t max_index           = 15;
    static constexpr std::size_t secondary_max_index = 15;
    // No primary_excludes_secondary trait: both projections intentionally overlap.
    static bool
    secondary(Key const &key) noexcept {
        return key.secondary;
    }
    static std::size_t
    of(Key const &key) noexcept {
        return key.slot;
    }
    static std::size_t
    primary_of(Key const &key) noexcept {
        return key.slot;
    }
    static std::size_t
    secondary_of(Key const &key) noexcept {
        return key.slot;
    }
};

} // namespace qb::router

TEST(EventRouting, PartitionedEraseKeepsOverlappingProjectionsWithoutTheOptionalTrait) {
    using Key = event_router_partitioned_erase_test::Key;
    qb::router::internal::key_table<Key, int> table;
    const Key                                 primary{7, 1, false};
    const Key                                 secondary{7, 1, true};
    const Key                                 replacement{7, 2, true};
    table.insert_or_assign(primary, 17);
    table.insert_or_assign(secondary, 29);
    table.erase(secondary);
    EXPECT_EQ(table.find(secondary), nullptr);
    ASSERT_NE(table.find(primary), nullptr);
    EXPECT_EQ(*table.find(primary), 17);
    table.insert_or_assign(replacement, 31);
    table.erase(secondary);
    table.erase(primary);
    EXPECT_EQ(table.find(primary), nullptr);
    ASSERT_NE(table.find(replacement), nullptr);
    EXPECT_EQ(*table.find(replacement), 31);
}

TEST(EventRouting, ActorIdentityEraseDoesNotResetAnAlreadyUnusedSlot) {
    using Value = event_router_partitioned_erase_test::Value;
    for (bool service : {false, true}) {
        qb::router::internal::key_table<qb::ActorId, Value> table;
        const auto key = service ? qb::ActorId::service_instance(7, 2, 9) : qb::ActorId::ordinary(7, 2, 9);
        table.insert_or_assign(key, Value{17});
        Value::resets = 0;
        table.erase(key);
        EXPECT_EQ(Value::resets, 1u);
        table.erase(key); // the stale key is still stored, but used is now false
        EXPECT_EQ(Value::resets, 1u);
        EXPECT_EQ(table.find(key), nullptr);
    }
}

namespace event_router_identity_semh_test {

struct Probe {
    unsigned destroyed         = 0;
    unsigned payload_destroyed = 0;
    unsigned undelivered       = 0;
};

struct Payload {
    Probe        &probe;
    std::uint64_t stamp = 0xfedcba9876543210ULL;
    explicit Payload(Probe &value)
        : probe(value) {}
    ~Payload() {
        ++probe.payload_destroyed;
    }
};

template <typename Key>
struct OwnedEvent {
    using id_type         = std::uint16_t;
    using id_handler_type = Key;
    Key                      dest;
    Probe                   &probe;
    bool                     alive = false;
    std::unique_ptr<Payload> payload;

    OwnedEvent(Key target, Probe &value)
        : dest(target)
        , probe(value)
        , payload(std::make_unique<Payload>(value)) {}
    ~OwnedEvent() {
        ++probe.destroyed;
    }
    Key
    getDestination() const noexcept {
        return dest;
    }
    bool
    is_alive() const noexcept {
        return alive;
    }
    void
    __retire__() noexcept {
        alive = true;
    }
    static void
    __undelivered__(OwnedEvent &event) noexcept {
        ++event.probe.undelivered;
    }
};

template <typename Key>
struct Handler {
    Key      key;
    unsigned hits      = 0;
    bool     live      = true;
    bool     transfer  = false;
    bool     throw_now = false;

    Key
    id() const noexcept {
        return key;
    }
    bool
    is_alive() const noexcept {
        return live;
    }
    void
    on(OwnedEvent<Key> &event) {
        ++hits;
        ASSERT_NE(event.payload, nullptr);
        EXPECT_EQ(event.payload->stamp, 0xfedcba9876543210ULL);
        if (transfer)
            event.alive = true;
        if (throw_now)
            throw std::runtime_error("identity-keyed handler failed");
    }
};

// The router owns the placement-constructed lifetime; inspect only the external
// probe after dispatch, and clean up explicitly if a faulty router left it alive.
template <typename Key>
void
route_owned(qb::router::semh<OwnedEvent<Key>> &router, Key target, Probe &probe) {
    using Event = OwnedEvent<Key>;
    alignas(Event) unsigned char storage[sizeof(Event)];
    const auto                   destroyed = probe.destroyed;
    const auto                   released  = probe.payload_destroyed;
    auto                        *event     = new (storage) Event(target, probe);
    router.template route<true>(*event);
    EXPECT_EQ(probe.destroyed, destroyed + 1u);
    EXPECT_EQ(probe.payload_destroyed, released + 1u);
    if (probe.destroyed == destroyed)
        event->~Event();
}

TEST(EventRouting, ActorIdentitySemhRoutesOnlyTheCurrentCompleteIdentityAndBroadcastsBothBanks) {
    Probe                                     probe;
    qb::router::semh<OwnedEvent<qb::ActorId>> router;
    const auto                                ordinary_id = qb::ActorId::ordinary(7, 3, 0x12345678);
    const auto                                service_id  = qb::ActorId::service_instance(7, 3, 0x12345678);
    Handler<qb::ActorId>                      ordinary{ordinary_id};
    Handler<qb::ActorId>                      service{service_id};
    ASSERT_NE(ordinary_id.raw64() >> 32u, 0u);
    ASSERT_NE(ordinary_id.raw64(), service_id.raw64());

    router.subscribe(service); // a service must route even while the primary bank is empty
    route_owned(router, service_id, probe);
    EXPECT_EQ(service.hits, 1u);
    router.subscribe(ordinary);
    route_owned(router, ordinary_id, probe);
    route_owned(router, service_id, probe);
    EXPECT_EQ(ordinary.hits, 1u);
    EXPECT_EQ(service.hits, 2u);

    route_owned(router, qb::ActorId::ordinary(7, 3, 0x12345677), probe);
    route_owned(router, qb::ActorId::ordinary(7, 4, 0x12345678), probe);
    route_owned(router, qb::ActorId::service_instance(7, 3, 0x12345677), probe);
    route_owned(router, qb::ActorId::service_instance(7, 4, 0x12345678), probe);
    // Address-following belongs to the engine; a raw router subscription names an instance.
    route_owned(router, qb::ActorId::service_address(7, 3), probe);
    route_owned(router, qb::ActorId::ordinary(8, 3, 0x12345678), probe);
    EXPECT_EQ(probe.undelivered, 6u);
    EXPECT_EQ(ordinary.hits, 1u);
    EXPECT_EQ(service.hits, 2u);

    router.unsubscribe(ordinary_id);
    router.unsubscribe(service_id);
    Handler<qb::ActorId> next_ordinary{qb::ActorId::ordinary(7, 3, 0x12345679)};
    Handler<qb::ActorId> next_service{qb::ActorId::service_instance(7, 3, 0x12345679)};
    router.subscribe(next_ordinary);
    router.subscribe(next_service);
    route_owned(router, ordinary_id, probe);
    route_owned(router, service_id, probe);
    EXPECT_EQ(probe.undelivered, 8u);
    router.unsubscribe(ordinary_id); // an old incarnation must not erase its replacement
    router.unsubscribe(service_id);
    route_owned(router, next_ordinary.id(), probe);
    route_owned(router, next_service.id(), probe);
    EXPECT_EQ(next_ordinary.hits, 1u);
    EXPECT_EQ(next_service.hits, 1u);

    route_owned(router, qb::ActorId(qb::BroadcastId(3)), probe);
    EXPECT_EQ(next_ordinary.hits, 2u);
    EXPECT_EQ(next_service.hits, 2u);
    next_ordinary.live = false;
    route_owned(router, qb::ActorId(qb::BroadcastId(3)), probe);
    EXPECT_EQ(next_ordinary.hits, 2u);
    EXPECT_EQ(next_service.hits, 3u);
    EXPECT_EQ(probe.undelivered, 8u);
    EXPECT_EQ(ordinary.hits, 1u);
    EXPECT_EQ(service.hits, 2u);
}

TEST(EventRouting, ActorIdentitySemhPreservesTransferredOwnershipOnThrowAndDisposesUntransferredPayload) {
    for (bool secondary : {false, true}) {
        using Event = OwnedEvent<qb::ActorId>;
        Probe                   probe;
        qb::router::semh<Event> router;
        Handler<qb::ActorId>    handler{secondary ? qb::ActorId::service_instance(7, 3, 0x12345678) : qb::ActorId::ordinary(7, 3, 0x12345678)};
        router.subscribe(handler);
        handler.throw_now = true;
        handler.transfer  = true;
        alignas(Event) unsigned char storage[sizeof(Event)];
        auto                        *transferred = new (storage) Event(handler.id(), probe);
        EXPECT_THROW(router.template route<true>(*transferred), std::runtime_error);
        EXPECT_EQ(probe.destroyed, 0u);
        EXPECT_EQ(probe.payload_destroyed, 0u);
        if (probe.destroyed == 0u) {
            EXPECT_TRUE(transferred->is_alive());
            ASSERT_NE(transferred->payload, nullptr);
            EXPECT_EQ(transferred->payload->stamp, 0xfedcba9876543210ULL);
            transferred->~Event(); // the receiving owner releases it in this isolated unit fixture
        }
        EXPECT_EQ(probe.destroyed, 1u);
        EXPECT_EQ(probe.payload_destroyed, 1u);

        handler.transfer    = false;
        auto *untransferred = new (storage) Event(handler.id(), probe);
        EXPECT_THROW(router.template route<true>(*untransferred), std::runtime_error);
        EXPECT_EQ(probe.destroyed, 2u);
        EXPECT_EQ(probe.payload_destroyed, 2u);
        if (probe.destroyed == 1u)
            untransferred->~Event();
        EXPECT_EQ(handler.hits, 2u);
        EXPECT_EQ(probe.undelivered, 0u);
    }
}

} // namespace event_router_identity_semh_test

namespace event_router_overlapping_semh_test {

struct Key {
    std::uint16_t slot       = 0;
    std::uint16_t generation = 0;
    bool          secondary  = false;
    bool          broadcast  = false;
    bool
    is_broadcast() const noexcept {
        return broadcast;
    }
    bool operator==(Key const &) const noexcept = default;
};
inline unsigned invalid_primary_projections = 0;

} // namespace event_router_overlapping_semh_test

namespace qb::router {

template <>
struct dense_index<event_router_overlapping_semh_test::Key> {
    using Key                                        = event_router_overlapping_semh_test::Key;
    static constexpr bool        enabled             = true;
    static constexpr bool        partitioned         = true;
    static constexpr bool        broadcast_secondary = true;
    static constexpr std::size_t max_index           = 15;
    static constexpr std::size_t secondary_max_index = 15;
    // No exclusion trait: both banks use [0,15], and primary_of is defined only
    // for primary keys. Generic routing must respect that customization contract.
    static bool
    secondary(Key const &key) noexcept {
        return key.secondary;
    }
    static std::size_t
    of(Key const &key) noexcept {
        return key.slot;
    }
    static std::size_t
    secondary_of(Key const &key) noexcept {
        return key.slot;
    }
    static std::size_t
    primary_of(Key const &key) noexcept {
        if (key.secondary)
            ++event_router_overlapping_semh_test::invalid_primary_projections;
        return key.slot;
    }
};

} // namespace qb::router

namespace event_router_overlapping_semh_test {

TEST(EventRouting, PartitionedSemhKeepsOverlappingBanksWithoutTheOptionalExclusionTrait) {
    using namespace event_router_identity_semh_test;
    Probe                             probe;
    qb::router::semh<OwnedEvent<Key>> router;
    Handler<Key>                      primary{Key{7, 1, false, false}};
    Handler<Key>                      secondary{Key{7, 1, true, false}};
    const Key                         broadcast{15, 0, true, true};
    invalid_primary_projections = 0;
    router.subscribe(primary);
    router.subscribe(secondary);
    route_owned(router, primary.id(), probe);
    route_owned(router, secondary.id(), probe);
    route_owned(router, broadcast, probe);
    EXPECT_EQ(primary.hits, 2u);
    EXPECT_EQ(secondary.hits, 2u);

    route_owned(router, Key{7, 2, true, false}, probe);
    router.unsubscribe(secondary.id());
    route_owned(router, secondary.id(), probe);
    route_owned(router, primary.id(), probe);
    EXPECT_EQ(primary.hits, 3u);
    EXPECT_EQ(secondary.hits, 2u);
    EXPECT_EQ(probe.undelivered, 2u);
    EXPECT_EQ(invalid_primary_projections, 0u);
}

} // namespace event_router_overlapping_semh_test

namespace event_router_occupancy_test {

TEST(EventRouting, GenericDenseOccupancyKeepsDefaultValuesAndNullPointersPresent) {
    qb::router::internal::key_table<std::uint16_t, int>    values;
    qb::router::internal::key_table<std::uint16_t, void *> pointers;
    values.insert_or_assign(0, 0); // Neither the key nor the value is an occupancy sentinel.
    pointers.insert_or_assign(0, nullptr);
    ASSERT_NE(values.find(0), nullptr);
    ASSERT_NE(pointers.find(0), nullptr);
    EXPECT_EQ(*values.find(0), 0);
    EXPECT_EQ(*pointers.find(0), nullptr);
    unsigned value_visits = 0, pointer_visits = 0;
    values.for_each([&](std::uint16_t key, int value) {
        EXPECT_EQ(key, 0);
        EXPECT_EQ(value, 0);
        ++value_visits;
    });
    pointers.for_each([&](std::uint16_t key, void *value) {
        EXPECT_EQ(key, 0);
        EXPECT_EQ(value, nullptr);
        ++pointer_visits;
    });
    EXPECT_EQ(value_visits, 1u);
    EXPECT_EQ(pointer_visits, 1u);
    values.erase(0);
    pointers.erase(0);
    EXPECT_EQ(values.find(0), nullptr);
    EXPECT_EQ(pointers.find(0), nullptr);
}

TEST(EventRouting, HeterogeneousSemhCopyAndRvalueCopyKeepIndependentFullIdentityBanks) {
    using namespace event_router_identity_semh_test;
    using Router = qb::router::semh<OwnedEvent<qb::ActorId>>;
    Probe                probe;
    Handler<qb::ActorId> ordinary{qb::ActorId::ordinary(1024, 3, 0x89abcdef)};
    Handler<qb::ActorId> service{qb::ActorId::service_instance(7, 3, 0xfedcba98)};
    Handler<qb::ActorId> overwritten{qb::ActorId::ordinary(9, 3, 0x12345678)};
    Router               original;
    original.subscribe(ordinary);
    original.subscribe(service);
    Router copied(original);
    Router assigned;
    assigned.subscribe(overwritten);
    assigned = original;
    // Declared destructors suppress implicit moves: these are the current copy
    // operations on rvalues. Source tables must still retain their subscribers.
    Router rvalue_copied(std::move(original));
    Router rvalue_assigned;
    rvalue_assigned = std::move(copied);
    for (auto *router : {&original, &copied, &assigned, &rvalue_copied, &rvalue_assigned}) {
        route_owned(*router, ordinary.id(), probe);
        route_owned(*router, service.id(), probe);
    }
    EXPECT_EQ(ordinary.hits, 5u);
    EXPECT_EQ(service.hits, 5u);
    EXPECT_EQ(probe.undelivered, 0u);
    route_owned(assigned, overwritten.id(), probe);
    EXPECT_EQ(overwritten.hits, 0u);
    EXPECT_EQ(probe.undelivered, 1u);
    assigned.unsubscribe(ordinary.id());
    rvalue_copied.unsubscribe(service.id());
    route_owned(assigned, ordinary.id(), probe);
    route_owned(rvalue_copied, service.id(), probe);
    route_owned(original, ordinary.id(), probe);
    route_owned(copied, service.id(), probe);
    EXPECT_EQ(probe.undelivered, 3u);
    EXPECT_EQ(ordinary.hits, 6u);
    EXPECT_EQ(service.hits, 6u);
    EXPECT_EQ(probe.destroyed, 15u);
    EXPECT_EQ(probe.payload_destroyed, 15u);
}

} // namespace event_router_occupancy_test
