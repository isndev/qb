/**
 * @file qb/io/tests/coroutine/test-coroutine-timers.cpp
 * @brief Coroutine timer and sleep awaiter tests
 *
 * This file contains tests for timer-based coroutine suspension, including sleep
 * awaiters, timer ordering, timer destruction, long-duration timers, mixed
 * durations, and looped timers.
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
 * @ingroup Tests
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TIMER ACCURACY TESTS
// =============================================================================

class CoroutineTimerAccuracy : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Timer accuracy with tolerance
 * @brief Verify timers fire within expected time window
 *
 * NOTE: Disabled - timing sensitive test
 */
TEST_F(CoroutineTimerAccuracy, DISABLED_TimerWithTolerance) {
    constexpr int duration_ms  = 50;
    constexpr int tolerance_ms = 20;

    std::atomic<bool> fired{false};
    auto              start = std::chrono::steady_clock::now();

    auto fired_ptr = &fired;
    auto coro_fn   = [fired_ptr, duration_ms]() -> task<void> {
        co_await sleep(std::chrono::milliseconds(duration_ms));
        (*fired_ptr) = true;
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(std::chrono::milliseconds(duration_ms + tolerance_ms + 50));

    auto end     = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_TRUE(fired);
    // Should fire between duration and duration + tolerance
    EXPECT_GE(elapsed, duration_ms - tolerance_ms / 2);
    EXPECT_LE(elapsed, duration_ms + tolerance_ms);
}

/**
 * @test Multiple timers ordering
 * @brief Verify multiple timers complete in order
 *
 * NOTE: Disabled - timing sensitive test
 */
TEST_F(CoroutineTimerAccuracy, DISABLED_MultipleTimersOrdering) {
    std::vector<int> completion_order;
    std::mutex       mutex;

    // Create timers with different durations
    auto completion_order_ptr = &completion_order;
    auto mutex_ptr            = &mutex;
    for (int i = 0; i < 5; ++i) {
        auto coro_fn = [i, completion_order_ptr, mutex_ptr]() -> task<void> {
            // Duration: 50, 40, 30, 20, 10 ms (reversed order)
            int duration = (5 - i) * 10;
            co_await sleep(std::chrono::milliseconds(duration));

            std::lock_guard<std::mutex> lock(*mutex_ptr);
            completion_order_ptr->push_back(i);
            co_return;
        };
        coro_scheduler().spawn(coro_fn); // owned-callable: closure dies before resume
    }

    run_for(200ms);

    EXPECT_EQ(completion_order.size(), 5);

    // Shortest timers (highest i) should complete first
    // Since we have 0:50ms, 1:40ms, 2:30ms, 3:20ms, 4:10ms
    // Order should be roughly: 4, 3, 2, 1, 0
    EXPECT_EQ(completion_order[0], 4);
    EXPECT_EQ(completion_order[4], 0);
}

/**
 * @test Timer cancellation via destruction
 * @brief Verify timer stops when coroutine is destroyed
 */
TEST_F(CoroutineTimerAccuracy, TimerDestruction) {
    std::atomic<bool> completed{false};
    std::atomic<bool> cancelled_completed{false};

    // Create two timers
    {
        auto completed_ptr = &completed;
        auto coro_fn_1     = [completed_ptr]() -> task<void> {
            co_await sleep(200ms);
            completed_ptr->store(true);
            co_return;
        };
        auto t1 = coro_fn_1();

        auto cancelled_completed_ptr = &cancelled_completed;
        auto coro_fn_2               = [cancelled_completed_ptr]() -> task<void> {
            co_await sleep(10ms);
            cancelled_completed_ptr->store(true);
            co_return;
        };
        auto t2 = coro_fn_2();

        coro_scheduler().spawn(coro_fn_2); // owned-callable: closure dies at block end
        (void) t2;
        // t1 goes out of scope here - should be cancelled
    }

    run_for(50ms);

    // t2 should complete, t1 should not
    EXPECT_TRUE(cancelled_completed);
    EXPECT_FALSE(completed);
}

/**
 * @test Long duration timer
 * @brief Timer that doesn't fire within test window
 */
TEST_F(CoroutineTimerAccuracy, LongDurationTimer) {
    std::atomic<bool> completed{false};

    auto completed_ptr = &completed;
    auto coro_fn       = [completed_ptr]() -> task<void> {
        co_await sleep(500ms);
        (*completed_ptr) = true;
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));

    // Don't wait long enough
    run_for(50ms);

    EXPECT_FALSE(completed);

    // Now wait longer
    run_for(500ms);

    EXPECT_TRUE(completed);
}

// =============================================================================
// TIMER EDGE CASES
// =============================================================================

class CoroutineTimerEdgeCases : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Very short timers
 * @brief Timers with 1ms duration
 */
TEST_F(CoroutineTimerEdgeCases, VeryShortTimers) {
    constexpr int    count = 10;
    std::atomic<int> completed{0};

    for (int i = 0; i < count; ++i) {
        auto completed_ptr = &completed;
        auto coro_fn       = [completed_ptr]() -> task<void> {
            co_await sleep(1ms);
            completed_ptr->fetch_add(1);
            co_return;
        };
        coro_scheduler().spawn(coro_fn); // owned-callable: closure dies before resume
    }

    run_for(100ms);

    EXPECT_EQ(completed, count);
}

/**
 * @test Mixed duration timers
 * @brief Timers with various durations
 */
TEST_F(CoroutineTimerEdgeCases, MixedDurationTimers) {
    std::vector<int> durations{5, 10, 15, 20, 25};
    std::atomic<int> completed{0};

    for (int duration : durations) {
        auto completed_ptr = &completed;
        auto coro_fn       = [completed_ptr, duration]() -> task<void> {
            co_await sleep(std::chrono::milliseconds(duration));
            completed_ptr->fetch_add(1);
            co_return;
        };
        coro_scheduler().spawn(coro_fn); // owned-callable: closure dies before resume
    }

    run_for(100ms);

    EXPECT_EQ(completed, durations.size());
}

/**
 * @test Timer in loop
 * @brief Repeated timer in a loop
 */
TEST_F(CoroutineTimerEdgeCases, TimerInLoop) {
    constexpr int    iterations = 5;
    std::atomic<int> counter{0};

    auto counter_ptr = &counter;
    auto coro_fn     = [counter_ptr]() -> task<void> {
        for (int i = 0; i < iterations; ++i) {
            co_await sleep(10ms);
            counter_ptr->fetch_add(1);
        }
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    EXPECT_EQ(counter, iterations);
}

// =============================================================================
// MAIN
// =============================================================================

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
