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
 * Pure bit-packing and bitset logic: NO actor is ever instantiated, NO `qb::Main`, NO event loop.
 * Round-trip and packing assertions check *derived* structure (`sid()` / `index()` recovered from
 * the packed `uint32`), not the raw value the test set, so they verify the codec rather than echo
 * input. `CoreIdBitSet` cases pin the out-of-range filter (>= MaxCores silently ignored) and the
 * exact membership including the boundary value.
 *
 * Added over the original: NON-broadcast `(sid, index)` construction (every prior case routed
 * through `BroadcastId`, so the normal-service packing path — sid != BroadcastSid — was untested;
 * exercised here via a test-only subclass that exposes the protected two-arg ctor the same way
 * `BroadcastId` does); the exact 255-valid / 256-ignored `CoreIdBitSet` boundary (the original only
 * probed 300); and a set-from-raw round-trip equality (`CoreIdBitSet(b.raw()) == b` by membership).
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <qb/core/Actor.h> // qb::detail::ask_take & co — the correlation-id codec and the pending-ask registry
#include <qb/core/ActorId.h>

using qb::ActorId;
using qb::BroadcastId;
using qb::CoreId;
using qb::CoreIdBitSet;
using qb::ServiceId;

namespace {
// Test-only handle that exposes ActorId's protected (ServiceId, CoreId) constructor — the
// normal-service packing path. Mirrors how qb::BroadcastId (a public subclass) reaches the same
// protected ctor with BroadcastSid; here we drive it with an arbitrary non-broadcast sid.
struct NormalId : public ActorId {
    NormalId(ServiceId sid, CoreId index) noexcept
        : ActorId(sid, index) {}
};
} // namespace

// --- ActorId -----------------------------------------------------------------

TEST(ActorId, DefaultIsNotFoundAndInvalid) {
    ActorId id;
    EXPECT_FALSE(id.is_valid());
    EXPECT_EQ(static_cast<std::uint32_t>(id), ActorId::NotFound);
    EXPECT_FALSE(id.is_broadcast());
}

TEST(ActorId, Uint32RoundTripPreservesSidAndIndex) {
    BroadcastId   b(7); // sid = BroadcastSid, index = 7
    std::uint32_t packed = static_cast<std::uint32_t>(b);
    ActorId       round(packed); // unpack
    EXPECT_EQ(static_cast<std::uint32_t>(round), packed);
    EXPECT_EQ(round.sid(), b.sid());
    EXPECT_EQ(round.index(), b.index());
}

TEST(ActorId, BroadcastIdProperties) {
    BroadcastId b(3);
    EXPECT_TRUE(b.is_broadcast());
    EXPECT_TRUE(b.is_valid());
    EXPECT_EQ(b.sid(), ActorId::BroadcastSid);
    EXPECT_EQ(b.index(), static_cast<CoreId>(3));
}

TEST(ActorId, EqualityViaUint32) {
    BroadcastId b(5);
    ActorId     a(static_cast<std::uint32_t>(b));
    EXPECT_EQ(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
    ActorId none;
    EXPECT_NE(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(none));
}

TEST(ActorId, NonBroadcastSidIndexConstruction) {
    // The normal-service path: a non-broadcast sid packs with a core index and survives the
    // uint32 round-trip with both components recoverable. (Every other case routes through
    // BroadcastId, whose sid is always BroadcastSid — this exercises sid != BroadcastSid.)
    constexpr ServiceId sid   = 5;
    constexpr CoreId    index = 2;
    NormalId            id(sid, index);

    EXPECT_EQ(id.sid(), sid);
    EXPECT_EQ(id.index(), index);
    EXPECT_FALSE(id.is_broadcast()) << "a sid below BroadcastSid is NOT a broadcast id";
    EXPECT_TRUE(id.is_valid()) << "a non-zero service id is valid";

    // uint32 codec is a faithful inverse: unpacking the packed value recovers both components.
    const std::uint32_t packed = static_cast<std::uint32_t>(id);
    ActorId             round(packed);
    EXPECT_EQ(round.sid(), sid);
    EXPECT_EQ(round.index(), index);
    EXPECT_EQ(static_cast<std::uint32_t>(round), packed);
    EXPECT_FALSE(round.is_broadcast());

    // A normal id with the same components but a different core is a distinct value.
    NormalId other(sid, static_cast<CoreId>(index + 1));
    EXPECT_NE(static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(other));

    // Boundary: sid == BroadcastSid IS broadcast even via this construction path.
    NormalId asBroadcast(ActorId::BroadcastSid, index);
    EXPECT_TRUE(asBroadcast.is_broadcast());

    // The all-zero packing is exactly NotFound / invalid.
    NormalId zero(0, 0);
    EXPECT_EQ(static_cast<std::uint32_t>(zero), ActorId::NotFound);
    EXPECT_FALSE(zero.is_valid());
}

// No actor ever holds service id 0: every core's pool starts above the registered service tags, and a tag's id
// starts at 1. So an id whose sid is 0 names nothing on ANY core, not only on core 0 (Huly QB-1007). Until 3.3
// `is_valid()` tested the whole packed value, and `(0, core)` read valid on every core but 0 -- the very id a
// service was given when its tag's `ServiceIndex` was read before its static initialiser had run.
TEST(ActorId, ServiceIdZeroIsInvalidOnEveryCore) {
    for (CoreId const core : {CoreId{0}, CoreId{1}, CoreId{3}, CoreId{255}, std::numeric_limits<CoreId>::max()}) {
        NormalId const id(0, core);
        ASSERT_EQ(id.sid(), ServiceId{0});
        ASSERT_EQ(id.index(), core);
        EXPECT_FALSE(id.is_valid()) << "(0, " << core << ") names no actor";
        EXPECT_FALSE(ActorId(static_cast<std::uint32_t>(id)).is_valid()) << "(0, " << core << ") unpacked from its raw value";
    }
    // Validity is the service id alone: sid 1 on core 0, the smallest packed value an actor can have, is valid...
    EXPECT_TRUE(NormalId(1, 0).is_valid());
    // ...and so is a broadcast id, core 0 included.
    EXPECT_TRUE(BroadcastId(0).is_valid());
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
    qb::Event               resp{};

    probe_slot          p{owner};
    const std::uint64_t id = qb::detail::ask_take(owner, &p.slot);

    // Delivered to its owner, refused to anyone else — another actor of the same core, and the
    // same actor index on another core (the id's core bits do not match the delivering actor's).
    EXPECT_FALSE(qb::detail::ask_deliver(id, other_actor_same_core, resp));
    EXPECT_FALSE(qb::detail::ask_deliver(id, same_sid_other_core, resp));
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

TEST(ActorId, RetiredAskPreservesTheEntireOwnerCodec) {
    const NormalId owner(65534, 65535), wrong_core(65534, 65534), wrong_sid(65533, 65535);
    const auto     id = qb::detail::ask_take(owner, nullptr);
    EXPECT_EQ(id >> 48, 65535u);
    qb::detail::ask_unregister(id);
    qb::Event response{};
    EXPECT_TRUE(qb::detail::ask_deliver(id, owner, response));
    EXPECT_FALSE(qb::detail::ask_deliver(id, wrong_core, response));
    EXPECT_FALSE(qb::detail::ask_deliver(id, wrong_sid, response));
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
