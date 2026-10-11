/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/engine/mailbox-footprint.cpp
 * @brief What an engine holds before its first event: memory in proportion to its cores, and a mailbox ring only
 *        for a pair of cores that has talked (Huly QB-203).
 *
 * Every core's mailbox is a `qb::lockfree::mpsc::ringbuffer<EventBucket, MaxRingEvents, 0>` with one producer index
 * per core of the engine, and behind each index sits an SPSC ring of 1 024 event buckets -- 66 KB with the default
 * 64-byte bucket. Until 3.3 the mailbox built all of its rings at once and value-initialised them, so `Main::start()`
 * wrote N x N rings: 1 GiB at 128 cores, resident before the first event, almost all of it for pairs of cores that
 * never talk. A ring is now allocated by its producer's first enqueue.
 *
 * The witness reads the process's own memory -- the resident set on Linux (`/proc/self/statm`) and macOS
 * (`task_info`), the working set and the private commit on Windows (`GetProcessMemoryInfo`) -- across the start of an
 * engine of 64 idle cores that exchange no event, and bounds its growth by a budget PER CORE: sixteen rings' worth
 * (thirty-two under a sanitizer, whose shadow memory is counted too), several times what a core does hold before its
 * first event -- its 64 KiB receive buffer, its `VirtualCore` on its stack, its io loop. It counts bytes, never time,
 * so a loaded host cannot move it. The eager mailbox wrote 64 x 64 rings, 258 MiB, against a bound of 64 x 16 rings,
 * 64 MiB. A platform that gives a process no such reading skips the case and says so.
 *
 * An engine of the same shape is started and stopped first, so what the measured one finds already in place -- the
 * code an engine runs, the allocator arenas and thread stacks its cores leave behind, the process's one-time
 * initialisations -- is not counted against it. The eager rings could not hide that way: each mailbox's were one 4 MiB
 * allocation, a mapping of its own for glibc and for the Windows heap (above both thresholds), unmapped when the
 * warm-up's engine freed it -- so the measured engine wrote its rings into pages the process did not hold.
 *
 * The mechanism -- no ring before a producer enqueues, that producer's ring and no other after it, a ring published
 * while the consumer walks -- is pinned on the primitive itself, `qb-core-test-unit-mpsc-ring-allocation`. This file
 * uses nothing the eager mailbox did not have, so it builds, and fails, on the code before the fix.
 */

#include <chrono>
#include <cstdint>
#include <optional>

#include <gtest/gtest.h>
#include <qb/main.h>

#if defined(__linux__)
#include <fstream>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

// A sanitizer counts in the process's memory too: the shadow of what each core touches and its per-thread state. clang
// answers __has_feature; gcc and MSVC define __SANITIZE_*.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define QB_FOOTPRINT_UNDER_SANITIZER 1
#endif
#endif
#if !defined(QB_FOOTPRINT_UNDER_SANITIZER) && (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#define QB_FOOTPRINT_UNDER_SANITIZER 1
#endif
#if !defined(QB_FOOTPRINT_UNDER_SANITIZER)
#define QB_FOOTPRINT_UNDER_SANITIZER 0
#endif

namespace mailbox_footprint_test {

using namespace std::chrono_literals;

/// One mailbox ring, as the engine sizes it: what a pair of cores costs once one has sent to the other.
constexpr std::int64_t kRingBytes =
    static_cast<std::int64_t>(sizeof(qb::lockfree::spsc::ringbuffer<EventBucket, qb::detail::max_deliverable_buckets>));
constexpr std::int64_t kCores = 64;
/// What a core may hold before its first event, in rings: sixteen, thirty-two under a sanitizer. Either way below the
/// sixty-four a core held when every pair had a ring, so the case is red on the eager mailbox in every build.
constexpr std::int64_t kRingsPerCore = QB_FOOTPRINT_UNDER_SANITIZER ? 32 : 16;

/// The process's own memory as the platform counts it; `committed` is Windows' private commit, 0 elsewhere.
struct Footprint {
    std::int64_t resident  = 0;
    std::int64_t committed = 0;
};

[[nodiscard]] std::optional<Footprint>
read_footprint() {
#if defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::int64_t  pages    = 0;
    std::int64_t  resident = 0;
    if (!(statm >> pages >> resident))
        return std::nullopt;
    const long page_bytes = ::sysconf(_SC_PAGESIZE);
    if (page_bytes <= 0)
        return std::nullopt;
    return Footprint{resident * static_cast<std::int64_t>(page_bytes), 0};
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return std::nullopt;
    return Footprint{static_cast<std::int64_t>(info.resident_size), 0};
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
        return std::nullopt;
    return Footprint{static_cast<std::int64_t>(counters.WorkingSetSize), static_cast<std::int64_t>(counters.PrivateUsage)};
#else
    return std::nullopt;
#endif
}

[[nodiscard]] double
mib(std::int64_t const bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

/// A core's only actor: it registers nothing and sends nothing, and the default `SignalEvent` handler kills it on stop.
class Idle final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

/// Starts an engine of `kCores` idle cores, calls `while_running` once every core is past the start barrier -- no event
/// has crossed a core -- then stops the engine.
template <typename Probe>
void
with_idle_engine(Probe &&while_running) {
    qb::Main engine;
    for (std::int64_t core = 0; core < kCores; ++core)
        engine.core(static_cast<qb::CoreId>(core))
            .setAffinity(qb::CoreIdSet{qb::NoAffinity}) // 64 cores on any host: the OS places them
            .setLatency(1ms)                            // and an idle core parks instead of spinning
            .addActor<Idle>();
    engine.start(true);
    ASSERT_FALSE(engine.hasError()) << "the engine did not start";
    while_running();
    engine.stop();
    engine.join();
    EXPECT_FALSE(engine.hasError());
}

// ---- the witness: the process's memory across the start of an engine ---------------------------------------------------

TEST(MailboxFootprint, AnEngineHoldsMemoryInProportionToItsCoresBeforeItsFirstEvent) {
    if (!read_footprint())
        GTEST_SKIP() << "this platform gives a process no reading of its own memory (read here: Linux /proc/self/statm, "
                        "macOS task_info, Windows GetProcessMemoryInfo)";
    with_idle_engine([] {}); // the warm-up -- see the file note
    ASSERT_FALSE(HasFatalFailure());

    const Footprint before = read_footprint().value();
    Footprint       running{};
    with_idle_engine([&running] { running = read_footprint().value(); });
    ASSERT_FALSE(HasFatalFailure());

    constexpr std::int64_t bound = kCores * kRingsPerCore * kRingBytes;
    constexpr std::int64_t eager = kCores * kCores * kRingBytes;
    const std::int64_t     grew  = running.resident - before.resident;
    EXPECT_LE(grew, bound) << "an engine of " << kCores << " idle cores grew the process's resident memory by " << mib(grew)
                           << " MiB before its first event; the bound is " << kRingsPerCore << " rings per core, " << mib(bound)
                           << " MiB -- a ring for every pair of cores would be " << mib(eager) << " MiB";
#if defined(_WIN32)
    const std::int64_t committed = running.committed - before.committed;
    EXPECT_LE(committed, bound) << "an engine of " << kCores << " idle cores grew the process's private commit by " << mib(committed)
                                << " MiB before its first event; the bound is " << kRingsPerCore << " rings per core, " << mib(bound)
                                << " MiB -- a ring for every pair of cores would be " << mib(eager) << " MiB";
#endif
}

} // namespace mailbox_footprint_test
