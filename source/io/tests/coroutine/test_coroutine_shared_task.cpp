/**
 * @file test_coroutine_shared_task.cpp
 * @brief Tests for shared_task<T> — multi-consumer coroutine results
 *
 * Covers:
 * - Single consumer (baseline parity with task<T>)
 * - Multiple consumers sharing the same result
 * - Consumers that arrive AFTER the task has completed (late joiners)
 * - Exception propagation to all waiters
 * - shared_task<void> specialisation
 * - Copyability of the handle
 *
 * Note on spawn(lambda) vs spawn(lambda()):
 * -----------------------------------------
 * We always use  coro_scheduler().spawn(lambda)  (no trailing `()`)
 * and             scope.spawn(lambda)
 * The new callable overloads move the closure into a wrapper coroutine
 * frame, ensuring it outlives the spawning scope regardless of loop
 * iterations or temporary lambda lifetimes.
 *
 * @author qb - C++ Actor Framework
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <vector>
#include <string>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// Shared helpers
// =============================================================================

static task<int> compute_after(int value, std::chrono::milliseconds delay) {
    co_await sleep(delay);
    co_return value;
}

static task<int> shared_task_thrower() {
    co_await sleep(5ms);
    throw std::runtime_error("boom");
    co_return 0;
}

static task<void> shared_task_void_failing() {
    co_await sleep(5ms);
    throw std::runtime_error("void-fail");
}

// =============================================================================
// TEST SUITE: shared_task<int>
// =============================================================================

class SharedTaskTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override { qb::io::async::listener::current.clear(); }
};

TEST_F(SharedTaskTests, SingleConsumer) {
    int  result = 0;
    bool done   = false;

    auto sh = make_shared_task(compute_after(42, 10ms));

    // Named lambda is alive until end of test; spawn(lambda) also safe.
    coro_scheduler().spawn([sh, &result, &done]() mutable -> task<void> {
        result = co_await sh;
        done   = true;
    });
    run_for(100ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(result, 42);
}

TEST_F(SharedTaskTests, MultipleConsumers_SameResult) {
    std::vector<int> results(3, 0);
    auto sh = make_shared_task(compute_after(99, 10ms));

    // spawn(callable) — closure copied into wrapper frame, safe in loops.
    for (int i = 0; i < 3; ++i) {
        coro_scheduler().spawn([sh, &results, i]() mutable -> task<void> {
            results[i] = co_await sh;
        });
    }
    run_for(100ms);

    for (int r : results) EXPECT_EQ(r, 99);
}

TEST_F(SharedTaskTests, LateJoiner_GetsResultImmediately) {
    int  late_result = 0;
    bool done        = false;

    auto sh = make_shared_task(compute_after(77, 5ms));

    // Task finishes at ~5 ms; consumer joins at ~50 ms — should return immediately.
    coro_scheduler().spawn([sh, &late_result, &done]() mutable -> task<void> {
        co_await sleep(50ms);
        late_result = co_await sh;
        done        = true;
    });
    run_for(150ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(late_result, 77);
}

TEST_F(SharedTaskTests, HandleIsCopyable) {
    auto sh  = make_shared_task(compute_after(10, 5ms));
    auto sh2 = sh;
    auto sh3 = sh;

    int r1 = 0, r2 = 0, r3 = 0;

    coro_scheduler().spawn([sh,  &r1]() mutable -> task<void> { r1 = co_await sh;  });
    coro_scheduler().spawn([sh2, &r2]() mutable -> task<void> { r2 = co_await sh2; });
    coro_scheduler().spawn([sh3, &r3]() mutable -> task<void> { r3 = co_await sh3; });
    run_for(100ms);

    EXPECT_EQ(r1, 10);
    EXPECT_EQ(r2, 10);
    EXPECT_EQ(r3, 10);
}

TEST_F(SharedTaskTests, ExceptionPropagatedToAllWaiters) {
    auto sh = make_shared_task(shared_task_thrower());
    std::vector<bool> caught(3, false);

    for (int i = 0; i < 3; ++i) {
        coro_scheduler().spawn([sh, &caught, i]() mutable -> task<void> {
            try {
                co_await sh;
            } catch (const std::runtime_error&) {
                caught[i] = true;
            }
        });
    }
    run_for(100ms);

    for (bool c : caught) EXPECT_TRUE(c);
}

TEST_F(SharedTaskTests, ValidAndReadyState) {
    shared_task<int> empty;
    EXPECT_FALSE(empty.valid());

    auto sh = make_shared_task(compute_after(1, 5ms));
    EXPECT_TRUE(sh.valid());
    EXPECT_FALSE(sh.is_ready());

    bool done = false;
    coro_scheduler().spawn([sh, &done]() mutable -> task<void> {
        co_await sh;
        done = true;
    });
    run_for(50ms);
    EXPECT_TRUE(done);
    EXPECT_TRUE(sh.is_ready());
}

// =============================================================================
// TEST SUITE: shared_task<void>
// =============================================================================

class SharedTaskVoidTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override { qb::io::async::listener::current.clear(); }
};

static task<void> wait_and_signal(bool& flag, std::chrono::milliseconds delay) {
    co_await sleep(delay);
    flag = true;
}

TEST_F(SharedTaskVoidTests, MultipleWaiters) {
    bool flag  = false;
    int  woken = 0;

    auto sh = make_shared_task(wait_and_signal(flag, 10ms));

    for (int i = 0; i < 4; ++i) {
        coro_scheduler().spawn([sh, &woken]() mutable -> task<void> {
            co_await sh;
            ++woken;
        });
    }
    run_for(100ms);

    EXPECT_TRUE(flag);
    EXPECT_EQ(woken, 4);
}

TEST_F(SharedTaskVoidTests, LateJoiner) {
    bool flag = false;
    bool late = false;

    auto sh = make_shared_task(wait_and_signal(flag, 5ms));

    coro_scheduler().spawn([sh, &late]() mutable -> task<void> {
        co_await sleep(50ms);
        co_await sh;   // already done at this point
        late = true;
    });
    run_for(100ms);

    EXPECT_TRUE(flag);
    EXPECT_TRUE(late);
}

TEST_F(SharedTaskVoidTests, ExceptionPropagated) {
    auto sh    = make_shared_task(shared_task_void_failing());
    bool caught = false;

    coro_scheduler().spawn([sh, &caught]() mutable -> task<void> {
        try { co_await sh; }
        catch (const std::runtime_error&) { caught = true; }
    });
    run_for(50ms);
    EXPECT_TRUE(caught);
}

// =============================================================================
// TEST SUITE: shared_task with scope
// =============================================================================

class SharedTaskScopeTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override { qb::io::async::listener::current.clear(); }
};

static task<int> scope_load_data() {
    co_await sleep(10ms);
    co_return 100;
}

TEST_F(SharedTaskScopeTests, FanOut_MultipleWorkersAwaitSameData) {
    // Load shared data once; fan out to N workers each reading the same result.
    bool done = false;
    std::vector<int> outputs;

    coro_scheduler().spawn([&done, &outputs]() -> task<void> {
        auto data_handle = make_shared_task(scope_load_data());
        coroutine_scope scope;

        for (int i = 0; i < 5; ++i) {
            // scope.spawn(callable) — data_handle and i captured by value,
            // moved into the wrapper frame.  Safe across loop iterations.
            scope.spawn([data_handle, &outputs, i]() mutable -> task<void> {
                int data = co_await data_handle;
                outputs.push_back(data + i);
            });
        }

        co_await scope.join_all();
        done = true;
    });
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(outputs.size(), 5u);
    int sum = 0;
    for (int v : outputs) sum += v;
    // (100+0)+(100+1)+(100+2)+(100+3)+(100+4) = 510
    EXPECT_EQ(sum, 510);
}

// =============================================================================
// TEST SUITE: SharedTask Advanced
// =============================================================================

class SharedTaskAdvancedTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override { qb::io::async::listener::current.clear(); }
};

TEST_F(SharedTaskAdvancedTests, DefaultConstructedIsInvalid) {
    shared_task<int> st;
    EXPECT_FALSE(st.valid());
    EXPECT_FALSE(st.is_ready());
}

TEST_F(SharedTaskAdvancedTests, NonDefaultConstructibleType) {
    struct NoDefault {
        int value;
        explicit NoDefault(int v) : value(v) {}
    };

    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto st = make_shared_task([]() -> task<NoDefault> {
            co_return NoDefault{42};
        }());
        co_await sleep(10ms);
        auto result = co_await st;
        EXPECT_EQ(result.value, 42);
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(SharedTaskAdvancedTests, SharedTaskCopyAndValidState) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto st = make_shared_task([]() -> task<int> {
            co_await sleep(10ms);
            co_return 100;
        }());
        EXPECT_TRUE(st.valid());
        auto st_copy = st;
        EXPECT_TRUE(st_copy.valid());
        auto val = co_await st;
        EXPECT_EQ(val, 100);
        EXPECT_TRUE(st.is_ready());
        EXPECT_TRUE(st_copy.is_ready());
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
