/**
 * @file unit/coroutine/patterns-idioms.cpp
 * @brief Real-world async idioms built on the qb-io coroutine primitives.
 *
 * Where the other coroutine files pin individual API contracts, this file proves the framework composes
 * into the recipes applications actually write: fan-out-and-collect (when_all by hand), a barrier, a
 * staged pipeline, bounded producer/consumer, sequential dependent async steps (DB-query shape), retry
 * with backoff, a poll-with-timeout, a circuit breaker, and primary/fallback. Each idiom is built only
 * from `task<T>` + `sleep()` + the scheduler, runs in-process on the event loop (`init()` via
 * `reset_async_context()`, `spawn`, the de-flake pump `pump_until`) — NO sockets, NO daemon, NO TLS — so
 * this is a `unit` test (labelled `idiom`). qb-io ships real coroutine latch/semaphore/channel primitives
 * (covered in coroutine-sync-primitives.cpp / the channel tests); the hand-rolled barrier/producer-consumer
 * here intentionally exercise the bare scheduling fabric.
 *
 * Hardened over the original test-coroutine-patterns.cpp and enriched from the dissolved
 * test-coroutine-comprehensive.cpp:
 *   - the two stale "NOTE: Disabled" header comments are removed (the tests were never actually disabled);
 *   - every indexed access into a result vector is now guarded by ASSERT_EQ on its size first, so a
 *     partial run fails loudly instead of reading out of bounds (UB);
 *   - every fixed `run_for(Nms)` budget is replaced by a completion-flag + `pump_until`, and a TearDown
 *     drain is added so a timed-out test cannot leak in-flight frames into the next;
 *   - salvaged real-world idioms folded in: SequentialAsyncOperations, RetryWithBackoff,
 *     ProducerConsumerPattern, plus the composition idioms ChainOfThreeCoroutines and NoSuspensionPoints.
 * No file-local main(): shared gtest_main.
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace {

// Shared fixture base: fresh per-test loop, drain + reset on teardown so a timed-out idiom cannot leak
// in-flight frames into the next test.
class CoroutineIdiomFixture : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// =============================================================================
// FAN-OUT / SYNCHRONIZATION IDIOMS
// =============================================================================

class CoroutineSyncPatterns : public CoroutineIdiomFixture {};

/**
 * @test Fan-out then collect every result (hand-rolled when_all)
 * @brief Spawn N worker tasks, await each, collect results; assert the full multiset of expected values.
 */
TEST_F(CoroutineSyncPatterns, WhenAllWithResults) {
    constexpr int     count = 5;
    std::vector<int>  results;
    std::atomic<bool> done{false};

    auto results_ptr = &results;
    auto done_ptr    = &done;
    coro_scheduler().spawn([results_ptr, done_ptr]() -> task<void> {
        auto worker = [](int id) -> task<int> {
            co_await sleep(std::chrono::milliseconds(10 + id * 5));
            co_return id * 10;
        };

        std::vector<task<int>> tasks;
        for (int i = 0; i < count; ++i) {
            tasks.push_back(worker(i));
        }
        for (size_t i = 0; i < tasks.size(); ++i) {
            results_ptr->push_back(co_await tasks[i]);
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "fan-out coordinator never completed";

    ASSERT_EQ(results.size(), static_cast<size_t>(count)); // OOB guard before indexed access
    std::sort(results.begin(), results.end());
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(results[i], i * 10);
    }
}

/**
 * @test Barrier: all workers must reach the rendezvous before any continues
 * @brief Three workers do staggered work, increment a shared counter, then spin-wait until all have
 *        arrived before completing. All three complete exactly once.
 */
TEST_F(CoroutineSyncPatterns, BarrierPattern) {
    constexpr int    count = 3;
    std::atomic<int> at_barrier{0};
    std::atomic<int> completed{0};

    auto worker = [&at_barrier, &completed](int id) -> task<void> {
        co_await sleep(std::chrono::milliseconds(10 + id * 10));
        at_barrier.fetch_add(1);
        while (at_barrier.load() < count) {
            co_await sleep(5ms);
        }
        completed.fetch_add(1);
        co_return;
    };

    for (int i = 0; i < count; ++i) {
        coro_scheduler().spawn(worker(i));
    }

    EXPECT_TRUE(pump_until([&] { return completed.load() == count; })) << "not all workers passed the barrier";
    EXPECT_EQ(completed.load(), count);
}

/**
 * @test Two-stage pipeline with per-item ordering
 * @brief Each input flows through stage1 (x*2) then stage2 (+10); the produced vector is exact and ordered.
 */
TEST_F(CoroutineSyncPatterns, PipelineWithBackpressure) {
    constexpr int     count = 5;
    std::vector<int>  results;
    std::atomic<bool> done{false};

    auto results_ptr = &results;
    auto done_ptr    = &done;
    coro_scheduler().spawn([results_ptr, done_ptr]() -> task<void> {
        auto stage1 = [](int x) -> task<int> {
            co_await sleep(5ms);
            co_return x * 2;
        };
        auto stage2 = [](int x) -> task<int> {
            co_await sleep(5ms);
            co_return x + 10;
        };
        for (int i = 1; i <= count; ++i) {
            int r1 = co_await stage1(i);
            int r2 = co_await stage2(r1);
            results_ptr->push_back(r2);
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "pipeline never completed";

    ASSERT_EQ(results.size(), static_cast<size_t>(count)); // OOB guard before indexed access
    for (size_t i = 0; i < results.size(); ++i) {
        const int input    = static_cast<int>(i) + 1;
        const int expected = (input * 2) + 10;
        EXPECT_EQ(results[i], expected);
    }
}

/**
 * @test Bounded producer/consumer
 * @brief Producer never overruns a 3-slot buffer; consumer drains all items. Exactly num_items flow.
 */
TEST_F(CoroutineSyncPatterns, ProducerConsumerBounded) {
    constexpr int     num_items   = 10;
    constexpr int     buffer_size = 3;
    std::vector<int>  buffer;
    std::atomic<int>  produced{0};
    std::atomic<int>  consumed{0};
    // Gate on actual coroutine COMPLETION, not an intermediate counter: when `consumed` first hits
    // num_items the consumer is still parked on its trailing `sleep()` and has NOT yet co_returned.
    // Returning on the counter alone ends the test with that coroutine still live, so TearDown's
    // run_for() would resume it and read these now-destroyed stack atomics (ASan: stack-use-after-scope).
    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_done{false};

    auto producer = [&buffer, &produced, &producer_done]() -> task<void> {
        for (int i = 0; i < num_items; ++i) {
            while (static_cast<int>(buffer.size()) >= buffer_size) {
                co_await sleep(2ms); // backpressure: wait for the consumer to drain
            }
            buffer.push_back(i);
            produced.fetch_add(1);
            co_await sleep(5ms);
        }
        producer_done.store(true);
        co_return;
    };

    auto consumer = [&buffer, &consumed, &consumer_done]() -> task<void> {
        while (consumed.load() < num_items) {
            if (!buffer.empty()) {
                buffer.erase(buffer.begin());
                consumed.fetch_add(1);
            }
            co_await sleep(7ms);
        }
        consumer_done.store(true);
        co_return;
    };

    coro_scheduler().spawn(producer());
    coro_scheduler().spawn(consumer());

    EXPECT_TRUE(pump_until([&] { return producer_done.load() && consumer_done.load(); }))
        << "producer/consumer never drained";
    EXPECT_EQ(produced.load(), num_items);
    EXPECT_EQ(consumed.load(), num_items);
}

// =============================================================================
// COMPOSITION IDIOMS (salvaged from the dissolved comprehensive suite)
// =============================================================================

class CoroutineCompositionPatterns : public CoroutineIdiomFixture {};

/**
 * @test Three-level coroutine chain interleaves as expected
 * @brief level1 → level2 → level3 with sleeps between; additive markers prove the exact interleave and
 *        the innermost return value bubbles up.
 *
 * Salvaged from test-coroutine-comprehensive.cpp::ChainOfThreeCoroutines.
 */
TEST_F(CoroutineCompositionPatterns, ChainOfThreeCoroutines) {
    std::atomic<int>  counter{0};
    std::atomic<int>  bottom_value{-1};
    std::atomic<bool> done{false};

    auto counter_ptr = &counter;
    auto bottom_ptr  = &bottom_value;
    auto done_ptr    = &done;
    coro_scheduler().spawn([counter_ptr, bottom_ptr, done_ptr]() -> task<void> {
        auto level3 = [counter_ptr]() -> task<int> {
            counter_ptr->fetch_add(1);
            co_return 100;
        };
        auto level2 = [counter_ptr, level3]() -> task<int> {
            co_await sleep(5ms);
            counter_ptr->fetch_add(10);
            int val = co_await level3();
            co_return val;
        };
        co_await sleep(5ms);
        counter_ptr->fetch_add(100);
        int val = co_await level2();
        bottom_ptr->store(val);
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine chain never completed";
    EXPECT_EQ(counter.load(), 111); // 100 (level1) + 10 (level2) + 1 (level3)
    EXPECT_EQ(bottom_value.load(), 100);
}

/**
 * @test A coroutine with no suspension points runs to completion in one drain
 * @brief A pure-compute coroutine body (no co_await) completes synchronously when first resumed.
 *
 * Salvaged from test-coroutine-comprehensive.cpp::NoSuspensionPoints.
 */
TEST_F(CoroutineCompositionPatterns, NoSuspensionPoints) {
    std::atomic<int> result{-1};

    auto result_ptr = &result;
    coro_scheduler().spawn([result_ptr]() -> task<void> {
        int sum = 0;
        for (int i = 0; i < 1000; ++i) {
            sum += i;
        }
        result_ptr->store(sum);
        co_return;
    });

    coro_scheduler().run_ready();
    EXPECT_EQ(result.load(), 499500); // sum 0..999
}

// =============================================================================
// TIMING / ORDERING IDIOMS
// =============================================================================

class CoroutineTimingPatterns : public CoroutineIdiomFixture {};

/**
 * @test Sequential timers fire in order with at-least-the-requested gaps
 * @brief A chain of sleeps records timestamps; each interval is at least its (tolerance-reduced) target.
 */
TEST_F(CoroutineTimingPatterns, SequentialTimers) {
    std::vector<std::chrono::steady_clock::time_point> timestamps;
    const auto                                         start = std::chrono::steady_clock::now();
    std::atomic<bool>                                  done{false};

    auto timestamps_ptr = &timestamps;
    auto done_ptr       = &done;
    coro_scheduler().spawn([timestamps_ptr, done_ptr]() -> task<void> {
        co_await sleep(20ms);
        timestamps_ptr->push_back(std::chrono::steady_clock::now());
        co_await sleep(30ms);
        timestamps_ptr->push_back(std::chrono::steady_clock::now());
        co_await sleep(20ms);
        timestamps_ptr->push_back(std::chrono::steady_clock::now());
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "sequential timers never completed";

    ASSERT_EQ(timestamps.size(), 3u); // OOB guard before indexed access
    const auto first  = timestamps[0] - start;
    const auto second = timestamps[1] - timestamps[0];
    const auto third  = timestamps[2] - timestamps[1];
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(first).count(), 15);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(second).count(), 25);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(third).count(), 15);
}

/**
 * @test Poll-with-timeout: a slow operation that overruns the deadline is reported as timed-out
 * @brief The watcher polls the operation's readiness for 50ms; the operation needs 100ms, so the timeout
 *        path is taken and the (still-running) operation is observed as not-yet-complete.
 */
TEST_F(CoroutineTimingPatterns, TimeoutPattern) {
    std::atomic<bool> timeout_occurred{false};
    std::atomic<bool> operation_completed{false};
    std::atomic<bool> done{false};

    auto op_ptr      = &operation_completed;
    auto timeout_ptr = &timeout_occurred;
    auto done_ptr    = &done;
    coro_scheduler().spawn([op_ptr, timeout_ptr, done_ptr]() -> task<void> {
        auto slow_operation = [op_ptr]() -> task<void> {
            co_await sleep(100ms);
            op_ptr->store(true);
            co_return;
        };
        auto operation = slow_operation();
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < 50ms) {
            if (operation.handle().promise().is_ready()) {
                co_await operation;
                done_ptr->store(true);
                co_return;
            }
            co_await sleep(5ms);
        }
        timeout_ptr->store(true);
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "timeout watcher never completed";
    EXPECT_TRUE(timeout_occurred.load());
    EXPECT_FALSE(operation_completed.load());
}

// =============================================================================
// SEQUENTIAL DEPENDENT STEPS (DB-query shape) — salvaged
// =============================================================================

class CoroutineWorkflowPatterns : public CoroutineIdiomFixture {};

/**
 * @test Sequential dependent async operations thread their results
 * @brief query1 → query2(result1) → final(result2); the ordered trace is exact.
 *
 * Salvaged from test-coroutine-comprehensive.cpp::SequentialAsyncOperations.
 */
TEST_F(CoroutineWorkflowPatterns, SequentialAsyncOperations) {
    std::vector<std::string> operations;
    std::atomic<bool>        done{false};

    auto ops_ptr  = &operations;
    auto done_ptr = &done;
    coro_scheduler().spawn([ops_ptr, done_ptr]() -> task<void> {
        auto query1 = [ops_ptr]() -> task<std::string> {
            co_await sleep(10ms);
            ops_ptr->push_back("query1");
            co_return "result1";
        };
        auto query2 = [ops_ptr](std::string prev) -> task<std::string> {
            co_await sleep(10ms);
            ops_ptr->push_back("query2:" + prev);
            co_return "result2";
        };
        auto r1 = co_await query1();
        auto r2 = co_await query2(r1);
        ops_ptr->push_back("final:" + r2);
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "sequential operations never completed";

    ASSERT_EQ(operations.size(), 3u); // OOB guard before indexed access
    EXPECT_EQ(operations[0], "query1");
    EXPECT_EQ(operations[1], "query2:result1");
    EXPECT_EQ(operations[2], "final:result2");
}

/**
 * @test Retry with backoff stops at the first success
 * @brief A flaky operation succeeds on its 3rd attempt; the loop retries with growing backoff and stops.
 *
 * Salvaged from test-coroutine-comprehensive.cpp::RetryWithBackoff.
 */
TEST_F(CoroutineWorkflowPatterns, RetryWithBackoff) {
    constexpr int     max_retries = 5;
    std::atomic<int>  attempts{0};
    std::atomic<bool> succeeded{false};
    std::atomic<bool> done{false};

    auto attempts_ptr = &attempts;
    auto succeeded_ptr = &succeeded;
    auto done_ptr      = &done;
    coro_scheduler().spawn([attempts_ptr, succeeded_ptr, done_ptr]() -> task<void> {
        auto flaky = [attempts_ptr]() -> task<bool> {
            const int n = attempts_ptr->fetch_add(1) + 1;
            co_await sleep(5ms);
            co_return n >= 3; // succeed on the 3rd attempt
        };
        for (int i = 0; i < max_retries; ++i) {
            if (co_await flaky()) {
                succeeded_ptr->store(true);
                break;
            }
            co_await sleep(std::chrono::milliseconds(5 * (i + 1))); // backoff
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "retry loop never completed";
    EXPECT_TRUE(succeeded.load());
    EXPECT_EQ(attempts.load(), 3);
}

// =============================================================================
// ERROR-HANDLING IDIOMS
// =============================================================================

class CoroutineErrorPatterns : public CoroutineIdiomFixture {};

/**
 * @test Circuit breaker opens after the failure threshold
 * @brief Repeated failures trip the breaker open and stop further attempts at exactly the threshold.
 */
TEST_F(CoroutineErrorPatterns, CircuitBreaker) {
    constexpr int     failure_threshold = 3;
    std::atomic<int>  failures{0};
    std::atomic<int>  attempts{0};
    std::atomic<bool> circuit_open{false};
    std::atomic<bool> done{false};

    auto failures_ptr = &failures;
    auto attempts_ptr = &attempts;
    auto open_ptr     = &circuit_open;
    auto done_ptr     = &done;
    coro_scheduler().spawn([failures_ptr, attempts_ptr, open_ptr, done_ptr]() -> task<void> {
        auto flaky_operation = [attempts_ptr]() -> task<bool> {
            attempts_ptr->fetch_add(1);
            co_await sleep(5ms);
            throw std::runtime_error("failure"); // always fails in this scenario
            co_return true;
        };
        for (int i = 0; i < 5; ++i) {
            if (failures_ptr->load() >= failure_threshold) {
                open_ptr->store(true);
                break;
            }
            try {
                co_await flaky_operation();
            } catch (...) {
                failures_ptr->fetch_add(1);
            }
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "circuit-breaker loop never completed";
    EXPECT_TRUE(circuit_open.load());
    EXPECT_EQ(failures.load(), failure_threshold);
}

/**
 * @test Primary/fallback: the fallback supplies the value when the primary throws
 * @brief The primary fails after a delay; the fallback runs and its value is the final result.
 */
TEST_F(CoroutineErrorPatterns, FallbackPattern) {
    std::atomic<bool> used_fallback{false};
    std::atomic<int>  result{0};
    std::atomic<bool> done{false};

    auto used_ptr   = &used_fallback;
    auto result_ptr = &result;
    auto done_ptr   = &done;
    coro_scheduler().spawn([used_ptr, result_ptr, done_ptr]() -> task<void> {
        auto primary = []() -> task<int> {
            co_await sleep(10ms);
            throw std::runtime_error("primary failed");
            co_return 1;
        };
        auto fallback = [used_ptr]() -> task<int> {
            co_await sleep(5ms);
            used_ptr->store(true);
            co_return 42;
        };
        bool use_fallback = false;
        try {
            result_ptr->store(co_await primary());
        } catch (...) {
            use_fallback = true;
        }
        if (use_fallback) {
            result_ptr->store(co_await fallback());
        }
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "primary/fallback never completed";
    EXPECT_TRUE(used_fallback.load());
    EXPECT_EQ(result.load(), 42);
}
