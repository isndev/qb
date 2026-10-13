/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */
/**
 * @file tests/core/system/engine/engine-restart-faults.cpp
 * @brief An engine started after another one in the same process builds its per-core tables in
 *        memory its predecessor left faulted (Huly QB-1012), counted in page faults, never timed.
 *
 * The class (QB-1009, QB-1012): what a core builds that grows with the engine -- the router's
 * handler table of each event type, indexed by actor id; the actor registry; the kill queues;
 * the broadcast snapshot -- lived in `malloc`. The first engine's tables, above glibc's `mmap`
 * threshold, were mapped for themselves and unmapped when it stopped, and that free raised the
 * threshold process-wide; the next engine's then lived in its core thread's arena, and freeing
 * them at its stop merged the arena's top past the dynamic trim threshold, so glibc gave it back
 * with `madvise(MADV_DONTNEED)`. Every engine after the first re-faulted its tables page by page,
 * inside its first window. QB-1009 kept one 64 KiB chunk from tipping a 4 MB top over; any second
 * event type or a larger population tips it again, which is what this engine does on purpose.
 *
 * The shape: one core, 40 000 children alive at once, each subscribed to eight event types --
 * the ids pass 32 768, so each of the eight tables and the registry reaches 65 536 slots -- then
 * one broadcast `KillEvent` (the snapshot of 40 001 actors). Five such engines in a row, each
 * counted in the process's page faults over its whole life. The first is cold by definition and
 * is not judged; the verdict is the smallest count among the other four, so a fault burst from
 * elsewhere on a loaded host (reclaim, a neighbour) can raise one count, never the verdict.
 *
 * The bound is the mechanism's, in bytes: an engine that re-faults its tables takes at least one
 * fault per page of eight 2 MiB tables, 16 MiB (4 096 faults of 4 KiB); a warm one faults what a
 * new thread touches first -- its stack, a few dozen pages. 1 MiB of pages sits a factor of ten
 * from each, with the page size read from the platform.
 *
 * Registered on Linux and Windows outside the sanitizer builds (CMakeLists.txt): a sanitizer
 * replaces the allocator whose behaviour this counts. Windows' heap has no trim threshold; the
 * count holds it to the same bound.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include "../../shared/PageFaults.h"

// A named namespace: these actors are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace engine_restart_faults_test {

constexpr std::size_t kChildren = 40000; ///< sids past 32 768: every table and the registry at 65 536 slots
constexpr int         kEngines  = 5;

template <int N>
struct Tag : qb::Event {};

// Subscribed to eight event types, one table each on its core's router; never sent any.
class Child final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Tag<0>>(*this);
        registerEvent<Tag<1>>(*this);
        registerEvent<Tag<2>>(*this);
        registerEvent<Tag<3>>(*this);
        registerEvent<Tag<4>>(*this);
        registerEvent<Tag<5>>(*this);
        registerEvent<Tag<6>>(*this);
        registerEvent<Tag<7>>(*this);
        co_return true;
    }
    void
    on(Tag<0> const &) {}
    void
    on(Tag<1> const &) {}
    void
    on(Tag<2> const &) {}
    void
    on(Tag<3> const &) {}
    void
    on(Tag<4> const &) {}
    void
    on(Tag<5> const &) {}
    void
    on(Tag<6> const &) {}
    void
    on(Tag<7> const &) {}
};

// Builds the population in its own onInit, on the core's thread, then ends the engine with one
// broadcast: every child dies in the first pass, the spawner with them.
class Spawner final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        for (std::size_t i = 0; i < kChildren; ++i)
            if (!addRefActor<Child>().valid())
                co_return false;
        broadcast<qb::KillEvent>();
        co_return true;
    }
};

/// The process's page faults over one engine's whole life: construction, start, run, join, teardown.
[[nodiscard]] long long
faults_of_one_engine() {
    long long const before = qb::test::process_page_faults();
    {
        qb::Main main;
        main.core(0).addActor<Spawner>();
        main.start(); // a worker thread, as a benchmark repetition runs it: the core thread exits at the end
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    return qb::test::process_page_faults() - before;
}

} // namespace engine_restart_faults_test

using namespace engine_restart_faults_test;

TEST(EngineRestartFaults, AnEngineRebuildsItsPerCoreTablesInPagesItsPredecessorLeftFaulted) {
#ifdef QB_WITH_LOGGING
    // The per-core tables, not the logger: an unoptimised build logs every actor's lifecycle, and the logger's
    // per-thread buffers (8 MiB each, freed when a core thread ends) would be faulted in again by every engine --
    // 6 144 pages a warm engine under the coverage preset, 90 with the logger at WARN, as in Release.
    qb::io::log::setLevel(qb::io::log::Level::WARN);
#endif
    ASSERT_GE(qb::test::process_page_faults(), 0) << "the platform's fault counter is unreadable";
    std::array<long long, kEngines> faults{};
    for (auto &count : faults)
        count = faults_of_one_engine();

    std::string record;
    for (int i = 0; i < kEngines; ++i)
        record += (i ? " " : "") + std::to_string(faults[i]);
    std::printf("[ faults ] per engine, the first cold: %s (page %zu B, hugepages %s)\n", record.c_str(), qb::test::page_bytes(),
                qb::test::hugepage_mode().c_str());
    RecordProperty("faults_per_engine", record);

    long long const warm  = *std::min_element(faults.begin() + 1, faults.end());
    long long const bound = static_cast<long long>((std::size_t{1} << 20) / qb::test::page_bytes());
    EXPECT_LT(warm, bound) << "every engine after the first faulted at least " << warm
                           << " pages: its per-core tables were given back to the OS when its predecessor stopped "
                              "and were faulted in again (Huly QB-1012)";
}
