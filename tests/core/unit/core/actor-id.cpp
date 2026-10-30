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
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
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

TEST(ActorIdPool, FreeIdentitiesKeepIndependentGenerationsAndRejectUnownedTransitions) {
    qb::detail::ActorIdPool pool(255, 3, 3);
    const auto              first  = pool.reserve_ordinary();
    const auto              second = pool.reserve_ordinary();
    const auto              third  = pool.reserve_ordinary();
    ASSERT_TRUE(pool.mark_live(first));
    pool.release(first);
    const auto first_again = pool.reserve_ordinary();
    ASSERT_EQ(first_again.slot(), first.slot());
    ASSERT_EQ(first_again.generation(), 2u);
    pool.release(first_again);
    pool.release(second);
    pool.release(third);

    // A free row's link can contain a different row's generation. Even an identity
    // matching those bits must not make the free row Reserved, Live or releasable.
    for (const auto id :
         {first, second, third, ActorId::ordinary(second.slot(), 255, 3), ActorId::ordinary(third.slot(), 255, 2),
          ActorId::ordinary(second.slot(), 254, 3), ActorId::ordinary(third.slot(), 254, 2)}) {
        EXPECT_FALSE(pool.owns_reservation(id));
        EXPECT_FALSE(pool.mark_live(id));
        EXPECT_FALSE(pool.mark_retiring(id));
        pool.release(id);
    }

    const auto third_again  = pool.reserve_ordinary();
    const auto second_again = pool.reserve_ordinary();
    const auto first_last   = pool.reserve_ordinary();
    EXPECT_EQ(third_again, ActorId::ordinary(third.slot(), 255, 2));
    EXPECT_EQ(second_again, ActorId::ordinary(second.slot(), 255, 2));
    EXPECT_EQ(first_last, ActorId::ordinary(first.slot(), 255, 3));
    EXPECT_FALSE(pool.reserve_ordinary().is_valid());

    for (const auto id : {third_again, second_again, first_last}) {
        const auto foreign = ActorId::ordinary(id.slot(), 254, id.generation());
        EXPECT_FALSE(pool.owns_reservation(foreign));
        EXPECT_FALSE(pool.mark_live(foreign));
        EXPECT_FALSE(pool.mark_retiring(foreign));
        pool.release(foreign);
        EXPECT_TRUE(pool.owns_reservation(id));
    }
    ASSERT_TRUE(pool.mark_live(third_again));
    ASSERT_TRUE(pool.mark_retiring(third_again));
    pool.release(third_again);
    pool.release(first_last); // this row retires at its generation ceiling
    pool.release(second_again);
    const auto second_last = pool.reserve_ordinary();
    const auto third_last  = pool.reserve_ordinary();
    EXPECT_EQ(second_last, ActorId::ordinary(second.slot(), 255, 3));
    EXPECT_EQ(third_last, ActorId::ordinary(third.slot(), 255, 3));
    pool.release(second_last);
    pool.release(third_last);
    EXPECT_TRUE(pool.empty());
    EXPECT_FALSE(pool.reserve_ordinary().is_valid());
    EXPECT_EQ(pool.ordinary_slot_count(), 4u);
}

TEST(ActorIdPool, ClearAndTakePreserveFreeIdentitiesBesideLiveActorsAndServices) {
    qb::detail::ActorIdPool owner(255, 3, 3);
    const auto              first        = owner.reserve_ordinary();
    const auto              second       = owner.reserve_ordinary();
    const auto              live         = owner.reserve_ordinary();
    const auto              service      = owner.reserve_service(65534);
    const auto              live_service = owner.reserve_service(1);
    ASSERT_TRUE(owner.mark_live(live));
    ASSERT_TRUE(owner.mark_live(live_service));
    owner.release(first);
    owner.clear_reservations();

    auto worker = owner.take();
    EXPECT_EQ(owner.slot_count(), 0u);
    worker.clear_reservations(); // only free links and Live rows remain
    EXPECT_EQ(worker.reserve_ordinary(), ActorId::ordinary(second.slot(), 255, 2));
    EXPECT_EQ(worker.reserve_ordinary(), ActorId::ordinary(first.slot(), 255, 2));
    const auto service_again = worker.reserve_service(65534);
    EXPECT_EQ(service_again, ActorId::service_instance(65534, 255, 2));
    const auto foreign = ActorId::service_instance(65534, 254, 2);
    EXPECT_FALSE(worker.mark_live(foreign));
    worker.release(foreign);
    EXPECT_TRUE(worker.owns_reservation(service_again));
    EXPECT_FALSE(worker.owns_reservation(ActorId::service_address(65534, 255)));
    EXPECT_FALSE(worker.reserve_ordinary().is_valid());
    EXPECT_FALSE(worker.reserve_service(1).is_valid());

    worker.clear_reservations();
    owner = worker.take();
    EXPECT_EQ(worker.slot_count(), 0u);
    const auto first_last  = owner.reserve_ordinary();
    const auto second_last = owner.reserve_ordinary();
    EXPECT_EQ(first_last, ActorId::ordinary(second.slot(), 255, 3));
    EXPECT_EQ(second_last, ActorId::ordinary(first.slot(), 255, 3));
    const auto service_last = owner.reserve_service(65534);
    EXPECT_EQ(service_last, ActorId::service_instance(65534, 255, 3));
    owner.release(first_last);
    owner.release(second_last);
    owner.release(service_last);
    EXPECT_FALSE(owner.reserve_ordinary().is_valid());
    EXPECT_FALSE(owner.reserve_service(65534).is_valid());
    owner.release(live);
    owner.release(live_service);
    EXPECT_EQ(owner.reserve_ordinary(), ActorId::ordinary(live.slot(), 255, 2));
    EXPECT_EQ(owner.reserve_service(1), ActorId::service_instance(1, 255, 2));
    EXPECT_NE(service, service_again);
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
// released id never delivers again, but is CONSUMED as an obsolete own response, including
// after reuse by a later ask. An entry is never given to a different owner. Driven directly
// on the test thread.
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
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, resp));
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
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, resp)); // stale generation consumed, not delivered
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
        EXPECT_TRUE(qb::detail::ask_deliver(t, owner, resp));
    }
    EXPECT_EQ(q.delivered, 1 + static_cast<int>(taken.size()));
    qb::detail::ask_unregister(reissued);
    EXPECT_TRUE(qb::detail::ask_deliver(reissued, owner, resp));
}

TEST(ActorId, ReleasedAskSlotNeverChangesOwner) {
    constexpr std::uint64_t mask = (std::uint64_t{1} << 22) - 1;
    const NormalId          owner(29, 12), other(30, 12);
    probe_slot              first{owner}, second{other};
    qb::Event               response{};
    const auto              retired = qb::detail::ask_take(owner, &first.slot);
    qb::detail::ask_unregister(retired);

    std::vector<std::uint64_t> active;
    for (int i = 0; i < 300; ++i) {
        const auto next = qb::detail::ask_take(other, &second.slot);
        active.push_back(next);
        EXPECT_NE(next & mask, retired & mask);
    }
    EXPECT_TRUE(qb::detail::ask_deliver(retired, owner, response));
    EXPECT_FALSE(qb::detail::ask_deliver(retired, other, response));
    EXPECT_EQ(first.delivered, 0);
    EXPECT_EQ(second.delivered, 0);
    for (auto id : active)
        qb::detail::ask_unregister(id);

    const auto reused = qb::detail::ask_take(owner, &first.slot);
    EXPECT_EQ(reused & mask, retired & mask);
    EXPECT_NE(reused, retired);
    EXPECT_TRUE(qb::detail::ask_deliver(retired, owner, response));
    EXPECT_EQ(first.delivered, 0);
    EXPECT_TRUE(qb::detail::ask_deliver(reused, owner, response));
    EXPECT_EQ(first.delivered, 1);
    qb::detail::ask_unregister(reused);
}

TEST(ActorId, AskOwnerQueuesStayDistinctAcrossCoresAndPages) {
    constexpr std::uint64_t       mask = (std::uint64_t{1} << 22) - 1;
    const std::array<NormalId, 3> owners{NormalId(255, 211), NormalId(255, 212), NormalId(511, 211)};
    std::array<std::uint64_t, 3>  first{};
    qb::Event                     response{};
    for (std::size_t i = 0; i < owners.size(); ++i) {
        first[i] = qb::detail::ask_take(owners[i], nullptr);
        qb::detail::ask_unregister(first[i]);
        for (std::size_t j = 0; j < i; ++j)
            EXPECT_NE(first[i] & mask, first[j] & mask);
    }
    // Reacquire after every release: same SID on another core and same page offset
    // on another page must never hit the previous owner's cached free queue.
    for (int round = 0; round < 4; ++round) {
        for (std::size_t i = 0; i < owners.size(); ++i) {
            probe_slot probe{owners[i]};
            const auto id = qb::detail::ask_take(owners[i], &probe.slot);
            EXPECT_EQ(id & mask, first[i] & mask);
            EXPECT_NE(id, first[i]);
            EXPECT_TRUE(qb::detail::ask_deliver(first[i], owners[i], response));
            EXPECT_EQ(probe.delivered, 0);
            for (std::size_t j = 0; j < owners.size(); ++j)
                if (i != j) {
                    EXPECT_FALSE(qb::detail::ask_deliver(first[i], owners[j], response));
                    EXPECT_FALSE(qb::detail::ask_deliver(id, owners[j], response));
                }
            EXPECT_EQ(probe.delivered, 0);
            EXPECT_TRUE(qb::detail::ask_deliver(id, owners[i], response));
            EXPECT_EQ(probe.delivered, 1);
            qb::detail::ask_unregister(id);
        }
    }
}

TEST(ActorId, FinishedAskIsConsumedBeforeUnregister) {
    const NormalId owner(31, 12), other(32, 12);
    probe_slot     probe{owner};
    qb::Event      response{};
    const auto     id = qb::detail::ask_take(owner, &probe.slot);
    probe.slot.done   = true;
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_FALSE(qb::detail::ask_deliver(id, other, response));
    EXPECT_EQ(probe.delivered, 0);
    qb::detail::ask_unregister(id);
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_EQ(probe.delivered, 0);
}

TEST(ActorId, UnboundAskKeepsItsOwnerWithoutDereferencingASlot) {
    const NormalId owner(33, 12), other(34, 12);
    qb::Event      response{};
    const auto     id = qb::detail::ask_take(owner, nullptr);
    EXPECT_FALSE(qb::detail::ask_deliver(id, owner, response));
    qb::detail::ask_unregister(id);
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_FALSE(qb::detail::ask_deliver(id, other, response));
    // The next generation has not been issued, so it is not an obsolete response.
    EXPECT_FALSE(qb::detail::ask_deliver(id + (std::uint64_t{1} << 22), owner, response));
    const auto next = qb::detail::ask_take(owner, nullptr);
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_FALSE(qb::detail::ask_deliver(next, owner, response));
    qb::detail::ask_unregister(next);
}

TEST(ActorId, RetiredAskOutlivesItsDestroyedSlot) {
    const NormalId owner(35, 12);
    auto           probe = std::make_unique<probe_slot>(owner);
    const auto     id    = qb::detail::ask_take(owner, &probe->slot);
    qb::detail::ask_unregister(id);
    probe.reset(); // ASan must catch retaining the old slot pointer instead of the owner.
    qb::Event response{};
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    qb::detail::ask_unregister(id);
}

TEST(ActorId, AskCompletionCanDestroyItsSlotAndGrowTheTableInline) {
    const NormalId owner(36, 12);
    struct completion_state {
        std::unique_ptr<probe_slot> current;
        probe_slot                  replacement;
        std::vector<std::uint64_t>  taken;
        std::uint64_t               id    = 0;
        int                         calls = 0;
        explicit completion_state(ActorId owner)
            : current(std::make_unique<probe_slot>(owner))
            , replacement(owner) {}
    } state(owner);
    state.current->slot.self    = &state;
    state.current->slot.deliver = [](void *raw, qb::Event &) noexcept {
        auto &s = *static_cast<completion_state *>(raw);
        ++s.calls;
        qb::detail::ask_unregister(s.id);
        s.current.reset();
        // Enough simultaneous entries to cross several original table capacities.
        for (int i = 0; i < 2048; ++i)
            s.taken.push_back(qb::detail::ask_take(s.replacement.slot.owner, &s.replacement.slot));
    };
    state.id = qb::detail::ask_take(owner, &state.current->slot);
    qb::Event response{};
    EXPECT_TRUE(qb::detail::ask_deliver(state.id, owner, response));
    EXPECT_EQ(state.calls, 1);
    EXPECT_FALSE(state.current);
    EXPECT_TRUE(qb::detail::ask_deliver(state.id, owner, response));
    EXPECT_EQ(state.replacement.delivered, 0);
    for (auto id : state.taken)
        qb::detail::ask_unregister(id);
}

// The retired entry keeps its owner's COMPLETE identity (QB-1104 on the 64-bit codec): the widest ordinary
// slot, the last core and the last generation all survive the release, and a neighbour on any one axis -- the
// same slot on another core, another slot, the previous incarnation of the same slot -- is not its owner.
TEST(ActorId, RetiredAskPreservesTheEntireOwnerCodec) {
    constexpr CoreId last_core            = static_cast<CoreId>(qb::MaxCores - 1);
    const ActorId    owner                = ActorId::ordinary(ActorId::MaxOrdinarySlot, last_core, ActorId::MaxGeneration);
    const ActorId    wrong_core           = ActorId::ordinary(ActorId::MaxOrdinarySlot, last_core - 1, ActorId::MaxGeneration);
    const ActorId    wrong_slot           = ActorId::ordinary(ActorId::MaxOrdinarySlot - 1, last_core, ActorId::MaxGeneration);
    const ActorId    previous_incarnation = ActorId::ordinary(ActorId::MaxOrdinarySlot, last_core, ActorId::MaxGeneration - 1);
    ASSERT_TRUE(owner.is_ordinary() && wrong_core.is_ordinary() && wrong_slot.is_ordinary() && previous_incarnation.is_ordinary());
    const auto id = qb::detail::ask_take(owner, nullptr);
    EXPECT_EQ(id >> 48, static_cast<std::uint64_t>(last_core));
    qb::detail::ask_unregister(id);
    qb::Event response{};
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_FALSE(qb::detail::ask_deliver(id, wrong_core, response));
    EXPECT_FALSE(qb::detail::ask_deliver(id, wrong_slot, response));
    EXPECT_FALSE(qb::detail::ask_deliver(id, previous_incarnation, response));

    // A service incarnation (its slot in the service band, its queue in the tag directory) keeps its identity too.
    const ActorId service       = ActorId::service_instance(65534, last_core, ActorId::MaxGeneration);
    const ActorId service_other = ActorId::service_instance(65533, last_core, ActorId::MaxGeneration);
    ASSERT_TRUE(service.is_service_instance() && service_other.is_service_instance());
    const auto sid = qb::detail::ask_take(service, nullptr);
    qb::detail::ask_unregister(sid);
    EXPECT_TRUE(qb::detail::ask_deliver(sid, service, response));
    EXPECT_FALSE(qb::detail::ask_deliver(sid, service_other, response));
    EXPECT_FALSE(qb::detail::ask_deliver(sid, ActorId::service_address(65534, last_core), response));
}

TEST(ActorId, AskRegistryRejectsUnissuedTokensAndForeignCoreRelease) {
    constexpr std::uint64_t slot_mask       = (std::uint64_t{1} << 22) - 1;
    constexpr std::uint64_t generation_mask = ((std::uint64_t{1} << 26) - 1) << 22;
    const NormalId          owner(257, 213);
    probe_slot              probe{owner};
    qb::Event               response{};
    const auto              id             = qb::detail::ask_take(owner, &probe.slot);
    const auto              no_generation  = id & ~generation_mask;
    const auto              outside_table  = (id & ~slot_mask) | slot_mask;
    const auto              not_yet_issued = id + (std::uint64_t{1} << 22);
    for (auto invalid : {no_generation, outside_table, not_yet_issued}) {
        EXPECT_FALSE(qb::detail::ask_deliver(invalid, owner, response));
        qb::detail::ask_unregister(invalid);
    }
    // Changing only the core bits must not release this owner's live entry.
    qb::detail::ask_unregister(id ^ (std::uint64_t{1} << 48));
    EXPECT_EQ(probe.delivered, 0);
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_EQ(probe.delivered, 1);
    qb::detail::ask_unregister(id);
    EXPECT_FALSE(qb::detail::ask_deliver(not_yet_issued, owner, response));
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_EQ(probe.delivered, 1);
}

TEST(ActorId, AskOwnerFreeQueuePreservesItsRemainingEntries) {
    constexpr std::uint64_t      mask = (std::uint64_t{1} << 22) - 1;
    const NormalId               owner(258, 213);
    std::array<std::uint64_t, 3> original{};
    for (auto &id : original)
        id = qb::detail::ask_take(owner, nullptr);
    for (auto id : original)
        qb::detail::ask_unregister(id);
    const auto first  = qb::detail::ask_take(owner, nullptr);
    const auto second = qb::detail::ask_take(owner, nullptr);
    EXPECT_EQ(first & mask, original[0] & mask);
    EXPECT_EQ(second & mask, original[1] & mask);
    qb::detail::ask_unregister(first);
    qb::detail::ask_unregister(second);
    const auto third = qb::detail::ask_take(owner, nullptr);
    EXPECT_EQ(third & mask, original[2] & mask);
    qb::detail::ask_unregister(third);
}

// A bound entry goes back into the FIFO `ask_take` recorded in its slot (Huly QB-1171), not into one the release
// resolves: released while the thread serves ANOTHER core (a codec test, successive engines on one thread), it
// reaches its owner's FIFO -- the owner's next ask reuses it, the same slot on the core served now never does.
TEST(ActorId, AskBoundEntryReturnsToItsOwnersQueueFromAnotherCore) {
    constexpr std::uint64_t mask = (std::uint64_t{1} << 22) - 1;
    const NormalId          owner(259, 217), same_slot_other_core(259, 218);
    probe_slot              mine{owner}, theirs{same_slot_other_core};
    qb::Event               response{};
    const auto              id    = qb::detail::ask_take(owner, &mine.slot);
    const auto              other = qb::detail::ask_take(same_slot_other_core, &theirs.slot); // the thread serves 218
    qb::detail::ask_unregister(id);

    const auto next_other = qb::detail::ask_take(same_slot_other_core, &theirs.slot);
    EXPECT_NE(next_other & mask, id & mask);
    const auto reused = qb::detail::ask_take(owner, &mine.slot);
    EXPECT_EQ(reused & mask, id & mask);
    EXPECT_EQ(reused >> 22, (id >> 22) + 1); // the same core, the next generation
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_EQ(mine.delivered, 0);
    EXPECT_TRUE(qb::detail::ask_deliver(reused, owner, response));
    EXPECT_EQ(mine.delivered, 1);
    EXPECT_EQ(theirs.delivered, 0);
    for (auto t : {other, next_other, reused})
        qb::detail::ask_unregister(t);
}

// The same contract for an UNBOUND entry (no slot recorded its FIFO): its release walks the directories this thread
// keeps and finds its owner's core, not the core served now.
TEST(ActorId, AskUnboundEntryReturnsToItsOwnersQueueFromAnotherCore) {
    constexpr std::uint64_t mask = (std::uint64_t{1} << 22) - 1;
    const NormalId          owner(259, 222), same_slot_other_core(259, 223);
    const auto              id    = qb::detail::ask_take(owner, nullptr);
    const auto              other = qb::detail::ask_take(same_slot_other_core, nullptr); // the thread serves 223
    qb::detail::ask_unregister(id);

    const auto next_other = qb::detail::ask_take(same_slot_other_core, nullptr);
    EXPECT_NE(next_other & mask, id & mask);
    const auto reused = qb::detail::ask_take(owner, nullptr);
    EXPECT_EQ(reused & mask, id & mask);
    EXPECT_EQ(reused >> 22, (id >> 22) + 1);
    for (auto t : {other, next_other, reused})
        qb::detail::ask_unregister(t);
}

// A service instance's FIFO lives in its core's tag directory, not the ordinary one: a bound entry of a service goes
// back to its tag's FIFO, released from another core too -- never to the ordinary slot the tag's number would name.
TEST(ActorId, AskBoundServiceEntryReturnsToItsTagsQueue) {
    constexpr std::uint64_t mask    = (std::uint64_t{1} << 22) - 1;
    const ActorId           service = ActorId::service_instance(9, 224, 1);
    const NormalId          same_number(9, 224), elsewhere(9, 225);
    ASSERT_TRUE(service.is_service_instance());
    probe_slot mine{service}, ordinary{same_number}, away{elsewhere};
    qb::Event  response{};
    const auto id    = qb::detail::ask_take(service, &mine.slot);
    const auto other = qb::detail::ask_take(elsewhere, &away.slot); // the thread serves 225
    qb::detail::ask_unregister(id);

    const auto next_ordinary = qb::detail::ask_take(same_number, &ordinary.slot);
    EXPECT_NE(next_ordinary & mask, id & mask);
    const auto reused = qb::detail::ask_take(service, &mine.slot);
    EXPECT_EQ(reused & mask, id & mask);
    EXPECT_EQ(reused >> 22, (id >> 22) + 1);
    EXPECT_TRUE(qb::detail::ask_deliver(id, service, response));
    EXPECT_TRUE(qb::detail::ask_deliver(reused, service, response));
    EXPECT_EQ(mine.delivered, 1);
    EXPECT_EQ(ordinary.delivered, 0);
    for (auto t : {other, next_ordinary, reused})
        qb::detail::ask_unregister(t);
}

// The FIFO address `ask_take` records in a slot (Huly QB-1171) stays valid while the registry grows under the live
// entry: the owner core's page directory reallocated by a far slot (4 page pointers -> 8192), the entry table grown
// by 4096 entries, another core added to the thread. A page never moves once created; the release pushes into it
// with no lookup, so this is what it relies on (a moved page reads as a use after free under ASan, and as a lost
// entry here).
TEST(ActorId, AskRecordedQueueOutlivesTheRegistryGrowing) {
    constexpr std::uint64_t    mask = (std::uint64_t{1} << 22) - 1;
    const NormalId             owner(260, 219), far_slot(0x100000, 219), neighbour(261, 219), elsewhere(260, 220);
    probe_slot                 mine{owner}, far{far_slot}, next_door{neighbour}, away{elsewhere};
    qb::Event                  response{};
    const auto                 id = qb::detail::ask_take(owner, &mine.slot);
    std::vector<std::uint64_t> live;
    live.push_back(qb::detail::ask_take(far_slot, &far.slot)); // core 219's directory: 4 pages -> 8192
    for (int k = 0; k < 4096; ++k)                             // the entry table grows (doubles at least once)
        live.push_back(qb::detail::ask_take(neighbour, &next_door.slot));
    live.push_back(qb::detail::ask_take(elsewhere, &away.slot)); // a core this thread had not served
    qb::detail::ask_unregister(id);

    const auto reused = qb::detail::ask_take(owner, &mine.slot);
    EXPECT_EQ(reused & mask, id & mask);
    EXPECT_EQ(reused >> 22, (id >> 22) + 1);
    EXPECT_TRUE(qb::detail::ask_deliver(reused, owner, response));
    EXPECT_EQ(mine.delivered, 1);
    for (auto t : live)
        qb::detail::ask_unregister(t);
    qb::detail::ask_unregister(reused);
}

// An entry's generation never wraps -- past its 26 bits it would spill into the id's core field: the release of the
// LAST generation retires the entry for good, and that final id stays recognisable as an obsolete reply of its owner.
// Driven through a bound slot, the path whose release Huly QB-1171 moved: 2^26 - 1 asks on one entry, a release and
// a take each -- the registry's own calls, which no test can make cheaper. Every build drives it, the Debug ones the
// sanitizers run included: on x86-64 g++ 14, 1.8 s at -O0, 10.9 s under ASan+UBSan, 14.1 s under TSan, 16.0 s under
// coverage; an M4 Pro took 80 s at -O0 under ASan, past the unit tier's 60 s, so this binary's ctest entry has a
// budget of its own (tests/core/unit/CMakeLists.txt).
TEST(ActorId, AskEntryRetiresAtItsLastGeneration) {
    constexpr std::uint64_t mask    = (std::uint64_t{1} << 22) - 1;
    constexpr std::uint64_t gen_max = (std::uint64_t{1} << 26) - 1;
    const NormalId          owner(262, 221);
    probe_slot              probe{owner};
    qb::Event               response{};
    auto                    id    = qb::detail::ask_take(owner, &probe.slot);
    const auto              entry = id & mask;
    const std::uint64_t     first = (id >> 22) & gen_max; // 1 on a first run; a repeat run reuses the entry it left
    // Each generation released, then reissued by the owner's FIFO; bounded, so a release that does not advance the
    // generation fails here instead of looping until the suite's timeout.
    std::uint64_t releases = 0;
    while (releases < gen_max && ((id >> 22) & gen_max) != gen_max && (id & mask) == entry) {
        qb::detail::ask_unregister(id);
        id = qb::detail::ask_take(owner, &probe.slot);
        ++releases;
    }
    ASSERT_EQ(id & mask, entry) << "the owner's FIFO handed out another entry before the last generation";
    ASSERT_EQ((id >> 22) & gen_max, gen_max) << "the last generation was never issued";
    ASSERT_EQ(releases, gen_max - first) << "a release did not advance the generation by exactly one";
    ASSERT_EQ(id >> 48, 221u);
    qb::detail::ask_unregister(id); // the last generation: retired, never reissued
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_EQ(probe.delivered, 0);

    const auto next = qb::detail::ask_take(owner, &probe.slot);
    EXPECT_NE(next & mask, entry);
    EXPECT_EQ((next >> 22) & gen_max, 1u); // a fresh entry, not a wrapped one
    EXPECT_EQ(next >> 48, 221u);
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_TRUE(qb::detail::ask_deliver(next, owner, response));
    EXPECT_EQ(probe.delivered, 1);
    qb::detail::ask_unregister(next);
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
