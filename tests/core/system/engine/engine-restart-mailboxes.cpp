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
 * @brief The second engine of a process builds its mailboxes in memory the first left faulted
 *        (Huly QB-1012), counted in the page faults `Main::start()` takes on the calling thread.
 *
 * The mailboxes are the engine's largest resting buffer: one inbound ring per core and per
 * sender, `N x N` rings of 65 728 bytes (readme/0_foundations/buffers.md), built and
 * value-initialised by `Main::start()` on the thread that calls it, freed when the engine goes.
 * At 16 cores a mailbox's rings are one 1 MiB block. In a fresh process glibc maps each such
 * block for itself (it is above the default `mmap` threshold), unmaps it at the first engine's
 * end and raises the threshold to its size, so the second engine's blocks come from fresh heap
 * the start then faults page by page: 16.8 MB, 4 100 faults of 4 KiB, before the first event.
 *
 * The count is the calling thread's own (`RUSAGE_THREAD`), so the cores' thread starts stay out
 * of it; what `start()` faults there besides the rings is each new thread's descriptor and TLS
 * block, a couple of pages per core. The bound, 2 MiB of pages, sits an order of magnitude from
 * both. Only the second engine is judged: it is the one whose fate glibc decides the same way in
 * every fresh process, which is also why this case is a binary of its own.
 *
 * Registered on Linux outside the sanitizer builds (CMakeLists.txt): the per-thread count is
 * Linux's, and a sanitizer replaces the allocator whose behaviour this counts.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <cstddef>
#include <cstdio>
#include "../../shared/PageFaults.h"

// A named namespace: these actors are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace engine_restart_mailboxes_test {

constexpr qb::CoreId kCores = 16; ///< 16 x 16 rings: 16.8 MB of mailboxes

// One per core, gone after the first pass: the engine lives just long enough to be built.
class Quitter final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

/// The calling thread's page faults over `Main::start()`: where the mailboxes are built.
[[nodiscard]] long long
faults_of_one_start() {
    qb::Main main;
    for (qb::CoreId core = 0; core < kCores; ++core)
        main.core(core).addActor<Quitter>();
    long long const before = qb::test::thread_page_faults();
    main.start();
    long long const after = qb::test::thread_page_faults();
    main.join();
    EXPECT_FALSE(main.hasError());
    return after - before;
}

} // namespace engine_restart_mailboxes_test

using namespace engine_restart_mailboxes_test;

TEST(EngineRestartMailboxes, ASecondEngineBuildsItsMailboxesInPagesTheFirstLeftFaulted) {
    ASSERT_GE(qb::test::thread_page_faults(), 0) << "the platform has no per-thread fault counter";
    long long const cold = faults_of_one_start();
    long long const warm = faults_of_one_start();
    std::printf("[ faults ] Main::start() on the calling thread, %u cores: first %lld, second %lld (page %zu B, hugepages %s)\n",
                static_cast<unsigned>(kCores), cold, warm, qb::test::page_bytes(), qb::test::hugepage_mode().c_str());
    RecordProperty("faults_first_start", static_cast<int>(cold));
    RecordProperty("faults_second_start", static_cast<int>(warm));

    long long const bound = static_cast<long long>((std::size_t{2} << 20) / qb::test::page_bytes());
    EXPECT_LT(warm, bound) << "the second start faulted " << warm
                           << " pages on the calling thread: the first engine's mailboxes were given back to the OS "
                              "and the second built its own in fresh memory (Huly QB-1012)";
}
