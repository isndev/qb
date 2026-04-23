/**
 * @file test_coroutine_edge_cases.cpp
 * @brief Edge case and stress tests for coroutines
 *
 * Tests for unusual but valid coroutine usage patterns:
 * - Deeply nested coroutines
 * - Rapid spawn/destroy cycles
 * - Coroutines with varying lifetimes
 * - Error recovery patterns
 *
 * @author qb - C++ Actor Framework
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>
#include <vector>
#include <random>
#include <limits>
#include <set>
#include <mutex>

using namespace qb::io::async;
using namespace std::chrono_literals;

template <typename Predicate>
bool wait_until(Predicate &&predicate, std::chrono::milliseconds timeout,
                std::chrono::milliseconds step = 10ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        qb::io::async::run_for(step);
        if (std::chrono::steady_clock::now() >= deadline) {
            return predicate();
        }
    }
    return true;
}

// =============================================================================
// TEST SUITE: Deep Nesting
// =============================================================================

class CoroutineDeepNesting : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Deep coroutine nesting
 * @brief Test many levels of nested await
 * 
 * NOTE: Disabled due to stack depth limitations
 */
TEST_F(CoroutineDeepNesting, VeryDeepNesting) {
    constexpr int depth = 50;
    std::atomic<int> result{0};
    
    std::function<task<int>(int)> nested_coro = [&](int level) -> task<int> {
        if (level <= 0) {
            co_return 1;
        }
        co_await sleep(1ms);
        auto inner = co_await nested_coro(level - 1);
        co_return inner + 1;
    };
    
    auto result_ptr = &result;
    auto coro_fn = [result_ptr, &nested_coro]() -> task<void> {
        result_ptr->store(co_await nested_coro(depth));
        co_return;
    };
    auto starter = coro_fn();
    
    coro_scheduler().spawn(std::move(starter));
    EXPECT_TRUE(wait_until([&result]() { return result.load() == depth + 1; }, 1500ms));
    
    EXPECT_EQ(result, depth + 1);
}

/**
 * @test Mutual recursion
 * @brief Test mutually recursive coroutines
 */
TEST_F(CoroutineDeepNesting, MutualRecursion) {
    std::atomic<int> counter{0};
    constexpr int max_count = 10;
    
    std::function<task<void>(bool)> coro_a;
    std::function<task<void>(bool)> coro_b;
    
    coro_a = [&](bool first) -> task<void> {
        counter.fetch_add(1);
        if (counter < max_count) {
            co_await sleep(1ms);
            co_await coro_b(false);
        }
        co_return;
    };
    
    coro_b = [&](bool first) -> task<void> {
        counter.fetch_add(1);
        if (counter < max_count) {
            co_await sleep(1ms);
            co_await coro_a(false);
        }
        co_return;
    };
    
    coro_scheduler().spawn(coro_a(true));
    EXPECT_TRUE(wait_until([&counter]() { return counter.load() >= max_count; }, 500ms));
    
    EXPECT_GE(counter, max_count);
}

// =============================================================================
// TEST SUITE: Rapid Lifecycle Operations
// =============================================================================

class CoroutineRapidLifecycle : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Rapid spawn without waiting
 * @brief Spawn and immediately destroy many coroutines
 */
TEST_F(CoroutineRapidLifecycle, RapidSpawnAndDestroy) {
    constexpr int count = 100;
    
    // Spawn coroutines but don't run them
    for (int i = 0; i < count; ++i) {
        auto t = []() -> task<void> {
            co_await sleep(100ms);  // Long sleep - won't complete
            co_return;
        }();
        // Spawn and immediately let task go out of scope
        // Scheduler owns the handle now
        coro_scheduler().spawn(std::move(t));
    }
    
    // Run briefly - not enough time for any to complete
    run_for(10ms);
    
    // Cleanup should not crash even with pending coroutines
}

/**
 * @test Rapid scheduler clear
 * @brief Clear scheduler with many pending coroutines
 */
TEST_F(CoroutineRapidLifecycle, RapidSchedulerClear) {
    constexpr int count = 50;
    
    for (int cycle = 0; cycle < 3; ++cycle) {
        // Spawn many coroutines
        for (int i = 0; i < count; ++i) {
            auto t = []() -> task<void> {
                co_await sleep(100ms);
                co_return;
            }();
            coro_scheduler().spawn(std::move(t));
        }
        
        // Clear without running
        qb::io::async::listener::current.clear();
        
        // Reset for next cycle
        qb::io::async::init();
    }
    
    // Should not crash
    SUCCEED();
}

// =============================================================================
// TEST SUITE: Timing Edge Cases
// =============================================================================

class CoroutineTimingEdgeCases : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Very short timers
 * @brief Test with sub-millisecond timers
 */
TEST_F(CoroutineTimingEdgeCases, VeryShortTimers) {
    std::atomic<int> completed{0};
    
    for (int i = 0; i < 10; ++i) {
        auto completed_ptr = &completed;
        auto coro_fn = [completed_ptr]() -> task<void> {
            co_await sleep(1ms);  // Minimum practical sleep
            completed_ptr->fetch_add(1);
            co_return;
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }
    
    run_for(50ms);
    
    EXPECT_EQ(completed, 10);
}

/**
 * @test Timer ordering
 * @brief Verify timers with different durations complete in order
 * 
 * NOTE: Disabled - timing-sensitive, can fail on loaded systems
 */
TEST_F(CoroutineTimingEdgeCases, DISABLED_TimerOrdering) {
    constexpr int count = 10;
    std::vector<int> completion_order;
    std::mutex mutex;
    
    for (int i = 0; i < count; ++i) {
        auto completion_order_ptr = &completion_order;
        auto mutex_ptr = &mutex;
        auto coro_fn = [i, completion_order_ptr, mutex_ptr]() -> task<void> {
            // Sleep proportional to index (but reverse order)
            co_await sleep(std::chrono::milliseconds((count - i) * 10));
            std::lock_guard<std::mutex> lock(*mutex_ptr);
            completion_order_ptr->push_back(i);
            co_return;
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }
    
    run_for(500ms);
    
    EXPECT_EQ(completion_order.size(), count);

    // Should complete in reverse order (shortest timer first)
    // Verify all IDs present and unique
    std::set<int> unique_ids(completion_order.begin(), completion_order.end());
    EXPECT_EQ(unique_ids.size(), count);
    
    // First completion should be highest index (shortest timer)
    EXPECT_EQ(completion_order.front(), count - 1);
    
    // Last completion should be lowest index (longest timer)
    EXPECT_EQ(completion_order.back(), 0);
}

// =============================================================================
// TEST SUITE: Resource Exhaustion
// =============================================================================

class CoroutineResourceExhaustion : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Many concurrent timers
 * @brief Test with large number of concurrent timers
 */
TEST_F(CoroutineResourceExhaustion, ManyConcurrentTimers) {
    constexpr int count = 500;
    std::atomic<int> completed{0};
    
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < count; ++i) {
        auto completed_ptr = &completed;
        auto coro_fn = [completed_ptr]() -> task<void> {
            co_await sleep(50ms);
            completed_ptr->fetch_add(1);
            co_return;
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }
    
    run_for(200ms);
    
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    EXPECT_EQ(completed, count);
    EXPECT_LT(elapsed, 300);  // Should complete within 300ms
}

/**
 * @test Large data in coroutine
 * @brief Test with coroutines holding large amounts of data
 */
TEST_F(CoroutineResourceExhaustion, LargeDataInCoroutine) {
    constexpr int coro_count = 10;
    constexpr int data_size = 1024 * 1024;  // 1MB per coroutine
    std::atomic<int> completed{0};
    
    for (int i = 0; i < coro_count; ++i) {
        auto completed_ptr = &completed;
        auto coro_fn = [completed_ptr, data_size]() -> task<void> {
            // Allocate large data on heap
            auto data = std::make_unique<std::vector<char>>(data_size);
            // Touch all pages
            for (size_t j = 0; j < data->size(); j += 4096) {
                (*data)[j] = static_cast<char>(j % 256);
            }
            
            co_await sleep(10ms);
            
            // Data automatically freed when coroutine completes
            completed_ptr->fetch_add(1);
            co_return;
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }
    
    run_for(500ms);
    
    EXPECT_EQ(completed, coro_count);
}

// =============================================================================
// TEST SUITE: Error Recovery
// =============================================================================

class CoroutineErrorRecovery : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Exception in middle of chain
 * @brief Verify chain recovers when middle coroutine throws
 */
TEST_F(CoroutineErrorRecovery, ExceptionInMiddleOfChain) {
    std::atomic<bool> before_threw{false};
    std::atomic<bool> after_completed{false};
    std::atomic<bool> catcher_saw_error{false};
    
    auto before = [&before_threw]() -> task<void> {
        before_threw = true;
        co_return;
    };
    
    auto throwing = []() -> task<void> {
        co_await sleep(10ms);
        throw std::runtime_error("middle error");
        co_return;
    };
    
    auto after = [&after_completed]() -> task<void> {
        after_completed = true;
        co_return;
    };
    
    auto catcher_saw_error_ptr = &catcher_saw_error;
    auto coro_fn = [catcher_saw_error_ptr, &before, &throwing, &after]() -> task<void> {
        bool error_occurred = false;
        try {
            co_await before();
            co_await throwing();
        } catch (const std::runtime_error& e) {
            error_occurred = true;
        }
        
        if (error_occurred) {
            catcher_saw_error_ptr->store(true);
            co_await after();
        }
        co_return;
    };
    auto catcher = coro_fn();
    
    coro_scheduler().spawn(std::move(catcher));
    run_for(50ms);
    
    EXPECT_TRUE(before_threw);
    EXPECT_TRUE(catcher_saw_error);
    EXPECT_TRUE(after_completed);
}

/**
 * @test Retry pattern
 * @brief Verify retry logic works correctly
 */
TEST_F(CoroutineErrorRecovery, RetryPattern) {
    constexpr int max_attempts = 3;
    std::atomic<int> attempts{0};
    std::atomic<bool> succeeded{false};
    
    auto flaky_operation = [&attempts]() -> task<bool> {
        attempts.fetch_add(1);
        if (attempts < max_attempts) {
            throw std::runtime_error("temporary failure");
        }
        co_return true;
    };
    
    auto succeeded_ptr = &succeeded;
    auto coro_fn = [succeeded_ptr, &flaky_operation]() -> task<void> {
        bool should_retry = false;
        for (int i = 0; i < max_attempts; ++i) {
            if (should_retry) {
                co_await sleep(5ms);  // Wait before retry
            }
            
            try {
                if (co_await flaky_operation()) {
                    succeeded_ptr->store(true);
                    co_return;
                }
            } catch (...) {
                if (i == max_attempts - 1) {
                    throw;  // Final attempt failed
                }
                should_retry = true;
            }
        }
        co_return;
    };
    auto retry_wrapper = coro_fn();
    
    coro_scheduler().spawn(std::move(retry_wrapper));
    run_for(100ms);
    
    EXPECT_TRUE(succeeded);
    EXPECT_EQ(attempts, max_attempts);
}

// =============================================================================
// TEST SUITE: Random Behavior
// =============================================================================

class CoroutineRandomBehavior : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Random sleep durations
 * @brief Test with random sleep times
 */
TEST_F(CoroutineRandomBehavior, RandomSleepDurations) {
    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<int> dist(1, 50);
    
    constexpr int count = 20;
    std::atomic<int> completed{0};
    
    for (int i = 0; i < count; ++i) {
        int sleep_ms = dist(rng);
        auto completed_ptr = &completed;
        auto coro_fn = [completed_ptr, sleep_ms]() -> task<void> {
            co_await sleep(std::chrono::milliseconds(sleep_ms));
            completed_ptr->fetch_add(1);
            co_return;
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }
    
    run_for(200ms);
    
    EXPECT_EQ(completed, count);
}

/**
 * @test Random spawn pattern
 * @brief Test with bursty spawn patterns
 */
TEST_F(CoroutineRandomBehavior, BurstySpawnPattern) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> burst_dist(1, 20);
    
    std::atomic<int> total_spawned{0};
    std::atomic<int> completed{0};
    
    // Three bursts
    for (int burst = 0; burst < 3; ++burst) {
        int count = burst_dist(rng);
        for (int i = 0; i < count; ++i) {
            auto completed_ptr = &completed;
            auto coro_fn = [completed_ptr]() -> task<void> {
                co_await sleep(10ms);
                completed_ptr->fetch_add(1);
                co_return;
            };
            auto t = coro_fn();
            coro_scheduler().spawn(std::move(t));
            total_spawned.fetch_add(1);
        }
        
        // Small delay between bursts
        run_for(5ms);
    }
    
    run_for(200ms);
    
    EXPECT_EQ(completed, total_spawned);
}

// =============================================================================
// MAIN
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
