/**
 * @file qb/io/tests/coroutine/test-coroutine-safety.cpp
 * @brief Coroutine safety tests
 *
 * This file contains tests for safe coroutine usage patterns, including value capture,
 * move-only values, shared data access, exception safety, RAII behavior, memory
 * allocation pressure, and concurrency safety.
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

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Value Capture Safety
// =============================================================================

class CoroutineValueCapture : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Capturing by value vs reference
 * @brief Verify that value capture works correctly
 */
TEST_F(CoroutineValueCapture, ValueCaptureSafety) {
    // Given
    std::atomic<int> counter{0};
    int local_var = 42;
    
    // When - capture by value (CORRECT)
    auto counter_ptr = &counter;
    auto coro_fn = [local_var, counter_ptr]() -> task<void> {
        // local_var is copied, safe to use even if original goes out of scope
        if (local_var == 42) {
            counter_ptr->fetch_add(1);
        }
        co_return;
    };
    auto t = coro_fn();
    
    local_var = 999;  // Modify original - captured value should still be 42
    
    coro_scheduler().spawn(std::move(t));
    run_for(10ms);
    
    // Then
    EXPECT_EQ(counter, 1);
}

/**
 * @test String capture safety
 * @brief Verify string values are properly captured
 */
TEST_F(CoroutineValueCapture, StringCaptureSafety) {
    std::atomic<bool> success{false};
    std::string message = "original";
    
    auto success_ptr = &success;
    auto coro_fn = [message, success_ptr]() -> task<void> {
        co_await sleep(10ms);
        // message should still be "original" even if original was modified
        if (message == "original") {
            (*success_ptr) = true;
        }
        co_return;
    };
    auto t = coro_fn();
    
    message = "modified";  // Modify original
    
    coro_scheduler().spawn(std::move(t));
    run_for(20ms);
    
    EXPECT_TRUE(success);
}

/**
 * @test Complex object capture
 * @brief Verify complex objects are properly copied when captured by value
 */
TEST_F(CoroutineValueCapture, ComplexObjectCapture) {
    struct Data {
        int id;
        std::string name;
        std::vector<int> values;
    };
    
    std::atomic<bool> success{false};
    Data data{42, "test", {1, 2, 3}};
    
    // Capture data by value - the copy happens at lambda creation
    auto success_ptr = &success;
    auto coro_fn = [data, success_ptr]() -> task<void> {
        // Check that all fields were copied correctly
        if (data.id == 42 && data.name == "test" && data.values.size() == 3) {
            (*success_ptr) = true;
        }
        co_return;
    };
    auto t = coro_fn();
    
    // Modify original after lambda creation
    data.id = 999;
    data.name = "modified";
    
    coro_scheduler().spawn(std::move(t));
    run_for(20ms);
    
    EXPECT_TRUE(success);
}

// =============================================================================
// TEST SUITE: Move-Only Type Support
// =============================================================================

class CoroutineMoveOnly : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Move-only return type
 * @brief Verify move-only types work as return values
 */
TEST_F(CoroutineMoveOnly, MoveOnlyReturnValue) {
    struct MoveOnlyResource {
        std::unique_ptr<int> data;
        explicit MoveOnlyResource(int v) : data(std::make_unique<int>(v)) {}
        MoveOnlyResource(MoveOnlyResource&&) = default;
        MoveOnlyResource& operator=(MoveOnlyResource&&) = default;
        MoveOnlyResource(const MoveOnlyResource&) = delete;
        MoveOnlyResource& operator=(const MoveOnlyResource&) = delete;
    };
    
    auto producer_fn = []() -> task<MoveOnlyResource> {
        co_await sleep(10ms);
        co_return MoveOnlyResource{42};
    };
    
    std::atomic<int> result{0};
    auto result_ptr = &result;
    auto coro_fn = [result_ptr, &producer_fn]() -> task<void> {
        auto resource = co_await producer_fn();
        if (resource.data && *resource.data == 42) {
            result_ptr->store(1);
        }
        co_return;
    };
    auto consumer = coro_fn();
    
    coro_scheduler().spawn(std::move(consumer));
    run_for(20ms);
    
    EXPECT_EQ(result, 1);
}

/**
 * @test Move-only capture in coroutine
 * @brief Verify move-only types can be captured
 * 
 */
TEST_F(CoroutineMoveOnly, MoveOnlyCapture) {
    std::atomic<bool> success{false};
    
    // Create and capture unique_ptr in the same expression
    auto success_ptr = &success;
    auto coro_fn = [ptr = std::make_unique<int>(42), success_ptr]() -> task<void> {
        if (ptr && *ptr == 42) {
            (*success_ptr) = true;
        }
        co_return;
    };
    auto t = coro_fn();
    
    coro_scheduler().spawn(std::move(t));
    run_for(10ms);
    
    EXPECT_TRUE(success);
}

// =============================================================================
// TEST SUITE: Concurrency Safety
// =============================================================================

class CoroutineConcurrencySafety : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Multiple coroutines accessing shared data
 * @brief Verify thread-safe access patterns
 */
TEST_F(CoroutineConcurrencySafety, SharedDataAccess) {
    constexpr int num_coroutines = 10;
    std::atomic<int> counter{0};
    
    // Spawn many coroutines that all increment the same counter
    for (int i = 0; i < num_coroutines; ++i) {
        auto counter_ptr = &counter;
        auto coro_fn = [counter_ptr]() -> task<void> {
            co_await sleep(std::chrono::milliseconds(1 + (rand() % 10)));
            counter_ptr->fetch_add(1);
            co_return;
        };
        coro_scheduler().spawn(coro_fn); // owned-callable: closure dies before resume
    }
    
    run_for(100ms);
    
    EXPECT_EQ(counter, num_coroutines);
}

/**
 * @test Read-only shared data
 * @brief Verify safe read-only access to shared data using init-capture
 */
TEST_F(CoroutineConcurrencySafety, ReadOnlySharedData) {
    constexpr int num_coroutines = 5;
    std::atomic<int> sum{0};
    
    // Create coroutine function that takes value as parameter
    auto adder = [&sum](int value) -> task<void> {
        co_await sleep(5ms);
        sum.fetch_add(value);
        co_return;
    };
    
    for (int i = 0; i < num_coroutines; ++i) {
        coro_scheduler().spawn(adder(i + 1));
    }
    
    run_for(50ms);
    
    EXPECT_EQ(sum.load(), 15);  // 1+2+3+4+5
}

// =============================================================================
// TEST SUITE: Exception Safety
// =============================================================================

class CoroutineExceptionSafetyExtended : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Exception from captured variable
 * @brief Verify exceptions from captures are handled
 */
TEST_F(CoroutineExceptionSafetyExtended, ExceptionFromCapture) {
    std::atomic<bool> caught{false};
    
    auto throwing_func = []() {
        throw std::runtime_error("captured error");
    };
    
    auto caught_ptr = &caught;
    auto coro_fn = [caught_ptr, &throwing_func]() -> task<void> {
        try {
            throwing_func();
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "captured error") {
                caught_ptr->store(true);
            }
        }
        co_return;
    };
    auto t = coro_fn();
    
    coro_scheduler().spawn(std::move(t));
    run_for(10ms);
    
    EXPECT_TRUE(caught);
}

/**
 * @test Nested exception handling
 * @brief Verify nested try-catch blocks work
 */
TEST_F(CoroutineExceptionSafetyExtended, NestedExceptionHandling) {
    std::vector<std::string> log;
    
    // inner is a lambda that creates a coroutine
    auto log_ptr = &log;
    auto inner_fn = [log_ptr]() -> task<int> {
        log_ptr->push_back("inner-start");
        throw std::runtime_error("inner error");
        co_return 1;
    };
    
    // outer_task is the actual task object
    auto coro_fn = [log_ptr, &inner_fn]() -> task<void> {
        log_ptr->push_back("outer-start");
        try {
            co_await inner_fn();
        } catch (const std::runtime_error& e) {
            log_ptr->push_back("outer-caught: " + std::string(e.what()));
        }
        co_return;
    };
    auto outer_task = coro_fn();
    
    coro_scheduler().spawn(std::move(outer_task));
    
    run_for(50ms);
    
    EXPECT_GE(log.size(), 3);
    EXPECT_EQ(log[0], "outer-start");
    EXPECT_EQ(log[1], "inner-start");
    EXPECT_TRUE(log[2].find("outer-caught") != std::string::npos);
}

// =============================================================================
// TEST SUITE: RAII and Resource Management
// =============================================================================

class CoroutineRAII : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test RAII guard in coroutine
 * @brief Verify destructors run when coroutine completes
 */
TEST_F(CoroutineRAII, RAIIInCoroutine) {
    std::atomic<bool> guard_destroyed{false};
    std::atomic<bool> coroutine_completed{false};
    
    struct Guard {
        std::atomic<bool>& destroyed;
        Guard(std::atomic<bool>& d) : destroyed(d) { destroyed = false; }
        ~Guard() { destroyed = true; }
    };
    
    auto guard_destroyed_ptr = &guard_destroyed;
    auto coroutine_completed_ptr = &coroutine_completed;
    auto coro_fn = [guard_destroyed_ptr, coroutine_completed_ptr]() -> task<void> {
        Guard guard{*guard_destroyed_ptr};
        co_await sleep(10ms);
        coroutine_completed_ptr->store(true);
        co_return;
    };
    auto t = coro_fn();
    
    coro_scheduler().spawn(std::move(t));
    
    // Guard should not be destroyed yet
    EXPECT_FALSE(guard_destroyed);
    
    run_for(20ms);
    
    // Now both should be complete
    EXPECT_TRUE(coroutine_completed);
    EXPECT_TRUE(guard_destroyed);
}

/**
 * @test RAII with exception
 * @brief Verify destructors run even with exception
 */
TEST_F(CoroutineRAII, RAIIWithException) {
    std::atomic<bool> guard_destroyed{false};
    std::atomic<bool> exception_caught{false};
    
    struct Guard {
        std::atomic<bool>& destroyed;
        Guard(std::atomic<bool>& d) : destroyed(d) { destroyed = false; }
        ~Guard() { destroyed = true; }
    };
    
    // Wrap the throwing coroutine in another that catches the exception
    auto guard_destroyed_ptr = &guard_destroyed;
    auto exception_caught_ptr = &exception_caught;
    auto coro_fn = [guard_destroyed_ptr, exception_caught_ptr]() -> task<void> {
        auto inner_fn = [guard_destroyed_ptr]() -> task<void> {
            Guard guard{*guard_destroyed_ptr};
            co_await sleep(5ms);
            throw std::runtime_error("error");
            co_return;
        };
        auto inner = inner_fn();
        
        try {
            co_await inner;
        } catch (...) {
            exception_caught_ptr->store(true);
        }
        co_return;
    };
    auto wrapper = coro_fn();
    
    coro_scheduler().spawn(std::move(wrapper));
    
    run_for(20ms);
    
    EXPECT_TRUE(guard_destroyed);
    EXPECT_TRUE(exception_caught);
}

// =============================================================================
// TEST SUITE: Memory Safety
// =============================================================================

class CoroutineMemorySafety : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Large allocation in coroutine
 * @brief Verify large allocations work correctly
 */
TEST_F(CoroutineMemorySafety, LargeAllocation) {
    constexpr size_t alloc_size = 1024 * 1024;  // 1MB
    std::atomic<bool> success{false};
    
    auto success_ptr = &success;
    auto coro_fn = [success_ptr]() -> task<void> {
        auto data = std::make_unique<char[]>(alloc_size);
        // Touch the memory
        for (size_t i = 0; i < alloc_size; i += 4096) {
            data[i] = static_cast<char>(i % 256);
        }
        (*success_ptr) = true;
        co_return;
    };
    auto t = coro_fn();
    
    coro_scheduler().spawn(std::move(t));
    run_for(10ms);
    
    EXPECT_TRUE(success);
}

/**
 * @test Many small allocations
 * @brief Verify multiple allocations work
 */
TEST_F(CoroutineMemorySafety, ManySmallAllocations) {
    constexpr int count = 100;
    std::atomic<int> completed{0};
    
    for (int i = 0; i < count; ++i) {
        auto completed_ptr = &completed;
        auto coro_fn = [completed_ptr, i]() -> task<void> {
            auto data = std::make_unique<int>(i);
            co_await sleep(1ms);
            if (*data == i) {
                completed_ptr->fetch_add(1);
            }
            co_return;
        };
        coro_scheduler().spawn(coro_fn); // owned-callable: closure dies before resume
    }
    
    run_for(200ms);
    
    EXPECT_EQ(completed, count);
}

// =============================================================================
// MAIN
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
