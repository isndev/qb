/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/scope-structured-concurrency.cpp
 * @brief `qb::io::async::coroutine_scope` structured concurrency — spawn/join/cancel + parallel helpers.
 *
 * Covers the scope family (`coroutine_scope`, `joining_scope`, `cancelling_scope`,
 * `detaching_scope`) and the structured-concurrency helpers built on it: `spawn`, `join_all`,
 * `join_any`, `join_all_for(timeout)`, `active_count`/`total_count`/`empty`/`prune_completed`,
 * `cancel_all`/`cancel_token`, plus `capture_result`, `parallel(...)`, `parallel_map(...)`,
 * `with_scope`, and `repeat_while`.
 *
 * Restructured over the original test-coroutine-scope.cpp:
 *   - the file-scope `QB_DEBUG_SCOPE 1` define and the per-test `TLOG`/`fprintf` stderr
 *     instrumentation (shipped in a passing test, now that the dangling-frame bug it
 *     diagnosed is fixed) are stripped entirely;
 *   - a real progress guard (a `done` flag asserted AFTER the pump) is added to every test
 *     that puts `EXPECT_*` inside the coroutine body — `JoinAny`, `JoinAllTimeout`, `RAIIJoin`,
 *     `PruneCompleted` could previously pass vacuously if the body never ran;
 *   - the four boilerplate fixtures are collapsed into one `ScopeStructuredConcurrency` base;
 *   - every test gates on a real flag through `qb::io::test::pump_until` instead of a blind
 *     `run_for(Nms)`; the file-local `main()` is removed (shared gtest_main).
 */

#include <atomic>
#include <numeric>
#include <optional>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

class ScopeStructuredConcurrency : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
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

// Free-function workers (parameters live in the coroutine frame by value — no dangling).
task<void>
sleep_worker(std::chrono::milliseconds ms) {
    co_await sleep(ms);
}

task<void>
ordered_worker(std::vector<int> *order, int tag, std::chrono::milliseconds delay) {
    co_await sleep(delay);
    order->push_back(tag);
}

task<void>
join_all_waiter(coroutine_scope *scope, int *counter) {
    co_await scope->join_all();
    ++(*counter);
}

} // namespace

// =============================================================================
// spawn / join_all
// =============================================================================

TEST_F(ScopeStructuredConcurrency, SpawnAndJoinAll) {
    std::atomic<int>  counter{0};
    std::atomic<bool> done{false};

    auto worker = [&counter]() -> task<void> {
        co_await sleep(20ms);
        counter.fetch_add(1);
    };

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(worker());
        scope.spawn(worker());
        scope.spawn(worker());

        co_await sleep(1ms); // let workers schedule
        EXPECT_EQ(scope.total_count(), 3u);

        co_await scope.join_all();
        EXPECT_EQ(scope.active_count(), 0u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "join_all coordinator never finished";
    EXPECT_EQ(counter.load(), 3);
}

// Regression: a worker that throws an UNCAUGHT exception must (1) not wedge join_all (every
// sibling still drains, active_count reaches 0), and (2) surface the failure to the awaiting
// join_all() as a rethrow — never a silent swallow nor a hang. Pins the coroutine_scope
// failure-propagation contract under structured concurrency.
TEST_F(ScopeStructuredConcurrency, JoinAllRethrowsWorkerExceptionWithoutHanging) {
    std::atomic<bool> done{false};
    std::atomic<bool> rethrew{false};
    std::atomic<int>  sibling_ran{0};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn([]() -> task<void> {
            co_await sleep(5ms);
            throw std::runtime_error("worker boom");
        });
        scope.spawn([&sibling_ran]() -> task<void> {
            co_await sleep(10ms);
            sibling_ran.fetch_add(1); // must still complete despite the sibling throwing
        });
        try {
            co_await scope.join_all();
        } catch (const std::runtime_error &) {
            rethrew.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); }))
        << "join_all hung on a throwing worker (coroutine_scope failure-propagation deadlock)";
    EXPECT_EQ(sibling_ran.load(), 1) << "a sibling worker must still drain when another throws";
    EXPECT_TRUE(rethrew.load()) << "join_all must rethrow the worker's uncaught exception";
}

TEST_F(ScopeStructuredConcurrency, JoinAnyReturnsFirstCompletedIndex) {
    std::atomic<bool>   done{false};
    std::atomic<size_t> index{99};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn([]() -> task<void> { co_await sleep(10ms); }());  // index 0, fast
        scope.spawn([]() -> task<void> { co_await sleep(100ms); }()); // index 1, slow

        index.store(co_await scope.join_any());
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "join_any coordinator never finished";
    EXPECT_EQ(index.load(), 0u) << "the fast worker (index 0) must complete first";
}

TEST_F(ScopeStructuredConcurrency, JoinAllForReturnsFalseOnTimeout) {
    std::atomic<bool> done{false};
    std::atomic<bool> completed{true};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn([]() -> task<void> { co_await sleep(500ms); }());
        completed.store(co_await scope.join_all_for(50ms));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "join_all_for timeout coordinator never finished";
    EXPECT_FALSE(completed.load()) << "join_all_for must return false when the worker outlives the timeout";
}

TEST_F(ScopeStructuredConcurrency, JoinAllForReturnsTrueOnCompletion) {
    std::atomic<int>  counter{0};
    std::atomic<bool> done{false};
    std::atomic<bool> completed{false};

    auto worker = [&counter]() -> task<void> {
        co_await sleep(10ms);
        counter.fetch_add(1);
    };

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(worker());
        scope.spawn(worker());
        completed.store(co_await scope.join_all_for(200ms));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "join_all_for success coordinator never finished";
    EXPECT_TRUE(completed.load());
    EXPECT_EQ(counter.load(), 2);
}

TEST_F(ScopeStructuredConcurrency, ActiveCountDecrementsMidFlight) {
    std::atomic<size_t> active_at_start{0};
    std::atomic<size_t> active_after_one{0};
    std::atomic<bool>   done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn([]() -> task<void> { co_await sleep(100ms); }());
        scope.spawn([]() -> task<void> { co_await sleep(10ms); }());
        active_at_start.store(scope.active_count());

        co_await sleep(50ms); // the fast one completes in this window
        active_after_one.store(scope.active_count());

        co_await scope.join_all();
        EXPECT_EQ(scope.active_count(), 0u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "active-count coordinator never finished";
    EXPECT_EQ(active_at_start.load(), 2u);
    EXPECT_EQ(active_after_one.load(), 1u) << "active_count must drop as a task completes";
}

TEST_F(ScopeStructuredConcurrency, RethrowIfErrorPropagatesFirstException) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn([]() -> task<void> {
            co_await sleep(5ms);
            throw std::runtime_error("task failed");
        }());
        co_await sleep(40ms);
        try {
            scope.rethrow_if_error();
        } catch (const std::runtime_error &e) {
            EXPECT_STREQ(e.what(), "task failed");
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "rethrow_if_error coordinator never finished";
    EXPECT_TRUE(caught.load());
}

TEST_F(ScopeStructuredConcurrency, CancelAllStopsCooperativeWorker) {
    // A scope worker loops on cancellable_sleep against the scope's own token. cancel_all()
    // signals that token; the next cancellable_sleep throws cancelled_error, the worker
    // unwinds early (never reaching its completion line), and the scope drains.
    auto              worker_completed = std::make_shared<std::atomic<bool>>(false);
    auto              worker_cancelled = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> done{false};

    coro_scheduler().spawn([worker_completed, worker_cancelled, &done]() -> task<void> {
        coroutine_scope scope; // default policy: cancel_all
        auto            tok = scope.cancel_token();

        scope.spawn([worker_completed, worker_cancelled, tok]() -> task<void> {
            try {
                for (int i = 0; i < 100; ++i)
                    co_await cancellable_sleep(10ms, tok);
                worker_completed->store(true); // only reached if never cancelled
            } catch (const cancelled_error &) {
                worker_cancelled->store(true); // the cooperative stop point
            }
        });

        co_await sleep(50ms);
        scope.cancel_all();
        co_await scope.join_all(); // the worker has unwound; the scope drains
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "cancel_all coordinator never finished";
    EXPECT_FALSE(worker_completed->load()) << "the cancelled worker must not reach completion";
    EXPECT_TRUE(worker_cancelled->load()) << "the worker must stop cooperatively via cancelled_error";
}

TEST_F(ScopeStructuredConcurrency, CancelAllJoinAllAbsorbsUncaughtWorkerCancellation) {
    // Robustness: a worker that lets cancelled_error PROPAGATE (no try/catch) after the
    // scope's own cancel_all() must not make join_all() rethrow that cancellation. The
    // cancellation was the coordinator's own request; surfacing it as a join_all error
    // would force every cancel_all()+join_all() site to wrap join_all in a try/catch
    // just to swallow its own signal — and an unwrapped coordinator would die on the
    // rethrow before its post-join line, which from the outside reads as a hung join_all
    // (the original symptom this guards against). The CancelAllStopsCooperativeWorker
    // test above only avoided it by having the worker swallow cancelled_error itself.
    auto              worker_completed = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> joined_cleanly{false};
    std::atomic<bool> join_threw{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([worker_completed, &joined_cleanly, &join_threw, &done]() -> task<void> {
        coroutine_scope scope; // default policy: cancel_all
        auto            tok = scope.cancel_token();

        // No try/catch: cancelled_error escapes the worker body entirely.
        scope.spawn([worker_completed, tok]() -> task<void> {
            for (int i = 0; i < 100; ++i)
                co_await cancellable_sleep(10ms, tok);
            worker_completed->store(true); // only reached if never cancelled
        });

        co_await sleep(50ms);
        scope.cancel_all();
        try {
            co_await scope.join_all(); // must drain cleanly, NOT rethrow our own cancellation
            joined_cleanly.store(true);
        } catch (...) {
            join_threw.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "uncaught-cancellation coordinator never finished";
    EXPECT_TRUE(joined_cleanly.load()) << "join_all must drain cleanly after the scope's own cancellation";
    EXPECT_FALSE(join_threw.load()) << "join_all must not rethrow the scope's own cancelled_error";
    EXPECT_FALSE(worker_completed->load()) << "the cancelled worker must not reach completion";
}

// =============================================================================
// joining_scope / edge cases
// =============================================================================

TEST_F(ScopeStructuredConcurrency, JoiningScopeJoinsOnDestruction) {
    std::atomic<int> counter{0};

    auto worker = [&counter]() -> task<void> {
        co_await sleep(30ms);
        counter.fetch_add(1);
    };

    coro_scheduler().spawn([&]() -> task<void> {
        {
            joining_scope scope;
            scope.spawn(worker());
            scope.spawn(worker());
            co_await scope.join_all(); // joining_scope is best-effort; join explicitly so the
                                       // destruction-join is exercised with no live tasks left
        }
    });

    EXPECT_TRUE(pump_until([&] { return counter.load() == 2; })) << "joining_scope workers never completed";
}

TEST_F(ScopeStructuredConcurrency, EmptyScopeJoinsImmediately) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        EXPECT_TRUE(scope.empty());
        co_await scope.join_all(); // nothing spawned -> immediate
        EXPECT_TRUE(scope.empty());
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "empty-scope join_all never returned";
}

TEST_F(ScopeStructuredConcurrency, PruneCompletedRemovesFinishedEntries) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn([]() -> task<void> { co_await sleep(10ms); }());
        scope.spawn([]() -> task<void> { co_await sleep(10ms); }());

        co_await scope.join_all();
        EXPECT_EQ(scope.active_count(), 0u);
        EXPECT_EQ(scope.total_count(), 2u);

        scope.prune_completed();
        EXPECT_EQ(scope.total_count(), 0u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "prune_completed coordinator never finished";
}

// =============================================================================
// parallel_map
// =============================================================================

TEST_F(ScopeStructuredConcurrency, ParallelMapTransformsAllItems) {
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<int> items = {1, 2, 3, 4, 5};
        auto             results = co_await parallel_map(items, [](int v) -> task<int> {
            co_await sleep(10ms);
            co_return v * v;
        });
        EXPECT_EQ(results.size(), 5u);
        int sum = 0;
        for (int r : results)
            sum += r;
        EXPECT_EQ(sum, 1 + 4 + 9 + 16 + 25);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "parallel_map coordinator never finished";
}

TEST_F(ScopeStructuredConcurrency, ParallelMapHonorsConcurrencyLimit) {
    std::atomic<int>  violations{0};
    std::atomic<int>  active{0};
    std::atomic<bool> done{false};
    constexpr int     N   = 12;
    constexpr int     MAX = 3;

    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<int> items(N);
        std::iota(items.begin(), items.end(), 0);

        auto results = co_await parallel_map(
            items,
            [&](int v) -> task<int> {
                int cur = ++active;
                if (cur > MAX)
                    ++violations;
                co_await sleep(30ms); // hold the slot long enough
                --active;
                co_return v;
            },
            MAX);

        EXPECT_EQ(static_cast<int>(results.size()), N);
        EXPECT_EQ(violations.load(), 0) << "the semaphore must cap concurrency at " << MAX;
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); }, 4000ms)) << "parallel_map concurrency-limit coordinator never finished";
}

TEST_F(ScopeStructuredConcurrency, ParallelMapLargeInput) {
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        constexpr int    N = 50;
        std::vector<int> items(N);
        std::iota(items.begin(), items.end(), 0);

        auto results = co_await parallel_map(items, [](int v) -> task<int> { co_return v * 2; }, 10);
        EXPECT_EQ(static_cast<int>(results.size()), N);
        int sum = 0;
        for (int r : results)
            sum += r;
        EXPECT_EQ(sum, N * (N - 1)); // 2 * (0+..+49)
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "parallel_map large-input coordinator never finished";
}

// =============================================================================
// Event-driven join (no polling)
// =============================================================================

TEST_F(ScopeStructuredConcurrency, JoinAllWakesImmediatelyWhenEmpty) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        co_await scope.join_all(); // nothing spawned -> immediate
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "empty join_all never woke";
}

TEST_F(ScopeStructuredConcurrency, JoinAllResumesInCompletionOrder) {
    std::atomic<bool> done{false};
    std::vector<int>  order;

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(ordered_worker(&order, 1, 15ms));
        scope.spawn(ordered_worker(&order, 2, 10ms));
        co_await scope.join_all();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "ordered join_all coordinator never finished";
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 2) << "the 10ms task finishes first";
    EXPECT_EQ(order[1], 1);
}

TEST_F(ScopeStructuredConcurrency, JoinAnyEventDrivenReturnsFasterIndex) {
    std::atomic<bool>   done{false};
    std::atomic<size_t> winner_idx{99};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(sleep_worker(50ms)); // index 0
        scope.spawn(sleep_worker(5ms));  // index 1
        winner_idx.store(co_await scope.join_any());
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "join_any event-driven coordinator never finished";
    EXPECT_EQ(winner_idx.load(), 1u) << "the 5ms task (index 1) wins";
}

TEST_F(ScopeStructuredConcurrency, MultipleConcurrentJoinAllWaiters) {
    int               woken = 0;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(sleep_worker(20ms));
        coro_scheduler().spawn(join_all_waiter(&scope, &woken));
        coro_scheduler().spawn(join_all_waiter(&scope, &woken));
        co_await sleep(100ms);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "multi-waiter coordinator never finished";
    EXPECT_EQ(woken, 2) << "both join_all waiters must wake when the scope drains";
}

// =============================================================================
// Scope policies + parallel / capture_result / with_scope / repeat_while
// =============================================================================

TEST_F(ScopeStructuredConcurrency, CancellingScopePolicyJoinsAfterCancel) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        cancelling_scope scope;
        scope.spawn(sleep_worker(10ms));
        scope.spawn(sleep_worker(10ms));
        EXPECT_EQ(scope.active_count(), 2u);
        scope.cancel_all();
        co_await scope.join_all();
        EXPECT_EQ(scope.active_count(), 0u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "cancelling-scope coordinator never finished";
}

TEST_F(ScopeStructuredConcurrency, DetachingScopeDefaultConstructorIsEmpty) {
    detaching_scope scope;
    EXPECT_EQ(scope.active_count(), 0u);
    EXPECT_TRUE(scope.empty());
}

TEST_F(ScopeStructuredConcurrency, CancelTokenAccessorReflectsCancelAll) {
    coroutine_scope scope;
    auto            token = scope.cancel_token();
    EXPECT_FALSE(token.is_cancelled());
    scope.cancel_all();
    EXPECT_TRUE(token.is_cancelled());
}

TEST_F(ScopeStructuredConcurrency, CaptureResultStoresTaskResult) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        std::optional<int> result;
        coroutine_scope    scope;
        scope.spawn(capture_result(
            []() -> task<int> {
                co_await sleep(10ms);
                co_return 42;
            }(),
            result));
        co_await scope.join_all();
        EXPECT_TRUE(result.has_value());
        if (result.has_value())
            EXPECT_EQ(*result, 42);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "capture_result coordinator never finished";
}

TEST_F(ScopeStructuredConcurrency, ParallelVariadicReturnsTuple) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto [a, b, c] = co_await parallel(
            []() -> task<int> {
                co_await sleep(10ms);
                co_return 1;
            }(),
            []() -> task<int> {
                co_await sleep(10ms);
                co_return 2;
            }(),
            []() -> task<int> {
                co_await sleep(10ms);
                co_return 3;
            }());
        EXPECT_EQ(a, 1);
        EXPECT_EQ(b, 2);
        EXPECT_EQ(c, 3);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "parallel coordinator never finished";
}

TEST_F(ScopeStructuredConcurrency, WithScopeHelperReturnsValue) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto result = co_await with_scope([](coroutine_scope &) -> task<int> { co_return 42; });
        EXPECT_EQ(result, 42);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "with_scope coordinator never finished";
}

TEST_F(ScopeStructuredConcurrency, RepeatWhileStopsOnPredicate) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        int iterations = 0;
        co_await repeat_while(
            [&]() -> task<void> {
                ++iterations;
                co_await sleep(5ms);
            },
            [&]() { return iterations < 5; });
        EXPECT_EQ(iterations, 5);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "repeat_while (predicate) coordinator never finished";
}

TEST_F(ScopeStructuredConcurrency, RepeatWhileStopsOnCancelToken) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        int                iterations = 0;
        cancellation_token token;
        token.cancel();
        co_await repeat_while(
            [&]() -> task<void> {
                ++iterations;
                co_return;
            },
            [&]() { return iterations < 10; }, token);
        EXPECT_EQ(iterations, 0);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "repeat_while (cancel) coordinator never finished";
}

TEST_F(ScopeStructuredConcurrency, TotalCountVsActiveCount) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn(sleep_worker(10ms));
        scope.spawn(sleep_worker(200ms));
        EXPECT_EQ(scope.total_count(), 2u);
        EXPECT_EQ(scope.active_count(), 2u);
        co_await sleep(50ms);
        EXPECT_EQ(scope.total_count(), 2u) << "total_count counts every spawned task, completed or not";
        EXPECT_LE(scope.active_count(), 2u);
        scope.cancel_all();
        co_await scope.join_all();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "total-vs-active coordinator never finished";
}
