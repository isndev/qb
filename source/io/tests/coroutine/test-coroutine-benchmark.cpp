/**
 * @file qb/io/tests/coroutine/test-coroutine-benchmark.cpp
 * @brief Coroutine benchmark and throughput tests
 *
 * This file contains benchmark-oriented tests for coroutine overhead, including timer
 * latency, chained operations, nested awaits, coroutine frame allocation, spawn
 * throughput, concurrent execution, and timer accuracy distribution.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

#include <atomic>
#include <chrono>
#include <ev/ev++.h>
#include <functional>
#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <vector>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Single Operation Overhead
// =============================================================================

class CoroutineBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Timer latency comparison
 * @brief Compare timer latency: coroutine vs callback
 *
 * NOTE: Disabled due to timing sensitivity on different systems
 */
TEST_F(CoroutineBenchmark, DISABLED_TimerLatency) {
    constexpr int iterations = 100;

    // Coroutine version
    auto coro_start = std::chrono::steady_clock::now();
    std::atomic<int> coro_completed{0};

    for (int i = 0; i < iterations; ++i) {
        auto coro_completed_ptr = &coro_completed;
        auto coro_fn = [coro_completed_ptr]() -> task<void> {
            co_await sleep(1ms);
            coro_completed_ptr->fetch_add(1);
            co_return;
        };
        coro_scheduler().spawn(std::move(coro_fn));
    }

    run_for(200ms);
    auto coro_end = std::chrono::steady_clock::now();
    auto coro_ms = std::chrono::duration_cast<std::chrono::milliseconds>(coro_end - coro_start)
                       .count();

    // Cleanup
    qb::io::async::listener::current.clear();
    qb::io::async::init();

    // Callback version
    auto cb_start = std::chrono::steady_clock::now();
    std::atomic<int> cb_completed{0};

    struct CallbackTimer {
        ev_timer timer;
        std::atomic<int>& counter;
        static void callback(struct ev_loop*, ev_timer* w, int) {
            auto* self = static_cast<CallbackTimer*>(w->data);
            self->counter.fetch_add(1);
            delete self;
        }
        CallbackTimer(std::atomic<int>& c, ev::loop_ref loop)
            : counter(c) {
            ev_timer_init(&timer, callback, 0.001, 0);
            timer.data = this;
            ev_timer_start(loop, &timer);
        }
        ~CallbackTimer() {
            if (ev_is_active(&timer)) {
                ev_timer_stop(listener::current.loop(), &timer);
            }
        }
    };

    for (int i = 0; i < iterations; ++i) {
        new CallbackTimer(cb_completed, listener::current.loop());
    }

    // Wait for callbacks
    auto cb_wait_start = std::chrono::steady_clock::now();
    while (cb_completed < iterations && std::chrono::steady_clock::now() - cb_wait_start < 200ms) {
        listener::current.run(EVRUN_NOWAIT);
    }
    auto cb_end = std::chrono::steady_clock::now();
    auto cb_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cb_end - cb_start).count();

    // Both should complete all iterations
    EXPECT_EQ(coro_completed, iterations);
    EXPECT_EQ(cb_completed, iterations);

    // Coroutines should be within 2x of callback overhead
    EXPECT_LT(coro_ms, cb_ms * 2 + 50);
}

/**
 * @test Chained operations comparison
 * @brief Compare chained async operations
 *
 * NOTE: Disabled due to timing sensitivity
 */
TEST_F(CoroutineBenchmark, DISABLED_ChainedOperations) {
    constexpr int chain_length = 10;
    constexpr int iterations = 10;

    // Coroutine version - chained sleeps
    auto coro_start = std::chrono::steady_clock::now();
    std::atomic<int> coro_completed{0};

    for (int i = 0; i < iterations; ++i) {
        auto coro_completed_ptr = &coro_completed;
        auto coro_fn = [coro_completed_ptr]() -> task<void> {
            for (int j = 0; j < chain_length; ++j) {
                co_await sleep(1ms);
            }
            coro_completed_ptr->fetch_add(1);
            co_return;
        };
        coro_scheduler().spawn(std::move(coro_fn));
    }

    run_for(500ms);
    auto coro_end = std::chrono::steady_clock::now();
    auto coro_ms = std::chrono::duration_cast<std::chrono::milliseconds>(coro_end - coro_start)
                       .count();

    EXPECT_EQ(coro_completed, iterations);

    // Chained operations should complete within reasonable time
    // (chain_length * iterations * 1ms + overhead)
    EXPECT_LT(coro_ms, chain_length * iterations * 2 + 100);
}

/**
 * @test Nested await overhead
 * @brief Measure cost of nested co_await
 *
 * NOTE: Disabled due to timing sensitivity
 */
TEST_F(CoroutineBenchmark, DISABLED_NestedAwaitOverhead) {
    constexpr int depth = 5;
    constexpr int iterations = 20;
    std::atomic<int> completed{0};

    std::function<task<void>(int)> make_coro = [&](int level) -> task<void> {
        if (level <= 0) {
            completed.fetch_add(1);
            co_return;
        }
        co_await sleep(1ms);
        co_await make_coro(level - 1);
        co_return;
    };

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
        coro_scheduler().spawn(make_coro(depth));
    }

    run_for(300ms);

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    EXPECT_EQ(completed, iterations);
    // Nested depth * 1ms per level * iterations + overhead
    EXPECT_LT(ms, depth * iterations * 2 + 100);
}

// =============================================================================
// TEST SUITE: Memory Allocation
// =============================================================================

class CoroutineMemoryBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Coroutine frame allocation
 * @brief Test allocation overhead per coroutine
 */
TEST_F(CoroutineMemoryBenchmark, CoroutineFrameAllocation) {
    constexpr int count = 100;

    // Small coroutine - minimal frame
    for (int i = 0; i < count; ++i) {
        coro_scheduler().spawn([]() -> task<void> { co_return; });
    }
    run_for(50ms);

    // Large coroutine - with captures
    struct LargeData {
        char data[1024];
    };

    auto large_start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        LargeData data{};
        auto coro_fn = [data]() -> task<void> {
            co_await sleep(1ms);
            (void) data;
            co_return;
        };
        coro_scheduler().spawn(std::move(coro_fn));
    }
    run_for(200ms);
    auto large_end = std::chrono::steady_clock::now();
    auto large_ms = std::chrono::duration_cast<std::chrono::milliseconds>(large_end - large_start)
                        .count();

    // Large coroutines should still complete in reasonable time
    EXPECT_LT(large_ms, 500);
}

// =============================================================================
// TEST SUITE: Throughput
// =============================================================================

class CoroutineThroughput : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Maximum spawn rate
 * @brief Measure how fast we can spawn coroutines
 */
TEST_F(CoroutineThroughput, MaximumSpawnRate) {
    constexpr int count = 5000;
    std::atomic<int> completed{0};

    auto spawn_start = std::chrono::steady_clock::now();

    for (int i = 0; i < count; ++i) {
        auto completed_ptr = &completed;
        auto coro_fn = [completed_ptr]() -> task<void> {
            completed_ptr->fetch_add(1);
            co_return;
        };
        coro_scheduler().spawn(std::move(coro_fn));
    }

    auto spawn_end = std::chrono::steady_clock::now();
    auto spawn_us = std::chrono::duration_cast<std::chrono::microseconds>(spawn_end - spawn_start)
                        .count();

    run_for(100ms);

    // All should complete
    EXPECT_EQ(completed, count);

    // Spawn rate should be reasonable (less than 50us per coroutine on average)
    EXPECT_LT(spawn_us / count, 50);
}

/**
 * @test Concurrent execution throughput
 * @brief Measure throughput with concurrent coroutines
 */
TEST_F(CoroutineThroughput, ConcurrentExecution) {
    constexpr int concurrent = 100;
    constexpr int work_per_coro = 100;
    std::atomic<int> total_work{0};

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < concurrent; ++i) {
        auto total_work_ptr = &total_work;
        auto coro_fn = [total_work_ptr]() -> task<void> {
            for (int j = 0; j < work_per_coro; ++j) {
                co_await sleep(1ms);
                total_work_ptr->fetch_add(1);
            }
            co_return;
        };
        coro_scheduler().spawn(std::move(coro_fn));
    }

    run_for(2000ms);

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // All work should complete
    EXPECT_EQ(total_work, concurrent * work_per_coro);

    // Throughput should be reasonable
    // (concurrent * work_per_coro * 1ms + overhead)
    EXPECT_LT(ms, concurrent * work_per_coro * 2 + 500);
}

// =============================================================================
// TEST SUITE: Latency Distribution
// =============================================================================

class CoroutineLatency : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Timer accuracy distribution
 * @brief Test timer accuracy across many timers
 */
TEST_F(CoroutineLatency, TimerAccuracyDistribution) {
    constexpr int count = 50;
    constexpr int duration_ms = 50;
    std::vector<long long> actual_durations;
    actual_durations.reserve(count);

    for (int i = 0; i < count; ++i) {
        auto actual_durations_ptr = &actual_durations;
        auto coro_fn = [start = std::chrono::steady_clock::now(),
                        actual_durations_ptr,
                        duration_ms]() -> task<void> {
            co_await sleep(std::chrono::milliseconds(duration_ms));
            auto end = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                               .count();
            actual_durations_ptr->push_back(elapsed);
            co_return;
        };
        coro_scheduler().spawn(std::move(coro_fn));
    }

    run_for(200ms);

    EXPECT_EQ(actual_durations.size(), count);

    // Calculate statistics
    long long sum = 0;
    long long min_val = actual_durations[0];
    long long max_val = actual_durations[0];

    for (auto d : actual_durations) {
        sum += d;
        min_val = std::min(min_val, d);
        max_val = std::max(max_val, d);
    }

    double avg = static_cast<double>(sum) / count;

    // Average should be close to expected
    EXPECT_GE(avg, duration_ms - 5);
    EXPECT_LE(avg, duration_ms + 15);

    // Spread should be reasonable (max-min < 30ms)
    EXPECT_LT(max_val - min_val, 30);
}

// =============================================================================
// MAIN
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
