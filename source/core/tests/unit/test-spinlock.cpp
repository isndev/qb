/**
 * @file source/core/tests/unit/test-spinlock.cpp
 * @brief Unit tests for qb::lockfree::SpinLock.
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
 */

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <qb/system/lockfree/spinlock.h>
#include <qb/system/time.h>

using qb::lockfree::SpinLock;
using namespace std::chrono_literals;

TEST(SpinLock, StartsUnlocked) {
    SpinLock sl;
    EXPECT_FALSE(sl.locked());
}

TEST(SpinLock, TrylockTakesAndBlocks) {
    SpinLock sl;
    EXPECT_TRUE(sl.trylock());  // acquired
    EXPECT_TRUE(sl.locked());
    EXPECT_FALSE(sl.trylock()); // already held
    sl.unlock();
    EXPECT_FALSE(sl.locked());
    EXPECT_TRUE(sl.trylock());  // free again
    sl.unlock();
}

TEST(SpinLock, LockUnlock) {
    SpinLock sl;
    sl.lock();
    EXPECT_TRUE(sl.locked());
    sl.unlock();
    EXPECT_FALSE(sl.locked());
}

TEST(SpinLock, TrylockWithSpinCount) {
    SpinLock sl;
    EXPECT_TRUE(sl.trylock(100)); // free → acquires on first attempt
    EXPECT_FALSE(sl.trylock(50)); // held → exhausts spins, fails
    sl.unlock();
    EXPECT_TRUE(sl.trylock(50));  // free again
    sl.unlock();
}

TEST(SpinLock, TrylockForTimesOutThenSucceeds) {
    SpinLock sl;
    sl.lock();
    EXPECT_FALSE(sl.trylock_for(5ms)); // held → times out
    sl.unlock();
    EXPECT_TRUE(sl.trylock_for(5ms));  // free → acquires
    sl.unlock();
}

TEST(SpinLock, TrylockUntil) {
    SpinLock sl;
    sl.lock();
    EXPECT_FALSE(sl.trylock_until(qb::mono_now() + std::chrono::milliseconds{5}));
    sl.unlock();
    EXPECT_TRUE(sl.trylock_until(qb::mono_now() + std::chrono::milliseconds{5}));
    sl.unlock();
}

TEST(SpinLock, MutualExclusionUnderContention) {
    SpinLock         sl;
    std::uint64_t    counter = 0; // guarded by sl — no atomic, exclusivity must hold
    constexpr int    kThreads = 8;
    constexpr int    kIters   = 5000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&sl, &counter]() {
            for (int i = 0; i < kIters; ++i) {
                sl.lock();
                ++counter; // critical section
                sl.unlock();
            }
        });
    }
    for (auto &th : threads)
        th.join();
    EXPECT_EQ(counter, static_cast<std::uint64_t>(kThreads) * kIters);
    EXPECT_FALSE(sl.locked());
}
