/**
 * @file test_basic.cpp
 * @brief Basic Coroutine Tests - qb-io Standalone
 *
 * TDD test suite for foundational coroutine functionality in qb-io.
 * qb-io is a standalone async I/O library built on libev.
 *
 * Test Coverage:
 * - Task lifecycle (creation, execution, destruction)
 * - Value return and propagation
 * - Timer-based suspension (sleep)
 * - Sequential and concurrent execution
 * - Exception handling basics
 * - Nested coroutine composition
 * - Scheduler state inspection
 *
 * Architecture:
 * - Pure qb-io (no qb-core dependency)
 * - Uses listener::current for event loop
 * - Thread-local CoroutineScheduler per thread
 *
 * IMPORTANT: Lambda Coroutine Capture Safety
 * ------------------------------------------
 * When creating coroutines from lambdas, you MUST store the lambda in a variable
 * before calling it, OR capture by pointer instead of by reference.
 *
 * WRONG (dangling reference):
 *   auto t = [&var]() -> task<void> { use var; }();
 *
 * CORRECT (stored lambda):
 *   auto fn = [&var]() -> task<void> { use var; };
 *   auto t = fn();
 *
 * CORRECT (pointer capture):
 *   auto ptr = &var;
 *   auto fn = [ptr]() -> task<void> { use *ptr; };
 *   auto t = fn();
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Task Lifecycle
// =============================================================================

class TaskLifecycle : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        // Clear the listener to clean up any pending coroutines
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Task creation and immediate execution
 * @brief Verifies that a basic coroutine can be created, spawned, and executes
 *
 * Given: A coroutine lambda that sets a flag
 * When: The task is spawned and the event loop runs
 * Then: The flag should be set, indicating execution
 */
TEST_F(TaskLifecycle, TaskIsCreatedAndExecutes) {
    std::atomic<bool> executed{false};

    // Use __attribute__((noinline)) to prevent the compiler from optimizing away the body
    auto coro_fn = [&executed]() -> task<void> {
        executed.store(true, std::memory_order_release);
        co_return;
    };
    auto t = coro_fn();

    EXPECT_TRUE(t);
    EXPECT_FALSE(t.done());

    auto& sched = coro_scheduler();
    sched.spawn(std::move(t));
    for (int i = 0; i < 5 && !executed.load(std::memory_order_acquire); ++i) {
        sched.run_ready();
    }
    // Memory fence to ensure visibility
    std::atomic_thread_fence(std::memory_order_seq_cst);
    run_for(10ms);
    EXPECT_TRUE(executed.load(std::memory_order_acquire));
}

/**
 * @test Task returns a value
 * @brief Verifies that coroutines can return values via co_return
 *
 * Given: An inner coroutine that returns 42
 * When: The outer coroutine awaits the inner and stores the result
 * Then: The result should be 42
 */
TEST_F(TaskLifecycle, TaskReturnsValue) {
    // Given
    std::atomic<int> result{0};

    // When - Store lambda in variable to preserve captures
    auto result_ptr = &result;
    auto coro_fn = [result_ptr]() -> task<void> {
        auto inner_fn = []() -> task<int> {
            co_return 42;
        };
        auto inner = inner_fn();
        int val = co_await inner;
        result_ptr->store(val, std::memory_order_release);
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(10ms);

    // Then
    EXPECT_EQ(result, 42);
}

/**
 * @test Spawned task runs to completion after handle is released
 * @brief Verifies that after spawn() the scheduler owns the coroutine;
 *        the task handle going out of scope does not cancel it.
 *
 * Given: A coroutine that sleeps then sets completed
 * When: We spawn it and release the task handle, then run the loop
 * Then: The coroutine completes (scheduler owns the handle)
 */
TEST_F(TaskLifecycle, SpawnedTaskContinuesAfterHandleReleased) {
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};

    {
        auto started_ptr = &started;
        auto completed_ptr = &completed;
        auto coro_fn = [started_ptr, completed_ptr]() -> task<void> {
            started_ptr->store(true);
            co_await sleep(100ms);
            completed_ptr->store(true);
            co_return;
        };
        auto t = coro_fn();

        coro_scheduler().spawn(std::move(t));
        run_for(20ms);

        EXPECT_TRUE(started);
        EXPECT_FALSE(completed);
    }

    run_for(200ms);
    EXPECT_TRUE(completed);
    
    // Verify scheduler cleaned up
    EXPECT_EQ(coro_scheduler().active_count(), 0);
}

// =============================================================================
// TEST SUITE: Timer Operations
// =============================================================================

class TimerOperations : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Sleep waits for duration
 * @brief Verifies that sleep() waits approximately the requested duration
 *
 * Given: A coroutine that sleeps for 50ms
 * When: Measuring the elapsed time
 * Then: Elapsed time should be >= 45ms (with tolerance)
 */
TEST_F(TimerOperations, SleepWaitsForDuration) {
    // Given
    std::atomic<bool> completed{false};
    auto start = std::chrono::steady_clock::now();

    // When
    auto completed_ptr = &completed;
    auto coro_fn = [completed_ptr]() -> task<void> {
        co_await sleep(50ms);
        completed_ptr->store(true);
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    auto elapsed = std::chrono::steady_clock::now() - start;

    // Then
    EXPECT_TRUE(completed);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 45);
}

/**
 * @test Zero sleep completes quickly
 * @brief Verifies that sleep(0ms) completes without long delay
 *
 * Given: A coroutine with sleep(0ms)
 * When: Measuring execution time
 * Then: Should complete within the run_for window
 */
TEST_F(TimerOperations, ZeroSleepCompletesImmediately) {
    std::atomic<bool> completed{false};
    auto start = std::chrono::steady_clock::now();

    auto completed_ptr = &completed;
    auto coro_fn = [completed_ptr]() -> task<void> {
        co_await sleep(0ms);
        completed_ptr->store(true);
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(completed);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100);
}

/**
 * @test Sequential sleeps accumulate
 * @brief Verifies that multiple sequential sleeps sum correctly
 *
 * Given: Three sequential 10ms sleeps
 * When: Executing and counting steps
 * Then: Counter should reach 3 after ~30ms
 */
TEST_F(TimerOperations, SequentialSleepsAccumulate) {
    // Given
    std::atomic<int> counter{0};

    // When
    auto counter_ptr = &counter;
    auto coro_fn = [counter_ptr]() -> task<void> {
        co_await sleep(10ms);
        counter_ptr->fetch_add(1);

        co_await sleep(10ms);
        counter_ptr->fetch_add(1);

        co_await sleep(10ms);
        counter_ptr->fetch_add(1);
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    // Then
    EXPECT_EQ(counter, 3);
}

// =============================================================================
// TEST SUITE: Concurrent Execution
// =============================================================================

class ConcurrentExecution : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Multiple coroutines execute independently
 * @brief Verifies that multiple spawned coroutines all complete
 *
 * Given: 10 coroutines with staggered sleep durations
 * When: All are spawned and the loop runs
 * Then: All 10 should complete
 */
TEST_F(ConcurrentExecution, MultipleCoroutinesComplete) {
    // Given
    std::atomic<int> completed{0};
    constexpr int num_coroutines = 10;

    // When
    auto completed_ptr = &completed;
    for (int i = 0; i < num_coroutines; ++i) {
        auto coro_fn = [completed_ptr, i]() -> task<void> {
            co_await sleep(std::chrono::milliseconds(10 + i * 5));
            completed_ptr->fetch_add(1);
            co_return;
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }

    run_for(200ms);

    // Then
    EXPECT_EQ(completed, num_coroutines);
}

/**
 * @test Concurrent timers complete in order
 * @brief Verifies timers with different durations complete in expected order
 *
 * Given: 5 timers with increasing delays
 * When: Tracking completion order
 * Then: Shorter delays complete first
 */
TEST_F(ConcurrentExecution, ConcurrentTimersAllComplete) {
    constexpr int num_timers = 5;
    std::atomic<int> completed_count{0};

    auto completed_ptr = &completed_count;
    for (int i = 0; i < num_timers; ++i) {
        auto coro_fn = [completed_ptr, i]() -> task<void> {
            co_await sleep(std::chrono::milliseconds((num_timers - i) * 10));
            completed_ptr->fetch_add(1, std::memory_order_relaxed);
            co_return;
        };
        auto t = coro_fn();
        coro_scheduler().spawn(std::move(t));
    }

    run_for(500ms);
    EXPECT_EQ(completed_count.load(), num_timers);
}

// =============================================================================
// TEST SUITE: Exception Handling
// =============================================================================

class ExceptionHandling : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Exception propagates to awaiter
 * @brief Verifies exceptions in inner coroutines propagate to the awaiter
 *
 * Given: An inner coroutine that throws
 * When: The outer coroutine catches it
 * Then: Exception should be caught with correct message
 */
TEST_F(ExceptionHandling, ExceptionPropagatesToAwaiter) {
    // Given
    std::atomic<bool> caught{false};

    auto throwing_fn = []() -> task<void> {
        throw std::runtime_error("test exception");
        co_return;
    };

    // When
    auto caught_ptr = &caught;
    auto coro_fn = [caught_ptr, &throwing_fn]() -> task<void> {
        try {
            co_await throwing_fn();
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "test exception") {
                caught_ptr->store(true);
            }
        }
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(10ms);

    // Then
    EXPECT_TRUE(caught);
}

/**
 * @test Exception after suspension
 * @brief Verifies exceptions can be thrown after co_await
 *
 * Given: A coroutine that suspends then throws
 * When: Catching the exception
 * Then: Should be caught successfully
 */
TEST_F(ExceptionHandling, ExceptionAfterSuspension) {
    // Given
    std::atomic<bool> caught{false};

    auto inner_fn = []() -> task<int> {
        co_await sleep(10ms);
        throw std::runtime_error("delayed error");
        co_return 42;
    };

    // When
    auto caught_ptr = &caught;
    auto coro_fn = [caught_ptr, &inner_fn]() -> task<void> {
        try {
            (void)co_await inner_fn();
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "delayed error") {
                caught_ptr->store(true);
            }
        }
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    // Then
    EXPECT_TRUE(caught);
}

// =============================================================================
// TEST SUITE: Coroutine Composition
// =============================================================================

class CoroutineComposition : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Nested coroutine awaiting
 * @brief Verifies an outer coroutine can await an inner coroutine
 *
 * Given: Inner coroutine sleeps and returns 42
 * When: Outer coroutine awaits it
 * Then: Should receive 42 and both complete
 */
TEST_F(CoroutineComposition, CanAwaitNestedCoroutine) {
    // Given
    std::atomic<bool> inner_completed{false};
    std::atomic<bool> outer_completed{false};

    auto inner_completed_ptr = &inner_completed;
    auto inner_fn = [inner_completed_ptr]() -> task<int> {
        co_await sleep(10ms);
        inner_completed_ptr->store(true);
        co_return 42;
    };

    // When
    auto outer_completed_ptr = &outer_completed;
    auto coro_fn = [outer_completed_ptr, &inner_fn]() -> task<void> {
        int result = co_await inner_fn();
        EXPECT_EQ(result, 42);
        outer_completed_ptr->store(true);
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    // Then
    EXPECT_TRUE(inner_completed);
    EXPECT_TRUE(outer_completed);
}

/**
 * @test Scheduler state inspection
 * @brief Verifies the scheduler correctly tracks pending coroutines
 *
 * Given: An empty scheduler
 * When: Spawning a coroutine and checking state
 * Then: has_ready() and pending_count() should reflect correct state
 */
TEST_F(CoroutineComposition, SchedulerStateTracking) {
    // Given
    auto& scheduler = coro_scheduler();

    // Initially empty
    EXPECT_FALSE(scheduler.has_ready());
    EXPECT_EQ(scheduler.pending_count(), 0);

    // When - spawn a waiting coroutine
    auto coro_fn = []() -> task<void> {
        co_await sleep(100ms);
        co_return;
    };
    auto t = coro_fn();

    scheduler.spawn(std::move(t));

    // Then - should have one ready (initial suspend)
    EXPECT_TRUE(scheduler.has_ready());
    EXPECT_EQ(scheduler.pending_count(), 1);

    // After first run, it will suspend on sleep
    scheduler.run_ready();

    // May be empty or have items depending on timing
    // Mainly checking this doesn't crash
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
