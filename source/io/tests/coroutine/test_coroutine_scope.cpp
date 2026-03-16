/**
 * @file test_coroutine_scope.cpp
 * @brief Coroutine scope tests
 *
 * Tests for:
 * - coroutine_scope: lifetime management
 * - joining_scope: RAII join
 * - spawn/join operations
 *
 * @author qb - C++ Actor Framework
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>
#include <numeric>
#include <vector>
#if defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE
#include <cstdio>
#endif

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Coroutine Scope Basic
// =============================================================================

class ScopeBasicTests : public ::testing::Test {
protected:
    void SetUp() override {
#if defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE
        std::fprintf(stderr, "[test] SetUp ScopeBasicTests\n");
#endif
        qb::io::async::init();
    }
    void TearDown() override {
#if defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE
        std::fprintf(stderr, "[test] TearDown ScopeBasicTests drain then clear\n");
#endif
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
        }
        qb::io::async::listener::current.clear();
#if defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE
        std::fprintf(stderr, "[test] TearDown done\n");
#endif
    }
};

/**
 * @test Spawn and join all
 * @brief Wait for all tasks
 */
TEST_F(ScopeBasicTests, SpawnAndJoinAll) {
#if defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE
    std::fprintf(stderr, "[test] RUN SpawnAndJoinAll\n");
#endif
    std::atomic<int> counter{0};

    auto worker = [&counter]() -> task<void> {
        co_await sleep(20ms);
        counter++;
    };

    auto coro_fn = [&worker]() -> task<void> {
        coroutine_scope scope;

        scope.spawn(worker());
        scope.spawn(worker());
        scope.spawn(worker());

        co_await sleep(1ms);  // Yield so workers can be scheduled
        EXPECT_EQ(scope.total_count(), 3);

        co_await scope.join_all();

        EXPECT_EQ(scope.active_count(), 0);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(200ms);

    EXPECT_EQ(counter, 3);
#if defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE
    std::fprintf(stderr, "[test] END SpawnAndJoinAll OK\n");
#endif
}

/**
 * @test Join any
 * @brief Wait for first completion
 */
TEST_F(ScopeBasicTests, JoinAny) {
#if defined(QB_DEBUG_SCOPE) && QB_DEBUG_SCOPE
    std::fprintf(stderr, "[test] RUN JoinAny\n");
#endif
    std::atomic<int> counter{0};

    auto fast_worker = [&counter]() -> task<void> {
        co_await sleep(10ms);
        counter++;
    };

    auto slow_worker = [&counter]() -> task<void> {
        co_await sleep(100ms);
        counter++;
    };

    auto coro_fn = [&fast_worker, &slow_worker]() -> task<void> {
        coroutine_scope scope;

        scope.spawn(fast_worker());
        scope.spawn(slow_worker());

        size_t index = co_await scope.join_any();

        // Fast worker should complete first
        EXPECT_EQ(index, 0);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(200ms);
}

/**
 * @test Join all with timeout
 * @brief Timeout if not complete
 */
TEST_F(ScopeBasicTests, JoinAllTimeout) {
    auto slow_worker = []() -> task<void> {
        co_await sleep(500ms);
    };

    auto coro_fn = [&slow_worker]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(slow_worker());

        bool completed = co_await scope.join_all_for(50ms);

        EXPECT_FALSE(completed);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(100ms);
}

/**
 * @test Join all for - success when tasks complete in time
 * @brief join_all_for returns true when all complete before timeout
 */
TEST_F(ScopeBasicTests, JoinAllForSuccess) {
    std::atomic<int> counter{0};
    auto worker = [&counter]() -> task<void> {
        co_await sleep(10ms);
        counter++;
    };

    auto coro_fn = [&worker]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(worker());
        scope.spawn(worker());

        bool completed = co_await scope.join_all_for(100ms);
        EXPECT_TRUE(completed);
    };

    coro_scheduler().spawn(coro_fn());
    run_for(150ms);
    EXPECT_EQ(counter, 2);
}

/**
 * @test Active count decreases as tasks complete
 * @brief active_count() reflects running vs completed tasks
 */
TEST_F(ScopeBasicTests, ActiveCountDuringRun) {
    std::atomic<size_t> active_at_start{0};
    std::atomic<size_t> active_after_one{0};

    auto slow = []() -> task<void> { co_await sleep(100ms); };
    auto fast = []() -> task<void> { co_await sleep(10ms); };

    auto coro_fn = [&slow, &fast, &active_at_start, &active_after_one]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(slow());
        scope.spawn(fast());
        active_at_start = scope.active_count();
        EXPECT_EQ(active_at_start, 2u);

        co_await sleep(50ms);
        active_after_one = scope.active_count();
        EXPECT_EQ(active_after_one, 1u);

        co_await scope.join_all();
        EXPECT_EQ(scope.active_count(), 0u);
    };

    coro_scheduler().spawn(coro_fn());
    run_for(200ms);
    EXPECT_EQ(active_at_start, 2u);
    EXPECT_EQ(active_after_one, 1u);
}

/**
 * @test Rethrow if error
 * @brief rethrow_if_error() propagates first task exception
 */
TEST_F(ScopeBasicTests, RethrowIfError) {
    auto throwing_worker = []() -> task<void> {
        co_await sleep(5ms);
        throw std::runtime_error("task failed");
    };

    std::atomic<bool> caught{false};
    auto coro_fn = [&throwing_worker, &caught]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(throwing_worker());
        co_await sleep(50ms);
        try {
            scope.rethrow_if_error();
        } catch (const std::runtime_error& e) {
            EXPECT_STREQ(e.what(), "task failed");
            caught = true;
        }
    };

    coro_scheduler().spawn(coro_fn());
    run_for(100ms);
    EXPECT_TRUE(caught);
}

/**
 * @test Cancel all
 * @brief cancel_all() stops tasks that cooperatively check the token.
 *
 * Design notes:
 * - The cancel token is created externally and shared with the worker via
 *   a copy (value semantics, reference-counted internally).  This lets the
 *   worker call cancellable_sleep(duration, token) so it wakes up immediately
 *   when the token fires instead of blocking for the full sleep.
 * - The result flag is a shared_ptr<atomic> so that the worker coroutine
 *   frame cannot cause a use-after-free even if cleanup is slightly delayed.
 * - We join the scope after cancelling so every frame is guaranteed to have
 *   completed before coro_fn returns — no dangling references across tests.
 */
TEST_F(ScopeBasicTests, CancelAll) {
    auto worker_completed = std::make_shared<std::atomic<bool>>(false);
    cancellation_token tok;  // shared between caller and worker

    auto coro_fn = [worker_completed, tok]() mutable -> task<void> {
        coroutine_scope scope;

        // Named lambda stored in coro_fn's coroutine frame.
        // If the compiler stores a pointer to the lambda object in the worker
        // coroutine frame, that pointer stays valid because worker_fn lives
        // in coro_fn's frame which is alive until join_all() returns.
        auto worker_fn = [worker_completed, tok]() -> task<void> {
            for (int i = 0; i < 100; ++i)
                co_await cancellable_sleep(10ms, tok);
            *worker_completed = true;   // only reached if not cancelled
        };
        scope.spawn(worker_fn());

        co_await sleep(50ms);
        tok.cancel();               // cancel the shared token

        co_await scope.join_all();  // block until the worker has fully exited
    };

    coro_scheduler().spawn(coro_fn());
    run_for(500ms);

    EXPECT_FALSE(*worker_completed);
}

// =============================================================================
// TEST SUITE: Joining Scope
// =============================================================================

class JoiningScopeTests : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
        }
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test RAII join
 * @brief Scope joins on destruction
 */
TEST_F(JoiningScopeTests, RAIIJoin) {
    std::atomic<int> counter{0};

    auto worker = [&counter]() -> task<void> {
        co_await sleep(30ms);
        counter++;
    };

    {
        joining_scope scope;
        scope.spawn(worker());
        scope.spawn(worker());
        // Scope joins on destruction
    }

    run_for(100ms);

    EXPECT_EQ(counter, 2);
}

// =============================================================================
// TEST SUITE: Scope Edge Cases
// =============================================================================

class ScopeEdgeCases : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
        }
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Empty scope
 * @brief Join immediately returns
 */
TEST_F(ScopeEdgeCases, EmptyScope) {
    auto coro_fn = []() -> task<void> {
        coroutine_scope scope;

        EXPECT_TRUE(scope.empty());

        co_await scope.join_all();  // Should return immediately

        EXPECT_TRUE(scope.empty());
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(10ms);
}

/**
 * @test Prune completed
 * @brief Remove completed tasks
 */
TEST_F(ScopeEdgeCases, PruneCompleted) {
    auto worker = []() -> task<void> {
        co_await sleep(10ms);
    };

    auto coro_fn = [&worker]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(worker());
        scope.spawn(worker());

        co_await sleep(100ms);

        EXPECT_EQ(scope.active_count(), 0);
        EXPECT_EQ(scope.total_count(), 2);

        scope.prune_completed();

        EXPECT_EQ(scope.total_count(), 0);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(200ms);
}

// =============================================================================
// TEST SUITE: parallel_map
// Exercises the fixed detail::parallel_map_worker free function that replaced
// the local lambda to avoid dangling-pointer crashes in the per-iteration
// coroutine frames.
// =============================================================================

class ParallelMapTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
        }
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test parallel_map basic correctness
 * @brief Verify that all items are processed and results are correct.
 */
TEST_F(ParallelMapTests, BasicTransform) {
    std::atomic<bool> done{false};

    auto coro_fn = [&done]() -> task<void> {
        std::vector<int> items = {1, 2, 3, 4, 5};

        auto results = co_await parallel_map(items, [](int v) -> task<int> {
            co_await sleep(10ms);
            co_return v * v;
        });

        EXPECT_EQ(results.size(), 5u);
        int sum = 0;
        for (int r : results) sum += r;
        EXPECT_EQ(sum, 1 + 4 + 9 + 16 + 25);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(200ms);
    EXPECT_TRUE(done);
}

/**
 * @test parallel_map semaphore throttle — no over-permit
 * @brief Uses a counting latch to verify that at no point more than
 *        max_concurrency items occupy the "inside fn" region at the same time.
 *        Each task atomically increments on entry, decrements on exit; the
 *        semaphore in parallel_map_worker gates entry. Since we're single-
 *        threaded the CAS always wins on first try (no contention).
 */
TEST_F(ParallelMapTests, ConcurrencyLimit) {
    std::atomic<int>  violations{0};   // incremented if peak ever exceeds MAX
    std::atomic<int>  active{0};
    std::atomic<bool> done{false};
    constexpr int N   = 12;
    constexpr int MAX = 3;

    auto coro_fn = [&]() -> task<void> {
        std::vector<int> items(N);
        std::iota(items.begin(), items.end(), 0);

        auto results = co_await parallel_map(items, [&](int v) -> task<int> {
            int cur = ++active;
            if (cur > MAX) ++violations;
            co_await sleep(30ms);   // hold the "slot" long enough
            --active;
            co_return v;
        }, MAX);

        EXPECT_EQ(static_cast<int>(results.size()), N);
        EXPECT_EQ(violations.load(), 0) << "semaphore did not limit concurrency to " << MAX;
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(2000ms);
    EXPECT_TRUE(done);
}

/**
 * @test parallel_map with a large number of items
 * @brief Stress-tests the per-iteration free-function spawning to ensure no
 *        frame-pointer corruption across many loop iterations.
 */
TEST_F(ParallelMapTests, LargeInput) {
    std::atomic<bool> done{false};

    auto coro_fn = [&done]() -> task<void> {
        constexpr int N = 50;
        std::vector<int> items(N);
        std::iota(items.begin(), items.end(), 0);

        auto results = co_await parallel_map(items, [](int v) -> task<int> {
            co_return v * 2;
        }, 10);

        EXPECT_EQ(static_cast<int>(results.size()), N);
        int sum = 0;
        for (int r : results) sum += r;
        EXPECT_EQ(sum, N * (N - 1));  // sum of 0*2..49*2 = 2*(0+1+..+49) = 2*(49*50/2)
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(500ms);
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
