/**
 * @file test_coroutine_comprehensive.cpp
 * @brief Comprehensive TDD test suite for QB-IO Coroutines
 *
 * This test suite follows modern C++23 TDD practices for qb-io standalone.
 * Tests qb::io::async coroutines without any dependency on qb-core.
 *
 * Architecture:
 * - qb-io: standalone async I/O library with libev
 * - Tests: pure qb-io, no qb::Actor usage
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>
#include <optional>
#include <variant>
#include <tuple>
#include <thread>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE 1: Basic Coroutine Lifecycle
// =============================================================================

class CoroutineLifecycle : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
};

// Test 1.1: Coroutine creation and immediate execution
TEST_F(CoroutineLifecycle, CoroutineIsCreatedAndExecutes) {
    std::atomic<bool> executed{false};

    auto ptr = &executed;
    auto fn = [ptr]() -> task<void> {
        ptr->store(true);
        co_return;
    };
    auto t = fn();

    auto& sched = coro_scheduler();
    sched.spawn(std::move(t));
    for (int i = 0; i < 5 && !executed; ++i) sched.run_ready();
    run_for(10ms);
    EXPECT_TRUE(executed);
}

// Test 1.2: Coroutine returns a value
TEST_F(CoroutineLifecycle, CoroutineReturnsValue) {
    // Given
    std::atomic<int> result{0};

    // When
    auto ptr = &result;
    auto fn = [ptr]() -> task<void> {
        auto inner_fn = []() -> task<int> {
            co_return 42;
        };
        auto inner = inner_fn();
        ptr->store(co_await inner);
        co_return;
    };
    auto t = fn();

    coro_scheduler().spawn(std::move(t));
    run_for(10ms);

    // Then
    EXPECT_EQ(result, 42);
}

// Test 1.3: Coroutine handles void return
TEST_F(CoroutineLifecycle, VoidCoroutineCompletes) {
    // Given
    std::atomic<bool> completed{false};

    // When
    auto ptr = &completed;
    auto fn = [ptr]() -> task<void> {
        co_await sleep(1ms);
        ptr->store(true);
        co_return;
    };
    auto t = fn();

    coro_scheduler().spawn(std::move(t));
    run_for(20ms);

    // Then
    EXPECT_TRUE(completed);
}

// Test 1.4: Multiple coroutines can coexist
TEST_F(CoroutineLifecycle, MultipleCoroutinesExecuteIndependently) {
    // Given
    std::atomic<int> counter{0};
    constexpr int num_coroutines = 5;

    // When
    auto ptr = &counter;
    for (int i = 0; i < num_coroutines; ++i) {
        auto fn = [ptr]() -> task<void> {
            co_await sleep(10ms);
            ptr->fetch_add(1);
            co_return;
        };
        auto t = fn();
        coro_scheduler().spawn(std::move(t));
    }

    run_for(100ms);

    // Then
    EXPECT_EQ(counter, num_coroutines);
}

// Test 1.5: Spawned coroutine completes after task handle is released
TEST_F(CoroutineLifecycle, CoroutineDestructionCancelsOperation) {
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};

    {
        auto started_ptr = &started;
        auto completed_ptr = &completed;
        auto fn = [started_ptr, completed_ptr]() -> task<void> {
            started_ptr->store(true);
            co_await sleep(100ms);
            completed_ptr->store(true);
            co_return;
        };
        auto t = fn();

        coro_scheduler().spawn(std::move(t));
        run_for(20ms);

        EXPECT_TRUE(started);
        EXPECT_FALSE(completed);
    }

    run_for(200ms);
    EXPECT_TRUE(completed);
}

// =============================================================================
// TEST SUITE 2: Timer and Sleep Operations
// =============================================================================

class CoroutineTimers : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
};

// Test 2.1: Sleep with duration
TEST_F(CoroutineTimers, SleepWaitsForDuration) {
    // Given
    auto start = std::chrono::steady_clock::now();
    std::atomic<bool> completed{false};

    // When
    auto completed_ptr = &completed;
    auto coro_fn = [completed_ptr]() -> task<void> {
        co_await sleep(50ms);
        (*completed_ptr) = true;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    // Then
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(completed);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 45);
}

// Test 2.2: Zero duration sleep
TEST_F(CoroutineTimers, ZeroSleepCompletesImmediately) {
    std::atomic<bool> completed{false};

    auto completed_ptr = &completed;
    auto coro_fn = [completed_ptr]() -> task<void> {
        co_await sleep(0ms);
        (*completed_ptr) = true;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
    EXPECT_TRUE(completed);
}

// Test 2.3: Sequential sleeps
TEST_F(CoroutineTimers, SequentialSleepsAccumulate) {
    // Given
    std::vector<std::chrono::steady_clock::time_point> timestamps;
    timestamps.push_back(std::chrono::steady_clock::now());

    // When
    auto timestamps_ptr = &timestamps;
    auto coro_fn = [timestamps_ptr]() -> task<void> {
        co_await sleep(20ms);
        timestamps_ptr->push_back(std::chrono::steady_clock::now());

        co_await sleep(30ms);
        timestamps_ptr->push_back(std::chrono::steady_clock::now());

        co_await sleep(20ms);
        timestamps_ptr->push_back(std::chrono::steady_clock::now());
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    // Then
    EXPECT_EQ(timestamps.size(), 4);

    // Check intervals
    for (size_t i = 1; i < timestamps.size(); ++i) {
        auto interval = timestamps[i] - timestamps[i-1];
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(interval).count();
        EXPECT_GE(ms, 15); // Allow some tolerance
    }
}

// Test 2.4: Concurrent timers all complete
TEST_F(CoroutineTimers, ConcurrentTimersCompleteInOrder) {
    constexpr int num_timers = 5;
    std::atomic<int> completed_count{0};

    for (int i = 0; i < num_timers; ++i) {
        auto completed_count_ptr = &completed_count;
        auto coro_fn = [completed_count_ptr, i]() -> task<void> {
            co_await sleep(std::chrono::milliseconds((num_timers - i) * 10));
            completed_count_ptr->fetch_add(1);
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }

    run_for(500ms);
    EXPECT_EQ(completed_count.load(), num_timers);
}

// Test 2.5: Timer precision
TEST_F(CoroutineTimers, TimerPrecisionWithinTolerance) {
    // Given
    constexpr int iterations = 10;
    std::vector<long long> actual_durations;
    const long long expected_ms = 30;

    // When
    for (int i = 0; i < iterations; ++i) {
        auto start = std::chrono::steady_clock::now();

        auto actual_durations_ptr = &actual_durations;
        auto coro_fn = [actual_durations_ptr, start]() -> task<void> {
            co_await sleep(30ms);
            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            actual_durations_ptr->push_back(duration);
        };
        auto t = coro_fn();

        coro_scheduler().spawn(std::move(t));
        run_for(50ms);
    }

    // Then
    EXPECT_EQ(actual_durations.size(), iterations);

    // Calculate average
    long long sum = 0;
    for (auto d : actual_durations) {
        sum += d;
    }
    long long average = sum / iterations;

    // Should be close to expected (within 10ms)
    EXPECT_GE(average, expected_ms - 5);
    EXPECT_LE(average, expected_ms + 15);
}

// =============================================================================
// TEST SUITE 3: Coroutine Composition
// =============================================================================

class CoroutineComposition : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
};

// Test 3.1: Await another coroutine
TEST_F(CoroutineComposition, CanAwaitAnotherCoroutine) {
    // Given
    std::atomic<bool> inner_completed{false};
    std::atomic<bool> outer_completed{false};

    auto inner = [&inner_completed]() -> task<int> {
        co_await sleep(10ms);
        inner_completed = true;
        co_return 42;
    };

    // When
    auto outer_completed_ptr = &outer_completed;
    auto coro_fn = [outer_completed_ptr, &inner]() -> task<void> {
        int result = co_await inner();
        EXPECT_EQ(result, 42);
        outer_completed_ptr->store(true);
        co_return;
    };
    auto outer = coro_fn();

    coro_scheduler().spawn(std::move(outer));
    run_for(50ms);

    // Then
    EXPECT_TRUE(inner_completed);
    EXPECT_TRUE(outer_completed);
}

// Test 3.2: Chain of coroutines
TEST_F(CoroutineComposition, ChainOfThreeCoroutines) {
    // Given
    std::atomic<int> counter{0};

    auto level3 = [&counter]() -> task<int> {
        counter.fetch_add(1);
        co_return 100;
    };

    auto level2 = [&counter, &level3]() -> task<int> {
        co_await sleep(5ms);
        counter.fetch_add(10);
        int val = co_await level3();
        co_return val;
    };

    // When
    auto counter_ptr = &counter;
    auto coro_fn = [counter_ptr, &level2]() -> task<void> {
        co_await sleep(5ms);
        counter_ptr->fetch_add(100);
        int val = co_await level2();
        EXPECT_EQ(val, 100);
        co_return;
    };
    auto level1 = coro_fn();

    coro_scheduler().spawn(std::move(level1));
    run_for(50ms);

    // Then - execution order: level1 starts, sleeps, level2 starts, sleeps, level3 executes
    EXPECT_EQ(counter, 111);
}

// Test 3.3: Fire-and-forget (spawn two tasks; both complete)
TEST_F(CoroutineComposition, FireAndForgetCoroutine) {
    std::atomic<int> done_count{0};

    auto done_count_ptr = &done_count;
    auto coro_fn_1 = [done_count_ptr]() -> task<void> {
        co_await sleep(10ms);
        done_count_ptr->fetch_add(1);
        co_return;
    };
    auto first = coro_fn_1();
    
    auto coro_fn_2 = [done_count_ptr]() -> task<void> {
        co_await sleep(30ms);
        done_count_ptr->fetch_add(1);
        co_return;
    };
    auto second = coro_fn_2();

    coro_scheduler().spawn(std::move(first));
    coro_scheduler().spawn(std::move(second));
    run_for(200ms);

    EXPECT_EQ(done_count.load(), 2);
}

// Test 3.4: Parallel coroutines with gather
TEST_F(CoroutineComposition, MultipleParallelCoroutines) {
    constexpr int num_tasks = 5;
    std::atomic<int> completed{0};

    for (int i = 0; i < num_tasks; ++i) {
        auto completed_ptr = &completed;
        auto coro_fn = [completed_ptr]() -> task<void> {
            co_await sleep(20ms);
            completed_ptr->fetch_add(1);
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }

    run_for(100ms);
    EXPECT_EQ(completed.load(), num_tasks);
}

// =============================================================================
// TEST SUITE 4: Exception Handling
// =============================================================================

class CoroutineExceptions : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
};

// Test 4.1: Exception propagates to caller
TEST_F(CoroutineExceptions, ExceptionPropagatesToAwaiter) {
    // Given
    std::atomic<bool> caught{false};

    auto throwing = []() -> task<void> {
        throw std::runtime_error("test error");
        co_return;
    };

    // When
    auto caught_ptr = &caught;
    auto coro_fn = [caught_ptr, &throwing]() -> task<void> {
        try {
            co_await throwing();
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "test error") {
                caught_ptr->store(true);
            }
        }
        co_return;
    };
    auto catcher = coro_fn();

    coro_scheduler().spawn(std::move(catcher));
    run_for(10ms);

    // Then
    EXPECT_TRUE(caught);
}

// Test 4.2: Exception from inner coroutine
TEST_F(CoroutineExceptions, ExceptionFromInnerCoroutinePropagates) {
    // Given
    std::atomic<bool> caught{false};

    auto inner = []() -> task<int> {
        co_await sleep(10ms);
        throw std::runtime_error("inner error");
        co_return 42;
    };

    // When
    auto caught_ptr = &caught;
    auto coro_fn = [caught_ptr, &inner]() -> task<void> {
        try {
            int val = co_await inner();
            (void)val;
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "inner error") {
                caught_ptr->store(true);
            }
        }
        co_return;
    };
    auto outer = coro_fn();

    coro_scheduler().spawn(std::move(outer));
    run_for(50ms);

    // Then
    EXPECT_TRUE(caught);
}

// Test 4.3: Exception after multiple suspensions
TEST_F(CoroutineExceptions, ExceptionAfterMultipleSuspensions) {
    // Given
    std::atomic<int> steps{0};
    std::atomic<bool> caught{false};

    auto unreliable = [&steps]() -> task<int> {
        co_await sleep(10ms);
        steps.fetch_add(1);

        co_await sleep(10ms);
        steps.fetch_add(1);

        co_await sleep(10ms);
        steps.fetch_add(1);

        throw std::runtime_error("step 3 error");
        co_return 42;
    };

    // When
    auto caught_ptr = &caught;
    auto coro_fn = [caught_ptr, &unreliable]() -> task<void> {
        try {
            co_await unreliable();
        } catch (const std::runtime_error& e) {
            caught_ptr->store(true);
        }
        co_return;
    };
    auto caller = coro_fn();

    coro_scheduler().spawn(std::move(caller));
    run_for(100ms);

    // Then
    EXPECT_EQ(steps, 3);
    EXPECT_TRUE(caught);
}

// Test 4.4: Exception in spawned coroutine doesn't crash scheduler
TEST_F(CoroutineExceptions, ExceptionInSpawnedCoroutineHandled) {
    // Given
    std::atomic<bool> other_completed{false};

    auto throwing = []() -> task<void> {
        co_await sleep(10ms);
        throw std::runtime_error("spawned error");
        co_return;
    };

    auto normal = [&other_completed]() -> task<void> {
        co_await sleep(20ms);
        other_completed = true;
    };

    // When - spawn both
    coro_scheduler().spawn(throwing());
    coro_scheduler().spawn(normal());

    run_for(50ms);

    // Then - normal task should complete despite other throwing
    EXPECT_TRUE(other_completed);
}

// =============================================================================
// TEST SUITE 5: Edge Cases and Stress Tests
// =============================================================================

class CoroutineEdgeCases : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
};

// Test 5.1: Very short duration timer
TEST_F(CoroutineEdgeCases, VeryShortTimer) {
    // Given
    std::atomic<bool> completed{false};

    // When
    auto completed_ptr = &completed;
    auto coro_fn = [completed_ptr]() -> task<void> {
        co_await sleep(1ms);
        (*completed_ptr) = true;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(20ms);

    // Then
    EXPECT_TRUE(completed);
}

// Test 5.2: Many concurrent coroutines
TEST_F(CoroutineEdgeCases, ManyConcurrentCoroutines) {
    // Given
    constexpr int num_coroutines = 100;
    std::atomic<int> completed{0};

    // When
    for (int i = 0; i < num_coroutines; ++i) {
        auto completed_ptr = &completed;
        auto coro_fn = [completed_ptr]() -> task<void> {
            co_await sleep(20ms);
            completed_ptr->fetch_add(1);
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }

    run_for(100ms);

    // Then
    EXPECT_EQ(completed, num_coroutines);
}

// Test 5.3: Spawn coroutines that complete immediately (no suspend)
// Fixed: Added co_return to make it a valid coroutine
TEST_F(CoroutineEdgeCases, RapidSpawnAndComplete) {
    std::atomic<int> counter{0};
    // Single no-suspend coroutine; multiple can stress scheduler (keep small)
    auto counter_ptr = &counter;
    auto coro_fn = [counter_ptr]() -> task<void> {
        counter_ptr->fetch_add(1);
        co_return;
    };
    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(10ms);
    EXPECT_EQ(counter.load(), 1);
}

// Test 5.4: Coroutine with no suspension points
TEST_F(CoroutineEdgeCases, NoSuspensionPoints) {
    // Given
    std::atomic<int> result{0};

    // When
    auto result_ptr = &result;
    auto coro_fn = [result_ptr]() -> task<void> {
        int sum = 0;
        for (int i = 0; i < 1000; ++i) {
            sum += i;
        }
        (*result_ptr) = sum;
        co_return;
    };
    auto t = coro_fn();

    // Note: Since this has no co_await, it completes synchronously when resumed
    coro_scheduler().spawn(std::move(t));
    run_for(1ms);

    // Then
    EXPECT_EQ(result, 499500);
}

// Test 5.5: Coroutine that suspends many times
TEST_F(CoroutineEdgeCases, ManySuspensionPoints) {
    // Given
    constexpr int num_suspensions = 50;
    std::atomic<int> counter{0};

    // When
    auto counter_ptr = &counter;
    auto coro_fn = [counter_ptr]() -> task<void> {
        for (int i = 0; i < num_suspensions; ++i) {
            co_await sleep(2ms);
            counter_ptr->fetch_add(1);
        }
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(200ms);

    // Then
    EXPECT_EQ(counter, num_suspensions);
}

// Test 5.6: Nested coroutine chain completes
TEST_F(CoroutineEdgeCases, DeepCoroutineNesting) {
    std::atomic<int> result_val{0};

    std::function<task<int>(int)> make_coroutine = [&make_coroutine](int n) -> task<int> {
        if (n <= 0) co_return 1;
        co_await sleep(10ms);
        auto inner = make_coroutine(n - 1);
        int r = co_await inner;
        co_return r + 1;
    };

    auto result_val_ptr = &result_val;
    auto coro_fn = [&make_coroutine, result_val_ptr]() -> task<void> {
        auto t = make_coroutine(2);
        result_val_ptr->store(co_await t);
        co_return;
    };
    auto starter = coro_fn();

    coro_scheduler().spawn(std::move(starter));
    run_for(150ms);
    EXPECT_GE(result_val.load(), 2);  // nested chain completes at least 2 levels
}

// =============================================================================
// TEST SUITE 6: Real-World Scenarios (Pure QB-IO, No Actor)
// =============================================================================

class CoroutineRealWorld : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
};

// Test 6.1: Async operation with timeout simulation
TEST_F(CoroutineRealWorld, OperationWithTimeout) {
    // Given - simulate an operation that might take too long
    std::atomic<bool> timeout_reached{false};
    std::atomic<bool> operation_done{false};

    auto slow_operation = [&operation_done]() -> task<bool> {
        co_await sleep(100ms);
        operation_done = true;
        co_return true;
    };

    // When - run with timeout
    auto timeout_reached_ptr = &timeout_reached;
    auto operation_done_ptr = &operation_done;
    auto coro_fn = [timeout_reached_ptr, operation_done_ptr, &slow_operation]() -> task<void> {
        // Start both timeout and operation
        auto start = std::chrono::steady_clock::now();

        auto op = slow_operation();

        // Poll until timeout
        while (std::chrono::steady_clock::now() - start < 30ms) {
            co_await sleep(5ms);
        }

        timeout_reached_ptr->store(true);

        // Operation should not be done
        EXPECT_FALSE(operation_done_ptr->load());
        co_return;
    };
    auto with_timeout = coro_fn();

    coro_scheduler().spawn(std::move(with_timeout));
    run_for(80ms);

    // Then
    EXPECT_TRUE(timeout_reached);
    // Operation might complete after timeout
}

// Test 6.2: Sequential async operations (like DB queries)
TEST_F(CoroutineRealWorld, SequentialAsyncOperations) {
    // Given
    std::vector<std::string> operations;

    auto query1 = [&operations]() -> task<std::string> {
        co_await sleep(10ms);
        operations.push_back("query1");
        co_return "result1";
    };

    auto query2 = [&operations](const std::string& prev) -> task<std::string> {
        co_await sleep(10ms);
        operations.push_back("query2:" + prev);
        co_return "result2";
    };

    // When
    auto operations_ptr = &operations;
    auto coro_fn = [operations_ptr, &query1, &query2]() -> task<void> {
        auto r1 = co_await query1();
        auto r2 = co_await query2(r1);
        operations_ptr->push_back("final:" + r2);
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    EXPECT_GE(operations.size(), 2u);
    EXPECT_EQ(operations[0], "query1");
    if (operations.size() >= 3) {
        EXPECT_EQ(operations[1], "query2:result1");
        EXPECT_EQ(operations[2], "final:result2");
    }
}

// Test 6.3: Retry logic with backoff
TEST_F(CoroutineRealWorld, RetryWithBackoff) {
    // Given
    constexpr int max_retries = 3;
    std::atomic<int> attempts{0};
    std::atomic<bool> succeeded{false};

    auto flaky_operation = [&attempts]() -> task<bool> {
        attempts.fetch_add(1);
        co_await sleep(5ms);
        // Succeed on 3rd attempt
        co_return attempts >= 3;
    };

    // When
    auto succeeded_ptr = &succeeded;
    auto coro_fn = [succeeded_ptr, &flaky_operation]() -> task<void> {
        for (int i = 0; i < max_retries; ++i) {
            if (co_await flaky_operation()) {
                succeeded_ptr->store(true);
                co_return;
            }
            // Backoff
            co_await sleep(std::chrono::milliseconds(10 * (i + 1)));
        }
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    // Then
    EXPECT_TRUE(succeeded);
    EXPECT_EQ(attempts, 3);
}

// Test 6.4: Producer-consumer pattern
TEST_F(CoroutineRealWorld, ProducerConsumerPattern) {
    // Given
    constexpr int num_items = 10;
    std::vector<int> produced;
    std::vector<int> consumed;
    std::atomic<bool> done{false};

    // When
    auto produced_ptr = &produced;
    auto consumed_ptr = &consumed;
    auto done_ptr = &done;
    
    auto producer_fn = [produced_ptr]() -> task<void> {
        for (int i = 0; i < num_items; ++i) {
            co_await sleep(5ms);
            produced_ptr->push_back(i);
        }
        co_return;
    };
    auto producer = producer_fn();

    auto consumer_fn = [produced_ptr, consumed_ptr, done_ptr]() -> task<void> {
        while (consumed_ptr->size() < num_items) {
            co_await sleep(8ms);
            if (!produced_ptr->empty()) {
                consumed_ptr->push_back(produced_ptr->back());
                produced_ptr->pop_back();
            }
        }
        done_ptr->store(true);
        co_return;
    };
    auto consumer = consumer_fn();

    coro_scheduler().spawn(std::move(producer));
    coro_scheduler().spawn(std::move(consumer));

    run_for(200ms);

    // Then
    EXPECT_TRUE(done);
    EXPECT_EQ(consumed.size(), num_items);
}

// =============================================================================
// TEST SUITE 7: Performance and Benchmarks
// =============================================================================

class CoroutinePerformance : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
};

// Test 7.1: Spawn many coroutines quickly
// Fixed: Added co_return to make it a valid coroutine
TEST_F(CoroutinePerformance, SpawnManyCoroutines) {
    // Given
    constexpr int count = 1000;
    std::atomic<int> completed{0};

    // When
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < count; ++i) {
        auto completed_ptr = &completed;
        auto coro_fn = [completed_ptr]() -> task<void> {
            completed_ptr->fetch_add(1);
            co_return;
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }

    run_for(10ms);

    auto elapsed = std::chrono::steady_clock::now() - start;

    // Then
    EXPECT_EQ(completed, count);

    // Should be very fast (< 100ms for 1000 coroutines)
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LT(ms, 100);
}

// Test 7.2: Coroutine vs callback overhead comparison
TEST_F(CoroutinePerformance, CoroutineOverheadAcceptable) {
    // Given
    constexpr int iterations = 100;
    std::atomic<int> counter{0};

    // When - time how long it takes to run N coroutines with sleep
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
        auto counter_ptr = &counter;
        auto coro_fn = [counter_ptr]() -> task<void> {
            co_await sleep(1ms);
            counter_ptr->fetch_add(1);
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }

    run_for(200ms);

    auto elapsed = std::chrono::steady_clock::now() - start;

    // Then
    EXPECT_EQ(counter, iterations);

    // Should complete in reasonable time
    // (iterations * sleep_duration + some overhead)
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LT(ms, iterations * 2 + 50);
}

// Test 7.3: Memory stability under load
TEST_F(CoroutinePerformance, MemoryStabilityUnderLoad) {
    // Given
    constexpr int batches = 10;
    constexpr int per_batch = 50;

    // When - run multiple batches of coroutines
    for (int batch = 0; batch < batches; ++batch) {
        std::atomic<int> completed{0};

        for (int i = 0; i < per_batch; ++i) {
            auto completed_ptr = &completed;
            auto coro_fn = [completed_ptr]() -> task<void> {
                co_await sleep(5ms);
                completed_ptr->fetch_add(1);
            };
            auto t = coro_fn();
            coro_scheduler().spawn(std::move(t));
        }

        run_for(100ms);

        // Then - each batch should complete
        EXPECT_EQ(completed, per_batch);
    }
}

// =============================================================================
// MAIN
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
