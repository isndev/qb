/**
 * @file test_coroutine_combinators.cpp
 * @brief Coroutine combinators tests
 *
 * Tests for combining multiple coroutines:
 * - when_all: wait for all coroutines
 * - when_any: wait for first coroutine
 * - race: competition between coroutines
 * - select: choose from multiple operations
 *
 * @author qb - C++ Actor Framework
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>
#include <vector>
#include <tuple>
#include <set>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: when_all Pattern
// =============================================================================

class CoroutineWhenAll : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Manual when_all implementation
 * @brief Wait for multiple coroutines with different types
 */
TEST_F(CoroutineWhenAll, ManualWhenAllMixedTypes) {
    std::atomic<bool> task1_done{false};
    std::atomic<bool> task2_done{false};
    std::atomic<bool> all_done{false};
    
    auto task1 = [&task1_done]() -> task<int> {
        co_await sleep(30ms);
        task1_done = true;
        co_return 42;
    };
    
    auto task2 = [&task2_done]() -> task<std::string> {
        co_await sleep(20ms);
        task2_done = true;
        co_return "hello";
    };
    
    auto all_done_ptr = &all_done;
    auto coro_fn = [all_done_ptr, &task1, &task2]() -> task<void> {
        auto t1 = task1();
        auto t2 = task2();
        
        // Wait for both
        int r1 = co_await t1;
        std::string r2 = co_await t2;
        
        EXPECT_EQ(r1, 42);
        EXPECT_EQ(r2, "hello");
        
        (*all_done_ptr) = true;
        co_return;
    };
    auto coordinator = coro_fn();
    
    coro_scheduler().spawn(std::move(coordinator));
    run_for(100ms);
    
    EXPECT_TRUE(task1_done);
    EXPECT_TRUE(task2_done);
    EXPECT_TRUE(all_done);
}

/**
 * @test when_all with void tasks
 * @brief Wait for multiple void coroutines
 */
TEST_F(CoroutineWhenAll, WhenAllVoidTasks) {
    constexpr int count = 5;
    std::atomic<int> completed{0};
    std::atomic<bool> all_done{false};
    
    auto completed_ptr = &completed;
    auto all_done_ptr = &all_done;
    
    // Worker function outside the coroutine
    auto worker_fn = [completed_ptr](int delay_ms) -> task<void> {
        co_await sleep(std::chrono::milliseconds(delay_ms));
        completed_ptr->fetch_add(1);
        co_return;
    };
    
    auto coro_fn = [completed_ptr, all_done_ptr, &worker_fn]() -> task<void> {
        std::vector<task<void>> tasks;
        
        for (int i = 0; i < count; ++i) {
            tasks.push_back(worker_fn(10 + (i * 5)));
        }
        
        // Wait for all
        for (auto& t : tasks) {
            co_await t;
        }
        
        all_done_ptr->store(true);
        co_return;
    };
    auto coordinator = coro_fn();
    
    coro_scheduler().spawn(std::move(coordinator));
    run_for(200ms);
    
    EXPECT_EQ(completed, count);
    EXPECT_TRUE(all_done);
}

/**
 * @test when_all with early failure
 * @brief Verify failure handling in when_all
 */
TEST_F(CoroutineWhenAll, WhenAllWithFailure) {
    std::atomic<bool> task1_completed{false};
    std::atomic<bool> task2_completed{false};
    std::atomic<bool> saw_exception{false};
    
    auto task1 = [&task1_completed]() -> task<int> {
        co_await sleep(50ms);
        task1_completed = true;
        co_return 1;
    };
    
    auto task2 = [&task2_completed]() -> task<int> {
        co_await sleep(10ms);
        task2_completed = true;
        throw std::runtime_error("task2 failed");
        co_return 2;
    };
    
    auto saw_exception_ptr = &saw_exception;
    auto coro_fn = [saw_exception_ptr, &task1, &task2]() -> task<void> {
        auto t1 = task1();
        auto t2 = task2();
        
        try {
            co_await t1;
        } catch (...) {
            // task1 doesn't throw
        }
        
        try {
            co_await t2;
        } catch (const std::runtime_error& e) {
            (*saw_exception_ptr) = true;
        }
        
        co_return;
    };
    auto coordinator = coro_fn();
    
    coro_scheduler().spawn(std::move(coordinator));
    run_for(100ms);
    
    EXPECT_TRUE(task1_completed);
    EXPECT_TRUE(task2_completed);
    EXPECT_TRUE(saw_exception);
}

// =============================================================================
// TEST SUITE: when_any Pattern
// =============================================================================

class CoroutineWhenAny : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test when_any - first completion wins
 * @brief Race multiple coroutines, first wins using atomic compare-exchange
 */
TEST_F(CoroutineWhenAny, WhenAnyFirstCompletion) {
    std::atomic<int> winner{0};
    std::atomic<int> completed_count{0};
    
    auto winner_ptr = &winner;
    auto completed_ptr = &completed_count;
    
    // Create workers inline to avoid nested lambda capture issues
    auto w1 = [winner_ptr, completed_ptr]() -> task<void> {
        co_await sleep(20ms);
        completed_ptr->fetch_add(1);
        int expected = 0;
        winner_ptr->compare_exchange_strong(expected, 1);
        co_return;
    };
    
    auto w2 = [winner_ptr, completed_ptr]() -> task<void> {
        co_await sleep(50ms);
        completed_ptr->fetch_add(1);
        int expected = 0;
        winner_ptr->compare_exchange_strong(expected, 2);
        co_return;
    };
    
    auto w3 = [winner_ptr, completed_ptr]() -> task<void> {
        co_await sleep(100ms);
        completed_ptr->fetch_add(1);
        int expected = 0;
        winner_ptr->compare_exchange_strong(expected, 3);
        co_return;
    };
    
    // Spawn all three
    coro_scheduler().spawn(w1());
    coro_scheduler().spawn(w2());
    coro_scheduler().spawn(w3());
    
    run_for(150ms);
    
    EXPECT_EQ(winner.load(), 1);  // Fast should complete first
    EXPECT_EQ(completed_count.load(), 3);  // All should eventually complete
}

/**
 * @test race with same completion time
 * @brief Race with tasks of similar duration
 */
TEST_F(CoroutineWhenAny, RaceSimilarDuration) {
    constexpr int count = 3;
    std::vector<int> completion_order;
    std::mutex mutex;
    
    auto t = [&completion_order, &mutex](int id) -> task<void> {
        co_await sleep(20ms);  // Same duration
        std::lock_guard<std::mutex> lock(mutex);
        completion_order.push_back(id);
        co_return;
    };
    
    // Launch all at once
    auto coro_fn = [&t]() -> task<void> {
        for (int i = 0; i < count; ++i) {
            coro_scheduler().spawn(t(i));
        }
        co_return;
    };
    auto coordinator = coro_fn();
    
    coro_scheduler().spawn(std::move(coordinator));
    run_for(100ms);
    
    EXPECT_EQ(completion_order.size(), count);
    
    // Verify all IDs are present and unique
    std::set<int> unique_ids(completion_order.begin(), completion_order.end());
    EXPECT_EQ(unique_ids.size(), count);
    
    // Verify all IDs are in valid range
    for (int id : completion_order) {
        EXPECT_GE(id, 0);
        EXPECT_LT(id, count);
    }
}

// =============================================================================
// TEST SUITE: Select Pattern
// =============================================================================

class CoroutineSelect : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test select from multiple operations
 * @brief Choose first available result using spawn + polling
 */
TEST_F(CoroutineSelect, SelectFromMultiple) {
    std::atomic<int> selected{-1};
    std::atomic<bool> done{false};
    
    auto selected_ptr = &selected;
    auto done_ptr = &done;
    
    // Create operations that track completion
    auto w1 = [selected_ptr]() -> task<void> {
        co_await sleep(30ms);
        int expected = -1;
        selected_ptr->compare_exchange_strong(expected, 1);
        co_return;
    };
    
    auto w2 = [selected_ptr]() -> task<void> {
        co_await sleep(10ms);
        int expected = -1;
        selected_ptr->compare_exchange_strong(expected, 2);
        co_return;
    };
    
    auto w3 = [selected_ptr]() -> task<void> {
        co_await sleep(50ms);
        int expected = -1;
        selected_ptr->compare_exchange_strong(expected, 3);
        co_return;
    };
    
    // Monitor coroutine
    auto monitor = [selected_ptr, done_ptr]() -> task<void> {
        // Wait for first to complete
        while (selected_ptr->load() == -1) {
            co_await sleep(1ms);
        }
        done_ptr->store(true);
        co_return;
    };
    
    // Spawn all operations and monitor
    coro_scheduler().spawn(w1());
    coro_scheduler().spawn(w2());
    coro_scheduler().spawn(w3());
    coro_scheduler().spawn(monitor());
    
    run_for(100ms);
    
    EXPECT_TRUE(done.load());
    EXPECT_EQ(selected.load(), 2);  // op2 is fastest
}

/**
 * @test select with timeout
 * @brief Select with a timeout option
 */
TEST_F(CoroutineSelect, SelectWithTimeout) {
    std::atomic<bool> slow_completed{false};
    std::atomic<bool> timeout_hit{false};
    std::atomic<bool> done{false};
    
    auto slow_operation = [&slow_completed]() -> task<int> {
        co_await sleep(100ms);
        slow_completed = true;
        co_return 42;
    };
    
    auto timeout_hit_ptr = &timeout_hit;
    auto done_ptr = &done;
    auto coro_fn = [timeout_hit_ptr, done_ptr, &slow_operation]() -> task<void> {
        auto slow = slow_operation();
        
        auto start = std::chrono::steady_clock::now();
        
        while (std::chrono::steady_clock::now() - start < 50ms) {
            if (slow.handle().promise().is_ready()) {
                break;
            }
            co_await sleep(5ms);
        }
        
        if (!slow.handle().promise().is_ready()) {
            (*timeout_hit_ptr) = true;
        }
        
        (*done_ptr) = true;
        co_return;
    };
    auto with_timeout = coro_fn();
    
    coro_scheduler().spawn(std::move(with_timeout));
    run_for(150ms);
    
    EXPECT_TRUE(done);
    EXPECT_TRUE(timeout_hit);
    EXPECT_FALSE(slow_completed);  // Didn't complete in time
}

// =============================================================================
// TEST SUITE: Scatter-Gather Pattern
// =============================================================================

class CoroutineScatterGather : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test scatter-gather pattern
 * @brief Fan out work and collect results
 * 
 * NOTE: Disabled - complex capture issue causing incorrect results
 */
TEST_F(CoroutineScatterGather, ScatterGatherPattern) {
    constexpr int workers = 5;
    std::vector<int> results;
    std::mutex mutex;
    
    // Worker function with parameter (not captured lambda)
    auto worker = [](int id) -> task<int> {
        co_await sleep(std::chrono::milliseconds(10 + id * 5));
        co_return id * 10;
    };
    
    auto results_ptr = &results;
    auto mutex_ptr = &mutex;
    
    auto coro_fn = [results_ptr, mutex_ptr, &worker]() -> task<void> {
        std::vector<task<int>> tasks;
        
        // Scatter
        for (int i = 0; i < workers; ++i) {
            tasks.push_back(worker(i));
        }
        
        // Gather
        for (auto& t : tasks) {
            int result = co_await t;
            std::lock_guard<std::mutex> lock(*mutex_ptr);
            results_ptr->push_back(result);
        }
        
        co_return;
    };
    auto coordinator = coro_fn();
    
    coro_scheduler().spawn(std::move(coordinator));
    run_for(200ms);
    
    EXPECT_EQ(results.size(), workers);
    
    // Sort results for deterministic comparison
    std::sort(results.begin(), results.end());
    for (int i = 0; i < workers; ++i) {
        EXPECT_EQ(results[i], i * 10);
    }
}

/**
 * @test partial gather
 * @brief Gather only successful results, handling exceptions
 * 
 * NOTE: Disabled - complex capture issue causing incorrect exception handling
 */
TEST_F(CoroutineScatterGather, PartialGather) {
    constexpr int workers = 5;
    std::vector<int> successful_results;
    std::atomic<int> failures{0};
    
    // Worker function with parameter
    auto worker = [](int id) -> task<int> {
        co_await sleep(10ms);
        if (id % 2 == 0) {
            co_return id * 10;
        } else {
            throw std::runtime_error("odd id failed");
        }
    };
    
    auto successful_results_ptr = &successful_results;
    auto failures_ptr = &failures;
    
    auto coro_fn = [successful_results_ptr, failures_ptr, &worker]() -> task<void> {
        std::vector<task<int>> tasks;
        
        for (int i = 0; i < workers; ++i) {
            tasks.push_back(worker(i));
        }
        
        for (auto& t : tasks) {
            try {
                int result = co_await t;
                successful_results_ptr->push_back(result);
            } catch (...) {
                failures_ptr->fetch_add(1);
            }
        }
        
        co_return;
    };
    auto coordinator = coro_fn();
    
    coro_scheduler().spawn(std::move(coordinator));
    run_for(100ms);
    
    EXPECT_EQ(successful_results.size(), 3);  // IDs 0, 2, 4 succeed
    EXPECT_EQ(failures, 2);  // IDs 1, 3 fail
}

// =============================================================================
// TEST SUITE: Pipeline Pattern
// =============================================================================

class CoroutinePipeline : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test pipeline processing
 * @brief Chain of processing stages with sequential await
 * 
 * NOTE: Disabled - complex capture issue causing incorrect stage results
 */
TEST_F(CoroutinePipeline, PipelineProcessing) {
    std::vector<int> input{1, 2, 3, 4, 5};
    std::vector<int> output;
    std::mutex mutex;
    
    // Define stages OUTSIDE coroutine
    auto stage1 = [](int x) -> task<int> {
        co_await sleep(5ms);
        co_return x * 2;
    };
    
    auto stage2 = [](int x) -> task<int> {
        co_await sleep(5ms);
        co_return x + 10;
    };
    
    auto stage3 = [](int x) -> task<int> {
        co_await sleep(5ms);
        co_return x * x;
    };
    
    auto input_ptr = &input;
    auto output_ptr = &output;
    auto mutex_ptr = &mutex;
    
    auto coro_fn = [input_ptr, output_ptr, mutex_ptr, &stage1, &stage2, &stage3]() -> task<void> {
        for (int x : *input_ptr) {
            int r1 = co_await stage1(x);
            int r2 = co_await stage2(r1);
            int r3 = co_await stage3(r2);
            
            std::lock_guard<std::mutex> lock(*mutex_ptr);
            output_ptr->push_back(r3);
        }
        co_return;
    };
    auto pipeline = coro_fn();
    
    coro_scheduler().spawn(std::move(pipeline));
    run_for(500ms);
    
    EXPECT_EQ(output.size(), input.size());
    
    // Calculate expected: ((x * 2) + 10)^2
    for (size_t i = 0; i < input.size(); ++i) {
        int expected = ((input[i] * 2) + 10) * ((input[i] * 2) + 10);
        EXPECT_EQ(output[i], expected);
    }
}

/**
 * @test parallel pipeline stages
 * @brief Pipeline with parallel processing within stages
 */
TEST_F(CoroutinePipeline, ParallelPipelineStages) {
    constexpr int count = 10;
    std::vector<int> results;
    std::mutex mutex;
    
    auto parallel_stage = [&mutex, &results](int start, int end) -> task<void> {
        for (int i = start; i < end; ++i) {
            co_await sleep(5ms);
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(i);
        }
        co_return;
    };
    
    auto coro_fn = [&parallel_stage]() -> task<void> {
        // Process in parallel chunks
        auto t1 = parallel_stage(0, 5);
        auto t2 = parallel_stage(5, 10);
        
        co_await t1;
        co_await t2;
        
        co_return;
    };
    auto coordinator = coro_fn();
    
    coro_scheduler().spawn(std::move(coordinator));
    run_for(200ms);
    
    EXPECT_EQ(results.size(), count);
}

// =============================================================================
// MAIN
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
