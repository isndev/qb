/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */
/**
 * @file tests/core/system/engine/engine-restart-mailboxes.cpp
 * @brief The second engine of a process writes its mailbox rings into pages the first left faulted (Huly QB-1012,
 *        QB-203), counted in the page faults of the threads that write them.
 *
 * Every core's mailbox is one `region_cache` block (QB-1012) holding a ring per core, each constructed by `Main::start()`
 * without its slot array (QB-203): a ring's slot pages are written by the laps of the core that sends into it. When an
 * engine stops, its mailboxes give their blocks back to the cache, still mapped, and the next engine builds its own in
 * them. Until 3.3 the rings were all built and value-initialised by `Main::start()` on the calling thread, in
 * `malloc`: glibc mapped each 1 MiB mailbox block of a 16-core engine for itself, unmapped it at the first engine's end
 * and raised its thresholds, so the second engine's rings came from fresh heap that `start()` faulted page by page --
 * 16.8 MB, 4 100 faults of 4 KiB, before the first event.
 *
 * Mapped is not enough. A mailbox never writes the slots of the ring that is its own core's index -- a core does not send
 * to itself through its mailbox -- so a block that passes to ANOTHER core's mailbox brings it a ring whose slot pages were
 * never written, and that core's laps fault them: 16 pages a mailbox. The cache lends the region of a size it took back
 * last, first, so the mailboxes are given back in the exact reverse of the order they were built in (Main) for each
 * core's mailbox to find its own block, and a block lays its rings out with no colour, for them to sit where they sat.
 * Given back in the order they were built, a second 16-core engine faulted 248 pages here, 220 of them in the cores; in
 * the reverse order, 25 (24 + 1).
 *
 * The engine: 16 cores, one actor each, every actor sending every other core a lap of its ring -- 1 024 buckets, the
 * ring's every slot -- so every one of the 16 x 15 rings that carries events has every one of its pages written, in
 * every engine. An actor acks each peer whose lap it has received whole, and is done once it has acked every peer and
 * every peer has acked it: by then everything it enqueued has been received.
 *
 * The count is taken where a ring is written. On the calling thread, over `Main::start()`: where the mailboxes are built
 * and their rings' lock and index lines written. On each core's thread, from before its first send to the moment it is
 * done: where a ring's slots are written, by the sender's flush. Every count is the thread's own (`RUSAGE_THREAD`), so
 * the threads' own starts and exits stay out of it. A second engine whose rings are faulted again takes at least one
 * fault per page of 240 rings of 16 pages, 3 840; one whose blocks changed cores, 16 per mailbox. A warm one faults what
 * an engine start faults however its rings are kept -- 24 to 28 pages measured on Linux with g++-14, a region per ring
 * and a block per mailbox alike -- plus what a core touches for the first time in a pass, a page or two at most. The
 * bound is three pages a core, 48 at 16 cores: above the warm count by half again, and crossed by two mailboxes that
 * swapped blocks (16 pages each) as by all of them. Only the second engine is judged: the first is cold by definition,
 * and the second is the one whose fate the eager mailbox's `malloc` decided the same way in every fresh process, which
 * is also why this case is a binary of its own.
 *
 * Registered on Linux outside the sanitizer builds (CMakeLists.txt): the per-thread count is Linux's, and a sanitizer
 * replaces the allocator whose behaviour the eager mailbox's count depended on.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/system/allocator/pipe.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include "../../shared/PageFaults.h"

// A named namespace: these actors are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace engine_restart_mailboxes_test {

using namespace std::chrono_literals;

constexpr qb::CoreId kCores = 16; ///< 16 x 15 rings carry events, 16.7 MB of blocks once every pair has talked
/// What a warm second engine may fault, in pages: three a core (the file note derives it).
constexpr long long kWarmPagesPerCore = 3;

/// A lap's worth of one event: 32 buckets of 64 bytes.
struct Lap : qb::Event {
    std::array<std::byte, 1984> fill{};
};
/// Buckets in a ring (`max_deliverable_buckets + 1` slots): a sender that writes this many has written every one.
constexpr std::size_t kLapBuckets      = qb::detail::max_deliverable_buckets + 1;
constexpr std::size_t kLapEventBuckets = qb::allocator::getItemSize<Lap, EventBucket>();
constexpr std::size_t kLapEvents       = (kLapBuckets + kLapEventBuckets - 1) / kLapEventBuckets;

/// A lap received whole.
struct Ack : qb::Event {};

std::atomic<long long> g_window_faults{0}; ///< the cores' windows, summed
std::atomic<int>       g_windows{0};       ///< windows closed with a count

// One per core: a lap to every other core, an ack for every lap received whole, done once both are complete.
class Peer final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Lap>(*this);
        registerEvent<Ack>(*this);
        _faults_before = qb::test::thread_page_faults(); // the window opens before this core sends anything
        for (qb::CoreId core = 0; core < kCores; ++core)
            if (core != getIndex())
                for (std::size_t i = 0; i < kLapEvents; ++i)
                    push<Lap>(qb::BroadcastId(core));
        co_return true;
    }

    void
    on(Lap const &event) {
        if (++_laps[event.getSource().index()] == kLapEvents) {
            push<Ack>(event.getSource());
            ++_laps_whole;
            done_if_complete();
        }
    }

    void
    on(Ack const &) {
        ++_acks;
        done_if_complete();
    }

private:
    void
    done_if_complete() {
        if (_laps_whole != kCores - 1 || _acks != kCores - 1)
            return;
        long long const after = qb::test::thread_page_faults();
        if (_faults_before >= 0 && after >= 0) {
            g_window_faults.fetch_add(after - _faults_before, std::memory_order_relaxed);
            g_windows.fetch_add(1, std::memory_order_relaxed);
        }
        kill();
    }

    long long                       _faults_before = -1;
    std::array<std::size_t, kCores> _laps{};
    std::size_t                     _laps_whole = 0;
    std::size_t                     _acks       = 0;
};

struct Faults {
    long long start;   ///< the calling thread's, over `Main::start()`
    long long windows; ///< the cores' threads', from before their first send to their last ack
};

/// One engine whose pairs all talk, counted where its rings are written.
[[nodiscard]] Faults
faults_of_one_engine() {
    g_window_faults = 0;
    g_windows       = 0;
    qb::Main main;
    for (qb::CoreId core = 0; core < kCores; ++core)
        main.core(core)
            .setAffinity(qb::CoreIdSet{qb::NoAffinity}) // 16 cores on any host: the OS places them
            .setLatency(1ms)                            // and an idle core parks instead of spinning
            .addActor<Peer>();
    long long const before = qb::test::thread_page_faults();
    main.start();
    long long const after = qb::test::thread_page_faults();
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_windows.load(), static_cast<int>(kCores)) << "a core ended without closing its window";
    return {after - before, g_window_faults.load()};
}

} // namespace engine_restart_mailboxes_test

using namespace engine_restart_mailboxes_test;

TEST(EngineRestartMailboxes, ASecondEngineWritesItsMailboxRingsInPagesTheFirstLeftFaulted) {
    ASSERT_GE(qb::test::thread_page_faults(), 0) << "the platform has no per-thread fault counter";
    Faults const cold = faults_of_one_engine();
    Faults const warm = faults_of_one_engine();
    std::printf("[ faults ] %u cores, every pair a lap of its ring: first engine %lld at start() + %lld in the cores, "
                "second %lld + %lld (page %zu B, hugepages %s)\n",
                static_cast<unsigned>(kCores), cold.start, cold.windows, warm.start, warm.windows, qb::test::page_bytes(),
                qb::test::hugepage_mode().c_str());
    RecordProperty("faults_first_engine", static_cast<int>(cold.start + cold.windows));
    RecordProperty("faults_second_engine", static_cast<int>(warm.start + warm.windows));

    long long const bound = kWarmPagesPerCore * kCores;
    EXPECT_LE(warm.start + warm.windows, bound)
        << "the second engine faulted " << warm.start << " pages in Main::start() and " << warm.windows
        << " in its cores' exchange, against a bound of " << bound
        << ": its mailbox rings were written into memory the OS "
           "had taken back, or into blocks another core's mailbox had left, not into the block its own core's mailbox "
           "left mapped (Huly QB-1012, QB-203)";
}
