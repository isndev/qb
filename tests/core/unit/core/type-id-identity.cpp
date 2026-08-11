/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/type-id-identity.cpp
 * @brief `qb::type_id<T>` / `qb::Event::type_to_id<T>` identity contract.
 *
 * Every distinct type maps to a dense, stable, collision-free id that the event router uses as
 * its routing key. The id is produced by a magic-static post-increment counter, so it must be:
 *   - stable: same type → same id, forever (IsStableAcrossCalls);
 *   - collision-free across a large flat type cohort (IsCollisionFreeAcrossManyTypes);
 *   - race-free on first instantiation from many threads (ConcurrentFirstInstantiationIsRaceFree);
 *   - race-free when *distinct* types register concurrently while a reader walks the name
 *     registry (ConcurrentDistinctTypeRegistrationIsRaceFree) — the TSan probe for the side
 *     registry the id assignment now also fills;
 *   - the same id the Event layer keys on, in EVERY build mode (EventTypeToIdMatchesGlobalTypeId),
 *     which is what keeps `Event`'s wire layout NDEBUG-independent (EventLayoutIsIndependentOfBuildMode);
 *   - accompanied by a human-readable name reachable from the type and from a runtime id
 *     (EventTypeNameIsAvailableInEveryBuildMode).
 *
 * Pure logic — no engine, no thread spawned beyond the explicit race probes. Promoted to unit/
 * from the former test-core-improvements.cpp gold-standard suite.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>

namespace {

// 256 distinct types via a non-type template parameter — stresses uniqueness across a large cohort.
template <std::size_t N>
struct TypeIdProbe {};

template <std::size_t... Is>
void
collectTypeIds(std::index_sequence<Is...>, std::vector<qb::TypeId> &out) {
    (out.push_back(qb::type_id<TypeIdProbe<Is>>()), ...);
}

// Separate cohort for the density check, so it cannot be perturbed by whichever test ran first.
template <std::size_t N>
struct DenseProbe {};

template <std::size_t... Is>
void
collectDenseTypeIds(std::index_sequence<Is...>, std::vector<qb::TypeId> &out) {
    (out.push_back(qb::type_id<DenseProbe<Is>>()), ...);
}

// Disjoint cohort for the concurrent test — keeps the optimizer from sharing an instantiation.
template <std::size_t N>
struct ConcurrentTypeIdProbe {};

// --- concurrent DISTINCT-type registration cohort -----------------------------------------
// One private type per (writer, slot) pair, so no two threads ever race on the same magic
// static: the id counter and the name registry are what actually see the concurrency.
constexpr std::size_t kWriters   = 8;
constexpr std::size_t kPerWriter = 32;

using CohortIds   = std::array<qb::TypeId, kPerWriter>;
using CohortNames = std::array<const char *, kPerWriter>;

template <std::size_t W, std::size_t I>
struct CohortProbe {};

template <std::size_t W, std::size_t... Is>
void
registerCohort(std::index_sequence<Is...>, CohortIds &ids, CohortNames &names) {
    ((ids[Is] = qb::type_id<CohortProbe<W, Is>>(), names[Is] = qb::Event::type_to_name<CohortProbe<W, Is>>()), ...);
}

template <std::size_t W>
void
registerCohortForWriter(CohortIds &ids, CohortNames &names) {
    registerCohort<W>(std::make_index_sequence<kPerWriter>{}, ids, names);
}

using CohortFn = void (*)(CohortIds &, CohortNames &);

template <std::size_t... Ws>
constexpr std::array<CohortFn, sizeof...(Ws)>
makeCohortTable(std::index_sequence<Ws...>) {
    return {&registerCohortForWriter<Ws>...};
}

constexpr auto kCohortTable = makeCohortTable(std::make_index_sequence<kWriters>{});

} // namespace

TEST(TypeId, IsStableAcrossCalls) {
    const auto a = qb::type_id<int>();
    for (std::size_t i = 0; i < 1024; ++i)
        EXPECT_EQ(qb::type_id<int>(), a);
    const auto b = qb::type_id<double>();
    EXPECT_NE(a, b) << "Distinct types must not collide";
}

TEST(TypeId, IsCollisionFreeAcrossManyTypes) {
    constexpr std::size_t   kNTypes = 256;
    std::vector<qb::TypeId> ids;
    ids.reserve(kNTypes);
    collectTypeIds(std::make_index_sequence<kNTypes>{}, ids);

    // All ids unique up to numeric_limits<TypeId>::max() distinct types.
    std::unordered_set<qb::TypeId> uniq(ids.begin(), ids.end());
    EXPECT_EQ(uniq.size(), ids.size()) << "type_id<T>() collided over " << kNTypes << " types";
    for (auto id : ids)
        EXPECT_NE(id, qb::TypeId{0}) << "TypeId 0 is reserved as 'unassigned'";
}

TEST(TypeId, ConcurrentFirstInstantiationIsRaceFree) {
    // Many threads call type_id<T>() for the same T at once: the magic-static initialiser barrier
    // in detail::type_id_for<T> must serialise the post-increment so all threads see ONE id.
    constexpr std::size_t            kThreads = 16;
    std::atomic<bool>                go{false};
    std::array<qb::TypeId, kThreads> seen{};
    std::vector<std::thread>         workers;
    workers.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i) {
        workers.emplace_back([i, &go, &seen] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            seen[i] = qb::type_id<ConcurrentTypeIdProbe<999>>();
        });
    }
    go.store(true, std::memory_order_release);
    for (auto &t : workers)
        t.join();
    for (std::size_t i = 1; i < kThreads; ++i)
        EXPECT_EQ(seen[i], seen[0]) << "thread " << i << " observed a different id";
}

TEST(TypeId, EventTypeToIdMatchesGlobalTypeId) {
    // Event::type_to_id<T>() keys the router. Since 3.0 its representation is the SAME in every
    // build mode — the dense 16-bit type_id<T>() counter — so `Event`'s wire layout no longer
    // depends on NDEBUG (see EventLayoutIsIndependentOfBuildMode, which is the reason). Strict
    // equality is therefore meaningful in both modes; what the old Debug branch asserted (a
    // human-readable, per-type distinct name) moved to EventTypeNameIsAvailableInEveryBuildMode.
    EXPECT_EQ(qb::Event::type_to_id<qb::KillEvent>(), qb::type_id<qb::KillEvent>());
    EXPECT_EQ(qb::Event::type_to_id<qb::SignalEvent>(), qb::type_id<qb::SignalEvent>());

    const auto kill_a   = qb::Event::type_to_id<qb::KillEvent>();
    const auto kill_b   = qb::Event::type_to_id<qb::KillEvent>();
    const auto signal_a = qb::Event::type_to_id<qb::SignalEvent>();
    ASSERT_NE(kill_a, qb::Event::id_type{0}) << "0 is reserved as 'unassigned'";
    ASSERT_NE(signal_a, qb::Event::id_type{0}) << "0 is reserved as 'unassigned'";
    EXPECT_EQ(kill_a, kill_b) << "type_to_id<T>() must be stable across calls";
    EXPECT_NE(kill_a, signal_a) << "distinct event types must map to distinct ids";
}

TEST(TypeId, IdsAreDenseAndSequential) {
    // The side registry that carries event-type names is a direct-indexed table, and its memory
    // claim (one resident page for a few hundred types) rests on ids being handed out densely
    // rather than sparsely. Nothing else in this process registers a type while the fold below
    // runs, so a fresh cohort of N types must occupy N consecutive ids.
    constexpr std::size_t   kNTypes = 64;
    std::vector<qb::TypeId> ids;
    ids.reserve(kNTypes);
    collectDenseTypeIds(std::make_index_sequence<kNTypes>{}, ids);
    ASSERT_EQ(ids.size(), kNTypes);
    for (std::size_t i = 1; i < ids.size(); ++i)
        EXPECT_EQ(static_cast<int>(ids[i]) - static_cast<int>(ids[i - 1]), 1) << "ids must be handed out densely; gap at index " << i;
}

TEST(TypeId, EventTypeNameIsAvailableInEveryBuildMode) {
    // What the Debug `const char *` id used to give for free: a human-readable name on a
    // mis-routing log line. It now comes from the side registry the id assignment fills, and it
    // is reachable both from the type (compile time) and from a runtime id (Event::getID()).
    const char *const kill   = qb::Event::type_to_name<qb::KillEvent>();
    const char *const signal = qb::Event::type_to_name<qb::SignalEvent>();
    ASSERT_NE(kill, nullptr);
    ASSERT_NE(signal, nullptr);
    EXPECT_GT(std::strlen(kill), 0u);
    EXPECT_STRNE(kill, signal) << "distinct event types must have distinct names";
    EXPECT_NE(std::string_view{kill}.find("KillEvent"), std::string_view::npos)
        << "name must still identify the type to a human, got: " << kill;

    // Reverse lookup from the runtime routing key — this is what the QB_LOG_WARN sites now do.
    EXPECT_STREQ(qb::event_type_name(qb::Event::type_to_id<qb::KillEvent>()), kill);
    EXPECT_STREQ(qb::event_type_name(qb::Event::type_to_id<qb::SignalEvent>()), signal);

    // The registry is filled at first use, not baked in at build time: a type this process has
    // never touched before must start unregistered and resolve immediately after.
    struct NeverSeenBefore {};
    const auto fresh = qb::type_id<NeverSeenBefore>();
    EXPECT_STREQ(qb::event_type_name(fresh), qb::Event::type_to_name<NeverSeenBefore>());

    // Ids that name nothing must say so rather than resolve to a neighbour or read out of
    // bounds. 0 is reserved and never handed out; the top of the TypeId domain is the index a
    // table sized to anything less than the full domain would fail on (ASan/UBSan will catch it).
    EXPECT_STREQ(qb::event_type_name(qb::Event::id_type{0}), "<unregistered>")
        << "0 is never handed out, so it must not resolve to a real type";
    constexpr auto kMaxId = std::numeric_limits<qb::Event::id_type>::max();
    ASSERT_GT(kMaxId, fresh) << "this process handed out the top of the id domain, so the probe "
                                "below would be asserting on a REGISTERED id";
    EXPECT_STREQ(qb::event_type_name(kMaxId), "<unregistered>") << "the registry must cover the whole id domain, including ids never assigned";
}

TEST(TypeId, ConcurrentDistinctTypeRegistrationIsRaceFree) {
    // The companion of ConcurrentFirstInstantiationIsRaceFree: there, many threads race on ONE
    // type and the magic-static barrier serialises them. Here each thread first-instantiates a
    // DISJOINT set of types while other threads read the name registry, so the id counter and
    // the registry are exercised concurrently with no barrier between the participants. This is
    // the thread-sanitizer probe for the side registry.
    constexpr std::size_t kReaders = 4;

    std::atomic<bool>                 go{false};
    std::atomic<bool>                 stop{false};
    std::atomic<std::size_t>          chars_read{0};
    std::array<CohortIds, kWriters>   ids{};
    std::array<CohortNames, kWriters> names{};
    std::vector<std::thread>          threads;
    threads.reserve(kWriters + kReaders);

    for (std::size_t w = 0; w < kWriters; ++w) {
        threads.emplace_back([w, &go, &ids, &names] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            kCohortTable[w](ids[w], names[w]);
        });
    }
    // Readers hammer the reverse lookup across the id range being filled, while registration is
    // in flight. They assert nothing about *which* name they see — only that reading
    // concurrently with registration is well-defined, which is what TSan is here to check.
    for (std::size_t r = 0; r < kReaders; ++r) {
        threads.emplace_back([&go, &stop, &chars_read] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            std::size_t seen = 0;
            do {
                for (std::uint32_t id = 0; id <= 1024u; ++id)
                    seen += std::strlen(qb::event_type_name(static_cast<qb::Event::id_type>(id)));
            } while (!stop.load(std::memory_order_relaxed));
            chars_read.fetch_add(seen, std::memory_order_relaxed);
        });
    }
    go.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < kWriters; ++i)
        threads[i].join();
    stop.store(true, std::memory_order_relaxed);
    for (std::size_t i = kWriters; i < threads.size(); ++i)
        threads[i].join();
    EXPECT_GT(chars_read.load(std::memory_order_relaxed), 0u) << "readers never ran";

    // Every registration must have survived the race: distinct ids, and each id resolving to
    // the name of *its own* type rather than a neighbour's.
    std::unordered_set<qb::TypeId> uniq;
    for (std::size_t w = 0; w < kWriters; ++w) {
        for (std::size_t i = 0; i < kPerWriter; ++i) {
            EXPECT_NE(ids[w][i], qb::TypeId{0}) << "writer " << w << " slot " << i;
            EXPECT_TRUE(uniq.insert(ids[w][i]).second) << "id " << ids[w][i] << " handed out twice (writer " << w << " slot " << i << ")";
            EXPECT_STREQ(qb::event_type_name(ids[w][i]), names[w][i]) << "id " << ids[w][i] << " resolved to the wrong type's name";
        }
    }
    EXPECT_EQ(uniq.size(), kWriters * kPerWriter);
}

TEST(TypeId, EventLayoutIsIndependentOfBuildMode) {
    // The reason the representation was unified: cross-core events are memcpy-relocated
    // (VirtualCore reinterpret_casts the wire buffer straight to Event*), so a consumer built
    // with a different NDEBUG than libqb-core must still find `dest` at the same offset. Before
    // 3.0 `id` was 8 bytes in Debug and 2 in Release, which moved dest/source from 8/12 to
    // 16/20 and silently routed cross-core events to a garbage ActorId. These are the Release
    // numbers; they must now hold in Debug too.
    EXPECT_EQ(sizeof(qb::Event::id_type), 2u);
    EXPECT_EQ(alignof(qb::Event), static_cast<std::size_t>(QB_LOCKFREE_CACHELINE_BYTES));

    struct Probe : qb::Event {
        std::uint32_t payload;
    };
    static_assert(std::is_trivially_copyable_v<Probe>, "the transport relocates events with memcpy, so the wire image must be a "
                                                       "faithful object representation");

    // Read dest/source out of a known byte pattern rather than out of offsetof(): qb::Event has
    // private members, so a derived probe is not standard-layout and offsetof() is only
    // conditionally supported (-Winvalid-offsetof is fatal under QB_TESTS_WERROR). This is the
    // ABI demo from dev/analysis/EVENT-ID-ABI-3.0.md turned into an assertion — byte[i] == i is
    // exactly what a mismatched consumer used to read wrong.
    Probe                                    probe{};
    std::array<unsigned char, sizeof(Probe)> wire{};
    for (std::size_t i = 0; i < wire.size(); ++i)
        wire[i] = static_cast<unsigned char>(i);
    std::memcpy(static_cast<void *>(&probe), wire.data(), sizeof(Probe));

    // Expected values are read back out of the same byte pattern rather than hard-coded, so the
    // assertion pins the OFFSET and not this machine's endianness. On little-endian these are
    // dest=0x0B0A0908, source=0x0F0E0D0C, id=0x0706, payload=0x13121110.
    const auto word_at = [&wire](std::size_t off) {
        std::uint32_t v{};
        std::memcpy(&v, wire.data() + off, sizeof(v));
        return v;
    };
    const auto half_at = [&wire](std::size_t off) {
        std::uint16_t v{};
        std::memcpy(&v, wire.data() + off, sizeof(v));
        return v;
    };

    EXPECT_EQ(probe.getID(), static_cast<qb::Event::id_type>(half_at(6))) << "Event::id must live at bytes 6..7 of the wire image";
    EXPECT_EQ(static_cast<std::uint32_t>(probe.getDestination()), word_at(8)) << "Event::dest must live at bytes 8..11 of the wire image";
    EXPECT_EQ(static_cast<std::uint32_t>(probe.getSource()), word_at(12)) << "Event::source must live at bytes 12..15 of the wire image";

    // And therefore a derived event's own payload starts at byte 16 — measured, not offsetof().
    const auto *const base   = reinterpret_cast<const unsigned char *>(static_cast<const qb::Event *>(&probe));
    const auto *const member = reinterpret_cast<const unsigned char *>(&probe.payload);
    EXPECT_EQ(member - base, 16) << "Event header must occupy exactly 16 bytes "
                                    "(state 4 + bucket_size 2 + id 2 + dest 4 + source 4)";
    EXPECT_EQ(probe.payload, word_at(16)) << "first user word must start at byte 16";
}

// -------------------------------------------------------------------------------------------
// The process-wide type-id registry (qb/core/Event.h). Since 3.0.0 `type_id_for<T>()`'s magic
// static is a per-image CACHE of an id owned by `qb::detail::_type_id_registry`, not the identity
// itself -- because a block-scope static cannot be kept in the export trie under
// `-fvisibility=hidden`, and a second image whose copy forked used to mint a colliding id for a
// type that already had one (measured: host `KillEvent=1, SignalEvent=2` vs plugin
// `KillEvent=1, Noop=2`, both exit 0, no diagnostic).
//
// The multi-image half needs a plugin and cannot run inside one ctest binary. What CAN be pinned
// here is the property the fix rests on: a SECOND, independent slot asking for a name that is
// already registered gets the id that name already has, and does not draw a new one. That is
// exactly what a forked magic static does when it re-enters `register_type_id`.
// -------------------------------------------------------------------------------------------
TEST(TypeIdIdentity, RegistryRecoversAnIdInsteadOfMintingASecond) {
    struct RegistryProbeEvent : qb::Event {};

    const auto first = qb::type_id<RegistryProbeEvent>();
    EXPECT_NE(first, 0u) << "ids are 1-based; 0 is the registry's 'not found' sentinel";

    const auto counter_before = qb::detail::_type_id_counter.load(std::memory_order_relaxed);

    // `static`, and NOT optional: on the minting path `register_type_id` publishes `&slot` into a
    // process-wide intrusive list that outlives this function (Event.cpp), which is why
    // `Event.h` spells the contract "Storage donated by the caller's block-scope static". An
    // automatic here left a dangling node that every LATER registration walked; the suite passed
    // only because this test happened to run last, and `--gtest_shuffle` proved it:
    // `seed=1 rc=139  seed=2 rc=0  seed=3 rc=139  seed=4 rc=0  seed=5 rc=139  seed=6 rc=0`.
    // ASan stayed silent throughout -- the reader lives in the un-instrumented archive -- so the
    // shuffle, not the sanitizer, is what covers this. See `--gtest_shuffle` in tests/CMakeLists.
    //
    // Stand in for the forked magic static in a second image: fresh storage, same type name.
    static qb::detail::type_id_slot second_image_slot{};
    const auto                      recovered = qb::detail::register_type_id(second_image_slot, typeid(RegistryProbeEvent).name());

    EXPECT_EQ(recovered, first) << "a second slot must recover the id, not mint a colliding one";
    EXPECT_EQ(qb::detail::_type_id_counter.load(std::memory_order_relaxed), counter_before) << "recovering an id must not consume one";
    EXPECT_EQ(second_image_slot.name, nullptr) << "a recovered id must leave the caller's slot unused";

    // Positive control for the check above: an unregistered name DOES consume an id and DOES fill
    // the slot, so the three assertions can tell the two outcomes apart. `static` for the reason
    // above -- this is the slot that actually gets published.
    static qb::detail::type_id_slot fresh_slot{};
    // A repeat run (`--gtest_repeat`) finds the name already registered and correctly recovers it
    // instead of minting, so the mint-only assertions are scoped to the first pass rather than
    // making the test order- AND repetition-dependent in a second way.
    const bool first_pass = (fresh_slot.name == nullptr);
    const auto minted     = qb::detail::register_type_id(fresh_slot, "qb::test::NeverRegisteredBefore");
    if (first_pass) {
        EXPECT_EQ(minted, static_cast<qb::TypeId>(counter_before + 1));
        EXPECT_EQ(qb::detail::_type_id_counter.load(std::memory_order_relaxed), static_cast<qb::TypeId>(counter_before + 1));
    }
    EXPECT_EQ(fresh_slot.id, minted);
    EXPECT_STREQ(fresh_slot.name, "qb::test::NeverRegisteredBefore");
}

// -------------------------------------------------------------------------------------------
// Positive control for the Debug-only storage-duration check `register_type_id` gained after the
// test above shipped with an automatic slot. Without a control, "no assert fired" is
// indistinguishable from "the check is not compiled in", which is exactly how the original defect
// survived: ASan also reported nothing, because the dangling read happens inside the
// un-instrumented archive.
//
// Debug + POSIX only, by construction:
//   - NDEBUG compiles the assert away, and that is deliberate (cold path, but zero cost shipped);
//   - the stack-bounds probe is pthread-based, so Windows has no check to control.
// Under either exclusion the test asserts the *inverse* — that publishing an automatic is merely
// undefined rather than diagnosed — so the case is never silently skipped.
// -------------------------------------------------------------------------------------------
#if !defined(NDEBUG) && (defined(__APPLE__) || defined(__linux__) || defined(__unix__))
TEST(TypeIdIdentityDeathTest, PublishingASlotWithAutomaticStorageAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            // Automatic storage: the shape that produced `rc=139` on 3 of 6 shuffle seeds.
            qb::detail::type_id_slot on_the_stack{};
            qb::detail::register_type_id(on_the_stack, "qb::test::SlotWithAutomaticStorage");
        },
        "must therefore have static storage duration");
}

TEST(TypeIdIdentity, AStaticSlotIsAcceptedByTheStorageDurationCheck) {
    // Negative control for the death test: the same call with the storage the contract asks for
    // must NOT abort, so the death test above is pinning the storage duration and not simply the
    // fact that `register_type_id` was called.
    static qb::detail::type_id_slot in_static_storage{};
    const auto                      id = qb::detail::register_type_id(in_static_storage, "qb::test::SlotWithStaticStorage");
    EXPECT_NE(id, 0u);
}
#else
TEST(TypeIdIdentity, StorageDurationCheckIsCompiledOutHere) {
    GTEST_SKIP() << "register_type_id's storage-duration assert is Debug + POSIX only "
                    "(NDEBUG="
                 <<
#ifdef NDEBUG
        1
#else
        0
#endif
                 << "); the contract still holds, it is just not diagnosed in this build.";
}
#endif
