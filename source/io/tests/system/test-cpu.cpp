/**
 * @file qb/source/io/tests/system/test-cpu.cpp
 * @brief System tests for qb CPU utilities.
 *
 * These tests exercise the public CPU information API and small RAII helpers
 * that live with the CPU utilities. They intentionally assert invariants rather
 * than machine-specific values so the suite remains portable across CI hosts.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @ingroup Tests
 */

#include <gtest/gtest.h>
#include <qb/system/cpu.h>

#include <cstdlib>
#include <memory>
#include <string>

TEST(CPU, ReportsConsistentTopology) {
    const std::string architecture = qb::CPU::Architecture();
    EXPECT_FALSE(architecture.empty());

    const int affinity = qb::CPU::Affinity();
    EXPECT_NE(affinity, 0);

    const auto [logical, physical] = qb::CPU::TotalCores();
    EXPECT_EQ(qb::CPU::LogicalCores(), logical);
    EXPECT_EQ(qb::CPU::PhysicalCores(), physical);

    if (logical > 0 && physical > 0) {
        EXPECT_GE(logical, physical);
        EXPECT_EQ(qb::CPU::HyperThreading(), logical != physical);
    } else {
        EXPECT_FALSE(qb::CPU::HyperThreading());
    }
}

TEST(CPU, ClockSpeedUsesDocumentedSentinel) {
    const auto clock_speed = qb::CPU::ClockSpeed();
    EXPECT_TRUE(clock_speed > 0 || clock_speed == -1);
}

TEST(CPU, SpinLoopPauseIsCallable) {
    for (int i = 0; i < 32; ++i) {
        qb::spin_loop_pause();
    }
}

TEST(Resource, WrapsPointerWithCustomDeleter) {
    bool deleted = false;
    auto ptr     = qb::resource(new int(42), [&deleted](int *value) {
        deleted = true;
        delete value;
    });

    ASSERT_NE(ptr.get(), nullptr);
    EXPECT_EQ(*ptr, 42);
    ptr.reset();
    EXPECT_TRUE(deleted);
}

TEST(Resource, WrapsVoidPointerWithCustomDeleter) {
    bool  deleted = false;
    void *raw     = std::malloc(8);
    ASSERT_NE(raw, nullptr);

    auto ptr = qb::resource(raw, [&deleted](void *value) {
        deleted = true;
        std::free(value);
    });

    ASSERT_NE(ptr.get(), nullptr);
    ptr.reset();
    EXPECT_TRUE(deleted);
}

TEST(ScopeGuard, RunsOnScopeExitUnlessDismissed) {
    int counter = 0;
    {
        qb::scope_guard guard([&counter] { ++counter; });
    }
    EXPECT_EQ(counter, 1);

    {
        qb::scope_guard guard([&counter] { ++counter; });
        guard.dismiss();
    }
    EXPECT_EQ(counter, 1);
}

TEST(ScopeGuard, MoveTransfersOwnership) {
    int counter = 0;
    {
        qb::scope_guard first([&counter] { ++counter; });
        auto            second = std::move(first);
        (void) second;
    }
    EXPECT_EQ(counter, 1);
}
