/**
 * @file test_coroutine_patterns.cpp
 * @brief Concurrency Patterns for Coroutines
 *
 * Tests for common async patterns: producer-consumer, pipeline,
 * scatter-gather, circuit breaker, fallback, and barrier patterns.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>
#include <vector>
#include <mutex>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// SYNCHRONIZATION PATTERNS
// =============================================================================

class CoroutineSyncPatterns : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test WhenAll with proper synchronization
 * @brief Collect all results before continuing
 * 
 * NOTE: Disabled - complex issue with task vector awaiting
 */
TEST_F(CoroutineSyncPatterns, WhenAllWithResults) {
    constexpr int count = 5;
    std::vector<int> results;
    std::mutex mutex;
    
    // Define worker outside coroutine
    auto worker = [](int id) -> task<int> {
        co_await sleep(std::chrono::milliseconds(10 + id * 5));
        co_return id * 10;
    };
    
    auto results_ptr = &results;
    auto mutex_ptr = &mutex;
    auto coro_fn = [results_ptr, mutex_ptr, &worker]() -> task<void> {
        std::vector<task<int>> tasks;
        
        // Create all tasks
        for (int i = 0; i < count; ++i) {
            tasks.push_back(worker(i));
        }
        
        // Wait for all and collect results
        for (size_t i = 0; i < tasks.size(); ++i) {
            int result = co_await tasks[i];
            std::lock_guard<std::mutex> lock(*mutex_ptr);
            results_ptr->push_back(result);
        }
        
        co_return;
    };
    auto coordinator = coro_fn();
    
    coro_scheduler().spawn(std::move(coordinator));
    run_for(200ms);
    
    EXPECT_EQ(results.size(), count);
    
    // Sort and verify
    std::sort(results.begin(), results.end());
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(results[i], i * 10);
    }
}

/**
 * @test Barrier pattern
 * @brief Wait for all tasks to reach a barrier
 */
TEST_F(CoroutineSyncPatterns, BarrierPattern) {
    constexpr int count = 3;
    std::atomic<int> at_barrier{0};
    std::atomic<int> completed{0};
    
    auto worker = [&at_barrier, &completed](int id) -> task<void> {
        // Phase 1: Do work
        co_await sleep(std::chrono::milliseconds(10 + id * 10));
        at_barrier.fetch_add(1);
        
        // Phase 2: Wait at barrier
        while (at_barrier < count) {
            co_await sleep(5ms);
        }
        
        // Phase 3: Continue
        completed.fetch_add(1);
        co_return;
    };
    auto coro_fn = [&worker]() -> task<void> {
        for (int i = 0; i < count; ++i) {
            coro_scheduler().spawn(worker(i));
        }
        co_return;
    };
    auto coordinator = coro_fn();
    
    coro_scheduler().spawn(std::move(coordinator));
    run_for(200ms);
    
    EXPECT_EQ(completed, count);
}

/**
 * @test Pipeline with backpressure
 * @brief Control flow through pipeline stages
 * 
 * NOTE: Disabled - stage lambdas not returning correct values
 */
TEST_F(CoroutineSyncPatterns, PipelineWithBackpressure) {
    std::vector<int> results;
    std::mutex mutex;
    constexpr int count = 5;
    
    // Define stages OUTSIDE coroutine
    auto stage1 = [](int x) -> task<int> {
        co_await sleep(10ms);
        co_return x * 2;
    };
    
    auto stage2 = [](int x) -> task<int> {
        co_await sleep(10ms);
        co_return x + 10;
    };
    
    auto results_ptr = &results;
    auto mutex_ptr = &mutex;
    
    auto coro_fn = [results_ptr, mutex_ptr, &stage1, &stage2]() -> task<void> {
        for (int i = 1; i <= count; ++i) {
            int r1 = co_await stage1(i);
            int r2 = co_await stage2(r1);
            
            std::lock_guard<std::mutex> lock(*mutex_ptr);
            results_ptr->push_back(r2);
        }
        co_return;
    };
    auto pipeline = coro_fn();
    
    coro_scheduler().spawn(std::move(pipeline));
    run_for(500ms);
    
    EXPECT_EQ(results.size(), count);
    
    // Verify: ((x * 2) + 10)
    for (size_t i = 0; i < results.size(); ++i) {
        int input = i + 1;
        int expected = (input * 2) + 10;
        EXPECT_EQ(results[i], expected);
    }
}

/**
 * @test Producer-consumer with bounded buffer
 * @brief Classic producer-consumer pattern
 */
TEST_F(CoroutineSyncPatterns, ProducerConsumerBounded) {
    constexpr int num_items = 10;
    constexpr int buffer_size = 3;
    std::vector<int> buffer;
    std::mutex mutex;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    
    auto producer = [&buffer, &mutex, &produced]() -> task<void> {
        for (int i = 0; i < num_items; ++i) {
            // Wait if buffer is full
            while (true) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (buffer.size() < buffer_size) {
                        buffer.push_back(i);
                        produced.fetch_add(1);
                        break;
                    }
                }
                co_await sleep(5ms);
            }
            co_await sleep(10ms);  // Production time
        }
        co_return;
    };
    
    auto consumer = [&buffer, &mutex, &consumed]() -> task<void> {
        while (consumed < num_items) {
            // Check if buffer has items
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!buffer.empty()) {
                    buffer.erase(buffer.begin());
                    consumed.fetch_add(1);
                }
            }
            co_await sleep(15ms);  // Consumption time
        }
        co_return;
    };
    
    coro_scheduler().spawn(producer());
    coro_scheduler().spawn(consumer());
    
    run_for(500ms);
    
    EXPECT_EQ(produced, num_items);
    EXPECT_EQ(consumed, num_items);
}

// =============================================================================
// TIMING AND ORDERING TESTS
// =============================================================================

class CoroutineTimingPatterns : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Sequential timers
 * @brief Chain of timers with specific delays
 */
TEST_F(CoroutineTimingPatterns, SequentialTimers) {
    std::vector<std::chrono::steady_clock::time_point> timestamps;
    auto start = std::chrono::steady_clock::now();
    
    auto timestamps_ptr = &timestamps;
    auto coro_fn = [timestamps_ptr]() -> task<void> {
        co_await sleep(20ms);
        timestamps_ptr->push_back(std::chrono::steady_clock::now());
        
        co_await sleep(30ms);
        timestamps_ptr->push_back(std::chrono::steady_clock::now());
        
        co_await sleep(20ms);
        timestamps_ptr->push_back(std::chrono::steady_clock::now());
        
        co_return;
    };
    auto chain = coro_fn();
    
    coro_scheduler().spawn(std::move(chain));
    run_for(100ms);
    
    EXPECT_EQ(timestamps.size(), 3);
    
    // Check intervals
    auto first = timestamps[0] - start;
    auto second = timestamps[1] - timestamps[0];
    auto third = timestamps[2] - timestamps[1];
    
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(first).count(), 15);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(second).count(), 25);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(third).count(), 15);
}

/**
 * @test Timeout pattern
 * @brief Implement timeout for an operation
 */
TEST_F(CoroutineTimingPatterns, TimeoutPattern) {
    std::atomic<bool> timeout_occurred{false};
    std::atomic<bool> operation_completed{false};
    
    auto slow_operation = [&operation_completed]() -> task<void> {
        co_await sleep(100ms);
        operation_completed = true;
        co_return;
    };
    
    auto timeout_occurred_ptr = &timeout_occurred;
    auto coro_fn = [timeout_occurred_ptr, &slow_operation]() -> task<void> {
        auto operation = slow_operation();
        auto start = std::chrono::steady_clock::now();
        
        // Poll with timeout
        while (std::chrono::steady_clock::now() - start < 50ms) {
            if (operation.handle().promise().is_ready()) {
                co_await operation;
                co_return;
            }
            co_await sleep(5ms);
        }
        
        // Timeout occurred
        (*timeout_occurred_ptr) = true;
        co_return;
    };
    auto with_timeout = coro_fn();
    
    coro_scheduler().spawn(std::move(with_timeout));
    run_for(150ms);
    
    EXPECT_TRUE(timeout_occurred);
    EXPECT_FALSE(operation_completed);
}

// =============================================================================
// ERROR HANDLING PATTERNS
// =============================================================================

class CoroutineErrorPatterns : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Circuit breaker pattern
 * @brief Stop after too many failures
 */
TEST_F(CoroutineErrorPatterns, CircuitBreaker) {
    constexpr int failure_threshold = 3;
    std::atomic<int> failures{0};
    std::atomic<int> attempts{0};
    std::atomic<bool> circuit_open{false};
    
    auto flaky_operation = [&attempts]() -> task<bool> {
        attempts.fetch_add(1);
        co_await sleep(10ms);
        
        if (attempts < failure_threshold + 1) {
            throw std::runtime_error("failure");
        }
        
        co_return true;
    };
    auto failures_ptr = &failures;
    auto circuit_open_ptr = &circuit_open;
    auto coro_fn = [&flaky_operation, failures_ptr, circuit_open_ptr]() -> task<void> {
        for (int i = 0; i < 5; ++i) {
            if (failures_ptr->load() >= failure_threshold) {
                circuit_open_ptr->store(true);
                break;
            }
            
            try {
                co_await flaky_operation();
            } catch (...) {
                failures_ptr->fetch_add(1);
            }
        }
        co_return;
    };
    auto caller = coro_fn();
    
    coro_scheduler().spawn(std::move(caller));
    run_for(200ms);
    
    EXPECT_TRUE(circuit_open);
    EXPECT_EQ(failures, failure_threshold);
}

/**
 * @test Fallback pattern
 * @brief Use fallback when primary fails
 */
TEST_F(CoroutineErrorPatterns, FallbackPattern) {
    std::atomic<bool> used_fallback{false};
    std::atomic<int> result{0};
    
    auto primary = []() -> task<int> {
        co_await sleep(10ms);
        throw std::runtime_error("primary failed");
        co_return 1;
    };
    
    auto fallback = [&used_fallback]() -> task<int> {
        co_await sleep(5ms);
        used_fallback = true;
        co_return 42;
    };
    
    auto result_ptr = &result;
    auto coro_fn = [result_ptr, &primary, &fallback]() -> task<void> {
        bool use_fallback = false;
        try {
            result_ptr->store(co_await primary());
        } catch (...) {
            use_fallback = true;
        }
        
        if (use_fallback) {
            result_ptr->store(co_await fallback());
        }
        co_return;
    };
    auto caller = coro_fn();
    
    coro_scheduler().spawn(std::move(caller));
    run_for(50ms);
    
    EXPECT_TRUE(used_fallback);
    EXPECT_EQ(result, 42);
}

// =============================================================================
// MAIN
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
