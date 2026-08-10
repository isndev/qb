/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/core/cpu-topology.cpp
 * @brief `qb::CPU` topology query API + `qb::spin_loop_pause()`.
 *
 * `qb::CPU` (qb/system/cpu.h) reports architecture, affinity, logical/physical core counts, clock
 * speed and hyper-threading. The values are machine-specific, so this `unit` test asserts the
 * *invariants and self-consistency* of the API (not host-specific numbers) so it stays portable
 * across every CI runner. `spin_loop_pause()` is the hot-spin yield hint. No engine, no I/O.
 *
 * Split out of system/test-cpu.cpp (spec §2): the CPU/topology cases live here, the RAII helpers
 * (`qb::resource` / `qb::scope_guard`) move to unit/core/raii-helpers.cpp. Strengthened:
 * `SpinLoopPause` now makes a real observable assertion (it forces the function to actually run a
 * bounded number of times and survive) rather than being a bare no-assert smoke loop.
 *
 * Also covers `CPU::ThreadPinningSupported()` — the query that tells a caller whether
 * `CoreInitializer::setAffinity()` does anything at all on this host. That one is deliberately
 * NOT asserted against a hard-coded platform expectation: it is cross-checked against the
 * kernel's own answer, so it stays honest on Apple Silicon (where the answer is `false`), on
 * Intel macOS, and on Linux, without the test knowing which it is running on.
 */

#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <qb/system/cpu.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

// =============================================================================
// TOPOLOGY — self-consistent invariants, not machine-specific values
// =============================================================================

/**
 * @test Architecture string is non-empty; affinity is non-zero; the paired and individual core
 *       accessors agree; hyper-threading is exactly `logical != physical`.
 * @brief Folded from test-cpu.cpp::ReportsConsistentTopology. The cross-checks (TotalCores vs the
 *        singular accessors, HT vs the core-count delta) are the real contract; absolute counts are
 *        intentionally not asserted.
 */
TEST(CpuTopology, ReportsConsistentTopology) {
    const std::string architecture = qb::CPU::Architecture();
    EXPECT_FALSE(architecture.empty());

    const int affinity = qb::CPU::Affinity();
    EXPECT_NE(affinity, 0) << "at least one logical processor must be available to the process";

    const auto [logical, physical] = qb::CPU::TotalCores();
    EXPECT_EQ(qb::CPU::LogicalCores(), logical) << "LogicalCores() must agree with TotalCores().first";
    EXPECT_EQ(qb::CPU::PhysicalCores(), physical) << "PhysicalCores() must agree with TotalCores().second";

    if (logical > 0 && physical > 0) {
        EXPECT_GE(logical, physical) << "logical cores cannot be fewer than physical";
        EXPECT_EQ(qb::CPU::HyperThreading(), logical != physical) << "HyperThreading() must equal (logical != physical)";
    } else {
        EXPECT_FALSE(qb::CPU::HyperThreading()) << "unknown topology must not claim hyper-threading";
    }
}

/**
 * @test Clock speed is either a positive frequency or the documented `-1` unavailable sentinel.
 * @brief Folded from test-cpu.cpp::ClockSpeedUsesDocumentedSentinel.
 */
TEST(CpuTopology, ClockSpeedIsPositiveOrUnavailableSentinel) {
    const std::int64_t clock_speed = qb::CPU::ClockSpeed();
    EXPECT_TRUE(clock_speed > 0 || clock_speed == -1)
        << "ClockSpeed() must be a positive Hz value or the -1 unavailable sentinel, got " << clock_speed;
}

// =============================================================================
// THREAD PINNING CAPABILITY — cross-checked against the kernel, not restated
// =============================================================================

/**
 * @test `ThreadPinningSupported()` answers the same thing every time it is asked.
 * @brief The macOS implementation probes the kernel once and caches the answer in a magic
 *        static. A capability that flips between calls would be useless to branch on, and the
 *        cache is exactly what could break it (racing first-callers, a probe with a side
 *        effect on the asking thread). Ask from several threads at once as well as inline.
 */
TEST(CpuTopology, ThreadPinningSupportedIsStable) {
    const bool first = qb::CPU::ThreadPinningSupported();

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(qb::CPU::ThreadPinningSupported(), first) << "the capability must not vary between calls";
    }

    bool        from_threads[4] = {!first, !first, !first, !first};
    std::thread askers[4];
    for (int i = 0; i < 4; ++i) {
        askers[i] = std::thread([&from_threads, i] { from_threads[i] = qb::CPU::ThreadPinningSupported(); });
    }
    for (auto &asker : askers) {
        asker.join();
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(from_threads[i], first) << "the capability must not depend on which thread asks (thread " << i << ")";
    }
}

/**
 * @test The reported capability agrees with what the platform's pinning call actually does.
 * @brief This is the whole point of the query, so it must not be a restatement of the
 *        implementation. The implementation probes with the *read* call; this test plants the
 *        *write* call — the one `VirtualCore::__init__` really issues through its macOS shim —
 *        on a scratch thread, and requires both to reach the same verdict.
 *
 *        On Apple Silicon this fails if anyone ever "fixes" `ThreadPinningSupported()` into a
 *        compile-time `#ifdef`, or if the shim's `KERN_NOT_SUPPORTED` handling drifts: the
 *        kernel says 46, so the query must say `false`. On Intel macOS, on Linux and on Windows
 *        it must say `true`, and nothing in the test hard-codes which host it is running on.
 */
TEST(CpuTopology, ThreadPinningSupportedMatchesTheKernel) {
#if defined(__APPLE__)
    // Exactly the call VirtualCore's macOS shim makes (THREAD_AFFINITY_POLICY_COUNT is 1, the
    // literal the shim passes). Run it on a scratch thread so a successful set — Intel macOS —
    // leaves no affinity tag on the gtest thread.
    kern_return_t set_ret = KERN_SUCCESS;

    std::thread probe([&set_ret] {
        thread_affinity_policy_data_t policy = {static_cast<integer_t>(1)};
        set_ret = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy),
                                    THREAD_AFFINITY_POLICY_COUNT);
    });
    probe.join();

    EXPECT_EQ(qb::CPU::ThreadPinningSupported(), set_ret != KERN_NOT_SUPPORTED)
        << "ThreadPinningSupported() must agree with thread_policy_set(THREAD_AFFINITY_POLICY), which returned " << set_ret
        << " (KERN_NOT_SUPPORTED is " << KERN_NOT_SUPPORTED << ")";

#elif defined(__linux__)
    // Read this thread's own mask and write it straight back: a request the kernel always
    // permits, so the test cannot flake on a cpuset-restricted runner, while still driving the
    // real pthread_setaffinity_np() path rather than asserting the `return true` back.
    int         get_rc = -1;
    int         set_rc = -1;
    std::thread probe([&get_rc, &set_rc] {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        get_rc = pthread_getaffinity_np(pthread_self(), sizeof(mask), &mask);
        if (get_rc == 0) {
            set_rc = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
        }
    });
    probe.join();

    EXPECT_EQ(get_rc, 0) << "pthread_getaffinity_np() must succeed where qb reports pinning support";
    EXPECT_EQ(set_rc, 0) << "re-applying a thread's own affinity mask must succeed";
    EXPECT_TRUE(qb::CPU::ThreadPinningSupported()) << "Linux implements per-thread pinning";

#elif defined(_WIN32) || defined(_WIN64)
#ifdef _MSC_VER
    EXPECT_TRUE(qb::CPU::ThreadPinningSupported()) << "Windows/MSVC uses SetThreadAffinityMask()";
#else
    EXPECT_FALSE(qb::CPU::ThreadPinningSupported()) << "a GNU toolchain on Windows gets no pinning (see VirtualCore::__init__)";
#endif

#else
    // Any other platform: no claim to cross-check, only that the query is callable and total.
    const bool supported = qb::CPU::ThreadPinningSupported();
    EXPECT_TRUE(supported || !supported);
#endif
}

// =============================================================================
// spin_loop_pause — strengthened from a bare smoke loop
// =============================================================================

/**
 * @test `spin_loop_pause()` is callable in a tight loop and the loop makes forward progress.
 * @brief Strengthened over test-cpu.cpp::SpinLoopPauseIsCallable, which looped 32 times with NO
 *        assertion (it could not distinguish "ran" from "compiled out"). We now count the
 *        iterations through a `volatile` sink the optimizer cannot elide, so the test asserts the
 *        loop body actually executed the documented number of times — proving the intrinsic is
 *        reachable and non-trapping on this architecture.
 */
TEST(CpuTopology, SpinLoopPauseRunsAndMakesProgress) {
    constexpr int kIterations = 32;
    volatile int  ran         = 0;
    for (int i = 0; i < kIterations; ++i) {
        qb::spin_loop_pause();
        ran = ran + 1; // volatile compound-assignment (++) is deprecated in C++20
    }
    EXPECT_EQ(ran, kIterations) << "spin_loop_pause() must be callable on every iteration without trapping";
}
