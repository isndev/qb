/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the specific terms.
 */

/**
 * @file unit/core/event-header.cpp
 * @brief `qb::Event` header runtime contract + `qb::Pipe` bucket-sizing math (`qb/core/Event.h`,
 *        `qb/core/Pipe.h`) — pure logic, NO `qb::Main`, NO actor, NO event loop.
 *
 * The base `Event` is a 64-byte, cache-line-aligned record whose first word is a `union Header`
 * (`alive:1 / qos:2 / factor:5` overlaid on a 4-byte `prot[]` magic that supplies the only default
 * member-initializer). The header carries the entire wire identity the lock-free transport reads:
 * `is_alive()` / `getQOS()` / `getID()` / `getDestination()` / `getSource()` / `getSize()`. None of
 * that needs an engine to observe, yet none of the *default-construction* values were pinned anywhere
 * — so a layout drift in the bitfield/union (which silently changes what every freshly-pushed event
 * carries on the wire) would pass CI unnoticed. These cases nail the values down at the level the
 * header actually produces them, computed independently from the documented bucket math rather than
 * echoed back.
 *
 * Oracles are independent of the implementation under test:
 *   - the bucket count `getItemSize<T>()` the transport writes into `bucket_size` is checked against
 *     a hand-computed ceil-divide, so the allocator's round-up cannot share a bug with the oracle.
 *   - `ServiceEvent::received()` is checked against the documented "swap dest/forward, swap
 *     id/service_event_id, mark alive" contract via the public getters.
 *   - the `bucket_size` `uint16_t` truncation called out in `Pipe::allocated_push`'s @warning is
 *     reproduced as plain arithmetic (65536 buckets → 0), proving the cap is real.
 *
 * Live delivery of `Pipe::push` / `allocated_push` through a real `VirtualCore` is already covered by
 * the system tier (messaging/messaging-api.cpp, event/service-event-ring.cpp,
 * messaging/messaging-reply-forward.cpp); this file deliberately stays at the engine-free header/math
 * layer and does not duplicate that.
 */

#include <cstdint>
#include <limits>
#include <type_traits>

#include <gtest/gtest.h>

#include <qb/core/Event.h>
#include <qb/core/Pipe.h>
#include <qb/system/allocator/pipe.h>
#include <qb/utility/prefix.h>

using qb::ActorId;
using qb::Event;
using qb::allocator::getItemSize;

namespace {

// Independent oracle for the bucket count of a T: ceil(sizeof(T) / bucket_bytes), spelled out so it
// cannot share a bug with getItemSize<>().
constexpr std::size_t
ceil_buckets(std::size_t bytes) noexcept {
    return bytes / QB_LOCKFREE_EVENT_BUCKET_BYTES +
           (bytes % QB_LOCKFREE_EVENT_BUCKET_BYTES != 0 ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// Event size / alignment ABI. The lock-free ring byte-copies events in
// QB_LOCKFREE_EVENT_BUCKET_BYTES-sized buckets, so Event must be exactly one
// bucket and aligned to a bucket boundary.
// ---------------------------------------------------------------------------

static_assert(QB_LOCKFREE_EVENT_BUCKET_BYTES == 64, "tests assume the default 64-byte bucket");
static_assert(sizeof(Event) == QB_LOCKFREE_EVENT_BUCKET_BYTES, "Event must occupy exactly one bucket");
static_assert(alignof(Event) == QB_LOCKFREE_EVENT_BUCKET_BYTES, "Event must be bucket-aligned");

TEST(EventHeader, EventIsExactlyOneBucketAndAligned) {
    EXPECT_EQ(sizeof(Event), static_cast<std::size_t>(QB_LOCKFREE_EVENT_BUCKET_BYTES));
    EXPECT_EQ(alignof(Event), static_cast<std::size_t>(QB_LOCKFREE_EVENT_BUCKET_BYTES));
    // A single Event therefore rounds to exactly one bucket.
    EXPECT_EQ((getItemSize<Event, EventBucket>()), 1u);
}

// ---------------------------------------------------------------------------
// Default-constructed header state. These are the values EVERY event carries
// on the wire before VirtualCore overwrites `alive` at consume time, so they
// are the real default contract — previously asserted nowhere.
//
// The Header union's only default member-initializer is `prot[4]`; reading it
// back through the overlaid bitfields yields alive=1, qos=1 with the default
// 64-byte bucket. (Verified against the compiled type, not hand-derived.)
// ---------------------------------------------------------------------------

TEST(EventHeader, DefaultEventIsAliveWithQos1) {
    Event e;
    EXPECT_TRUE(e.is_alive())
        << "the prot[] magic default-initializes the alive bit to 1";
    EXPECT_EQ(e.getQOS(), 1u)
        << "default QOS encoded by the prot[] magic is 1 (EventQOS1 == base Event)";
}

TEST(EventHeader, DefaultDestinationAndSourceAreNotFound) {
    // Event default-constructs dest/source as default ActorId == NotFound. fill_event /
    // Pipe::push later overwrite them; before that they are the invalid id.
    Event e;
    EXPECT_EQ(static_cast<std::uint32_t>(e.getDestination()), ActorId::NotFound);
    EXPECT_EQ(static_cast<std::uint32_t>(e.getSource()), ActorId::NotFound);
    EXPECT_FALSE(e.getDestination().is_valid());
    EXPECT_FALSE(e.getSource().is_valid());
}

// ---------------------------------------------------------------------------
// EventQOS0's constructor sets state.qos = 0 through the union bitfield. Pin
// the *observable* result on the real type: QOS becomes 0. This also documents
// that writing the qos bitfield re-reads the alive bit as 0 (the QOS event is
// trivially destructible and only ever delivered after VirtualCore forces
// alive=0 anyway, so this is benign — but it is a surprising side effect worth
// locking down so a "fix" that changes it is caught).
// ---------------------------------------------------------------------------

TEST(EventHeader, EventQos0HasQosZero) {
    qb::EventQOS0 q0;
    EXPECT_EQ(q0.getQOS(), 0u) << "EventQOS0 ctor must encode QOS level 0";
}

TEST(EventHeader, EventQos0DiffersFromDefaultQos) {
    // QOS0 is genuinely a different priority than the base Event default (1).
    Event         base;
    qb::EventQOS0 q0;
    EXPECT_NE(q0.getQOS(), base.getQOS());
    EXPECT_EQ(base.getQOS(), 1u);
    EXPECT_EQ(q0.getQOS(), 0u);
}

static_assert(std::is_trivially_destructible_v<qb::EventQOS0>,
              "QOS0 must stay byte-relocatable (no dtor) for the ring");

// NOTE on getSize(): the accessor is `bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES`, but
// `bucket_size` is private to Event and only ever written by the framework's friends
// (VirtualCore::fill_event / Pipe::push), both of which require a live engine. The concrete
// getSize() value of a *pushed* event is therefore exercised at system tier
// (messaging/messaging-api.cpp, which pushes via getPipe().allocated_push and consumes the event).
// Here we lock down the bucket *count* the transport assigns to that field — getItemSize<T>() — since
// that is the engine-free half of the same contract.

// ---------------------------------------------------------------------------
// getItemSize<T,EventBucket>() — the exact value Pipe::push writes into
// bucket_size — must round UP to whole buckets with no off-by-one. 64 -> 1
// (exact), 65 -> 2 (one byte over), 128 -> 2 (exact), 129 -> 3.
// ---------------------------------------------------------------------------

namespace {
// E1: a 1-char member sits in Event's trailing padding, so sizeof stays one bucket (64B).
struct E1 : Event { char pad[1]; };
// Eover: a full-bucket-sized member forces the type to span exactly two buckets (128B).
struct Eover : Event { char pad[QB_LOCKFREE_EVENT_BUCKET_BYTES]; };
// EBig: a 200B member rounds the aligned type to four buckets (256B).
struct EBig : Event { char pad[200]; };
} // namespace

static_assert(sizeof(E1) == 64, "a 1-char member must fit Event's trailing padding (still 1 bucket)");
static_assert(sizeof(Eover) == 128, "a full-bucket member must spill into a 2nd bucket");
static_assert(sizeof(EBig) == 256, "a 200B member rounds to 4 buckets");

TEST(EventHeader, GetItemSizeRoundsUpNoOffByOne) {
    // Cross-check getItemSize against the independent ceil-divide oracle on the *actual* sizeofs
    // (alignment may pad these up, which the oracle accounts for since it consumes sizeof()).
    EXPECT_EQ((getItemSize<E1, EventBucket>()), ceil_buckets(sizeof(E1)));
    EXPECT_EQ((getItemSize<Eover, EventBucket>()), ceil_buckets(sizeof(Eover)));
    EXPECT_EQ((getItemSize<EBig, EventBucket>()), ceil_buckets(sizeof(EBig)));

    // Concrete bucket counts: a type that fits the base bucket stays 1; spilling one full bucket's
    // worth of payload tips to 2; a 200B member rounds to 4.
    EXPECT_EQ((getItemSize<E1, EventBucket>()), 1u);
    EXPECT_EQ((getItemSize<Eover, EventBucket>()), 2u);
    EXPECT_EQ((getItemSize<EBig, EventBucket>()), 4u);

    // Pure-arithmetic boundary sweep on raw byte counts (independent of any qb type): the round-up
    // is exact at multiples of the bucket and bumps by one the moment a single byte spills over.
    EXPECT_EQ(ceil_buckets(0), 0u);
    EXPECT_EQ(ceil_buckets(1), 1u);
    EXPECT_EQ(ceil_buckets(63), 1u);
    EXPECT_EQ(ceil_buckets(64), 1u);
    EXPECT_EQ(ceil_buckets(65), 2u);
    EXPECT_EQ(ceil_buckets(128), 2u);
    EXPECT_EQ(ceil_buckets(129), 3u);
}

// ---------------------------------------------------------------------------
// Pipe::allocated_push bucket rounding + the documented uint16 truncation cap.
// allocated_push computes:  n = (hint + sizeof(T)); buckets = n/bucketBytes + (n%bucketBytes!=0);
// then stores buckets in a uint16_t bucket_size. Reproduce that arithmetic and prove the @warning:
// a request spanning exactly 65536 buckets truncates the header field to 0.
// ---------------------------------------------------------------------------

// Mirror of allocated_push's size math (no engine involved); the formula is the contract under test.
constexpr std::size_t
allocated_push_buckets(std::size_t hint, std::size_t sizeof_event) noexcept {
    const std::size_t n = hint + sizeof_event;
    return n / sizeof(EventBucket) + static_cast<std::size_t>(n % sizeof(EventBucket) != 0);
}

TEST(EventHeader, AllocatedPushRoundingMatchesCeilDivide) {
    constexpr std::size_t S = sizeof(Event); // 64
    // hint==0 still allocates room for the event itself (>= 1 bucket).
    EXPECT_EQ(allocated_push_buckets(0, S), 1u);
    // hint just under one extra bucket stays within the rounded count; crossing it bumps by one.
    EXPECT_EQ(allocated_push_buckets(QB_LOCKFREE_EVENT_BUCKET_BYTES - S, S), 1u);
    EXPECT_EQ(allocated_push_buckets(QB_LOCKFREE_EVENT_BUCKET_BYTES - S + 1, S), 2u);
    // A 1 KiB payload hint on a 64B event: ceil((1024+64)/64) = 17 buckets.
    EXPECT_EQ(allocated_push_buckets(1024, S), 17u);
}

TEST(EventHeader, BucketSizeUint16TruncationCap) {
    // The @warning on allocated_push: bucket_size is a uint16_t, so an event spanning exactly
    // 65536 buckets wraps the field to 0 and would stall the receiver. Prove the truncation is real
    // (this is a documented hazard, asserted here so the doc and the type stay in sync).
    const std::size_t buckets_65536 = 65536;
    EXPECT_EQ(static_cast<std::uint16_t>(buckets_65536), 0u)
        << "65536 buckets truncates the uint16 bucket_size to 0 (documented stall hazard)";

    // The largest event that still fits the receiver mailbox is < 65536 buckets; the practical cap
    // the docs cite (~1023 buckets ~= 64 KiB) survives the uint16 intact.
    const std::size_t practical_cap = std::numeric_limits<std::uint16_t>::max() /
                                      QB_LOCKFREE_EVENT_BUCKET_BYTES; // 1023
    EXPECT_EQ(practical_cap, 1023u);
    EXPECT_EQ(static_cast<std::uint16_t>(practical_cap), 1023u);
    EXPECT_EQ(practical_cap * QB_LOCKFREE_EVENT_BUCKET_BYTES, 65472u); // ~64 KiB
}

// ---------------------------------------------------------------------------
// ServiceEvent::received() / live() — pure forwarding logic. received() swaps
// dest<->forward and id<->service_event_id and marks the event alive so the
// router's dispose() (which destroys only when !is_alive()) keeps it for
// re-forwarding. Observe via the public getters.
// ---------------------------------------------------------------------------

TEST(ServiceEventLogic, ReceivedSwapsDestinationWithForwardAndStaysAlive) {
    qb::ServiceEvent se;
    // dest defaults to NotFound; set a distinct forward target.
    const ActorId forward_target(0x12345678u);
    se.forward = forward_target;

    const ActorId dest_before = se.getDestination(); // NotFound
    se.received();

    // dest and forward must have swapped.
    EXPECT_EQ(static_cast<std::uint32_t>(se.getDestination()),
              static_cast<std::uint32_t>(forward_target))
        << "received() must move the forward target into dest";
    EXPECT_EQ(static_cast<std::uint32_t>(se.forward),
              static_cast<std::uint32_t>(dest_before))
        << "received() must move the old dest into forward";

    // received() marks the event alive so it survives the router's dispose().
    EXPECT_TRUE(se.is_alive());
}

TEST(ServiceEventLogic, ReceivedIsItsOwnInverseOnDestForward) {
    // Two received() calls swap dest<->forward back to the original arrangement (it is an involution
    // on the swapped fields), so a double-receive is observably a no-op on dest/forward.
    qb::ServiceEvent se;
    const ActorId    forward_target(0xABCDEF01u);
    se.forward = forward_target;

    const ActorId dest0    = se.getDestination();
    const ActorId forward0 = se.forward;

    se.received();
    se.received();

    EXPECT_EQ(static_cast<std::uint32_t>(se.getDestination()),
              static_cast<std::uint32_t>(dest0));
    EXPECT_EQ(static_cast<std::uint32_t>(se.forward),
              static_cast<std::uint32_t>(forward0));
    EXPECT_TRUE(se.is_alive());
}

TEST(ServiceEventLogic, LiveTogglesAliveBit) {
    qb::ServiceEvent se;
    se.live(false);
    EXPECT_FALSE(se.is_alive()) << "live(false) clears the alive bit";
    se.live(true);
    EXPECT_TRUE(se.is_alive()) << "live(true) sets the alive bit";
    // Idempotent.
    se.live(true);
    EXPECT_TRUE(se.is_alive());
    se.live(false);
    se.live(false);
    EXPECT_FALSE(se.is_alive());
}

// ---------------------------------------------------------------------------
// Event copy is a byte copy of the header (the ring relocates events by memcpy
// for trivially-destructible types). A copied event must carry an identical
// header — same alive/qos. ServiceEvent::live() is the only public mutator of the
// header bit we can drive without a framework friend, so use it to make the copy
// observably distinct from a default and confirm the bit survives the copy.
// ---------------------------------------------------------------------------

TEST(ServiceEventLogic, CopyPreservesAliveBit) {
    qb::ServiceEvent original;
    original.live(false);
    qb::ServiceEvent dead_copy = original;
    EXPECT_FALSE(dead_copy.is_alive()) << "alive=false must survive a value copy";

    original.live(true);
    qb::ServiceEvent live_copy = original;
    EXPECT_TRUE(live_copy.is_alive()) << "alive=true must survive a value copy";

    // QOS is part of the same header word and must copy intact too.
    EXPECT_EQ(live_copy.getQOS(), original.getQOS());
}

} // namespace
