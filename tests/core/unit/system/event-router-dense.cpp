/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/system/event-router-dense.cpp
 * @brief The DENSE form of the router tables (`qb::router::dense_index` / `internal::key_table`).
 *
 * `event-router.cpp` keys its mocks on `const char *` and a 32-bit id, so every router it
 * exercises runs on the hash-map fallback and the dense vector — the table the real engine
 * uses, since `EventId` is a 16-bit counter and `qb::ActorId` is dense by `sid()` — was
 * covered by nothing but the engine itself. This file keys both tables densely: events on a
 * `uint16_t` id and handlers on a two-field key whose dense form is ONE of the fields, so the
 * one property the vector cannot get for free is asserted explicitly:
 *
 *   - a key that shares an index with a live subscriber is a MISS, never a misdelivery (the
 *     slot stores the full key and compares it; `{slot 2, gen 2}` must not reach `{slot 2, gen 1}`);
 *   - a miss still runs the `_CleanEvent` dispose path (the disposal oracle counts dtors);
 *   - the table grows to any index in range and keeps the slots below it (0, 1000, 65534);
 *   - broadcast visits each live slot exactly once and forgets an erased one;
 *   - re-subscribing a key replaces its slot in place (no double delivery);
 *   - an id beyond the table's end is reported through `onError`, never read out of bounds.
 *
 * Both handler flavours are covered per router (`_Handler = void` trampolines and a fixed
 * handler type), since the two `semh` specialisations own separate table code paths.
 * No `qb::Main`, no event loop: pure table logic, fully deterministic.
 */

#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <new>
#include <qb/system/event/router.h>
#include <type_traits>
#include <vector>

namespace event_router_dense_test {

// A handler id whose dense form is a PROJECTION: two keys can share `slot` and differ by
// `gen`, which is exactly the shape a recycled ActorId slot has after its previous owner died.
struct DenseId {
    static constexpr uint16_t kBroadcastSlot = std::numeric_limits<uint16_t>::max();

    uint16_t slot = 0;
    uint16_t gen  = 0;

    [[nodiscard]] bool
    is_broadcast() const noexcept {
        return slot == kBroadcastSlot;
    }
    bool
    operator==(DenseId const &rhs) const noexcept {
        return slot == rhs.slot && gen == rhs.gen;
    }
};

} // namespace event_router_dense_test

namespace qb::router {
template <>
struct dense_index<event_router_dense_test::DenseId> {
    static constexpr bool        enabled   = true;
    static constexpr std::size_t max_index = event_router_dense_test::DenseId::kBroadcastSlot - 1;

    [[nodiscard]] static constexpr std::size_t
    of(event_router_dense_test::DenseId const id) noexcept {
        return id.slot;
    }
};
} // namespace qb::router

namespace event_router_dense_test {

// The compile-time selection is part of the contract: unsigned ids of at most 16 bits are
// dense by identity, everything else keeps the hash map unless specialised.
static_assert(qb::router::dense_index<uint16_t>::enabled);
static_assert(qb::router::dense_index<uint8_t>::enabled);
static_assert(qb::router::dense_index<DenseId>::enabled);
static_assert(!qb::router::dense_index<bool>::enabled);
static_assert(!qb::router::dense_index<uint32_t>::enabled);
static_assert(!qb::router::dense_index<int>::enabled);
static_assert(!qb::router::dense_index<const char *>::enabled);
static_assert(qb::router::dense_index<uint16_t>::max_index == 65535);
static_assert(qb::router::dense_index<DenseId>::max_index == 65534);

struct RawEvent {
    using id_type         = uint16_t;
    using id_handler_type = DenseId;

    RawEvent()                 = default;
    RawEvent(RawEvent const &) = delete;

    // Counter-assigned like the engine's `Event::type_to_id`: the first type asked gets 0.
    template <typename T>
    static id_type
    type_to_id() {
        static const id_type id = next_id()++;
        return id;
    }

    id_type         id = 0;
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

private:
    static id_type &
    next_id() {
        static id_type counter = 0;
        return counter;
    }
};

struct HitEvent : RawEvent {
    HitEvent() {
        id = type_to_id<HitEvent>();
    }
};

struct OtherEvent : RawEvent {
    OtherEvent() {
        id = type_to_id<OtherEvent>();
    }
};

// The disposal oracle: `alive = false` so the `_CleanEvent` path destroys it after routing,
// and the destructor is the only thing that increments `destroyed`.
struct DisposeEvent : RawEvent {
    static inline std::size_t destroyed = 0;
    std::vector<int>          payload{1, 2, 3}; // non-trivial, so dispose() is not elided

    DisposeEvent() {
        id    = type_to_id<DisposeEvent>();
        alive = false;
    }
    ~DisposeEvent() {
        ++destroyed;
    }
};

struct FakeActor {
    DenseId     _id;
    std::size_t hits       = 0;
    std::size_t other_hits = 0;

    explicit FakeActor(uint16_t const slot, uint16_t const gen = 1) noexcept
        : _id{slot, gen} {}

    [[nodiscard]] DenseId
    id() const noexcept {
        return _id;
    }
    [[nodiscard]] bool
    is_alive() const noexcept {
        return true;
    }
    void
    on(HitEvent &) noexcept {
        ++hits;
    }
    void
    on(OtherEvent &) noexcept {
        ++other_hits;
    }
    void
    on(DisposeEvent const &) const noexcept {}
};

// Route one HitEvent to `{slot, gen}` and report how many handlers saw it — derived from the
// actors, never from the event.
template <typename Router>
std::size_t
hit(Router &router, std::vector<FakeActor *> const &actors, uint16_t const slot, uint16_t const gen = 1) {
    std::size_t before = 0;
    for (auto const *a : actors)
        before += a->hits;
    HitEvent event;
    event.dest = DenseId{slot, gen};
    router.route(event);
    std::size_t after = 0;
    for (auto const *a : actors)
        after += a->hits;
    return after - before;
}

constexpr uint16_t kBroadcast = DenseId::kBroadcastSlot;

// ---------------------------------------------------------------------------------------------
// semh — handler table keyed by DenseId, both specialisations
// ---------------------------------------------------------------------------------------------

template <typename Router>
class DenseSemh : public ::testing::Test {};

using SemhTypes = ::testing::Types<qb::router::semh<HitEvent, void>, qb::router::semh<HitEvent, FakeActor>>;
TYPED_TEST_SUITE(DenseSemh, SemhTypes);

TYPED_TEST(DenseSemh, TargetedRouteReachesExactlyTheSubscribedSlot) {
    TypeParam                      router;
    FakeActor                      a1(1), a2(2), a5(5);
    const std::vector<FakeActor *> actors{&a1, &a2, &a5};
    for (auto *a : actors)
        router.subscribe(*a);

    EXPECT_EQ(hit(router, actors, 1), 1u);
    EXPECT_EQ(hit(router, actors, 2), 1u);
    EXPECT_EQ(hit(router, actors, 5), 1u);
    EXPECT_EQ(a1.hits, 1u);
    EXPECT_EQ(a2.hits, 1u);
    EXPECT_EQ(a5.hits, 1u);

    // Never-subscribed slots below, between and beyond the table's end: all misses, no crash.
    for (const uint16_t miss : {uint16_t{0}, uint16_t{3}, uint16_t{4}, uint16_t{7}, uint16_t{60000}})
        EXPECT_EQ(hit(router, actors, miss), 0u) << "slot " << miss;
}

TYPED_TEST(DenseSemh, MissIsStillDisposed) {
    // Same handler flavour as TypeParam, over the disposal-oracle event.
    constexpr bool heterogeneous = std::is_same_v<TypeParam, qb::router::semh<HitEvent, void>>;
    using Router                 = qb::router::semh<DisposeEvent, std::conditional_t<heterogeneous, void, FakeActor>>;

    Router    router;
    FakeActor a1(1);
    router.subscribe(a1);

    DisposeEvent::destroyed = 0;
    alignas(DisposeEvent) unsigned char storage[sizeof(DisposeEvent)];

    // A hit disposes (alive == false)...
    auto *hit_event = new (storage) DisposeEvent();
    hit_event->dest = DenseId{1, 1};
    router.template route<true>(*hit_event);
    EXPECT_EQ(DisposeEvent::destroyed, 1u);

    // ...and so does a miss on a slot beyond the table: the payload must not leak because
    // nobody was listening.
    auto *miss_event = new (storage) DisposeEvent();
    miss_event->dest = DenseId{9000, 1};
    router.template route<true>(*miss_event);
    EXPECT_EQ(DisposeEvent::destroyed, 2u);

    // `_CleanEvent = false` leaves the event to its owner.
    auto *kept = new (storage) DisposeEvent();
    kept->dest = DenseId{1, 1};
    router.template route<false>(*kept);
    EXPECT_EQ(DisposeEvent::destroyed, 2u);
    kept->~DisposeEvent();
    EXPECT_EQ(DisposeEvent::destroyed, 3u);
}

TYPED_TEST(DenseSemh, SameIndexDifferentKeyIsAMissNeverAMisdelivery) {
    TypeParam                      router;
    FakeActor                      live(2, 1);
    const std::vector<FakeActor *> actors{&live};
    router.subscribe(live);

    // Same dense index (slot 2), different key (gen 2): the slot belongs to ANOTHER key.
    EXPECT_EQ(hit(router, actors, 2, 2), 0u);
    EXPECT_EQ(hit(router, actors, 2, 1), 1u);
    EXPECT_EQ(live.hits, 1u);

    // Unsubscribing by the wrong generation must not evict the live one either.
    router.unsubscribe(DenseId{2, 2});
    EXPECT_EQ(hit(router, actors, 2, 1), 1u);
    router.unsubscribe(DenseId{2, 1});
    EXPECT_EQ(hit(router, actors, 2, 1), 0u);
    EXPECT_EQ(live.hits, 2u);
}

TYPED_TEST(DenseSemh, BroadcastVisitsEveryLiveSlotOnceAndTracksUnsubscribe) {
    TypeParam                      router;
    FakeActor                      a0(0), a3(3), a4(4), a9(9);
    const std::vector<FakeActor *> actors{&a0, &a3, &a4, &a9};
    for (auto *a : actors)
        router.subscribe(*a);

    EXPECT_EQ(hit(router, actors, kBroadcast, 0), actors.size());
    for (auto const *a : actors)
        EXPECT_EQ(a->hits, 1u) << "slot " << a->_id.slot;

    router.unsubscribe(a4.id());
    EXPECT_EQ(hit(router, actors, kBroadcast, 0), actors.size() - 1);
    EXPECT_EQ(a4.hits, 1u);
    EXPECT_EQ(a0.hits, 2u);
    EXPECT_EQ(a9.hits, 2u);

    // The erased slot is reusable by its next owner (a recycled ServiceId).
    FakeActor a4b(4, 2);
    router.subscribe(a4b);
    const std::vector<FakeActor *> now{&a0, &a3, &a4b, &a9};
    EXPECT_EQ(hit(router, now, kBroadcast, 0), now.size());
    EXPECT_EQ(a4b.hits, 1u);
    EXPECT_EQ(a4.hits, 1u);
}

TYPED_TEST(DenseSemh, ResubscribeReplacesTheSlotInPlace) {
    TypeParam                      router;
    FakeActor                      a(7);
    const std::vector<FakeActor *> actors{&a};
    router.subscribe(a);
    router.subscribe(a);
    router.subscribe(a);

    // One slot, one delivery: a table that appended on re-subscribe would deliver thrice.
    EXPECT_EQ(hit(router, actors, 7), 1u);
    EXPECT_EQ(hit(router, actors, kBroadcast, 0), 1u);
}

TYPED_TEST(DenseSemh, GrowsToAnyIndexAndKeepsEarlierSlots) {
    TypeParam                      router;
    constexpr auto                 last_slot = static_cast<uint16_t>(qb::router::dense_index<DenseId>::max_index);
    FakeActor                      first(0), middle(1000), last(last_slot);
    const std::vector<FakeActor *> actors{&first, &middle, &last};

    router.subscribe(first);
    EXPECT_EQ(hit(router, actors, 0), 1u);
    router.subscribe(middle);
    EXPECT_EQ(hit(router, actors, 0), 1u);
    EXPECT_EQ(hit(router, actors, 1000), 1u);
    router.subscribe(last);
    EXPECT_EQ(hit(router, actors, 0), 1u);
    EXPECT_EQ(hit(router, actors, 1000), 1u);
    EXPECT_EQ(hit(router, actors, last_slot), 1u);
    EXPECT_EQ(first.hits, 3u);
    EXPECT_EQ(middle.hits, 2u);
    EXPECT_EQ(last.hits, 1u);

    // Broadcast walks the whole (now 65535-slot) table and finds exactly the three.
    EXPECT_EQ(hit(router, actors, kBroadcast, 0), 3u);
}

TYPED_TEST(DenseSemh, UnsubscribeUnknownIsANoop) {
    TypeParam                      router;
    FakeActor                      a(5);
    const std::vector<FakeActor *> actors{&a};
    router.subscribe(a);

    router.unsubscribe(DenseId{5000, 1}); // beyond the table's end
    router.unsubscribe(DenseId{4, 1});    // inside, never used
    router.unsubscribe(DenseId{5, 3});    // same index, other key
    EXPECT_EQ(hit(router, actors, 5), 1u);
}

// ---------------------------------------------------------------------------------------------
// memh — event table keyed by uint16_t, both specialisations
// ---------------------------------------------------------------------------------------------

template <typename Router>
class DenseMemh : public ::testing::Test {};

using MemhTypes = ::testing::Types<qb::router::memh<RawEvent, true, void>, qb::router::memh<RawEvent, true, FakeActor>>;
TYPED_TEST_SUITE(DenseMemh, MemhTypes);

TYPED_TEST(DenseMemh, RoutesByEventIdAndReportsAnUnregisteredOne) {
    TypeParam router;
    FakeActor a(1);
    router.template subscribe<HitEvent>(a);
    router.template subscribe<OtherEvent>(a);

    std::size_t errors   = 0;
    const auto  on_error = [&errors](RawEvent const &) {
        ++errors;
    };

    HitEvent hit_event;
    hit_event.dest = a.id();
    router.route(hit_event, on_error);
    OtherEvent other;
    other.dest = a.id();
    router.route(other, on_error);
    EXPECT_EQ(a.hits, 1u);
    EXPECT_EQ(a.other_hits, 1u);
    EXPECT_EQ(errors, 0u);

    // An id no type ever claimed, far beyond the table's end: reported, not read out of bounds.
    RawEvent stray;
    stray.id   = std::numeric_limits<uint16_t>::max();
    stray.dest = a.id();
    router.route(stray, on_error);
    EXPECT_EQ(errors, 1u);
    EXPECT_EQ(a.hits, 1u);
    EXPECT_EQ(a.other_hits, 1u);
}

TYPED_TEST(DenseMemh, UnsubscribeByIdWalksEveryRegisteredEvent) {
    TypeParam router;
    FakeActor a(1), b(2);
    router.template subscribe<HitEvent>(a);
    router.template subscribe<OtherEvent>(a);
    router.template subscribe<HitEvent>(b);
    router.template subscribe<OtherEvent>(b);

    router.unsubscribe(a.id());

    std::size_t errors   = 0;
    const auto  on_error = [&errors](RawEvent const &) {
        ++errors;
    };
    for (auto *target : {&a, &b}) {
        HitEvent h;
        h.dest = target->id();
        router.route(h, on_error);
        OtherEvent o;
        o.dest = target->id();
        router.route(o, on_error);
    }
    EXPECT_EQ(errors, 0u); // both ids stay REGISTERED; only the first actor's slot is empty
    EXPECT_EQ(a.hits, 0u);
    EXPECT_EQ(a.other_hits, 0u);
    EXPECT_EQ(b.hits, 1u);
    EXPECT_EQ(b.other_hits, 1u);
}

TYPED_TEST(DenseMemh, DisposesOnMissAndOnHit) {
    TypeParam router;
    FakeActor a(1);
    router.template subscribe<DisposeEvent>(a);
    const auto on_error = [](RawEvent const &) {
        FAIL() << "DisposeEvent is registered";
    };

    DisposeEvent::destroyed = 0;
    alignas(DisposeEvent) unsigned char storage[sizeof(DisposeEvent)];

    auto *hit_event = new (storage) DisposeEvent();
    hit_event->dest = a.id();
    router.route(*hit_event, on_error);
    EXPECT_EQ(DisposeEvent::destroyed, 1u);

    auto *miss_event = new (storage) DisposeEvent();
    miss_event->dest = DenseId{3000, 1};
    router.route(*miss_event, on_error);
    EXPECT_EQ(DisposeEvent::destroyed, 2u);
}

} // namespace event_router_dense_test
