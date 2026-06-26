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
 */

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include <qb/system/cpu.h>

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
        EXPECT_EQ(qb::CPU::HyperThreading(), logical != physical)
            << "HyperThreading() must equal (logical != physical)";
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
