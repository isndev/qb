/**
 * @file qb/io/tests/coroutine/test-coroutine-exceptions.cpp
 * @brief Coroutine exception handling tests
 *
 * This file contains tests for exception propagation through coroutine stacks,
 * including exceptions before and after suspension, different exception types, layered
 * rethrow behavior, scheduler stability, and parallel failures.
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
#include <atomic>
#include <stdexcept>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST FIXTURE
// =============================================================================

class CoroutineExceptionTests : public ::testing::Test {
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

task<void>
parallel_throwing_task(std::atomic<int> *total_caught) {
    try {
        co_await sleep(5ms);
        throw std::runtime_error("parallel throw");
    } catch (...) {
        total_caught->fetch_add(1);
    }
    co_return;
}

// =============================================================================
// BASIC EXCEPTION PROPAGATION
// =============================================================================

/**
 * @test Exception propagates from inner to outer coroutine
 * @brief Verify exceptions thrown in awaited coroutine are caught by awaiter
 */
TEST_F(CoroutineExceptionTests, ExceptionPropagatesFromInnerCoroutine) {
    std::atomic<bool> caught{false};
    std::atomic<bool> after_catch{false};

    auto caught_ptr = &caught;
    auto after_ptr  = &after_catch;

    auto coro_fn = [caught_ptr, after_ptr]() -> task<void> {
        try {
            auto inner_fn = []() -> task<void> {
                co_await sleep(1ms);
                throw std::runtime_error("test exception");
                co_return;
            };
            auto inner = inner_fn();
            co_await inner;
        } catch (const std::runtime_error &e) {
            if (std::string(e.what()) == "test exception") {
                caught_ptr->store(true);
            }
        }
        after_ptr->store(true);
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    EXPECT_TRUE(caught.load());
    EXPECT_TRUE(after_catch.load()); // Execution continues after catch
}

/**
 * @test Exception before suspension point
 * @brief Verify exceptions thrown before co_await are caught
 */
TEST_F(CoroutineExceptionTests, ExceptionBeforeSuspension) {
    std::atomic<bool> caught{false};
    auto              caught_ptr = &caught;

    auto coro_fn = [caught_ptr]() -> task<void> {
        try {
            auto inner_fn = []() -> task<void> {
                throw std::logic_error("immediate throw");
                co_await sleep(1ms); // Never reached
                co_return;
            };
            auto inner = inner_fn();
            co_await inner;
        } catch (const std::logic_error &) {
            caught_ptr->store(true);
        }
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    EXPECT_TRUE(caught.load());
}

/**
 * @test Exception after suspension point
 * @brief Verify exceptions thrown after co_await are caught
 */
TEST_F(CoroutineExceptionTests, ExceptionAfterSuspension) {
    std::atomic<bool> caught{false};
    auto              caught_ptr = &caught;

    auto coro_fn = [caught_ptr]() -> task<void> {
        try {
            auto inner_fn = []() -> task<void> {
                co_await sleep(10ms);
                throw std::invalid_argument("after suspension");
                co_return;
            };
            auto inner = inner_fn();
            co_await inner;
        } catch (const std::invalid_argument &) {
            caught_ptr->store(true);
        }
        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    EXPECT_TRUE(caught.load());
}

// =============================================================================
// EXCEPTION TYPES
// =============================================================================

/**
 * @test Different exception types propagate correctly
 * @brief Verify various exception types are handled properly
 */
TEST_F(CoroutineExceptionTests, DifferentExceptionTypes) {
    std::atomic<int> caught_types{0};
    auto             types_ptr = &caught_types;

    auto coro_fn = [types_ptr]() -> task<void> {
        // Test std::runtime_error
        try {
            auto fn1 = []() -> task<void> {
                throw std::runtime_error("runtime");
                co_return;
            };
            co_await fn1();
        } catch (const std::runtime_error &) {
            types_ptr->fetch_add(1);
        }

        // Test std::logic_error
        try {
            auto fn2 = []() -> task<void> {
                throw std::logic_error("logic");
                co_return;
            };
            co_await fn2();
        } catch (const std::logic_error &) {
            types_ptr->fetch_add(10);
        }

        // Test custom exception
        struct CustomException : std::exception {};
        try {
            auto fn3 = []() -> task<void> {
                throw CustomException{};
                co_return;
            };
            co_await fn3();
        } catch (const CustomException &) {
            types_ptr->fetch_add(100);
        }

        co_return;
    };
    auto t = coro_fn();

    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    // All three exception types should be caught: 1 + 10 + 100 = 111
    EXPECT_EQ(caught_types.load(), 111);
}

// =============================================================================
// EXCEPTION CHAINING
// =============================================================================

/**
 * @test Exception through multiple coroutine layers
 * @brief Verify exceptions propagate through deep call stacks
 */
TEST_F(CoroutineExceptionTests, ExceptionThroughMultipleLayers) {
    std::atomic<int> depth_caught{0};
    auto             depth_ptr = &depth_caught;

    auto level3_fn = []() -> task<void> {
        co_await sleep(1ms);
        throw std::runtime_error("from level 3");
        co_return;
    };

    auto level2_fn = [&level3_fn]() -> task<void> {
        co_await level3_fn(); // Exception propagates through
        co_return;
    };

    auto level1_fn = [&level2_fn, depth_ptr]() -> task<void> {
        try {
            co_await level2_fn();
        } catch (const std::runtime_error &e) {
            if (std::string(e.what()) == "from level 3") {
                depth_ptr->store(3);
            }
        }
        co_return;
    };

    coro_scheduler().spawn(level1_fn());
    run_for(50ms);

    EXPECT_EQ(depth_caught.load(), 3);
}

/**
 * @test Rethrow in coroutine
 * @brief Verify catch-and-rethrow works correctly
 */
TEST_F(CoroutineExceptionTests, RethrowInCoroutine) {
    std::atomic<int> catch_count{0};
    auto             count_ptr = &catch_count;

    auto thrower_fn = []() -> task<void> {
        throw std::runtime_error("original");
        co_return;
    };

    auto rethrow_fn = [&thrower_fn, count_ptr]() -> task<void> {
        try {
            co_await thrower_fn();
        } catch (...) {
            count_ptr->fetch_add(1);
            throw; // Rethrow
        }
        co_return;
    };

    auto catcher_fn = [&rethrow_fn, count_ptr]() -> task<void> {
        try {
            co_await rethrow_fn();
        } catch (const std::runtime_error &) {
            count_ptr->fetch_add(10);
        }
        co_return;
    };

    coro_scheduler().spawn(catcher_fn());
    run_for(50ms);

    // Should catch twice: inner(1) + outer(10) = 11
    EXPECT_EQ(catch_count.load(), 11);
}

// =============================================================================
// EXCEPTION WITH VALUES
// =============================================================================

/**
 * @test Exception from value-returning coroutine
 * @brief Verify exceptions work with task<T> (not just task<void>)
 */
TEST_F(CoroutineExceptionTests, ExceptionFromValueReturningCoroutine) {
    std::atomic<bool> caught{false};
    std::atomic<int>  fallback_value{0};
    auto              caught_ptr   = &caught;
    auto              fallback_ptr = &fallback_value;

    auto thrower_fn = []() -> task<int> {
        co_await sleep(1ms);
        throw std::runtime_error("no value");
        co_return 42; // Never reached
    };

    auto handler_fn = [&thrower_fn, caught_ptr, fallback_ptr]() -> task<void> {
        int result = 0;
        try {
            result = co_await thrower_fn();
        } catch (const std::runtime_error &) {
            caught_ptr->store(true);
            result = -1; // Fallback value
        }
        fallback_ptr->store(result);
        co_return;
    };

    coro_scheduler().spawn(handler_fn());
    run_for(50ms);

    EXPECT_TRUE(caught.load());
    EXPECT_EQ(fallback_value.load(), -1);
}

// =============================================================================
// EXCEPTION SAFETY
// =============================================================================

/**
 * @test Scheduler remains stable after exception
 * @brief Verify scheduler continues working after coroutine throws
 */
TEST_F(CoroutineExceptionTests, SchedulerStableAfterException) {
    std::atomic<int> completed_count{0};
    auto             count_ptr = &completed_count;

    // First coroutine throws
    auto thrower_fn = []() -> task<void> {
        throw std::runtime_error("error");
        co_return;
    };

    auto catcher_fn = [&thrower_fn, count_ptr]() -> task<void> {
        try {
            co_await thrower_fn();
        } catch (...) {
            count_ptr->fetch_add(1);
        }
        co_return;
    };

    // Second coroutine succeeds
    auto normal_fn = [count_ptr]() -> task<void> {
        co_await sleep(10ms);
        count_ptr->fetch_add(10);
        co_return;
    };

    coro_scheduler().spawn(catcher_fn());
    coro_scheduler().spawn(normal_fn());
    run_for(50ms);

    // Both should complete: 1 + 10 = 11
    EXPECT_EQ(completed_count.load(), 11);

    // Scheduler should be clean
    EXPECT_EQ(coro_scheduler().active_count(), 0);
}

/**
 * @test Multiple exceptions in parallel coroutines
 * @brief Verify multiple coroutines can throw independently
 */
TEST_F(CoroutineExceptionTests, MultipleExceptionsInParallel) {
    std::atomic<int> total_caught{0};
    auto             total_ptr = &total_caught;

    // Spawn 5 coroutines that all throw
    for (int i = 0; i < 5; ++i) {
        coro_scheduler().spawn(parallel_throwing_task(total_ptr));
    }

    run_for(50ms);

    // All 5 should have caught their exceptions
    EXPECT_EQ(total_caught.load(), 5);
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
