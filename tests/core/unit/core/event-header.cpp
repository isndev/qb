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
 *   - `qb::detail::event_wire` (3.2: the 16-byte header moved as ONE machine word by `reply()`,
 *     `forward()` and every same-core copy) is driven on events whose header bytes were written
 *     at the documented wire OFFSETS through `std::memcpy`, and read back through the public
 *     getters — so the byte oracle and the accessor agree on where each field lives, and the
 *     SSE2 / NEON / scalar bodies are all held to the same byte-level answer.
 *
 * Live delivery of `Pipe::push` / `allocated_push` through a real `VirtualCore` is already covered by
 * the system tier (messaging/messaging-api.cpp, event/service-event-ring.cpp,
 * messaging/messaging-reply-forward.cpp); this file deliberately stays at the engine-free header/math
 * layer and does not duplicate that.
 */

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <new>
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
    return bytes / QB_LOCKFREE_EVENT_BUCKET_BYTES + (bytes % QB_LOCKFREE_EVENT_BUCKET_BYTES != 0 ? 1u : 0u);
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
// on the wire (since 3.2 nothing on the receive path writes the header: a
// pipe copy is born with `alive == 0`, see the EventWire section), so they
// are the real default contract — previously asserted nowhere.
//
// The Header union's only default member-initializer is `prot[4]`; the bitfields read it back
// out of `prot[3]` (the `: 16, : 8` padding declarators place `alive` at bit 24). The magic
// `4 | ((bucket_bytes / 16) << 3)` therefore decodes to alive=0, qos=2, factor=bucket_bytes/16.
//
// This is a bitfield-layout oracle, hand-derived from the magic constant rather than echoed
// back from the type: if the bitfields ever stop living in their own named struct member, every
// one of them collapses onto bit 0 of `prot[0]` (in a union each member — including each
// bitfield declarator — sits at offset 0) and these values all change.
// ---------------------------------------------------------------------------

TEST(EventHeader, DefaultEventIsNotAliveWithQos2) {
    Event e;
    EXPECT_FALSE(e.is_alive()) << "a freshly built event has NOT been re-enqueued: the liveness bit "
                                  "is the reply()/forward() reuse marker and must start clear";
    EXPECT_EQ(e.getQOS(), 2u) << "the prot[] magic encodes QOS 2 (`EventQOS2 == Event`, the "
                                 "documented default) — a 1 here means the bitfields aliased bit 0";
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
// EventQOS0's constructor sets state.bits.qos = 0. `qos` and `alive` are DISJOINT fields of the
// header word, so that write must leave the liveness bit alone — and, symmetrically, the
// `alive = 1` that `reply()` / `forward()` perform on a re-enqueued event must leave `qos`
// alone. If the two ever overlap again, `__flush_all__`'s `if (!event.state.bits.qos)` drop
// test starts reading the liveness bit: a forwarded QOS0 event silently stops being droppable
// under backpressure, and QOS0 construction silently marks the event as already-reused.
// ---------------------------------------------------------------------------

TEST(EventHeader, EventQos0HasQosZero) {
    qb::EventQOS0 q0;
    EXPECT_EQ(q0.getQOS(), 0u) << "EventQOS0 ctor must encode QOS level 0";
}

TEST(EventHeader, EventQos0ConstructionDoesNotDisturbTheLivenessBit) {
    qb::EventQOS0 q0;
    EXPECT_FALSE(q0.is_alive()) << "setting qos must not write the alive bit";
}

TEST(EventHeader, EventQos0DiffersFromDefaultQos) {
    // QOS0 is genuinely a different priority than the base Event default (2).
    Event         base;
    qb::EventQOS0 q0;
    EXPECT_NE(q0.getQOS(), base.getQOS());
    EXPECT_EQ(base.getQOS(), 2u);
    EXPECT_EQ(q0.getQOS(), 0u);
}

// The reply()/forward() shape: a QOS0 event is consumed (alive=0), then re-enqueued (alive=1).
// Its QOS must survive both writes, or the engine promotes a best-effort event to guaranteed
// and spends the full 512-attempt backoff budget on it instead of dropping it.
TEST(EventHeader, TogglingLivenessPreservesQosAcrossTheReplyForwardCycle) {
    // `live()` is public only on ServiceEvent, and ServiceEvent is never QOS0 — so check the
    // qos-write half on the QOS0 event the engine actually builds, and the alive-write half on
    // a ServiceEvent. Together they pin both directions of the disjointness.
    qb::EventQOS0 q0;
    EXPECT_EQ(q0.getQOS(), 0u);
    EXPECT_FALSE(q0.is_alive());

    qb::ServiceEvent se;
    const auto       qos_before = se.getQOS();
    se.live(false); // the state every pipe copy is born with (event_wire::copy clears it)
    EXPECT_EQ(se.getQOS(), qos_before) << "clearing the liveness bit must not touch qos";
    se.live(true); // reply()/forward() raise it on the original once the copy is in the pipe
    EXPECT_EQ(se.getQOS(), qos_before) << "setting the liveness bit must not touch qos";
}

static_assert(std::is_trivially_destructible_v<qb::EventQOS0>, "QOS0 must stay byte-relocatable (no dtor) for the ring");

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
struct E1 : Event {
    char pad[1];
};
// Eover: a full-bucket-sized member forces the type to span exactly two buckets (128B).
struct Eover : Event {
    char pad[QB_LOCKFREE_EVENT_BUCKET_BYTES];
};
// EBig: a 200B member rounds the aligned type to four buckets (256B).
struct EBig : Event {
    char pad[200];
};
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
    EXPECT_EQ(static_cast<std::uint16_t>(buckets_65536), 0u) << "65536 buckets truncates the uint16 bucket_size to 0 (documented stall hazard)";

    // The largest event that still fits the receiver mailbox is < 65536 buckets; the practical cap
    // the docs cite (~1023 buckets ~= 64 KiB) survives the uint16 intact.
    const std::size_t practical_cap = std::numeric_limits<std::uint16_t>::max() / QB_LOCKFREE_EVENT_BUCKET_BYTES; // 1023
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
    EXPECT_EQ(static_cast<std::uint32_t>(se.getDestination()), static_cast<std::uint32_t>(forward_target))
        << "received() must move the forward target into dest";
    EXPECT_EQ(static_cast<std::uint32_t>(se.forward), static_cast<std::uint32_t>(dest_before))
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

    EXPECT_EQ(static_cast<std::uint32_t>(se.getDestination()), static_cast<std::uint32_t>(dest0));
    EXPECT_EQ(static_cast<std::uint32_t>(se.forward), static_cast<std::uint32_t>(forward0));
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

// ---------------------------------------------------------------------------
// CorrelatedEvent / PingEvent / RequireEvent constructors — the discovery /
// coroutine-reply carriers. `type` is a public payload field; `correlation_id`
// is inherited from CorrelatedEvent (NSDMI default 0) and set only by the
// two-arg ctors. Pure ctor logic — no engine, no router.
// ---------------------------------------------------------------------------

TEST(EventHeader, CorrelatedEventDefaultCorrelationIsZero) {
    qb::CorrelatedEvent ce;
    EXPECT_EQ(ce.correlation_id, 0ull) << "0 means 'not correlated' (the NSDMI default)";
}

TEST(EventHeader, RequireEventSingleArgCtorSetsTypeAndZeroCorrelation) {
    // The legacy broadcast reply path: only `type` is provided; correlation_id stays at the
    // CorrelatedEvent default (0 == not correlated).
    qb::RequireEvent re{42u};
    EXPECT_EQ(re.type, 42u);
    EXPECT_EQ(re.correlation_id, 0ull) << "single-arg ctor leaves correlation_id at the base default";
}

TEST(EventHeader, RequireEventTwoArgCtorCarriesCorrelation) {
    // The coroutine qb::require / qb::ping reply path: the two-arg ctor stamps the inherited
    // correlation_id so the awaiting continuation can match the reply.
    qb::RequireEvent re{7u, 0xDEADBEEFCAFEull};
    EXPECT_EQ(re.type, 7u);
    EXPECT_EQ(re.correlation_id, 0xDEADBEEFCAFEull);
}

TEST(EventHeader, PingEventCtorsSetTypeAndCorrelation) {
    // Single-arg: discovery target type, no correlation (the legacy broadcast require<>()).
    qb::PingEvent p1{9u};
    EXPECT_EQ(p1.type, 9u);
    EXPECT_EQ(p1.correlation_id, 0ull);

    // Two-arg: correlated liveness ping, correlation echoed back in the RequireEvent reply.
    qb::PingEvent p2{3u, 12345ull};
    EXPECT_EQ(p2.type, 3u);
    EXPECT_EQ(p2.correlation_id, 12345ull);
}

// ---------------------------------------------------------------------------
// Event::getSize() accessor. getSize() == bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES, but
// `bucket_size` is PRIVATE and only ever written by framework friends (VirtualCore::fill_event /
// Pipe::push), both of which require a live engine — so a *pushed* event's concrete getSize() is a
// system-tier concern (see the NOTE above). What CAN be pinned engine-free is the accessor's
// arithmetic on a value-initialized Event: value-initialization zero-inits `bucket_size` (Event's
// defaulted default ctor is not user-provided), so getSize() is exactly 0.
// ---------------------------------------------------------------------------

TEST(EventHeader, ValueInitializedEventGetSizeIsZero) {
    qb::Event e{};
    EXPECT_EQ(e.getSize(), static_cast<std::size_t>(0)) << "a value-initialized event has bucket_size 0 -> getSize() 0";
}

// ---------------------------------------------------------------------------
// qb::detail::event_wire — the 16-byte framework header as one machine word.
//
// reply() writes `dest`/`source` exchanged, forward() writes `dest` replaced — both with
// `alive` CLEARED in the same store — and every same-core copy (send/push/reply/forward into
// a pipe) reads the header as ONE 16-byte load and stores it with `alive` cleared again. The
// invariant since 3.2 is that an event handed to ANY transport carries alive == 0, whatever
// its original said (the cross-core mailbox is a raw memcpy of the rewritten original), and
// reply()/forward() raise the ORIGINAL's flag only after the copy (VirtualCore.cpp). These
// cases drive the three
// primitives on a hand-laid header: each field is written at its documented wire offset with
// std::memcpy (Event is standard-layout and trivially copyable, so the object representation
// is the wire), then read back through the PUBLIC getters. That makes the offsets an
// independent oracle: if event_wire and the getters disagreed on where `dest` lives, the
// shuffle would land on the wrong lane and getDestination() would say so.
//
// Every assertion is on bytes, so the SSE2, NEON and scalar bodies are held to one answer.
// ---------------------------------------------------------------------------

namespace {

using qb::detail::event_wire;

// The wire offsets, spelled here rather than taken from event_wire, so the test cannot inherit
// a drift from the code under test.
constexpr std::size_t kOffState  = 0;
constexpr std::size_t kOffBucket = 4;
constexpr std::size_t kOffId     = 6;
constexpr std::size_t kOffDest   = 8;
constexpr std::size_t kOffSource = 12;
constexpr std::size_t kHeader    = 16;
constexpr std::size_t kBucket    = QB_LOCKFREE_EVENT_BUCKET_BYTES;

static_assert(std::is_trivially_copyable_v<Event>, "the header is driven through its object representation");
static_assert(std::is_standard_layout_v<Event>, "the wire offsets below assume standard layout");

constexpr std::uint32_t
bits(ActorId const &a) noexcept {
    return std::bit_cast<std::uint32_t>(a);
}

// ActorId's (ServiceId, CoreId) constructor is protected; its public u32 form is the wire word
// itself — service id in the low half, core index in the high half (little-endian, `_service_id`
// declared first). The oracle test below checks that reading through sid()/index().
inline ActorId
actor(std::uint32_t const sid, std::uint32_t const core) noexcept {
    return ActorId(sid | (core << 16));
}

template <typename T>
void
poke(void *base, std::size_t off, T const &v) noexcept {
    std::memcpy(static_cast<unsigned char *>(base) + off, &v, sizeof v);
}

// A bucket-aligned byte image large enough for `Buckets` buckets, holding one Event at 0.
template <std::size_t Buckets>
struct Image {
    alignas(QB_LOCKFREE_EVENT_BUCKET_BYTES) unsigned char bytes[Buckets * kBucket];

    Image() noexcept {
        // A recognisable, position-dependent tail so a payload byte moved or dropped is visible.
        for (std::size_t i = 0; i < sizeof bytes; ++i)
            bytes[i] = static_cast<unsigned char>(0xA0u + (i * 7u) % 0x5Fu);
        new (bytes) Event(); // default header: alive=0, qos=2, dest/source NotFound
        poke(bytes, kOffBucket, static_cast<std::uint16_t>(Buckets));
        poke(bytes, kOffId, static_cast<std::uint16_t>(0xBEEF));
        poke(bytes, kOffDest, std::bit_cast<std::uint32_t>(actor(0x1234, 2)));
        poke(bytes, kOffSource, std::bit_cast<std::uint32_t>(actor(0x5678, 1)));
    }
    Event &
    event() noexcept {
        return *reinterpret_cast<Event *>(bytes);
    }
    Event const &
    event() const noexcept {
        return *reinterpret_cast<Event const *>(bytes);
    }
    void
    set_alive() noexcept {
        bytes[3] |= 0x01; // Header bit 24 == bit 0 of byte 3 (little-endian), see EventHeader above
    }
};

// Byte-equality of a range, reported with the first differing offset.
::testing::AssertionResult
same_bytes(unsigned char const *a, unsigned char const *b, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to; ++i)
        if (a[i] != b[i])
            return ::testing::AssertionFailure() << "byte " << i << " differs: " << int{a[i]} << " vs " << int{b[i]};
    return ::testing::AssertionSuccess();
}

} // namespace

// The byte oracle itself: the offsets the section relies on are the ones the getters read.
TEST(EventWire, HandLaidHeaderReadsBackThroughTheGetters) {
    Image<1> img;
    Event   &e = img.event();
    EXPECT_FALSE(e.is_alive());
    EXPECT_EQ(e.getQOS(), 2u);
    EXPECT_EQ(e.getSize(), kBucket);
    EXPECT_EQ(e.getID(), static_cast<qb::EventId>(0xBEEF));
    EXPECT_EQ(bits(e.getDestination()), bits(actor(0x1234, 2)));
    EXPECT_EQ(e.getDestination().sid(), 0x1234u);
    EXPECT_EQ(e.getDestination().index(), 2u);
    EXPECT_EQ(bits(e.getSource()), bits(actor(0x5678, 1)));
    EXPECT_EQ(e.getSource().sid(), 0x5678u);
    EXPECT_EQ(e.getSource().index(), 1u);
    img.set_alive();
    EXPECT_TRUE(e.is_alive()) << "byte 3 bit 0 must be the liveness bit event_wire masks";
    EXPECT_EQ(e.getQOS(), 2u) << "raising alive through the byte must not touch qos";
}

TEST(EventWire, SwapDestSourceExchangesExactlyTheTwoIds) {
    Image<1>       img;
    Image<1> const before = img;
    event_wire::swap_dest_source(img.event());

    EXPECT_EQ(bits(img.event().getDestination()), bits(actor(0x5678, 1))) << "dest must take the old source";
    EXPECT_EQ(bits(img.event().getSource()), bits(actor(0x1234, 2))) << "source must take the old dest";
    EXPECT_TRUE(same_bytes(img.bytes, before.bytes, kOffState, kOffDest)) << "state/bucket_size/id must not move";
    EXPECT_TRUE(same_bytes(img.bytes, before.bytes, kHeader, sizeof img.bytes)) << "payload must not move";

    event_wire::swap_dest_source(img.event());
    EXPECT_TRUE(same_bytes(img.bytes, before.bytes, 0, sizeof img.bytes)) << "swap is an involution";
}

TEST(EventWire, SwapDestSourceClearsTheLivenessBitAndNothingElseInTheStateWord) {
    // reply() on an event whose original is already alive (replied once, or a hand-marked
    // ServiceEvent): the rewrite must hand send() an original that says 0, because the
    // cross-core mailbox relocates it byte for byte. qos, bucket_size and id ride through.
    Image<1> img;
    img.set_alive();
    Image<1> const before = img;
    event_wire::swap_dest_source(img.event());
    EXPECT_FALSE(img.event().is_alive()) << "the header handed to the transport must say alive == 0";
    EXPECT_EQ(img.event().getQOS(), 2u);
    EXPECT_EQ(img.event().getSize(), kBucket);
    EXPECT_EQ(img.event().getID(), static_cast<qb::EventId>(0xBEEF));
    EXPECT_TRUE(same_bytes(img.bytes, before.bytes, kOffState, 3)) << "the magic bytes must not move";
    EXPECT_EQ(img.bytes[3], static_cast<unsigned char>(before.bytes[3] & ~0x01u)) << "only bit 24 changes";
}

TEST(EventWire, SetDestReplacesOnlyTheDestinationLane) {
    Image<1>       img;
    Image<1> const before = img;
    const ActorId  target = actor(0x0042, 3);
    event_wire::set_dest(img.event(), target);

    EXPECT_EQ(bits(img.event().getDestination()), bits(target));
    EXPECT_EQ(bits(img.event().getSource()), bits(actor(0x5678, 1))) << "forward() preserves the source";
    EXPECT_TRUE(same_bytes(img.bytes, before.bytes, kOffState, kOffDest)) << "state/bucket_size/id must not move";
    EXPECT_TRUE(same_bytes(img.bytes, before.bytes, kOffSource, sizeof img.bytes)) << "source and payload must not move";
}

TEST(EventWire, SetDestClearsTheLivenessBitAndNothingElseInTheStateWord) {
    // forward() after reply() on the same event: the original says alive, the rewrite must not.
    Image<1> img;
    img.set_alive();
    Image<1> const before = img;
    event_wire::set_dest(img.event(), actor(0x0042, 3));
    EXPECT_FALSE(img.event().is_alive());
    EXPECT_EQ(img.event().getQOS(), 2u);
    EXPECT_EQ(img.event().getSize(), kBucket);
    EXPECT_EQ(img.event().getID(), static_cast<qb::EventId>(0xBEEF));
    EXPECT_EQ(bits(img.event().getDestination()), bits(actor(0x0042, 3)));
    EXPECT_TRUE(same_bytes(img.bytes, before.bytes, kOffState, 3));
    EXPECT_EQ(img.bytes[3], static_cast<unsigned char>(before.bytes[3] & ~0x01u));
    EXPECT_TRUE(same_bytes(img.bytes, before.bytes, kOffSource, sizeof img.bytes));
}

TEST(EventWire, SetDestAcceptsEveryBitPatternIncludingNotFoundAndBroadcast) {
    // The lane insert must be a full 32-bit replace: no bit of the previous dest may survive
    // (an OR without the AND would keep 0x1234 in the low half of an all-zero id).
    Image<1> img;
    event_wire::set_dest(img.event(), ActorId());
    EXPECT_EQ(static_cast<std::uint32_t>(img.event().getDestination()), ActorId::NotFound);
    event_wire::set_dest(img.event(), qb::BroadcastId(5));
    EXPECT_TRUE(img.event().getDestination().is_broadcast());
    EXPECT_EQ(img.event().getDestination().index(), 5u);
    event_wire::set_dest(img.event(), actor(0xFFFF, 0xFFFF));
    EXPECT_EQ(static_cast<std::uint32_t>(img.event().getDestination()), 0xFFFFFFFFu);
    EXPECT_EQ(bits(img.event().getSource()), bits(actor(0x5678, 1)));
}

TEST(EventWire, CopyOfALiveEventIsBornDeadAndOtherwiseByteIdentical) {
    Image<1> src;
    src.set_alive(); // the original of a reply(): raised AFTER its own copy, then copied again by a re-push
    Image<1> dst;
    std::fill(std::begin(dst.bytes), std::end(dst.bytes), 0x00);

    Event &copied = event_wire::copy(dst.bytes, src.event(), kBucket);
    EXPECT_EQ(&copied, &dst.event()) << "copy() returns the copy in place";

    EXPECT_FALSE(copied.is_alive()) << "an event in a pipe always carries alive == 0";
    EXPECT_TRUE(src.event().is_alive()) << "the source is read, never written";
    // Everything but the liveness bit is the source, byte for byte.
    Image<1> expect = src;
    expect.bytes[3] &= static_cast<unsigned char>(~0x01u);
    EXPECT_TRUE(same_bytes(dst.bytes, expect.bytes, 0, sizeof dst.bytes));
    EXPECT_EQ(copied.getQOS(), 2u);
    EXPECT_EQ(copied.getSize(), kBucket);
    EXPECT_EQ(copied.getID(), static_cast<qb::EventId>(0xBEEF));
    EXPECT_EQ(bits(copied.getDestination()), bits(actor(0x1234, 2)));
    EXPECT_EQ(bits(copied.getSource()), bits(actor(0x5678, 1)));
}

TEST(EventWire, CopyOfADeadEventIsByteIdentical) {
    Image<1> src;
    Image<1> dst;
    std::fill(std::begin(dst.bytes), std::end(dst.bytes), 0xFF);
    event_wire::copy(dst.bytes, src.event(), kBucket);
    EXPECT_TRUE(same_bytes(dst.bytes, src.bytes, 0, sizeof dst.bytes));
}

TEST(EventWire, OneBucketCopyWritesExactlyOneBucket) {
    // The fast path is three unrolled 16-byte moves after the header; a fourth would overrun
    // the slot the pipe allocated. Guard the bytes past the bucket with a canary.
    Image<1> src;
    Image<2> dst;
    std::fill(std::begin(dst.bytes), std::end(dst.bytes), 0xCC);
    event_wire::copy(dst.bytes, src.event(), kBucket);
    EXPECT_TRUE(same_bytes(dst.bytes, src.bytes, 0, kBucket));
    for (std::size_t i = kBucket; i < sizeof dst.bytes; ++i)
        ASSERT_EQ(dst.bytes[i], 0xCC) << "byte " << i << " past the bucket was written";
}

TEST(EventWire, MultiBucketCopyMovesTheWholeTailAndClearsAlive) {
    // A 3-bucket event takes the memcpy tail path; the header treatment is the same.
    Image<3> src;
    src.set_alive();
    Image<4> dst;
    std::fill(std::begin(dst.bytes), std::end(dst.bytes), 0x00);

    Event &copied = event_wire::copy(dst.bytes, src.event(), 3 * kBucket);
    EXPECT_FALSE(copied.is_alive());
    EXPECT_EQ(copied.getSize(), 3 * kBucket);
    Image<3> expect = src;
    expect.bytes[3] &= static_cast<unsigned char>(~0x01u);
    EXPECT_TRUE(same_bytes(dst.bytes, expect.bytes, 0, 3 * kBucket));
    for (std::size_t i = 3 * kBucket; i < sizeof dst.bytes; ++i)
        ASSERT_EQ(dst.bytes[i], 0x00) << "byte " << i << " past the event was written";
}

TEST(EventWire, TwoBucketCopyIsExactOnTheBoundary) {
    // Exactly two buckets: the tail memcpy is one full bucket, neither short nor long.
    Image<2> src;
    Image<3> dst;
    std::fill(std::begin(dst.bytes), std::end(dst.bytes), 0x5A);
    event_wire::copy(dst.bytes, src.event(), 2 * kBucket);
    EXPECT_TRUE(same_bytes(dst.bytes, src.bytes, 0, 2 * kBucket));
    for (std::size_t i = 2 * kBucket; i < sizeof dst.bytes; ++i)
        ASSERT_EQ(dst.bytes[i], 0x5A) << "byte " << i << " past the event was written";
}

TEST(EventWire, ReplyShapeSwapThenCopyLeavesTheOriginalToBeRaised) {
    // VirtualCore::reply(): swap the original's ids, copy it (the copy is dead), THEN raise
    // the original — so the dispatcher skips the original's destructor while the copy runs
    // its own once. Composed here without an engine, on an original that was ALREADY replied
    // once (alive set), which is the fan-out shape the cross-core memcpy relies on.
    Image<1> original;
    original.set_alive();
    Image<1> pipe;
    event_wire::swap_dest_source(original.event());
    EXPECT_FALSE(original.event().is_alive()) << "what send() sees";
    Event &in_pipe = event_wire::copy(pipe.bytes, original.event(), kBucket);
    original.set_alive();

    EXPECT_TRUE(original.event().is_alive());
    EXPECT_FALSE(in_pipe.is_alive());
    EXPECT_EQ(bits(in_pipe.getDestination()), bits(actor(0x5678, 1))) << "the reply goes back to the source";
    EXPECT_EQ(bits(in_pipe.getSource()), bits(actor(0x1234, 2)));
    EXPECT_EQ(in_pipe.getID(), original.event().getID());
    EXPECT_EQ(in_pipe.getSize(), original.event().getSize());
    EXPECT_TRUE(same_bytes(pipe.bytes, original.bytes, kHeader, kBucket)) << "payload travels intact";
}

} // namespace
