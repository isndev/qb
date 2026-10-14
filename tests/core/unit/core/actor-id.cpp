/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/actor-id.cpp
 * @brief `qb::ActorId` / `qb::BroadcastId` / `qb::CoreIdBitSet` value types (`qb/core/ActorId.h`).
 *
 * Engine-free codec, identity and bitset tests. Literal packed values are an independent oracle
 * for the 24-slot / 8-core / 32-generation representation. Ordinary instances, service addresses
 * and service instances stay distinct; a rejected value never becomes an address by narrowing.
 */

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <qb/core/Actor.h> // qb::detail::ask_take & co — the correlation-id codec and the pending-ask registry
#include <qb/core/ActorId.h>
#include <qb/core/detail/actor_id_pool.h>

using qb::ActorId;
using qb::BroadcastId;
using qb::CoreId;
using qb::CoreIdBitSet;
using qb::ServiceId;

namespace {
// Ordinary owners for the engine-free ask-registry cases below.
struct NormalId : public ActorId {
    NormalId(qb::ActorSlotId slot, CoreId index) noexcept
        : ActorId(ActorId::ordinary(slot, index, 1)) {}
};
} // namespace

// --- ActorId -----------------------------------------------------------------

TEST(ActorId, DefaultIsNotFoundAndInvalid) {
    ActorId id;
    EXPECT_FALSE(id.is_valid());
    EXPECT_EQ(id.raw64(), 0u);
    EXPECT_FALSE(id.is_broadcast());
}

TEST(ActorId, Raw64RoundTripPreservesAllThreeComponents) {
    constexpr auto id = ActorId::ordinary(0x123456u, 255, 0x11223344u);
    EXPECT_EQ(id.raw64(), 0x11223344ff123456ull);
    const auto round = ActorId::from_raw(0x11223344ff123456ull);
    EXPECT_EQ(round, id);
    EXPECT_EQ(round.slot(), 0x123456u);
    EXPECT_EQ(round.index(), 255u);
    EXPECT_EQ(round.generation(), 0x11223344u);
    EXPECT_TRUE(round.is_ordinary());
    EXPECT_TRUE(round.is_instance());
}

TEST(ActorId, BroadcastIdProperties) {
    BroadcastId b(3);
    EXPECT_TRUE(b.is_broadcast());
    EXPECT_TRUE(b.is_valid());
    EXPECT_EQ(b.sid(), ActorId::BroadcastSid);
    EXPECT_EQ(b.index(), static_cast<CoreId>(3));
}

TEST(ActorId, SameSlotDifferentGenerationIsADifferentUnorderedKey) {
    const auto old         = ActorId::ordinary(0x10000u, 5, 1);
    const auto replacement = ActorId::ordinary(0x10000u, 5, 0x80000001u);
    EXPECT_EQ(old.slot(), replacement.slot());
    EXPECT_EQ(old.index(), replacement.index());
    EXPECT_NE(old, replacement);
    std::unordered_set<ActorId> ids{old, replacement};
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids.count(old), 1u);
    EXPECT_EQ(ids.count(replacement), 1u);
    ids.erase(old);
    EXPECT_EQ(ids.count(replacement), 1u);
}

TEST(ActorId, LogicalServiceAndCapturedInstanceAreDistinct) {
    const auto address  = ActorId::service_address(0x1234u, 7);
    const auto instance = ActorId::service_instance(0x1234u, 7, 0x88776655u);
    EXPECT_EQ(address.raw64(), 0x0000000007ff1234ull);
    EXPECT_EQ(instance.raw64(), 0x8877665507ff1234ull);
    EXPECT_EQ(address.service_tag(), 0x1234u);
    EXPECT_EQ(address.slot(), 0xff1234u);
    EXPECT_TRUE(address.is_service_address());
    EXPECT_FALSE(address.is_instance());
    EXPECT_TRUE(instance.is_service_instance());
    EXPECT_TRUE(instance.is_instance());
    EXPECT_NE(address, instance);
    EXPECT_EQ(ActorId::from_raw(instance.raw64()), instance);
    EXPECT_FALSE(ActorId::ordinary(address.slot(), 7, 1).is_valid());
}

TEST(ActorId, ServiceIdZeroIsInvalidOnEveryCore) {
    for (CoreId const core : {CoreId{0}, CoreId{1}, CoreId{3}, CoreId{255}, std::numeric_limits<CoreId>::max()}) {
        EXPECT_FALSE(ActorId::service_address(0, core).is_valid());
        EXPECT_FALSE(ActorId::ordinary(0, core, 1).is_valid());
    }
    EXPECT_TRUE(NormalId(1, 0).is_valid());
    EXPECT_TRUE(BroadcastId(0).is_valid());
}

TEST(ActorId, DecoderRejectsMalformedKindsAndConstructorsCheckCoreBeforeNarrowing) {
    // Literal encodings: zero ordinary generation, service tag zero, and a generated broadcast.
    for (const std::uint64_t raw :
         {0x0000000007000001ull, 0x0000000107000000ull, 0x0000000007ff0000ull, 0x0000000107ff0000ull, 0x0000000107ffffffull}) {
        const auto rejected = ActorId::from_raw(raw);
        EXPECT_FALSE(rejected.is_valid()) << raw;
        EXPECT_EQ(rejected.raw64(), 0u);
    }
    EXPECT_FALSE(ActorId::ordinary(1, 256, 1).is_valid());
    EXPECT_FALSE(ActorId::service_address(1, 256).is_valid());
    EXPECT_FALSE(ActorId::service_instance(1, 256, 1).is_valid());
    EXPECT_FALSE(BroadcastId(256).is_valid());
    EXPECT_FALSE(BroadcastId(65536).is_valid());
    EXPECT_FALSE(ActorId::ordinary(1, 0, 0).is_valid());
    EXPECT_FALSE(ActorId::service_instance(1, 0, 0).is_valid());
    EXPECT_FALSE(ActorId::service_address(65535, 0).is_valid());
    const auto last = ActorId::ordinary(0xfeffffu, 255, 0xffffffffu);
    EXPECT_EQ(last.raw64(), 0xfffffffffffeffffull);
    EXPECT_TRUE(last.is_valid());
    EXPECT_EQ(BroadcastId(255).raw64(), 0x00000000ffffffffull);
}

static_assert(!std::is_constructible_v<ActorId, std::uint32_t>, "a 32-bit value cannot preserve an instance identity");
static_assert(!std::is_convertible_v<ActorId, std::uint32_t>, "use the explicit lossless codec");
static_assert(!std::is_convertible_v<ActorId, std::uint64_t>, "use raw64 rather than implicit numeric persistence");
static_assert(std::is_trivially_copyable_v<ActorId>);
static_assert(sizeof(ActorId) == 8);

TEST(ActorIdPool, ReleasedInstancesAdvanceAndExhaustedGenerationRetiresTheSlot) {
    qb::detail::ActorIdPool pool(3, 2, 3);
    const auto              first = pool.reserve_ordinary();
    const auto              other = pool.reserve_ordinary();
    ASSERT_TRUE(first.is_valid());
    ASSERT_TRUE(other.is_valid());
    ASSERT_NE(first.slot(), other.slot());
    EXPECT_FALSE(pool.reserve_ordinary().is_valid());
    EXPECT_TRUE(pool.mark_live(first));
    pool.release(first);

    const auto second = pool.reserve_ordinary();
    ASSERT_EQ(second.slot(), first.slot());
    EXPECT_EQ(second.generation(), 2u);
    EXPECT_NE(second, first);
    pool.release(first); // a stale release must not return the current owner's slot
    EXPECT_TRUE(pool.owns_reservation(second));
    EXPECT_FALSE(pool.reserve_ordinary().is_valid());
    pool.release(second);

    const auto last = pool.reserve_ordinary();
    ASSERT_EQ(last.slot(), first.slot());
    EXPECT_EQ(last.generation(), 3u);
    EXPECT_TRUE(pool.mark_live(last));
    EXPECT_TRUE(pool.mark_retiring(last));
    pool.release(last);
    EXPECT_FALSE(pool.reserve_ordinary().is_valid()) << "an exhausted generation must retire, never wrap";
    pool.release(other);
    const auto reused_other = pool.reserve_ordinary();
    EXPECT_EQ(reused_other.slot(), other.slot());
    EXPECT_EQ(reused_other.generation(), 2u);
    EXPECT_EQ(pool.ordinary_slot_count(), 3u) << "the indexed bank includes its unused slot-zero row";
}

TEST(ActorIdPool, LogicalServiceIsNeverAReservationAndServiceGenerationCannotWrap) {
    qb::detail::ActorIdPool pool(7, 1, 2);
    const auto              ordinary = pool.reserve_ordinary();
    const auto              first    = pool.reserve_service(65534);
    ASSERT_TRUE(first.is_service_instance());
    EXPECT_EQ(first.service_tag(), 65534u);
    EXPECT_EQ(first.index(), 7u);
    EXPECT_FALSE(pool.reserve_service(65534).is_valid());
    EXPECT_FALSE(pool.owns_reservation(ActorId::service_address(65534, 7)));
    EXPECT_EQ(pool.ordinary_slot_count(), 2u) << "one ordinary slot plus row zero; a service must not grow this bank";
    EXPECT_LT(pool.metadata_bytes(), 1024u * 1024u) << "one service must not allocate the 24-bit address space";
    EXPECT_TRUE(pool.mark_live(first));
    pool.release(first);
    const auto second = pool.reserve_service(65534);
    ASSERT_TRUE(second.is_valid());
    EXPECT_EQ(second.generation(), 2u);
    EXPECT_NE(second, first);
    pool.release(first);
    EXPECT_TRUE(pool.owns_reservation(second));
    pool.release(second);
    EXPECT_FALSE(pool.reserve_service(65534).is_valid());
    EXPECT_TRUE(pool.owns_reservation(ordinary));
}

TEST(ActorIdPool, ClearingPromisesInvalidatesReservationsAndPreservesLiveOwners) {
    qb::detail::ActorIdPool pool(0, 2, 3);
    const auto              live     = pool.reserve_ordinary();
    const auto              promised = pool.reserve_ordinary();
    ASSERT_TRUE(pool.mark_live(live));
    pool.clear_reservations();
    const auto replacement = pool.reserve_ordinary();
    EXPECT_EQ(replacement.slot(), promised.slot());
    EXPECT_NE(replacement, promised);
    pool.release(promised);
    EXPECT_TRUE(pool.owns_reservation(replacement));
    EXPECT_FALSE(pool.reserve_ordinary().is_valid()) << "clear must keep the live owner's capacity taken";
}

TEST(ActorIdPool, TakingAndReturningMetadataPreservesGenerationAndResetsTheSource) {
    qb::detail::ActorIdPool owner(7, 2, 3);
    const auto              first = owner.reserve_ordinary();
    owner.release(first);
    const auto live = owner.reserve_ordinary();
    ASSERT_EQ(live.generation(), 2u);
    ASSERT_TRUE(owner.mark_live(live));
    {
        auto worker = owner.take();
        EXPECT_EQ(owner.metadata_bytes(), 0u);
        EXPECT_EQ(owner.slot_count(), 0u);
        EXPECT_GT(worker.metadata_bytes(), 0u);
        worker.release(live);
        owner = worker.take();
        EXPECT_EQ(worker.metadata_bytes(), 0u);
        EXPECT_EQ(worker.slot_count(), 0u);
    } // destroying the emptied worker must not free the metadata returned to owner
    const auto last = owner.reserve_ordinary();
    EXPECT_EQ(last.slot(), first.slot());
    EXPECT_EQ(last.index(), 7u);
    EXPECT_EQ(last.generation(), 3u);
    owner.release(last);
    const auto other = owner.reserve_ordinary();
    EXPECT_NE(other.slot(), first.slot()) << "the transferred generation ceiling must still retire the slot";
    EXPECT_FALSE(owner.reserve_ordinary().is_valid()) << "the transferred ordinary limit remains two";
}

// `ask_take()` names the OWNER actor's core index in the high 16 bits of the id, so two
// VirtualCores' independent registries (each handing out the same (generation, slot) pairs) can
// never produce the same id. Without it (the audited type-confusion bug), a cross-core `ask` reply
// carrying a colliding id resolved the receiver's own pending slot and handed its `ask_awaiter<E>`
// an event of the wrong type. Pure codec — no engine, no running core; the entries are given back
// so the test thread's registry is left as it was found.
TEST(ActorId, AskCorrelationIdIsSaltedWithOwnerCoreIndex) {
    const NormalId owner_core0(5, 0);
    const NormalId owner_core7(5, 7);

    const std::uint64_t id0 = qb::detail::ask_take(owner_core0, nullptr);
    const std::uint64_t id7 = qb::detail::ask_take(owner_core7, nullptr);

    // High 16 bits carry the owning core index — the cross-core disambiguator.
    EXPECT_EQ(id0 >> 48, 0u);
    EXPECT_EQ(id7 >> 48, 7u);
    // Distinct owning cores therefore never collide, whatever the registries hand out.
    EXPECT_NE(id0, id7);
    // Low 48 bits locate the registry entry (generation + slot), never 0 (0 is "not an ask").
    EXPECT_NE(id0 & 0x0000FFFFFFFFFFFFull, 0u);
    EXPECT_NE(id7 & 0x0000FFFFFFFFFFFFull, 0u);

    // Same owner, successive calls: a different entry while the core bits stay put.
    const std::uint64_t id0b = qb::detail::ask_take(owner_core0, nullptr);
    EXPECT_EQ(id0b >> 48, 0u);
    EXPECT_NE(id0b, id0);

    qb::detail::ask_unregister(id0);
    qb::detail::ask_unregister(id7);
    qb::detail::ask_unregister(id0b);
}

namespace {
struct probe_slot {
    qb::detail::ask_slot slot{};
    int                  delivered = 0;
    static void
    deliver(void *self, qb::Event &) noexcept {
        ++static_cast<probe_slot *>(self)->delivered;
    }
    explicit probe_slot(qb::ActorId owner) {
        slot.owner   = owner;
        slot.self    = this;
        slot.deliver = &probe_slot::deliver;
    }
};
} // namespace

// The registry is a slot table the id indexes (`[core:16][generation:26][slot:22]`), not a hash
// map: an entry is taken AND bound to its owner's slot by `ask_take` (one call — every owner is
// built before the send, so there is no unbound window), released by `ask_unregister`, and a
// released id must MISS for ever after — including once its entry has been handed to a later
// ask, which is what the generation is for. Driven directly on the test thread.
TEST(ActorId, AskRegistryReleasedIdMissesAndSlotReuseAdvancesGeneration) {
    constexpr std::uint64_t slot_mask = (std::uint64_t{1} << 22) - 1;
    const NormalId          owner(5, 3);
    const NormalId          other_actor_same_core(6, 3);
    const NormalId          same_sid_other_core(5, 4);
    const auto              same_slot_new_incarnation = ActorId::ordinary(5, 3, 0x80000001u);
    qb::Event               resp{};

    probe_slot          p{owner};
    const std::uint64_t id = qb::detail::ask_take(owner, &p.slot);

    // Delivered to its owner, refused to anyone else — another actor of the same core, and the
    // same actor index on another core (the id's core bits do not match the delivering actor's).
    EXPECT_FALSE(qb::detail::ask_deliver(id, other_actor_same_core, resp));
    EXPECT_FALSE(qb::detail::ask_deliver(id, same_sid_other_core, resp));
    EXPECT_FALSE(qb::detail::ask_deliver(id, same_slot_new_incarnation, resp));
    EXPECT_EQ(p.delivered, 0);
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, resp));
    EXPECT_EQ(p.delivered, 1);
    EXPECT_FALSE(qb::detail::ask_deliver(0, owner, resp)); // 0 is "not an ask"

    // Released: the same id resolves nothing, and releasing it again is a no-op.
    qb::detail::ask_unregister(id);
    EXPECT_FALSE(qb::detail::ask_deliver(id, owner, resp));
    qb::detail::ask_unregister(id);
    EXPECT_EQ(p.delivered, 1);

    // Drain the free list so the released entry is handed out again (FIFO: it is at the tail)
    // and keep going past the initial 64 entries so the table has to GROW under live ids; then
    // check the reissued id names the SAME slot under a DIFFERENT generation, and that the
    // stale id still misses even though its slot is live again — bound to `q` now, not `p`.
    probe_slot                 q{owner};
    std::vector<std::uint64_t> taken;
    std::uint64_t              reissued = 0;
    while (!reissued || taken.size() < 200) {
        const std::uint64_t t = qb::detail::ask_take(owner, &q.slot);
        if ((t & slot_mask) == (id & slot_mask))
            reissued = t;
        else
            taken.push_back(t);
    }
    ASSERT_NE(reissued, 0u) << "the released entry was never reissued";
    EXPECT_NE(reissued, id);
    EXPECT_EQ(reissued >> 48, id >> 48);
    EXPECT_EQ((reissued >> 22) & ((std::uint64_t{1} << 26) - 1), ((id >> 22) & ((std::uint64_t{1} << 26) - 1)) + 1);
    EXPECT_FALSE(qb::detail::ask_deliver(id, owner, resp)); // stale generation
    EXPECT_TRUE(qb::detail::ask_deliver(reissued, owner, resp));
    EXPECT_EQ(q.delivered, 1);
    EXPECT_EQ(p.delivered, 1);

    // Every other live entry delivers to its owner exactly once and releases cleanly; the table
    // grew past its initial 64 entries above and every id stayed distinct.
    std::sort(taken.begin(), taken.end());
    EXPECT_EQ(std::adjacent_find(taken.begin(), taken.end()), taken.end());
    EXPECT_GE(taken.size(), 200u);
    for (const auto t : taken) {
        EXPECT_TRUE(qb::detail::ask_deliver(t, owner, resp));
        qb::detail::ask_unregister(t);
        EXPECT_FALSE(qb::detail::ask_deliver(t, owner, resp));
    }
    EXPECT_EQ(q.delivered, 1 + static_cast<int>(taken.size()));
    qb::detail::ask_unregister(reissued);
    EXPECT_FALSE(qb::detail::ask_deliver(reissued, owner, resp));
}

// --- CoreIdBitSet ------------------------------------------------------------

TEST(CoreIdBitSet, DefaultEmpty) {
    CoreIdBitSet s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_FALSE(s.contains(0));
}

TEST(CoreIdBitSet, InsertContainsRemoveClear) {
    CoreIdBitSet s;
    s.insert(1);
    s.emplace(4); // alias for insert
    EXPECT_TRUE(s.contains(1));
    EXPECT_TRUE(s.contains(4));
    EXPECT_FALSE(s.contains(2));
    EXPECT_EQ(s.size(), 2u);
    EXPECT_FALSE(s.empty());
    s.remove(1);
    EXPECT_FALSE(s.contains(1));
    EXPECT_EQ(s.size(), 1u);
    s.clear();
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
}

TEST(CoreIdBitSet, OutOfRangeIdsAreIgnored) {
    CoreIdBitSet s;
    s.insert(static_cast<CoreId>(300)); // >= MaxCores (256) → ignored
    EXPECT_TRUE(s.empty());
    EXPECT_FALSE(s.contains(static_cast<CoreId>(300)));
    s.remove(static_cast<CoreId>(300)); // no-op, must not crash
}

TEST(CoreIdBitSet, ExactMaxCoresBoundary) {
    // MaxCores == 256, so valid indices are [0, 255]. Pin the exact boundary: 255 is accepted,
    // 256 (the first out-of-range value) is silently dropped — the original only probed 300.
    static_assert(qb::MaxCores == 256, "boundary cases assume a 256-core ceiling");
    CoreIdBitSet s;

    s.insert(static_cast<CoreId>(255)); // last valid index
    EXPECT_TRUE(s.contains(static_cast<CoreId>(255)));
    EXPECT_EQ(s.size(), 1u);

    s.insert(static_cast<CoreId>(256)); // first out-of-range index → ignored
    EXPECT_FALSE(s.contains(static_cast<CoreId>(256)));
    EXPECT_EQ(s.size(), 1u) << "256 must not be stored";

    // The initializer-list ctor applies the same filter: 256 is dropped, 255 kept.
    CoreIdBitSet viaList{static_cast<CoreId>(255), static_cast<CoreId>(256)};
    EXPECT_EQ(viaList.size(), 1u);
    EXPECT_TRUE(viaList.contains(static_cast<CoreId>(255)));
    EXPECT_FALSE(viaList.contains(static_cast<CoreId>(256)));

    // contains() is also range-guarded: querying an out-of-range id is false, never a crash.
    EXPECT_FALSE(s.contains(static_cast<CoreId>(256)));
    EXPECT_FALSE(s.contains(std::numeric_limits<CoreId>::max()));
}

TEST(CoreIdBitSet, SetFromRawRoundTripEquality) {
    // raw() (an unordered_set<CoreId>) reconstructs an equal bitset via the set ctor: round-trip
    // membership must be identical, including the high boundary index 255.
    CoreIdBitSet b{0, 4, 200, static_cast<CoreId>(255)};
    CoreIdBitSet rebuilt(b.raw());

    EXPECT_EQ(rebuilt.size(), b.size());
    EXPECT_EQ(rebuilt.raw(), b.raw()) << "set-from-raw must reproduce the exact membership";
    EXPECT_EQ(rebuilt.bits(), b.bits());
    for (CoreId c : b)
        EXPECT_TRUE(rebuilt.contains(c)) << "member " << c << " lost in raw round-trip";
    EXPECT_TRUE(rebuilt.contains(static_cast<CoreId>(255)));
}

TEST(CoreIdBitSet, InitializerListAndSetConstructors) {
    CoreIdBitSet a{2, 5, 9};
    EXPECT_EQ(a.size(), 3u);
    EXPECT_TRUE(a.contains(5));

    qb::unordered_set<CoreId> src{3, 7};
    CoreIdBitSet              b(src);
    EXPECT_EQ(b.size(), 2u);
    EXPECT_TRUE(b.contains(3));
    EXPECT_TRUE(b.contains(7));
}

TEST(CoreIdBitSet, ConvertersAndRawBits) {
    CoreIdBitSet s{1, 4, 8};
    auto         v = s.to_vector();
    EXPECT_EQ(v.size(), 3u);
    EXPECT_TRUE(std::find(v.begin(), v.end(), 4) != v.end());

    auto us = s.to_unordered_set();
    EXPECT_EQ(us.size(), 3u);
    EXPECT_EQ(us, s.raw());

    EXPECT_EQ(s.bits().count(), 3u); // underlying std::bitset
}

TEST(CoreIdBitSet, Iteration) {
    CoreIdBitSet        s{0, 3, 200};
    std::vector<CoreId> seen;
    for (CoreId c : s)
        seen.push_back(c);
    std::sort(seen.begin(), seen.end());
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], static_cast<CoreId>(0));
    EXPECT_EQ(seen[1], static_cast<CoreId>(3));
    EXPECT_EQ(seen[2], static_cast<CoreId>(200));
}

TEST(CoreIdBitSet, PostIncrementIteratorVisitsEachMember) {
    // The range-for Iteration test above only drives PRE-increment (operator++()); the
    // post-increment operator++(int) — which returns a COPY of the iterator positioned BEFORE the
    // advance — was never exercised. A manual for-loop with `it++` covers it and the same visited
    // membership must fall out.
    CoreIdBitSet        s{1, 3, 5};
    std::vector<CoreId> seen;
    for (auto it = s.begin(); it != s.end(); it++)
        seen.push_back(*it);
    std::sort(seen.begin(), seen.end());
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], static_cast<CoreId>(1));
    EXPECT_EQ(seen[1], static_cast<CoreId>(3));
    EXPECT_EQ(seen[2], static_cast<CoreId>(5));

    // The returned pre-advance copy is the whole point of operator++(int): capture it, advance the
    // live iterator, and confirm the copy still dereferences to the OLD position while the live one
    // has moved on to the next set bit.
    auto it  = s.begin(); // -> 1 (first set bit)
    auto old = it++;      // old -> 1, it -> 3
    EXPECT_EQ(*old, static_cast<CoreId>(1)) << "post-increment must return the pre-advance position";
    EXPECT_EQ(*it, static_cast<CoreId>(3)) << "the live iterator advanced to the next member";
}
