/**
 * @file qb/io/tests/coroutine/test-coroutine-combinators.cpp
 * @brief Coroutine combinator tests
 *
 * This file contains tests for coroutine composition helpers, including when_all,
 * when_any, race, timeout handling, vector overloads, result extraction, structured
 * binding support, and exception propagation.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
#include <tuple>
#include <set>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: when_all Pattern
// =============================================================================

class CoroutineWhenAll : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test when_all with heterogeneous types
 * @brief Wait for multiple coroutines with different types
 */
TEST_F(CoroutineWhenAll, WhenAllMixedTypes) {
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
    auto coro_fn      = [all_done_ptr, &task1, &task2]() -> task<void> {
        auto t1 = task1();
        auto t2 = task2();

        // Wait for both
        int         r1 = co_await t1;
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
 * @test when_all helper function
 * @brief Test the when_all combinator
 */
TEST_F(CoroutineWhenAll, WhenAllHelper) {
    std::atomic<int> sum{0};

    auto worker = [&sum](int value, int delay_ms) -> task<int> {
        co_await sleep(std::chrono::milliseconds(delay_ms));
        sum += value;
        co_return value;
    };

    auto coro_fn = [&worker]() -> task<void> {
        auto [r1, r2, r3] = co_await when_all(worker(10, 30), worker(20, 20), worker(30, 10));

        EXPECT_EQ(r1, 10);
        EXPECT_EQ(r2, 20);
        EXPECT_EQ(r3, 30);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    EXPECT_EQ(sum, 60);
}

/**
 * @test when_all with void tasks
 * @brief Wait for multiple void coroutines
 */
TEST_F(CoroutineWhenAll, WhenAllVoidTasks) {
    constexpr int     count = 5;
    std::atomic<int>  completed{0};
    std::atomic<bool> all_done{false};

    auto completed_ptr = &completed;
    auto all_done_ptr  = &all_done;

    auto worker_fn = [completed_ptr](int delay_ms) -> task<void> {
        co_await sleep(std::chrono::milliseconds(delay_ms));
        completed_ptr->fetch_add(1);
    };

    auto coro_fn = [all_done_ptr, &worker_fn]() -> task<void> {
        auto t1 = worker_fn(10);
        auto t2 = worker_fn(20);
        auto t3 = worker_fn(30);
        auto t4 = worker_fn(15);
        auto t5 = worker_fn(25);

        co_await t1;
        co_await t2;
        co_await t3;
        co_await t4;
        co_await t5;

        (*all_done_ptr) = true;
    };
    auto coordinator = coro_fn();

    coro_scheduler().spawn(std::move(coordinator));
    run_for(200ms);

    EXPECT_EQ(completed, count);
    EXPECT_TRUE(all_done);
}

/**
 * @test when_all with vector
 * @brief Wait for vector of coroutines
 */
TEST_F(CoroutineWhenAll, WhenAllVector) {
    constexpr int    count = 10;
    std::atomic<int> sum{0};

    auto worker_fn = [&sum](int value) -> task<int> {
        co_await sleep(10ms);
        sum += value;
        co_return value;
    };

    auto coro_fn = [&]() -> task<void> {
        std::vector<task<int>> tasks;
        for (int i = 0; i < count; ++i) {
            tasks.push_back(worker_fn(i));
        }
        auto results = co_await when_all(std::move(tasks));
        EXPECT_EQ(results.size(), static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            EXPECT_EQ(results[i], i);
        }
    };

    coro_scheduler().spawn(coro_fn());
    run_for(500ms);
    EXPECT_EQ(sum, 45);
}

/**
 * @test when_all with empty set
 * @brief when_all completes immediately with no tasks
 */
TEST_F(CoroutineWhenAll, WhenAllEmpty) {
    auto coro_fn = []() -> task<void> {
        // Empty when_all should complete immediately
        auto results = co_await when_all(std::vector<task<int>>());
        EXPECT_TRUE(results.empty());
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(10ms);
}

// =============================================================================
// TEST SUITE: when_any Pattern
// =============================================================================

class CoroutineWhenAny : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test when_any returns first completed
 * @brief Race between coroutines
 */
TEST_F(CoroutineWhenAny, WhenAnyBasic) {
    std::atomic<bool> done{false};

    auto worker = [](int delay_ms, int result) -> task<int> {
        co_await sleep(std::chrono::milliseconds(delay_ms));
        co_return result;
    };

    auto coro_fn = [&done, &worker]() -> task<void> {
        auto [index, value] = co_await when_any(worker(100, 1), worker(50, 2), worker(200, 3));

        // Second task (50ms) should win
        EXPECT_EQ(index, 1u);
        EXPECT_EQ(std::any_cast<int>(value), 2);
        done = true;
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    EXPECT_TRUE(done);
}

/**
 * @test when_any with vector
 * @brief Race with vector of coroutines
 */
TEST_F(CoroutineWhenAny, WhenAnyVector) {
    constexpr int    count = 10;
    std::atomic<int> winner_index{-1};

    auto worker_fn = [](int index, int delay_ms) -> task<int> {
        co_await sleep(std::chrono::milliseconds(delay_ms));
        co_return index;
    };

    auto coro_fn = [&worker_fn, &winner_index]() -> task<void> {
        std::vector<task<int>> tasks;
        for (int i = 0; i < count; ++i) {
            tasks.push_back(worker_fn(i, 10 + i * 5));
        }

        auto [index, value] = co_await when_any(std::move(tasks));

        // First task should win (10ms)
        EXPECT_EQ(index, 0);
        EXPECT_EQ(std::any_cast<int>(value), 0);
        winner_index = index;
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(100ms);

    EXPECT_EQ(winner_index, 0);
}

// =============================================================================
// TEST SUITE: race Pattern
// =============================================================================

class CoroutineRace : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test race is alias for when_any
 * @brief Race between coroutines
 */
TEST_F(CoroutineRace, RaceBasic) {
    auto worker = [](int delay_ms, int result) -> task<int> {
        co_await sleep(std::chrono::milliseconds(delay_ms));
        co_return result;
    };

    auto coro_fn = [&worker]() -> task<void> {
        auto [index, value] = co_await race(worker(100, 1), worker(30, 2), worker(200, 3));

        EXPECT_EQ(index, 1u); // 30ms wins
        EXPECT_EQ(std::any_cast<int>(value), 2);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(100ms);
}

// =============================================================================
// TEST SUITE: with_timeout
// =============================================================================

class CoroutineTimeout : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test with_timeout success case
 * @brief Operation completes before timeout
 */
TEST_F(CoroutineTimeout, WithTimeoutSuccess) {
    std::atomic<bool> done{false};
    auto              done_ptr = &done;

    auto coro_fn = [done_ptr]() -> task<void> {
        auto result = co_await coro_with_timeout(
            []() -> task<int> {
                co_await sleep(50ms);
                co_return 42;
            }(),
            100ms);

        EXPECT_EQ(result, 42);
        *done_ptr = true;
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(200ms);

    EXPECT_TRUE(done);
}

/**
 * @test with_timeout timeout case
 * @brief Operation exceeds timeout
 */
TEST_F(CoroutineTimeout, WithTimeoutTimeout) {
    std::atomic<bool> caught{false};
    auto              caught_ptr = &caught;

    auto coro_fn = [caught_ptr]() -> task<void> {
        try {
            co_await coro_with_timeout(
                []() -> task<int> {
                    co_await sleep(100ms);
                    co_return 42;
                }(),
                50ms);
        } catch (const timeout_error &) {
            *caught_ptr = true;
        }
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(200ms);

    EXPECT_TRUE(caught);
}

/**
 * @test with_timeout with void task
 * @brief Timeout with void return type
 */
TEST_F(CoroutineTimeout, WithTimeoutVoidSuccess) {
    std::atomic<bool> done{false};
    auto              done_ptr = &done;

    auto coro_fn = [done_ptr]() -> task<void> {
        auto op = [done_ptr]() -> task<void> {
            co_await sleep(30ms);
            *done_ptr = true;
        };
        co_await coro_with_timeout(op(), 100ms);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(150ms);

    EXPECT_TRUE(done);
}

/**
 * @test with_timeout void timeout case
 * @brief Void task times out
 */
TEST_F(CoroutineTimeout, WithTimeoutVoidTimeout) {
    std::atomic<bool> caught{false};
    auto              caught_ptr = &caught;

    auto coro_fn = [caught_ptr]() -> task<void> {
        try {
            co_await coro_with_timeout(
                []() -> task<void> {
                    co_await sleep(100ms);
                }(),
                50ms);
        } catch (const timeout_error &) {
            *caught_ptr = true;
        }
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(200ms);

    EXPECT_TRUE(caught);
}

/**
 * @test race with vector of tasks
 * @brief First completing task wins
 */
TEST_F(CoroutineRace, RaceVector) {
    std::atomic<int> winner{-1};
    auto             worker = [](int id, int delay_ms) -> task<int> {
        co_await sleep(std::chrono::milliseconds(delay_ms));
        co_return id;
    };

    auto coro_fn = [&worker, &winner]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.push_back(worker(1, 80));
        tasks.push_back(worker(2, 30));
        tasks.push_back(worker(3, 50));
        auto [index, value] = co_await race(std::move(tasks));
        winner              = std::any_cast<int>(value);
        EXPECT_EQ(index, 1u);
        EXPECT_EQ(winner, 2);
    };

    coro_scheduler().spawn(coro_fn());
    run_for(100ms);
    EXPECT_EQ(winner, 2);
}

/**
 * @test when_any result index and value access
 * @brief result.index and result.value (any_cast) work
 */
TEST_F(CoroutineWhenAny, WhenAnyResultIndexAndValue) {
    std::atomic<bool> done{false};
    auto              worker = [](int v) -> task<int> {
        co_await sleep(20ms);
        co_return v;
    };

    auto coro_fn = [&worker, &done]() -> task<void> {
        auto result = co_await when_any(worker(10), worker(30), worker(20));
        int  val    = std::any_cast<int>(result.value);
        EXPECT_TRUE(val == 10 || val == 20 || val == 30);
        EXPECT_LT(result.index, 3u);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(100ms);
    EXPECT_TRUE(done);
}

// =============================================================================
// TEST SUITE: Lifetime & Lambda Safety
// Stresses the shared_ptr<state_t> lifetime pattern introduced to fix
// dangling-[this] captures in the spawned run_one coroutines.
// =============================================================================

class CoroutineLifetime : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test when_any first task completes instantly; the losing branches are reclaimed
 * @brief The instant winner finishes before the slow branches even start their sleep;
 *        when_any tears the losers down (frame + timer) instead of letting them run to
 *        completion, so only the winner's body runs. Validates the detached-frame
 *        reclamation fix (losers no longer run detached — finding 2.B.3, superseded).
 */
TEST_F(CoroutineLifetime, WhenAnyFirstTaskImmediateOthersLong) {
    std::atomic<bool> done{false};
    std::atomic<int>  completed_count{0};

    auto fast = [&completed_count]() -> task<int> {
        ++completed_count;
        co_return 1;
    };
    auto slow = [&completed_count](int ms) -> task<int> {
        co_await sleep(std::chrono::milliseconds(ms));
        ++completed_count;
        co_return ms;
    };

    auto coro_fn = [&]() -> task<void> {
        auto res = co_await when_any(fast(), slow(50), slow(80));
        EXPECT_EQ(res.index, 0u);
        EXPECT_EQ(std::any_cast<int>(res.value), 1);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(200ms);
    EXPECT_TRUE(done);
    // Only the winner ran: the two slow losers were torn down before their sleep
    // could resume (pre-fix this was 3 — winner + 2 losers run to completion).
    EXPECT_EQ(completed_count.load(), 1);
}

/**
 * @test when_any all tasks complete at the same "tick"
 * @brief No delay: tests the first-winner logic. The branches are spawned (queued) but
 *        not yet run when the first-popped branch wins instantly, so the winner reclaims
 *        the other two before they ever execute — only one branch body runs.
 */
TEST_F(CoroutineLifetime, WhenAnyAllImmediate) {
    std::atomic<bool> done{false};
    std::atomic<int>  run_count{0};

    auto instant = [&run_count](int v) -> task<int> {
        ++run_count;
        co_return v;
    };

    auto coro_fn = [&]() -> task<void> {
        auto res = co_await when_any(instant(10), instant(20), instant(30));
        // Exactly one winner; index in [0,2], value matches index*10+10
        EXPECT_LT(res.index, 3u);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(50ms);
    EXPECT_TRUE(done);
    // The winner reclaims the still-queued losers before they run (pre-fix this was 3).
    EXPECT_EQ(run_count.load(), 1);
}

/**
 * @test when_all with many tasks
 * @brief Stress test with 20 tasks to ensure no per-task allocation or CAS bug.
 */
TEST_F(CoroutineLifetime, WhenAllLargeCount) {
    std::atomic<bool> done{false};
    constexpr int     N = 20;

    auto worker = [](int v) -> task<int> {
        co_await sleep(10ms);
        co_return v;
    };

    // Build a when_all over a vector (uses the vector overload).
    auto coro_fn = [&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.reserve(N);
        for (int i = 0; i < N; ++i)
            tasks.push_back(worker(i));
        auto results = co_await when_all(std::move(tasks));
        EXPECT_EQ(static_cast<int>(results.size()), N);
        int expected_sum = N * (N - 1) / 2;
        int actual_sum   = 0;
        for (auto v : results)
            actual_sum += v;
        EXPECT_EQ(actual_sum, expected_sum);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(300ms);
    EXPECT_TRUE(done);
}

/**
 * @test coro_with_timeout where operation completes well before the deadline
 * @brief Verifies the timeout coroutine (spawned as a separate task) cleans up
 *        safely after the main operation wins and the awaiter is gone.
 */
TEST_F(CoroutineLifetime, WithTimeoutWinnerBeforeDeadline) {
    std::atomic<bool> done{false};

    auto coro_fn = [&]() -> task<void> {
        auto result = co_await coro_with_timeout(
            []() -> task<int> {
                co_await sleep(10ms);
                co_return 99;
            }(),
            200ms);
        EXPECT_EQ(result, 99);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(300ms);
    EXPECT_TRUE(done);
}

/**
 * @test coro_with_timeout where timeout fires before the operation
 * @brief The timeout-loser path: operation's run_one coroutine must not crash
 *        after the awaiter is destroyed on timeout.
 */
TEST_F(CoroutineLifetime, WithTimeoutFiresBeforeOperation) {
    std::atomic<bool> timed_out{false};

    auto coro_fn = [&]() -> task<void> {
        try {
            co_await coro_with_timeout(
                []() -> task<int> {
                    co_await sleep(500ms);
                    co_return 1;
                }(),
                20ms);
            ADD_FAILURE() << "Expected timeout_error";
        } catch (const timeout_error &) {
            timed_out = true;
        }
    };

    coro_scheduler().spawn(coro_fn());
    run_for(600ms);
    EXPECT_TRUE(timed_out);
}

/**
 * @test when_any vector version — first task wins immediately
 * @brief Same lifetime stress as variadic version but using the vector overload.
 *        Note: the vector version returns std::pair<size_t, std::any>.
 */
TEST_F(CoroutineLifetime, WhenAnyVectorFirstWins) {
    std::atomic<bool> done{false};
    std::atomic<int>  count{0};

    auto make_task = [&count](int ms) -> task<int> {
        if (ms > 0)
            co_await sleep(std::chrono::milliseconds(ms));
        ++count;
        co_return ms;
    };

    auto coro_fn = [&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.push_back(make_task(0)); // instant winner
        tasks.push_back(make_task(50));
        tasks.push_back(make_task(80));
        // Vector overload returns std::pair<size_t, std::any>
        auto res = co_await when_any(std::move(tasks));
        EXPECT_EQ(res.first, 0u);
        EXPECT_EQ(std::any_cast<int>(res.second), 0);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(200ms);
    EXPECT_TRUE(done);
    // Vector when_any reclaims the losing branches on win, same as the variadic version:
    // only the instant winner ran (pre-fix this was 3 — losers ran to completion).
    EXPECT_EQ(count.load(), 1);
}

// =============================================================================
// Detached-frame reclamation: when_any tears down the losing branches the instant
// a winner is decided. The oracle is `live_frames` returning to baseline AFTER the
// winner resolves but LONG BEFORE the losers' (multi-second) sleeps could complete:
// pre-fix each loser left its run_one frame + inner task frame + armed ev_timer
// parked here; post-fix every loser frame/timer is reclaimed on win.
// =============================================================================

/**
 * @test when_any reclaims losing branch frames + timers on win (variadic)
 */
TEST_F(CoroutineLifetime, WhenAnyLosersReclaimedNoLeak) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    auto fast = []() -> task<int> {
        co_await sleep(5ms);
        co_return 1;
    };
    auto slow = []() -> task<int> {
        co_await sleep(5000ms); // far longer than the run window — would leak if not torn down
        co_return 2;
    };

    auto coro_fn = [&]() -> task<void> {
        auto res = co_await when_any(fast(), slow(), slow());
        EXPECT_EQ(res.index, 0u);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(300ms);
    coro_scheduler().run_ready(); // drain the winner's final-suspend defer-destroy
    EXPECT_TRUE(done);
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "when_any leaked " << (after - baseline) << " losing-branch frame(s) — losers must be reclaimed on win";
}

/**
 * @test when_any (vector overload) reclaims losing branch frames + timers on win
 */
TEST_F(CoroutineLifetime, WhenAnyVectorLosersReclaimedNoLeak) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    auto make = [](int ms) -> task<int> {
        co_await sleep(std::chrono::milliseconds(ms));
        co_return ms;
    };

    auto coro_fn = [&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.push_back(make(5));    // winner
        tasks.push_back(make(5000)); // loser — must not linger
        tasks.push_back(make(5000)); // loser — must not linger
        auto res = co_await when_any(std::move(tasks));
        EXPECT_EQ(res.first, 0u);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(300ms);
    coro_scheduler().run_ready();
    EXPECT_TRUE(done);
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "when_any(vector) leaked " << (after - baseline) << " losing-branch frame(s)";
}

/**
 * @test coro_with_timeout leaves no zombie timeout watcher when the operation wins
 * @brief The inner task completes (~10ms) far before a far-off timeout (10s). With the old
 *        design the timeout was a spawned `co_await sleep(timeout)` coroutine that stayed
 *        parked on its sleep for the FULL 10s after the task won — leaking its frame + the
 *        ev_timer it was parked on for the rest of the timeout window. The raw self-stopping
 *        ev_timer is instead stopped in finish() the instant the awaiter resumes. Oracle:
 *        live_frames returns to baseline long before the timeout would have elapsed.
 *        (Mirror of WithDeadlineTests.WithDeadlineOperationWonTimerReclaimedNoLeak.)
 */
TEST_F(CoroutineLifetime, WithTimeoutOperationWonNoZombieTimer) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    auto coro_fn = [&]() -> task<void> {
        auto result = co_await coro_with_timeout(
            []() -> task<int> {
                co_await sleep(10ms);
                co_return 7;
            }(),
            10000ms); // far off — pre-fix the timeout coroutine lingers here for 10s
        EXPECT_EQ(result, 7);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(150ms);
    coro_scheduler().run_ready(); // drain the winner's final-suspend defer-destroy
    EXPECT_TRUE(done);
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "coro_with_timeout leaked " << (after - baseline)
                               << " frame(s) — the timeout watcher must be stopped when the operation wins, "
                                  "not left parked for the rest of the timeout window";
}

/**
 * @test coro_with_timeout (void overload) leaves no zombie timeout watcher when the op wins
 * @brief Same oracle as the non-void case, exercising timeout_awaiter<void>.
 */
TEST_F(CoroutineLifetime, WithTimeoutVoidOperationWonNoZombieTimer) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    auto coro_fn = [&]() -> task<void> {
        co_await coro_with_timeout(
            []() -> task<void> {
                co_await sleep(10ms);
            }(),
            10000ms);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(150ms);
    coro_scheduler().run_ready();
    EXPECT_TRUE(done);
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "coro_with_timeout<void> leaked " << (after - baseline) << " frame(s)";
}

// Free-function coroutines (not lambdas) for the nested-race test: a lambda
// coroutine's closure would have to outlive the frame, which is fiddly here.
namespace {
task<int>
nested_slow_5s() {
    co_await sleep(5000ms);
    co_return 9;
}
task<int>
nested_inner_race() {
    // Parks on an inner when_any that never wins in the test window; when the OUTER
    // when_any reclaims this branch, destroying this frame runs the inner
    // when_any_awaiter destructor → reclaim_all() tears down the two slow branches.
    auto r = co_await when_any(nested_slow_5s(), nested_slow_5s());
    co_return r.get<int>();
}
task<int>
nested_fast_5ms() {
    co_await sleep(5ms);
    co_return 1;
}
} // namespace

/**
 * @test when_any reclaims a losing branch that is itself parked on a nested when_any
 * @brief Exercises the awaiter-destructor (reclaim_all) path: the outer race's loser is
 *        an inner when_any over two long sleeps. On the outer winner, reclaiming that
 *        branch destroys the inner when_any_awaiter, which must tear down its own still
 *        -parked branches. Oracle: live_frames returns to baseline (whole nest reclaimed).
 */
TEST_F(CoroutineLifetime, WhenAnyNestedLoserDtorReclaimsNoLeak) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    auto coro_fn = [&]() -> task<void> {
        auto res = co_await when_any(nested_fast_5ms(), nested_inner_race());
        EXPECT_EQ(res.index, 0u);
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(300ms);
    coro_scheduler().run_ready();
    EXPECT_TRUE(done);
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "nested when_any leaked " << (after - baseline)
                               << " frame(s) — the inner race's branches must be reclaimed via the awaiter dtor";
}

// =============================================================================
// TEST SUITE: when_any_result API
// =============================================================================

class WhenAnyResultTests : public ::testing::Test {
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

TEST_F(WhenAnyResultTests, GetExtractsTypedValue) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto result = co_await when_any(
            []() -> task<int> {
                co_return 42;
            }(),
            []() -> task<std::string> {
                co_await sleep(1s);
                co_return "slow";
            }());
        auto val = result.get<int>();
        EXPECT_EQ(val, 42);
        EXPECT_EQ(result.index, 0u);
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(WhenAnyResultTests, HasExceptionReflectsState) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto result = co_await when_any(
            []() -> task<int> {
                throw std::runtime_error("boom");
                co_return 0;
            }(),
            []() -> task<int> {
                co_await sleep(1s);
                co_return 99;
            }());
        EXPECT_TRUE(result.has_exception());
        EXPECT_THROW(result.get<int>(), std::runtime_error);
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(WhenAnyResultTests, HasExceptionFalseOnSuccess) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto result = co_await when_any(
            []() -> task<int> {
                co_return 1;
            }(),
            []() -> task<int> {
                co_await sleep(1s);
                co_return 2;
            }());
        EXPECT_FALSE(result.has_exception());
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(WhenAnyResultTests, StructuredBindingDecomposition) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto result = co_await when_any(
            []() -> task<int> {
                co_return 7;
            }(),
            []() -> task<int> {
                co_await sleep(1s);
                co_return 0;
            }());
        auto [idx, val] = result;
        EXPECT_EQ(idx, 0u);
        EXPECT_EQ(std::any_cast<int>(val), 7);
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(WhenAnyResultTests, WhenAllWithExceptionPropagation) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await when_all(
                []() -> task<int> {
                    co_return 1;
                }(),
                []() -> task<int> {
                    throw std::runtime_error("fail");
                    co_return 0;
                }());
            ADD_FAILURE() << "Should have thrown";
        } catch (const std::runtime_error &e) {
            EXPECT_STREQ(e.what(), "fail");
        }
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
