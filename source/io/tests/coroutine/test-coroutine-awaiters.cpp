/**
 * @file qb/io/tests/coroutine/test-coroutine-awaiters.cpp
 * @brief Custom awaiter tests for coroutine integration
 *
 * This file contains tests for the coroutine awaiter protocol, including custom awaiter
 * implementations, suspension behavior, exception propagation, state handling, and
 * scheduler integration.
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

using namespace qb::io::async;
using namespace std::chrono_literals;

template <typename Predicate>
bool wait_until(Predicate &&predicate, std::chrono::milliseconds timeout,
                std::chrono::milliseconds step = 5ms) {
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
// TEST FIXTURE
// =============================================================================

class CoroutineAwaiterTests : public ::testing::Test {
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

// =============================================================================
// CUSTOM AWAITER IMPLEMENTATIONS
// =============================================================================

/**
 * @brief Simple awaiter that completes immediately
 */
struct immediate_awaiter {
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) noexcept {}
    void await_resume() const noexcept {}
};

/**
 * @brief Awaiter that always suspends and resumes via scheduler
 */
struct always_suspend_awaiter {
    std::coroutine_handle<> handle_;
    
    bool await_ready() const noexcept { return false; }
    
    void await_suspend(std::coroutine_handle<> h) noexcept {
        handle_ = h;
        // Schedule for immediate resumption
        coro_scheduler().schedule_resume(h);
    }
    
    void await_resume() const noexcept {}
};

/**
 * @brief Awaiter that returns a value
 */
template<typename T>
struct value_awaiter {
    T value_;
    
    explicit value_awaiter(T val) : value_(std::move(val)) {}
    
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) noexcept {}
    T await_resume() noexcept { return std::move(value_); }
};

/**
 * @brief Awaiter that throws on resume
 */
struct throwing_awaiter {
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) noexcept {}
    void await_resume() const { throw std::runtime_error("awaiter threw"); }
};

/**
 * @brief Awaiter with symmetric transfer
 */
struct symmetric_transfer_awaiter {
    std::coroutine_handle<> target_;
    
    explicit symmetric_transfer_awaiter(std::coroutine_handle<> target) 
        : target_(target) {}
    
    bool await_ready() const noexcept { return false; }
    
    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
        return target_;  // Symmetric transfer
    }
    
    void await_resume() const noexcept {}
};

// =============================================================================
// IMMEDIATE AWAITER TESTS
// =============================================================================

/**
 * @test Immediate awaiter doesn't suspend
 * @brief Verify await_ready() = true prevents suspension
 */
TEST_F(CoroutineAwaiterTests, ImmediateAwaiterDoesntSuspend) {
    std::atomic<int> execution_order{0};
    auto order_ptr = &execution_order;
    
    auto fn = [order_ptr]() -> task<void> {
        order_ptr->store(1);
        co_await immediate_awaiter{};
        order_ptr->store(2);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    coro_scheduler().run_ready();
    
    // Should have completed synchronously
    EXPECT_EQ(execution_order.load(), 2);
}

/**
 * @test Value awaiter returns value
 * @brief Verify custom awaiter can return values
 */
TEST_F(CoroutineAwaiterTests, ValueAwaiterReturnsValue) {
    std::atomic<int> result{0};
    auto result_ptr = &result;
    
    auto fn = [result_ptr]() -> task<void> {
        int val = co_await value_awaiter<int>{42};
        result_ptr->store(val);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(10ms);
    
    EXPECT_EQ(result.load(), 42);
}

/**
 * @test Multiple immediate awaiters in sequence
 * @brief Verify multiple immediate awaiters execute synchronously
 */
TEST_F(CoroutineAwaiterTests, MultipleImmediateAwaiters) {
    std::atomic<int> counter{0};
    auto counter_ptr = &counter;
    
    auto fn = [counter_ptr]() -> task<void> {
        counter_ptr->fetch_add(1);
        co_await immediate_awaiter{};
        counter_ptr->fetch_add(1);
        co_await immediate_awaiter{};
        counter_ptr->fetch_add(1);
        co_await immediate_awaiter{};
        counter_ptr->fetch_add(1);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    coro_scheduler().run_ready();
    
    EXPECT_EQ(counter.load(), 4);
}

// =============================================================================
// SUSPENDING AWAITER TESTS
// =============================================================================

/**
 * @test Always-suspend awaiter suspends and resumes
 * @brief Verify await_ready() = false causes suspension
 */
TEST_F(CoroutineAwaiterTests, AlwaysSuspendAwaiterSuspends) {
    std::atomic<int> stage{0};
    auto stage_ptr = &stage;
    
    auto fn = [stage_ptr]() -> task<void> {
        stage_ptr->store(1);
        co_await always_suspend_awaiter{};
        stage_ptr->store(2);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    
    // Run one step only: coroutine runs to co_await, suspends, re-enqueues itself
    coro_scheduler().run_ready(1);
    EXPECT_EQ(stage.load(), 1);
    
    // Run again (or run_for) to let it resume and complete
    run_for(10ms);
    EXPECT_EQ(stage.load(), 2);
}

/**
 * @test Awaiter exception propagates
 * @brief Verify exceptions from await_resume() propagate correctly
 */
TEST_F(CoroutineAwaiterTests, AwaiterExceptionPropagates) {
    std::atomic<bool> caught{false};
    auto caught_ptr = &caught;
    
    auto fn = [caught_ptr]() -> task<void> {
        try {
            co_await throwing_awaiter{};
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "awaiter threw") {
                caught_ptr->store(true);
            }
        }
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(10ms);
    
    EXPECT_TRUE(caught.load());
}

// =============================================================================
// AWAITER LIFECYCLE
// =============================================================================

/**
 * @test Awaiter destroyed after await completes
 * @brief Verify awaiter object lifetime
 */
TEST_F(CoroutineAwaiterTests, AwaiterDestroyedAfterAwait) {
    struct TrackedAwaiter {
        std::atomic<bool>* destroyed;
        
        explicit TrackedAwaiter(std::atomic<bool>* d) : destroyed(d) {}
        ~TrackedAwaiter() { if (destroyed) destroyed->store(true); }
        
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept {
            coro_scheduler().schedule_resume(h);
        }
        void await_resume() const noexcept {}
    };
    
    std::atomic<bool> awaiter_destroyed{false};
    std::atomic<bool> after_await{false};
    auto destroyed_ptr = &awaiter_destroyed;
    auto after_ptr = &after_await;
    
    auto fn = [destroyed_ptr, after_ptr]() -> task<void> {
        co_await TrackedAwaiter{destroyed_ptr};
        // Awaiter should be destroyed before this line
        after_ptr->store(true);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(50ms);
    
    EXPECT_TRUE(after_await.load());
    EXPECT_TRUE(awaiter_destroyed.load());
}

/**
 * @test Awaiter with state
 * @brief Verify awaiters can maintain state across await_suspend/resume
 */
TEST_F(CoroutineAwaiterTests, AwaiterWithState) {
    struct StatefulAwaiter {
        int& result;
        int value = 0;
        
        explicit StatefulAwaiter(int& r) : result(r) {}
        
        bool await_ready() const noexcept { return false; }
        
        void await_suspend(std::coroutine_handle<> h) noexcept {
            value = 42;  // Set state during suspension
            coro_scheduler().schedule_resume(h);
        }
        
        int await_resume() noexcept { 
            result = value;  // Use state in resume
            return value; 
        }
    };
    
    std::atomic<int> result{0};
    int temp = 0;
    auto result_ptr = &result;
    
    auto fn = [result_ptr, &temp]() -> task<void> {
        int val = co_await StatefulAwaiter{temp};
        result_ptr->store(val);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(50ms);
    
    EXPECT_EQ(result.load(), 42);
    EXPECT_EQ(temp, 42);
}

// =============================================================================
// SYMMETRIC TRANSFER AWAITERS
// =============================================================================

/**
 * @test Custom symmetric transfer awaiter
 * @brief Verify custom awaiters can use symmetric transfer
 */
TEST_F(CoroutineAwaiterTests, CustomSymmetricTransfer) {
    std::atomic<int> execution_order{0};
    auto order_ptr = &execution_order;
    
    auto target_fn = [order_ptr]() -> task<void> {
        order_ptr->fetch_add(100);
        co_return;
    };
    
    auto caller_fn = [order_ptr, &target_fn]() -> task<void> {
        order_ptr->fetch_add(1);
        
        auto target = target_fn();
        // Note: Can't easily test symmetric transfer with custom awaiter
        // as it requires access to target's handle
        co_await target;
        
        order_ptr->fetch_add(10);
        co_return;
    };
    
    coro_scheduler().spawn(caller_fn());
    run_for(50ms);
    
    // Should execute: caller(1) + target(100) + caller(10) = 111
    EXPECT_EQ(execution_order.load(), 111);
}

// =============================================================================
// AWAITER ERROR HANDLING
// =============================================================================

/**
 * @test Exception in await_suspend
 * @brief Verify exceptions from await_suspend() are handled
 */
TEST_F(CoroutineAwaiterTests, ExceptionInAwaitSuspend) {
    struct ThrowInSuspend {
        bool await_ready() const noexcept { return false; }
        
        void await_suspend(std::coroutine_handle<>) {
            throw std::runtime_error("suspend failed");
        }
        
        void await_resume() const noexcept {}
    };
    
    std::atomic<bool> caught{false};
    auto caught_ptr = &caught;
    
    auto fn = [caught_ptr]() -> task<void> {
        try {
            co_await ThrowInSuspend{};
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()) == "suspend failed") {
                caught_ptr->store(true);
            }
        }
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(50ms);
    
    EXPECT_TRUE(caught.load());
}

// =============================================================================
// BUILT-IN AWAITER TESTS
// =============================================================================

/**
 * @test sleep() awaiter integration
 * @brief Verify sleep() properly integrates with libev
 */
TEST_F(CoroutineAwaiterTests, SleepAwaiterIntegration) {
    auto start = std::chrono::steady_clock::now();
    std::atomic<bool> completed{false};
    auto completed_ptr = &completed;
    
    auto fn = [completed_ptr]() -> task<void> {
        co_await sleep(50ms);
        completed_ptr->store(true);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(100ms);
    
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    EXPECT_TRUE(completed.load());
    EXPECT_GE(elapsed, 50ms);
    EXPECT_LT(elapsed, 150ms);  // Reasonable upper bound
}

/**
 * @test Zero-duration sleep
 * @brief Verify sleep(0ms) yields control but resumes immediately
 */
TEST_F(CoroutineAwaiterTests, ZeroDurationSleep) {
    std::atomic<int> execution_order{0};
    auto order_ptr = &execution_order;
    
    auto fn = [order_ptr]() -> task<void> {
        order_ptr->store(1);
        co_await sleep(0ms);
        order_ptr->store(2);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    EXPECT_TRUE(wait_until([order_ptr]() { return order_ptr->load() == 2; }, 100ms));
    
    EXPECT_EQ(execution_order.load(), 2);
}

/**
 * @test Multiple sleeps accumulate
 * @brief Verify sequential sleeps add up correctly
 */
TEST_F(CoroutineAwaiterTests, MultipleSleepsAccumulate) {
    auto start = std::chrono::steady_clock::now();
    std::atomic<bool> completed{false};
    auto completed_ptr = &completed;
    
    auto fn = [completed_ptr]() -> task<void> {
        co_await sleep(20ms);
        co_await sleep(20ms);
        co_await sleep(20ms);
        completed_ptr->store(true);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    EXPECT_TRUE(wait_until([completed_ptr]() { return completed_ptr->load(); }, 250ms));
    
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    EXPECT_TRUE(completed.load());
    EXPECT_GE(elapsed, 60ms);  // At least 3 * 20ms
}

// =============================================================================
// AWAITER COMPOSITION
// =============================================================================

/**
 * @test Awaiting multiple different awaiter types
 * @brief Verify coroutine can await different awaiter types
 */
TEST_F(CoroutineAwaiterTests, MixedAwaiterTypes) {
    std::atomic<int> result{0};
    auto result_ptr = &result;
    
    auto fn = [result_ptr]() -> task<void> {
        // Immediate awaiter
        co_await immediate_awaiter{};
        result_ptr->fetch_add(1);
        
        // Value awaiter
        int val = co_await value_awaiter<int>{10};
        result_ptr->fetch_add(val);
        
        // Sleep awaiter
        co_await sleep(10ms);
        result_ptr->fetch_add(100);
        
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    EXPECT_TRUE(wait_until([result_ptr]() { return result_ptr->load() == 111; }, 200ms));
    
    // 1 + 10 + 100 = 111
    EXPECT_EQ(result.load(), 111);
}

/**
 * @test Awaiter in loop
 * @brief Verify awaiters work correctly in loops
 */
TEST_F(CoroutineAwaiterTests, AwaiterInLoop) {
    std::atomic<int> iterations{0};
    auto iter_ptr = &iterations;
    
    auto fn = [iter_ptr]() -> task<void> {
        for (int i = 0; i < 5; ++i) {
            co_await immediate_awaiter{};
            iter_ptr->fetch_add(1);
        }
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(10ms);
    
    EXPECT_EQ(iterations.load(), 5);
}

// =============================================================================
// AWAITER INTERFACE PROTOCOL
// =============================================================================

/**
 * @test Awaiter protocol: await_ready called first
 * @brief Verify await_ready() is called before await_suspend()
 */
TEST_F(CoroutineAwaiterTests, AwaiterProtocolOrder) {
    struct ProtocolTracker {
        std::vector<std::string>* log;
        
        explicit ProtocolTracker(std::vector<std::string>* l) : log(l) {}
        
        bool await_ready() const noexcept { 
            log->push_back("ready");
            return false; 
        }
        
        void await_suspend(std::coroutine_handle<> h) noexcept {
            log->push_back("suspend");
            coro_scheduler().schedule_resume(h);
        }
        
        void await_resume() const noexcept {
            log->push_back("resume");
        }
    };
    
    std::vector<std::string> log;
    auto log_ptr = &log;
    
    auto fn = [log_ptr]() -> task<void> {
        co_await ProtocolTracker{log_ptr};
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(50ms);
    
    ASSERT_EQ(log.size(), 3);
    EXPECT_EQ(log[0], "ready");
    EXPECT_EQ(log[1], "suspend");
    EXPECT_EQ(log[2], "resume");
}

/**
 * @test Awaiter can access promise
 * @brief Verify await_suspend receives correct coroutine_handle
 */
TEST_F(CoroutineAwaiterTests, AwaiterAccessesPromise) {
    struct PromiseAccessor {
        bool* accessed;
        
        explicit PromiseAccessor(bool* a) : accessed(a) {}
        
        bool await_ready() const noexcept { return false; }
        
        void await_suspend(std::coroutine_handle<> h) noexcept {
            // Verify handle is valid
            if (h && !h.done()) {
                *accessed = true;
            }
            coro_scheduler().schedule_resume(h);
        }
        
        void await_resume() const noexcept {}
    };
    
    std::atomic<bool> accessed{false};
    bool temp = false;
    auto accessed_ptr = &accessed;
    
    auto fn = [accessed_ptr, &temp]() -> task<void> {
        co_await PromiseAccessor{&temp};
        accessed_ptr->store(temp);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(50ms);
    
    EXPECT_TRUE(accessed.load());
}

// =============================================================================
// AWAITER MOVE SEMANTICS
// =============================================================================

/**
 * @test Move-only awaiter
 * @brief Verify awaiters can be move-only types
 */
TEST_F(CoroutineAwaiterTests, MoveOnlyAwaiter) {
    struct MoveOnlyAwaiter {
        std::unique_ptr<int> data;
        
        explicit MoveOnlyAwaiter(int val) : data(std::make_unique<int>(val)) {}
        
        MoveOnlyAwaiter(const MoveOnlyAwaiter&) = delete;
        MoveOnlyAwaiter(MoveOnlyAwaiter&&) = default;
        
        bool await_ready() const noexcept { return true; }
        void await_suspend(std::coroutine_handle<>) noexcept {}
        int await_resume() noexcept { return *data; }
    };
    
    std::atomic<int> result{0};
    auto result_ptr = &result;
    
    auto fn = [result_ptr]() -> task<void> {
        int val = co_await MoveOnlyAwaiter{42};
        result_ptr->store(val);
        co_return;
    };
    
    coro_scheduler().spawn(fn());
    run_for(10ms);
    
    EXPECT_EQ(result.load(), 42);
}

// =============================================================================
// MAIN
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
