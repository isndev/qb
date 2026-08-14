/**
 * @file qb/tests/core/unit/core/event-field-shadow.cpp
 * @brief Pin the routing-field shadow guard (`qb::detail::routing_safe_type_id`).
 *
 * THE DEFECT THIS PINS. `qb::Event` keeps its routing header in private members --
 * `state`, `bucket_size`, `id`, `dest`, `source` (Event.h:415-420) -- and `ServiceEvent`
 * adds two public ones, `forward` and `service_event_id` (Event.h:532-533). The three
 * sites that stamp that header (`VirtualCore::fill_event`, `Pipe::push`,
 * `Pipe::allocated_push`) write through a value of the DERIVED type, so a user event
 * declaring its own member of the same name hides the base one. Measured before the fix
 * with `struct Row : qb::Event { int id; }`: ten pushes to a live registered target
 * delivered ZERO rows, while a `Drain` pushed after them arrived normally. Nothing warned
 * -- not `-Wall -Wextra -Wpedantic -Wshadow-all -Wshadow-field`, because `-Wshadow-field`
 * does not fire on a base field the derived class cannot see.
 *
 * WHY THIS TEST LOOKS LIKE THIS. The guard is a `static_assert`, so the shape it rejects
 * cannot be written in a compiling test at all -- asserting it fires is the job of
 * `dev/agent/event-shadow-negative-control.sh`, which compiles a planted TU per field and
 * requires the compiler to refuse it. What a compiling test CAN pin is the PREDICATE the
 * assertions are built from, in BOTH polarities. That matters more than it looks: a guard
 * whose predicate silently became `false` for every type would still compile every plant,
 * and the only surviving evidence would be this file's `EXPECT_TRUE` half.
 *
 * These are runtime `EXPECT`s over `constexpr bool`s on purpose, not `static_assert`s: a
 * `static_assert` that stops firing is indistinguishable from a file nobody compiled.
 */

#include <cstdint>
#include <gtest/gtest.h>
#include <qb/core/Event.h>
#include <string>
#include <vector>

namespace event_field_shadow_test {

// -- the shapes that MUST be rejected (one per hidable name) ---------------------------
struct ShadowId : qb::Event {
    int id;
};
struct ShadowDest : qb::Event {
    qb::ActorId dest;
};
struct ShadowSource : qb::Event {
    qb::ActorId source;
};
struct ShadowBucketSize : qb::Event {
    std::uint16_t bucket_size;
};
struct ShadowState : qb::Event {
    int state;
};
struct ShadowForward : qb::ServiceEvent {
    int forward;
};
struct ShadowServiceId : qb::ServiceEvent {
    int service_event_id;
};

// -- variants of the same hazard -------------------------------------------------------
/** Same TYPE as the base field: a `decltype`-only detector would miss this one. */
struct ShadowSameType : qb::Event {
    qb::EventId id;
};
/** `std::string::operator=(char)` swallows the narrowed `EventId`, so this too was silent. */
struct ShadowString : qb::Event {
    std::string id;
};
struct Intermediate : qb::Event {
    int seq;
};
/** Declared two levels down, exactly as `-Wshadow-field` would also have missed it. */
struct ShadowThroughBase : Intermediate {
    int id;
};
/** A static member is written by `data.id = ...` just the same. */
struct ShadowStatic : qb::Event {
    static int id;
};

// -- the shapes that MUST be accepted ---------------------------------------------------
struct Clean : qb::Event {
    int row_id;
};
struct Identifier : qb::Event {
    int identifier;
};
struct NearMisses : qb::Event {
    int         id_;
    qb::ActorId dest_;
    int         source_id;
    int         bucketSize;
    int         State;
};
struct Payload {
    int         id;
    qb::ActorId dest;
    int         state;
    int         bucket_size;
};
/** A nested type may carry any of the seven names -- only the EVENT's own scope matters. */
struct HasNestedId : qb::Event {
    Payload payload;
};
struct CleanDeep : Intermediate {
    int row_id;
};
struct CleanService : qb::ServiceEvent {
    int payload;
};
struct CleanCorrelated : qb::CorrelatedEvent {
    int v;
};
struct CleanQOS0 : qb::EventQOS0 {
    int v;
};
struct WithContainers : qb::Event {
    std::vector<int> rows;
    std::string      name;
};

} // namespace event_field_shadow_test

using namespace event_field_shadow_test;
namespace d = qb::detail;

// =====================================================================================
// Polarity 1 -- the guard SEES every hidable name.
// =====================================================================================
TEST(EventFieldShadow, DetectsEachOfTheFivePrivateEventFields) {
    EXPECT_TRUE(d::hides_event_id<ShadowId>);
    EXPECT_TRUE(d::hides_event_dest<ShadowDest>);
    EXPECT_TRUE(d::hides_event_source<ShadowSource>);
    EXPECT_TRUE(d::hides_event_bucket_size<ShadowBucketSize>);
    EXPECT_TRUE(d::hides_event_state<ShadowState>);
}

TEST(EventFieldShadow, DetectsBothPublicServiceEventFields) {
    EXPECT_TRUE(d::hides_service_forward<ShadowForward>);
    EXPECT_TRUE(d::hides_service_event_id<ShadowServiceId>);
}

TEST(EventFieldShadow, DetectsTheVariantsAPlainTypeCompareWouldMiss) {
    // Identical type to the base field -- `decltype(member)` equality is not enough.
    EXPECT_TRUE(d::hides_event_id<ShadowSameType>);
    // Assignable-but-wrong type: this compiled and dropped events silently.
    EXPECT_TRUE(d::hides_event_id<ShadowString>);
    // Declared on an intermediate class rather than on the pushed type.
    EXPECT_TRUE(d::hides_event_id<ShadowThroughBase>);
    // A static data member is hidden by the same lookup rule.
    EXPECT_TRUE(d::hides_event_id<ShadowStatic>);
}

// =====================================================================================
// Polarity 2 -- the guard does NOT fire on anything legitimate.
//
// This half is the one that keeps the guard switched on. A routing-field check that
// tripped on an ordinary member would be worked around rather than fixed.
// =====================================================================================
TEST(EventFieldShadow, AcceptsAPlainEvent) {
    EXPECT_FALSE(d::hides_event_id<Clean>);
    EXPECT_FALSE(d::hides_event_dest<Clean>);
    EXPECT_FALSE(d::hides_event_source<Clean>);
    EXPECT_FALSE(d::hides_event_bucket_size<Clean>);
    EXPECT_FALSE(d::hides_event_state<Clean>);
}

TEST(EventFieldShadow, AcceptsQbEventItselfAndTheFrameworkEventTypes) {
    EXPECT_FALSE(d::hides_event_id<qb::Event>);
    EXPECT_FALSE(d::hides_event_id<qb::KillEvent>);
    EXPECT_FALSE(d::hides_event_id<qb::SignalEvent>);
    EXPECT_FALSE(d::hides_event_id<qb::CorrelatedEvent>);
    EXPECT_FALSE(d::hides_event_id<qb::PingEvent>);
    EXPECT_FALSE(d::hides_event_id<qb::ServiceEvent>);
    // ServiceEvent DECLARES forward/service_event_id -- it is the base, not a shadow of it.
    EXPECT_FALSE(d::hides_service_forward<qb::ServiceEvent>);
    EXPECT_FALSE(d::hides_service_event_id<qb::ServiceEvent>);
}

TEST(EventFieldShadow, AcceptsNamesThatMerelyResembleTheRoutingFields) {
    EXPECT_FALSE(d::hides_event_id<Identifier>);
    EXPECT_FALSE(d::hides_event_id<NearMisses>);
    EXPECT_FALSE(d::hides_event_dest<NearMisses>);
    EXPECT_FALSE(d::hides_event_source<NearMisses>);
    EXPECT_FALSE(d::hides_event_bucket_size<NearMisses>);
    EXPECT_FALSE(d::hides_event_state<NearMisses>);
}

TEST(EventFieldShadow, AcceptsANestedTypeCarryingTheSameNames) {
    EXPECT_FALSE(d::hides_event_id<HasNestedId>);
    EXPECT_FALSE(d::hides_event_dest<HasNestedId>);
    EXPECT_FALSE(d::hides_event_state<HasNestedId>);
    EXPECT_FALSE(d::hides_event_bucket_size<HasNestedId>);
}

TEST(EventFieldShadow, AcceptsInheritanceThroughAnIntermediateClass) {
    EXPECT_FALSE(d::hides_event_id<Intermediate>);
    EXPECT_FALSE(d::hides_event_id<CleanDeep>);
    EXPECT_FALSE(d::hides_event_dest<CleanDeep>);
}

TEST(EventFieldShadow, AcceptsTheFrameworkEventBasesUsersDeriveFrom) {
    EXPECT_FALSE(d::hides_event_id<CleanService>);
    EXPECT_FALSE(d::hides_service_forward<CleanService>);
    EXPECT_FALSE(d::hides_service_event_id<CleanService>);
    EXPECT_FALSE(d::hides_event_id<CleanCorrelated>);
    EXPECT_FALSE(d::hides_event_id<CleanQOS0>);
    EXPECT_FALSE(d::hides_event_state<CleanQOS0>); // EventQOS0's ctor writes Event::state
    EXPECT_FALSE(d::hides_event_id<WithContainers>);
}

// =====================================================================================
// The guard must not change what routing does. `routing_safe_type_id<T>()` replaced
// `Event::type_to_id<T>()` on the three stamping lines; if it ever returned anything
// else, every event would route to the wrong slot.
// =====================================================================================
TEST(EventFieldShadow, ReturnsExactlyTheSameTypeIdAsTypeToId) {
    EXPECT_EQ(d::routing_safe_type_id<Clean>(), qb::Event::type_to_id<Clean>());
    EXPECT_EQ(d::routing_safe_type_id<Identifier>(), qb::Event::type_to_id<Identifier>());
    EXPECT_EQ(d::routing_safe_type_id<qb::KillEvent>(), qb::Event::type_to_id<qb::KillEvent>());
    EXPECT_EQ(d::routing_safe_type_id<CleanService>(), qb::Event::type_to_id<CleanService>());
    // Stable across calls (the id is assigned once per type, by a magic static).
    EXPECT_EQ(d::routing_safe_type_id<Clean>(), d::routing_safe_type_id<Clean>());
    // Distinct types get distinct ids.
    EXPECT_NE(d::routing_safe_type_id<Clean>(), d::routing_safe_type_id<Identifier>());
}
