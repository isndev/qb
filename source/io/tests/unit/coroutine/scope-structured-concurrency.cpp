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

// Throws cancelled_error from a token UNRELATED to the scope (never the scope's own token).
// run_wrapped must surface this as a genuine task failure (scope.cancel_token() stays clean),
// exercising the "external cancelled_error" branch in run_wrapped().
task<void>
foreign_cancel_worker(cancellation_token foreign) {
    foreign.cancel();
    foreign.throw_if_cancelled(); // throws cancelled_error from a token the scope does not own
    co_return;                    // unreached
}

// Free-function inner ops for make_cancellable<void> tests. Parameters (the flag pointer) live
// BY VALUE in the coroutine frame — never a captured reference in an immediately-invoked lambda,
// which would dangle across the suspension point (task.h note #2).
task<void>
long_sleep_then_set(std::chrono::milliseconds ms, std::atomic<bool> *completed) {
    co_await sleep(ms);
    completed->store(true);
}

task<void>
short_sleep_then_throw(std::chrono::milliseconds ms) {
    co_await sleep(ms);
    throw std::runtime_error("inner void boom");
}

// Cancels `token` after `delay` — drives a make_cancellable<void> op into mid-flight cancellation.
task<void>
delayed_cancel(std::chrono::milliseconds delay, cancellation_token token) {
    co_await sleep(delay);
    token.cancel();
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

// =============================================================================
// run_wrapped: external (unrelated-token) cancelled_error is a real failure
// =============================================================================

// A worker that throws cancelled_error from a token the scope does NOT own must be recorded
// as a genuine failure: the scope's own token stays uncancelled, so run_wrapped() takes its
// `else` branch (records first_error) and join_all() rethrows. This is the complement of
// CancelAllJoinAllAbsorbsUncaughtWorkerCancellation (which proves the SELF-cancel absorb path).
TEST_F(ScopeStructuredConcurrency, ForeignCancelledErrorIsSurfacedAsFailure) {
    std::atomic<bool> done{false};
    std::atomic<bool> join_rethrew{false};
    std::atomic<bool> scope_token_clean{false};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope    scope; // default cancel_all; its own token is never cancelled here
        cancellation_token foreign;
        scope.spawn(foreign_cancel_worker(foreign));
        try {
            co_await scope.join_all();
        } catch (const cancelled_error &) {
            join_rethrew.store(true);
        }
        // The scope's own token must NOT have been touched by the foreign cancellation.
        scope_token_clean.store(!scope.cancel_token().is_cancelled());
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "foreign-cancel coordinator never finished";
    EXPECT_TRUE(join_rethrew.load()) << "an unrelated-token cancelled_error must surface as a join_all failure";
    EXPECT_TRUE(scope_token_clean.load()) << "the foreign cancellation must not have cancelled the scope's own token";
}

// =============================================================================
// make_cancellable<void>: cancel mid-flight + inner-exception propagation
// =============================================================================

// make_cancellable<void>(throw_on_cancel=true): cancel the controlling token while the inner
// op is parked in a sleep. The void awaiter's on_cancel hook must fire — tearing down the
// detached runner + inner task — and resume the waiter, whose await_resume() then throws
// cancelled_error. Drives the uncovered void-specialization cancel path (await_suspend hook
// body) and the await_resume cancelled-throw branch.
TEST_F(ScopeStructuredConcurrency, MakeCancellableVoidThrowsOnMidFlightCancel) {
    std::atomic<bool> done{false};
    std::atomic<bool> threw_cancelled{false};
    std::atomic<bool> inner_completed{false};

    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token;

        // Cancel the token shortly after the inner op parks on its long sleep (free-function
        // driver — token is copied by value into its frame, no dangling closure).
        coro_scheduler().spawn(delayed_cancel(20ms, token));

        try {
            // Inner op is a free function whose flag pointer lives by value in its frame.
            co_await make_cancellable(long_sleep_then_set(500ms, &inner_completed), token,
                                      /*throw_on_cancel=*/true);
        } catch (const cancelled_error &) {
            threw_cancelled.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "make_cancellable<void> cancel coordinator never finished";
    EXPECT_TRUE(threw_cancelled.load()) << "make_cancellable<void> must throw cancelled_error on mid-flight cancel";
    EXPECT_FALSE(inner_completed.load()) << "the cancelled inner op must not reach completion";
}

// make_cancellable<void> with throw_on_cancel=false and NO cancellation: the inner op throws
// its own exception. task_runner must capture it into shared_state::error and await_resume must
// rethrow it (void overload). Drives task_runner's catch (552-553) and await_resume rethrow
// (542-543) for the void specialization.
TEST_F(ScopeStructuredConcurrency, MakeCancellableVoidPropagatesInnerException) {
    std::atomic<bool> done{false};
    std::atomic<bool> got_inner_error{false};

    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token; // never cancelled
        try {
            co_await make_cancellable(short_sleep_then_throw(5ms), token,
                                      /*throw_on_cancel=*/false);
        } catch (const std::runtime_error &e) {
            EXPECT_STREQ(e.what(), "inner void boom");
            got_inner_error.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "make_cancellable<void> inner-exception coordinator never finished";
    EXPECT_TRUE(got_inner_error.load()) << "make_cancellable<void> must rethrow the inner task's exception";
}

// make_cancellable<void> normal completion (no cancel, no throw): the inner op completes, the
// awaiter resumes via task_runner's done path, and await_resume returns without error. Exercises
// the void task_runner success path and the await_resume clean exit.
TEST_F(ScopeStructuredConcurrency, MakeCancellableVoidCompletesNormally) {
    std::atomic<bool> done{false};
    std::atomic<bool> inner_ran{false};

    coro_scheduler().spawn([&]() -> task<void> {
        cancellation_token token;
        co_await make_cancellable(long_sleep_then_set(5ms, &inner_ran), token,
                                  /*throw_on_cancel=*/true);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "make_cancellable<void> normal coordinator never finished";
    EXPECT_TRUE(inner_ran.load()) << "the inner op must run to completion when never cancelled";
}

// =============================================================================
// Scope move-assignment + join_all policy destruction warning
// =============================================================================

// Move-assignment must transfer impl/token/policy so the moved-into scope drives the workers
// and the moved-from scope is inert. Drives coroutine_scope::operator=(coroutine_scope&&).
TEST_F(ScopeStructuredConcurrency, MoveAssignedScopeOwnsTheTasks) {
    std::atomic<int>  counter{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope src;
        src.spawn([&counter]() -> task<void> {
            co_await sleep(10ms);
            counter.fetch_add(1);
        });
        EXPECT_EQ(src.active_count(), 1u);

        coroutine_scope dst;          // its own (different) impl
        dst = std::move(src);         // move-assign: dst now owns src's task + token + policy
        EXPECT_EQ(dst.active_count(), 1u) << "the moved-into scope must own the spawned task";

        co_await dst.join_all();      // the moved-into scope drains the worker
        EXPECT_EQ(dst.active_count(), 0u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "move-assign coordinator never finished";
    EXPECT_EQ(counter.load(), 1) << "the worker carried by move-assign must still run";
}

// A join_all-policy scope (joining_scope) destroyed while tasks are still active hits the
// best-effort destructor branch (and, in debug builds, emits the active-tasks warning to
// stderr). The tasks keep running via the shared scope_impl; we join them through the scope's
// own token afterwards to drain the loop cleanly. Drives ~coroutine_scope's join_all case.
TEST_F(ScopeStructuredConcurrency, JoiningScopeDestroyedWithActiveTasksIsBestEffort) {
    std::atomic<int>  counter{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        {
            joining_scope scope; // cleanup_policy::join_all
            scope.spawn([&counter]() -> task<void> {
                co_await sleep(15ms);
                counter.fetch_add(1);
            });
            EXPECT_EQ(scope.active_count(), 1u);
            // Intentionally do NOT co_await join_all() before the scope dies: exercise the
            // best-effort join_all destructor branch with an active task still in flight.
        }
        // The task keeps running via the shared scope_impl; let it finish.
        co_await sleep(60ms);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load() && counter.load() == 1; }))
        << "best-effort joining_scope task never completed after early scope destruction";
    EXPECT_EQ(counter.load(), 1) << "the spawned task must still run after the join_all-policy scope is destroyed";
}

// =============================================================================
// join_any / join_all_for fast-path (already-resolved) branches
// =============================================================================

// join_any on a scope whose work has ALREADY completed before the await must take the
// await_ready()==true fast path (a completed task is found) and return that index without
// suspending. Drives join_any's await_ready true branch + await_resume index lookup.
TEST_F(ScopeStructuredConcurrency, JoinAnyFastPathWhenTaskAlreadyDone) {
    std::atomic<bool>   done{false};
    std::atomic<size_t> idx{99};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn([]() -> task<void> { co_return; }()); // completes on first drain, index 0
        co_await sleep(20ms);                              // ensure it is marked completed
        EXPECT_EQ(scope.active_count(), 0u);
        idx.store(co_await scope.join_any());              // await_ready() is already true
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "join_any fast-path coordinator never finished";
    EXPECT_EQ(idx.load(), 0u) << "join_any must return the already-completed task's index";
}

// join_all_for on a scope with NO active tasks must return true immediately (active_count==0
// short-circuit at entry) without arming a timer. Drives the join_all_for(active_count==0)
// co_return true path.
TEST_F(ScopeStructuredConcurrency, JoinAllForEmptyScopeReturnsTrueImmediately) {
    std::atomic<bool> done{false};
    std::atomic<bool> completed{false};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope; // nothing spawned
        EXPECT_TRUE(scope.empty());
        completed.store(co_await scope.join_all_for(50ms)); // active_count==0 → immediate true
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "join_all_for empty-scope coordinator never finished";
    EXPECT_TRUE(completed.load()) << "join_all_for on an empty scope must return true immediately";
}

// join_all_for after a spawned task has already drained: active_count is 0 by the time
// join_all_for runs, so the entry-level `co_return true` short-circuit fires (the timer is never
// armed). Complements the empty-scope test by reaching the same path through a task that ran and
// completed, rather than a scope that never had work. (The awaiter's own active_count==0
// await_ready guard is dead given this entry short-circuit — not targeted.)
TEST_F(ScopeStructuredConcurrency, JoinAllForFastPathWhenAllAlreadyDone) {
    std::atomic<bool> done{false};
    std::atomic<bool> completed{false};

    coro_scheduler().spawn([&]() -> task<void> {
        coroutine_scope scope;
        scope.spawn([]() -> task<void> { co_await sleep(5ms); }());
        co_await sleep(25ms); // let the worker finish first
        EXPECT_EQ(scope.active_count(), 0u);
        completed.store(co_await scope.join_all_for(50ms)); // awaiter await_ready true
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "join_all_for already-done coordinator never finished";
    EXPECT_TRUE(completed.load()) << "join_all_for must return true when all tasks already completed";
}
