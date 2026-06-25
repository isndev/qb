/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/type/event-traits.cpp
 * @brief Compile-time ABI invariants of the core id / event types — pure traits, no engine.
 *
 * Locks down the structural contracts the lock-free transport relies on. None of these
 * involve `qb::Main`, an actor, or the event loop: they are `sizeof` / `is_trivially_destructible`
 * facts, asserted at *build* time via `static_assert` wherever the type is complete (so a
 * regression breaks the compile, not just a run), and re-checked at run time so the case shows
 * up in the gtest report.
 *
 * The contracts and why they matter:
 *   - `sizeof(CoreId)==2`, `sizeof(ActorId)==4`, `sizeof(EventId)==2` — the id ABI that lets an
 *     `ActorId` pack into the pipe header and an `EventId` index the per-core dispatch table.
 *   - `EventQOS0/1/2` are all trivially destructible — events are byte-relocated (memcpy'd)
 *     through the lock-free ring with no per-move ctor/dtor, which is only sound if the QOS
 *     event bases run no destructor. (QOS1/QOS2 are aliases of `qb::Event`; QOS0 derives it.)
 *   - the trivial/non-trivial differential: an event of only trivially-destructible members is
 *     itself trivially destructible, and adding a single `std::vector<int>` member flips the
 *     trait to false — proving the disposer path in the router (which only runs a destructor for
 *     non-trivially-destructible payloads) keys off a trait that actually distinguishes the two.
 *
 * Rewritten from the former monolithic `TYPE.AllCheck`: the line-74 copy-paste (which asserted
 * `EventQOS0` twice and never checked `EventQOS2`) is fixed, the ~20 unasserted `std::cout`
 * diagnostic dumps are removed, the assertable invariants are promoted to `static_assert`, the
 * dead-commented `ServiceEvent` size check is restored with its current value, and the single
 * mega-test is split one-invariant-per-`TEST`.
 */

#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <qb/core/Event.h>
#include <qb/main.h>
#include <qb/string.h>

namespace {

// A probe event whose members are all trivially destructible.
struct TriviallyDestructibleEvent : public qb::Event {
    bool         b{};
    std::byte    by{};
    char         c{};
    double       d{};
    float        f{};
    char         e[10];
    std::size_t  s{};
    qb::string<> str{};
};

// The same, plus one owning (non-trivially-destructible) member — flips the trait.
struct NonTriviallyDestructibleEvent : public TriviallyDestructibleEvent {
    std::vector<int> vec{};
};

// ---------------------------------------------------------------------------
// Id ABI sizes — enforced at build time, mirrored at run time.
// ---------------------------------------------------------------------------

static_assert(sizeof(qb::CoreId) == 2, "CoreId must be a 16-bit id");
static_assert(sizeof(qb::ActorId) == 4, "ActorId must pack {ServiceId,CoreId} into 32 bits");
static_assert(sizeof(qb::EventId) == 2, "EventId must be a 16-bit type id");

TEST(EventTraits, IdSizes) {
    EXPECT_EQ(sizeof(qb::CoreId), 2u);
    EXPECT_EQ(sizeof(qb::ActorId), 4u);
    EXPECT_EQ(sizeof(qb::EventId), 2u);
}

// ---------------------------------------------------------------------------
// QOS event bases are trivially destructible (required for byte-relocation
// through the lock-free ring). QOS1/QOS2 are aliases of qb::Event, so the
// static_assert on each also nails down qb::Event itself.
// ---------------------------------------------------------------------------

static_assert(std::is_trivially_destructible_v<qb::EventQOS0>, "QOS0 must be byte-relocatable");
static_assert(std::is_trivially_destructible_v<qb::EventQOS1>, "QOS1 must be byte-relocatable");
static_assert(std::is_trivially_destructible_v<qb::EventQOS2>, "QOS2 must be byte-relocatable");

TEST(EventTraits, EventQosTriviallyDestructible) {
    EXPECT_TRUE(std::is_trivially_destructible_v<qb::EventQOS0>);
    EXPECT_TRUE(std::is_trivially_destructible_v<qb::EventQOS1>);
    // Fixes the original line-74 copy-paste: this used to re-assert QOS0 a second time,
    // leaving EventQOS2's trivial-destructibility unchecked.
    EXPECT_TRUE(std::is_trivially_destructible_v<qb::EventQOS2>);
}

// ---------------------------------------------------------------------------
// Differential: a trivially-destructible event stays trivial; adding one owning
// member makes it non-trivial. This is exactly the trait the router's disposer
// branches on, so both directions must hold.
// ---------------------------------------------------------------------------

static_assert(std::is_trivially_destructible_v<TriviallyDestructibleEvent>);
static_assert(!std::is_trivially_destructible_v<NonTriviallyDestructibleEvent>);

TEST(EventTraits, TrivialEventDetected) {
    EXPECT_TRUE(std::is_trivially_destructible_v<TriviallyDestructibleEvent>);
}

TEST(EventTraits, NonTrivialEventDetected) {
    EXPECT_FALSE(std::is_trivially_destructible_v<NonTriviallyDestructibleEvent>);
}

// ---------------------------------------------------------------------------
// ServiceEvent carries two extra id-sized fields (forward + service_event_id)
// on top of qb::Event. Pin its current size so an accidental layout change
// (which would corrupt the wire format) is caught. (Restored from the original
// dead-commented assertion; the previous `22` was stale — the current ABI is 64,
// equal to sizeof(qb::Event) since the two extra fields fit in the base padding.)
// ---------------------------------------------------------------------------

TEST(EventTraits, ServiceEventSize) {
    EXPECT_EQ(sizeof(qb::ServiceEvent), sizeof(qb::Event))
        << "ServiceEvent's forward/service_event_id fields must fit within the Event footprint";
    EXPECT_EQ(sizeof(qb::ServiceEvent), 64u);
}

} // namespace
