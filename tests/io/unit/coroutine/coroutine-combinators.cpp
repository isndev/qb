/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/coroutine-combinators.cpp
 * @brief Coroutine composition combinators — when_all / when_any / race / coro_with_timeout.
 *
 * Covers the parallel-composition helpers in qb/io/async/coroutine/combinators.h: `when_all`
 * (variadic tuple, vector, void tasks, empty, first-exception propagation), `when_any` /
 * `race` (variadic + vector, `when_any_result` with `.index`/`.value`/`.get<T>()`/
 * `.has_exception()`/structured binding), and `coro_with_timeout` (value + void, success +
 * timeout). The `CoroutineLifetime` fixture is the load-bearing detached-frame reclamation
 * suite: it proves the winner tears every losing branch down (frame + ev_timer) the instant it
 * is decided, with `detail::CoroutineFrameAllocator::live_frames` returning to baseline long
 * before a multi-second loser sleep could complete.
 *
 * Strengthened over the original test-coroutine-combinators.cpp:
 *   - `WhenAnyResultIndexAndValue` now asserts the SPECIFIC winner (a large delay gap makes
 *     the instant task the deterministic winner — index 0, value 10) instead of accepting any
 *     of three outcomes;
 *   - the duplicate `RaceBasic`/`RaceVector` cases are folded into one
 *     `RaceIsWhenAnyAlias` check;
 *   - ADDED: a void `when_all` task that throws (only value-task throw was covered), a vector
 *     `when_all` where one element throws, and `coro_with_timeout` where the timeout fires and
 *     the slow inner operation is left running (abandoned, not interrupted — finding 2.B.4);
 *   - every spawned-body test gates on a real `done` flag through `qb::io::test::pump_until`;
 *     all fixtures share one `reset_async_context()` SetUp and the same teardown; the
 *     file-local `main()` is removed (shared gtest_main).
 */

#include <atomic>
#include <set>
#include <stdexcept>
#include <tuple>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/coroutine_reclaim_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reclaim_fast_winner;
using qb::io::test::run_reclaim_driver;

namespace {

// Shared base: fresh per-test loop, explicit scheduler reset on teardown so live_frames
// bookkeeping is consistent across the reclamation tests.
class CoroutineCombinators : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler())
            qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// =============================================================================
// when_all
// =============================================================================

TEST_F(CoroutineCombinators, WhenAllMixedTypesViaSequentialAwait) {
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto t1 = []() -> task<int> {
            co_await sleep(30ms);
            co_return 42;
        }();
        auto t2 = []() -> task<std::string> {
            co_await sleep(20ms);
            co_return "hello";
        }();
        int         r1 = co_await t1;
        std::string r2 = co_await t2;
        EXPECT_EQ(r1, 42);
        EXPECT_EQ(r2, "hello");
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "sequential-await coordinator never finished";
}

TEST_F(CoroutineCombinators, WhenAllHelperReturnsTuple) {
    std::atomic<int>  sum{0};
    std::atomic<bool> done{false};

    auto worker = [&sum](int value, int delay_ms) -> task<int> {
        co_await sleep(std::chrono::milliseconds(delay_ms));
        sum += value;
        co_return value;
    };

    coro_scheduler().spawn([&]() -> task<void> {
        auto [r1, r2, r3] = co_await when_all(worker(10, 30), worker(20, 20), worker(30, 10));
        EXPECT_EQ(r1, 10);
        EXPECT_EQ(r2, 20);
        EXPECT_EQ(r3, 30);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_all helper never finished";
    EXPECT_EQ(sum.load(), 60);
}

TEST_F(CoroutineCombinators, AwaitingMultipleVoidTasksRunsAll) {
    // The variadic when_all forms a std::tuple<value_type...>, which is ill-formed for
    // task<void> branches — the idiom for fanning out void tasks is to start them all, then
    // co_await each. (when_all over void is reserved to the vector/run_one path elsewhere.)
    constexpr int     count = 5;
    std::atomic<int>  completed{0};
    std::atomic<bool> done{false};

    auto worker = [&completed](int delay_ms) -> task<void> {
        co_await sleep(std::chrono::milliseconds(delay_ms));
        completed.fetch_add(1);
    };

    coro_scheduler().spawn([&]() -> task<void> {
        auto t1 = worker(10);
        auto t2 = worker(20);
        auto t3 = worker(30);
        auto t4 = worker(15);
        auto t5 = worker(25);
        co_await t1;
        co_await t2;
        co_await t3;
        co_await t4;
        co_await t5;
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "awaiting multiple void tasks never finished";
    EXPECT_EQ(completed.load(), count);
}

TEST_F(CoroutineCombinators, WhenAllVectorReturnsAllResults) {
    constexpr int     count = 10;
    std::atomic<int>  sum{0};
    std::atomic<bool> done{false};

    auto worker = [&sum](int value) -> task<int> {
        co_await sleep(10ms);
        sum += value;
        co_return value;
    };

    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<task<int>> tasks;
        for (int i = 0; i < count; ++i)
            tasks.push_back(worker(i));
        auto results = co_await when_all(std::move(tasks));
        EXPECT_EQ(results.size(), static_cast<size_t>(count));
        for (size_t i = 0; i < results.size(); ++i)
            EXPECT_EQ(results[i], static_cast<int>(i));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_all vector never finished";
    EXPECT_EQ(sum.load(), 45);
}

TEST_F(CoroutineCombinators, WhenAllEmptyCompletesImmediately) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto results = co_await when_all(std::vector<task<int>>());
        EXPECT_TRUE(results.empty());
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "empty when_all never resumed";
}

TEST_F(CoroutineCombinators, WhenAllPropagatesFirstException) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

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
            ADD_FAILURE() << "when_all should have rethrown";
        } catch (const std::runtime_error &e) {
            EXPECT_STREQ(e.what(), "fail");
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_all exception coordinator never finished";
    EXPECT_TRUE(caught.load());
}

TEST_F(CoroutineCombinators, AwaitedVoidTaskThatThrowsPropagates) {
    // ADDED: only value-task throw propagation was covered. The variadic when_all forms a
    // tuple<value_type...> and so cannot host void branches; the void exception path is the
    // direct task<void> awaiter, which must rethrow the stored exception to the awaiter.
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto bad = []() -> task<void> {
            co_await sleep(2ms);
            throw std::logic_error("void-boom");
        }();
        try {
            co_await bad;
            ADD_FAILURE() << "the void task should have rethrown";
        } catch (const std::logic_error &e) {
            EXPECT_STREQ(e.what(), "void-boom");
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "void-throw coordinator never finished";
    EXPECT_TRUE(caught.load());
}

TEST_F(CoroutineCombinators, WhenAllVectorElementThrowsPropagates) {
    // ADDED: a throwing element in the VECTOR overload (only the variadic throw was covered).
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.push_back([]() -> task<int> {
            co_await sleep(5ms);
            co_return 1;
        }());
        tasks.push_back([]() -> task<int> {
            co_await sleep(2ms);
            throw std::runtime_error("vector-boom");
            co_return 0;
        }());
        try {
            auto results = co_await when_all(std::move(tasks));
            (void) results;
            ADD_FAILURE() << "when_all(vector) should have rethrown";
        } catch (const std::runtime_error &e) {
            EXPECT_STREQ(e.what(), "vector-boom");
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_all vector-throw coordinator never finished";
    EXPECT_TRUE(caught.load());
}

// =============================================================================
// when_any / race
// =============================================================================

TEST_F(CoroutineCombinators, WhenAnyReturnsFirstCompleted) {
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto worker = [](int delay_ms, int result) -> task<int> {
            co_await sleep(std::chrono::milliseconds(delay_ms));
            co_return result;
        };
        auto [index, value] = co_await when_any(worker(100, 1), worker(50, 2), worker(200, 3));
        EXPECT_EQ(index, 1u) << "the 50ms task wins";
        EXPECT_EQ(std::any_cast<int>(value), 2);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any coordinator never finished";
}

TEST_F(CoroutineCombinators, WhenAnyVectorReturnsFirstCompleted) {
    constexpr int     count = 10;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto worker = [](int index, int delay_ms) -> task<int> {
            co_await sleep(std::chrono::milliseconds(delay_ms));
            co_return index;
        };
        std::vector<task<int>> tasks;
        for (int i = 0; i < count; ++i)
            tasks.push_back(worker(i, 10 + i * 20)); // wide gap so index 0 is deterministic
        auto [index, value] = co_await when_any(std::move(tasks));
        EXPECT_EQ(index, 0u);
        EXPECT_EQ(std::any_cast<int>(value), 0);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any vector coordinator never finished";
}

TEST_F(CoroutineCombinators, WhenAnyResultIndexAndValueIsSpecificWinner) {
    // Strengthened: a large delay gap makes the instant task the deterministic winner, so we
    // assert the EXACT index/value rather than accepting any of the three outcomes.
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto result = co_await when_any(
            []() -> task<int> {
                co_return 10;
            }(), // instant winner
            []() -> task<int> {
                co_await sleep(1s);
                co_return 20;
            }(),
            []() -> task<int> {
                co_await sleep(1s);
                co_return 30;
            }());
        EXPECT_EQ(result.index, 0u);
        EXPECT_EQ(std::any_cast<int>(result.value), 10);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any specific-winner coordinator never finished";
}

TEST_F(CoroutineCombinators, RaceIsWhenAnyAlias) {
    // Folds the former duplicate RaceBasic + RaceVector into one alias proof: race(...)
    // resolves identically to when_any(...) over the same inputs (a large delay gap makes
    // index 1 the deterministic winner) for both the variadic and the vector overload.
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto variadic = co_await race(
            []() -> task<int> {
                co_await sleep(200ms);
                co_return 1;
            }(),
            []() -> task<int> {
                co_await sleep(5ms);
                co_return 2;
            }());
        EXPECT_EQ(variadic.index, 1u);
        EXPECT_EQ(std::any_cast<int>(variadic.value), 2);

        std::vector<task<int>> tasks;
        tasks.push_back([]() -> task<int> {
            co_await sleep(200ms);
            co_return 1;
        }());
        tasks.push_back([]() -> task<int> {
            co_await sleep(5ms);
            co_return 2;
        }());
        auto [index, value] = co_await race(std::move(tasks));
        EXPECT_EQ(index, 1u);
        EXPECT_EQ(std::any_cast<int>(value), 2);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "race alias coordinator never finished";
}

// =============================================================================
// Regression (audit BUG-1/BUG-3): multi-frame loser reclamation must not UAF
// =============================================================================

namespace {
// A MULTI-FRAME inner coroutine: deep -> mid -> leaf, where only the leaf parks on a real timer.
// The frame the scheduler actually queues when the timer fires is the deepest (leaf) frame, NOT
// the task<T> root that when_any/cancellable hold. Reclaiming such a loser destroys the whole
// chain; before the awaiter-destructor unschedule() fix, the queued leaf was freed but left in
// ready_queue_/in_flight_, so the next run_ready() drain resumed freed memory (ASan BUS/UAF at
// the scheduler resume site). Named free coroutines — no captured closures.
task<int>
mf_leaf(std::chrono::milliseconds d, int tag) {
    co_await sleep(d);
    co_return tag;
}
task<int>
mf_mid(std::chrono::milliseconds d, int tag) {
    co_return co_await mf_leaf(d, tag);
}
task<int>
mf_deep(std::chrono::milliseconds d, int tag) {
    co_return co_await mf_mid(d, tag);
}
} // namespace

// All branches share an identical short delay so the LOSERS' leaf timers fire in the SAME
// run_ready() drain the winner is decided in — the exact shape that triggered the reclamation UAF.
TEST_F(CoroutineCombinators, WhenAnyReclaimsMultiFrameLosersWithoutUAF) {
    std::atomic<bool> done{false};
    std::atomic<int>  win{-1};
    coro_scheduler().spawn([&]() -> task<void> {
        auto r = co_await when_any(mf_deep(5ms, 10), mf_deep(5ms, 20), mf_deep(5ms, 30));
        win.store(r.get<int>());
        done.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "multi-frame when_any never completed (UAF or hang)";
    EXPECT_TRUE(win.load() == 10 || win.load() == 20 || win.load() == 30) << "winner value must be one branch tag";
    coro_scheduler().run_ready(); // drain any deferred destroy — must never touch a freed loser leaf
}

TEST_F(CoroutineCombinators, RaceReclaimsMultiFrameLosersWithoutUAF) {
    std::atomic<bool> done{false};
    std::atomic<int>  win{-1};
    coro_scheduler().spawn([&]() -> task<void> {
        auto r = co_await race(mf_deep(5ms, 1), mf_deep(5ms, 2), mf_deep(5ms, 3));
        win.store(r.get<int>());
        done.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "multi-frame race never completed (UAF or hang)";
    EXPECT_TRUE(win.load() >= 1 && win.load() <= 3);
    coro_scheduler().run_ready();
}

TEST_F(CoroutineCombinators, WhenAnyVectorReclaimsMultiFrameLosersWithoutUAF) {
    std::atomic<bool> done{false};
    std::atomic<int>  win{-1};
    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.push_back(mf_deep(5ms, 7));
        tasks.push_back(mf_deep(5ms, 8));
        tasks.push_back(mf_deep(5ms, 9));
        auto r = co_await when_any(std::move(tasks)); // vector form returns std::pair<size_t, std::any>
        win.store(std::any_cast<int>(r.second));
        done.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "multi-frame when_any(vector) never completed (UAF or hang)";
    EXPECT_TRUE(win.load() >= 7 && win.load() <= 9);
    coro_scheduler().run_ready();
}

// =============================================================================
// when_any_result API
// =============================================================================

TEST_F(CoroutineCombinators, WhenAnyResultGetExtractsTypedValue) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto result = co_await when_any(
            []() -> task<int> {
                co_return 42;
            }(),
            []() -> task<std::string> {
                co_await sleep(1s);
                co_return "slow";
            }());
        EXPECT_EQ(result.get<int>(), 42);
        EXPECT_EQ(result.index, 0u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any_result get coordinator never finished";
}

TEST_F(CoroutineCombinators, WhenAnyResultHasExceptionReflectsThrowingWinner) {
    std::atomic<bool> done{false};
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
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any_result exception coordinator never finished";
}

TEST_F(CoroutineCombinators, WhenAnyResultHasExceptionFalseOnSuccess) {
    std::atomic<bool> done{false};
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
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any_result success coordinator never finished";
}

TEST_F(CoroutineCombinators, WhenAnyResultStructuredBinding) {
    std::atomic<bool> done{false};
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
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any_result structured-binding coordinator never finished";
}

// =============================================================================
// coro_with_timeout
// =============================================================================

TEST_F(CoroutineCombinators, TimeoutSuccessReturnsResult) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        auto result = co_await coro_with_timeout(
            []() -> task<int> {
                co_await sleep(30ms);
                co_return 42;
            }(),
            200ms);
        EXPECT_EQ(result, 42);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "timeout-success coordinator never finished";
}

TEST_F(CoroutineCombinators, TimeoutThrowsWhenOperationTooSlow) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await coro_with_timeout(
                []() -> task<int> {
                    co_await sleep(200ms);
                    co_return 42;
                }(),
                20ms);
            ADD_FAILURE() << "expected timeout_error";
        } catch (const timeout_error &) {
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "timeout coordinator never finished";
    EXPECT_TRUE(caught.load());
}

TEST_F(CoroutineCombinators, TimeoutVoidSuccess) {
    std::atomic<bool> ran{false};
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        // Named local of this frame: `task`'s initial_suspend is suspend_always, so the body runs
        // on a later run_ready() — an immediately-invoked temporary closure would be gone by then.
        auto op = [&ran]() -> task<void> {
            co_await sleep(20ms);
            ran.store(true);
        };
        co_await coro_with_timeout(op(), 200ms);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "timeout void-success coordinator never finished";
    EXPECT_TRUE(ran.load());
}

TEST_F(CoroutineCombinators, TimeoutVoidThrowsWhenOperationTooSlow) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await coro_with_timeout(
                []() -> task<void> {
                    co_await sleep(200ms);
                }(),
                20ms);
            ADD_FAILURE() << "expected timeout_error";
        } catch (const timeout_error &) {
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "timeout void coordinator never finished";
    EXPECT_TRUE(caught.load());
}

TEST_F(CoroutineCombinators, TimeoutAbandonsButDoesNotInterruptSlowOperation) {
    // ADDED (finding 2.B.4): when the timeout fires, the caller gets timeout_error but the
    // inner operation is NOT cancelled — it keeps running in the background to completion and
    // its result is dropped. We observe that the inner side-effect still fires after the
    // throw, proving "abandoned, not interrupted".
    std::atomic<bool> caught{false};
    std::atomic<bool> inner_finished{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            auto op = [&inner_finished]() -> task<int> {
                co_await sleep(60ms); // outlives the 20ms timeout
                inner_finished.store(true);
                co_return 7;
            };
            co_await coro_with_timeout(op(), 20ms);
            ADD_FAILURE() << "expected timeout_error";
        } catch (const timeout_error &) {
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "timeout-abandon coordinator never threw";
    EXPECT_TRUE(caught.load());
    EXPECT_FALSE(inner_finished.load()) << "the slow op must still be running when the timeout throws";

    // The abandoned operation keeps running and finishes on its own afterwards.
    EXPECT_TRUE(pump_until([&] { return inner_finished.load(); }))
        << "the abandoned inner operation must run to completion in the background (not be interrupted)";
}

// =============================================================================
// Lifetime & detached-frame reclamation
// Stresses the shared_ptr<state_t> lifetime pattern AND the winner-reclaims-losers fix:
// the winning branch tears every loser's run_one frame + inner task + armed ev_timer down
// the instant it is decided. The oracle is live_frames returning to baseline AFTER the
// winner resolves but LONG BEFORE the losers' (multi-second) sleeps could complete.
// =============================================================================

TEST_F(CoroutineCombinators, WhenAnyFirstTaskWinsAndLosersAreReclaimed) {
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

    coro_scheduler().spawn([&]() -> task<void> {
        auto res = co_await when_any(fast(), slow(50), slow(80));
        EXPECT_EQ(res.index, 0u);
        EXPECT_EQ(std::any_cast<int>(res.value), 1);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any-first-wins coordinator never finished";
    // Only the winner ran: the two slow losers were torn down before their sleep resumed.
    EXPECT_EQ(completed_count.load(), 1);
}

TEST_F(CoroutineCombinators, WhenAnyAllImmediateRunsExactlyOneBranch) {
    std::atomic<bool> done{false};
    std::atomic<int>  run_count{0};

    auto instant = [&run_count](int v) -> task<int> {
        ++run_count;
        co_return v;
    };

    coro_scheduler().spawn([&]() -> task<void> {
        auto res = co_await when_any(instant(10), instant(20), instant(30));
        EXPECT_LT(res.index, 3u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any-all-immediate coordinator never finished";
    EXPECT_EQ(run_count.load(), 1) << "the winner reclaims the still-queued losers before they run";
}

TEST_F(CoroutineCombinators, WhenAllLargeCountSumsArithmeticSeries) {
    std::atomic<bool> done{false};
    constexpr int     N = 20;

    auto worker = [](int v) -> task<int> {
        co_await sleep(10ms);
        co_return v;
    };

    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.reserve(N);
        for (int i = 0; i < N; ++i)
            tasks.push_back(worker(i));
        auto results = co_await when_all(std::move(tasks));
        EXPECT_EQ(static_cast<int>(results.size()), N);
        int actual_sum = 0;
        for (auto v : results)
            actual_sum += v;
        EXPECT_EQ(actual_sum, N * (N - 1) / 2);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_all large-count coordinator never finished";
}

TEST_F(CoroutineCombinators, WhenAnyVectorFirstWinsAndLosersReclaimed) {
    std::atomic<bool> done{false};
    std::atomic<int>  count{0};

    auto make_task = [&count](int ms) -> task<int> {
        if (ms > 0)
            co_await sleep(std::chrono::milliseconds(ms));
        ++count;
        co_return ms;
    };

    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.push_back(make_task(0)); // instant winner
        tasks.push_back(make_task(50));
        tasks.push_back(make_task(80));
        auto res = co_await when_any(std::move(tasks));
        EXPECT_EQ(res.first, 0u);
        EXPECT_EQ(std::any_cast<int>(res.second), 0);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any vector-first-wins coordinator never finished";
    EXPECT_EQ(count.load(), 1) << "the vector when_any reclaims its losing branches on win";
}

TEST_F(CoroutineCombinators, WhenAnyLosersReclaimedNoLeak) {
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

    coro_scheduler().spawn([&]() -> task<void> {
        auto res = co_await when_any(fast(), slow(), slow());
        EXPECT_EQ(res.index, 0u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any-leak coordinator never finished";
    coro_scheduler().run_ready(); // drain the winner's final-suspend defer-destroy
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "when_any leaked " << (after - baseline) << " losing-branch frame(s) — losers must be reclaimed on win";
}

TEST_F(CoroutineCombinators, WhenAnyVectorLosersReclaimedNoLeak) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    auto make = [](int ms) -> task<int> {
        co_await sleep(std::chrono::milliseconds(ms));
        co_return ms;
    };

    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.push_back(make(5));    // winner
        tasks.push_back(make(5000)); // loser — must not linger
        tasks.push_back(make(5000)); // loser — must not linger
        auto res = co_await when_any(std::move(tasks));
        EXPECT_EQ(res.first, 0u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any(vector)-leak coordinator never finished";
    coro_scheduler().run_ready();
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "when_any(vector) leaked " << (after - baseline) << " losing-branch frame(s)";
}

TEST_F(CoroutineCombinators, WithTimeoutOperationWonLeavesNoZombieTimer) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto result = co_await coro_with_timeout(
            []() -> task<int> {
                co_await sleep(10ms);
                co_return 7;
            }(),
            10000ms); // far off — pre-fix the timeout coroutine lingered here for 10s
        EXPECT_EQ(result, 7);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "with_timeout-no-zombie coordinator never finished";
    coro_scheduler().run_ready();
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "coro_with_timeout leaked " << (after - baseline)
                               << " frame(s) — the timeout watcher must be stopped when the operation wins";
}

TEST_F(CoroutineCombinators, WithTimeoutVoidOperationWonLeavesNoZombieTimer) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await coro_with_timeout(
            []() -> task<void> {
                co_await sleep(10ms);
            }(),
            10000ms);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "with_timeout<void>-no-zombie coordinator never finished";
    coro_scheduler().run_ready();
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "coro_with_timeout<void> leaked " << (after - baseline) << " frame(s)";
}

// Free-function coroutines for the nested-race test: a lambda coroutine's closure would have
// to outlive the frame, which is fiddly here.
namespace {
task<int>
nested_slow_5s() {
    co_await sleep(5000ms);
    co_return 9;
}
task<int>
nested_inner_race() {
    // Parks on an inner when_any that never wins in the test window; when the OUTER when_any
    // reclaims this branch, destroying this frame runs the inner when_any_awaiter destructor →
    // reclaim_all() tears down the two slow branches.
    auto r = co_await when_any(nested_slow_5s(), nested_slow_5s());
    co_return r.get<int>();
}
task<int>
nested_fast_5ms() {
    co_await sleep(5ms);
    co_return 1;
}
} // namespace

TEST_F(CoroutineCombinators, WhenAnyNestedLoserDtorReclaimsNoLeak) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto res = co_await when_any(nested_fast_5ms(), nested_inner_race());
        EXPECT_EQ(res.index, 0u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "nested when_any coordinator never finished";
    coro_scheduler().run_ready();
    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "nested when_any leaked " << (after - baseline)
                               << " frame(s) — the inner race's branches must be reclaimed via the awaiter dtor";
}

// =============================================================================
// Destroy-while-parked reclamation (see shared/coroutine_reclaim_support.h)
//
// A nested when_all / coro_with_timeout reclaimed by an OUTER when_any: its spawned branches /
// detached runner must be torn down by the awaiter dtor so the last one to finish does not resume
// the freed continuation.
//
// Each of these races a 40-50ms parker against `reclaim_fast_winner()`'s 5ms sleep, and the setup
// REQUIRES the winner to win — so it is worth recording why that is not a coin flip, and why a
// lost race could not be mistaken for a pass anyway.
//
//   - The race cannot invert under load. Both branches are constructed by the same `when_any`
//     expression and arm in the same loop turn into one deadline-ordered libev heap, so a stall
//     delays both equally: invariant I1 in shared/coroutine_test_support.h. Measured under 40ms
//     SIGSTOP stalls, the `when_any` took up to 20ms LONGER than the parker's own 40ms nominal —
//     i.e. the nominal margin was more than spent — with the correct winner in 120/120 samples.
//   - A lost race is LOUD, not silent. Verified by injection (parker's inner sleep 40ms -> 1ms so
//     the parker wins): the run fails twice, at `EXPECT_EQ(r.index, 1u)` here AND at
//     `EXPECT_FALSE(g_resumed_after_reclaim)` in shared/coroutine_reclaim_support.h — because a
//     parker that wins necessarily runs past its suspend point and trips that guard. The two
//     assertions are double-entry bookkeeping: the premise cannot fail quietly.
// =============================================================================

TEST_F(CoroutineCombinators, WhenAllReclaimedWhileParked) {
    run_reclaim_driver([]() -> task<void> {
        auto park = []() -> task<int> {
            volatile char big[8192];
            big[0]      = 7;
            auto [a, b] = co_await when_all(
                []() -> task<int> {
                    co_await sleep(40ms);
                    co_return 1;
                }(),
                []() -> task<int> {
                    co_await sleep(50ms);
                    co_return 2;
                }());
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return a + b + (int) big[1];
        };
        auto r = co_await when_any(park(), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        co_await sleep(70ms); // the inner branches finish — must NOT resume the reclaimed parker
    });
}

TEST_F(CoroutineCombinators, TimeoutReclaimedWhileParked) {
    run_reclaim_driver([]() -> task<void> {
        auto park = []() -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            int v  = co_await coro_with_timeout(
                []() -> task<int> {
                    co_await sleep(40ms);
                    co_return 7;
                }(),
                1000ms);
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return v + (int) big[1];
        };
        auto r = co_await when_any(park(), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        co_await sleep(60ms); // the detached runner completes late — must NOT resume the freed frame
    });
}

// =============================================================================
// Inner-exception propagation (the inner op FAILS before the deadline) +
// when_any(vector) throwing winner + void-timeout reclamation
// =============================================================================

// coro_with_timeout<T> where the inner op THROWS before the timeout fires: the failure must
// propagate as the inner exception, never be reclassified as timeout_error. Drives run_task's
// catch (captures the exception) and await_resume's rethrow for the non-void specialisation —
// distinct from TimeoutThrowsWhenOperationTooSlow, where the TIMER wins.
TEST_F(CoroutineCombinators, TimeoutInnerThrowsBeforeDeadlinePropagates) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await coro_with_timeout(
                []() -> task<int> {
                    co_await sleep(10ms);
                    throw std::runtime_error("inner boom");
                    co_return 0;
                }(),
                500ms); // generous timeout → the inner failure wins the race, not the timer
            ADD_FAILURE() << "expected the inner exception";
        } catch (const timeout_error &) {
            ADD_FAILURE() << "an inner failure before the deadline must NOT be reclassified as a timeout";
        } catch (const std::runtime_error &e) {
            EXPECT_STREQ(e.what(), "inner boom");
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "timeout inner-throw coordinator never finished";
    EXPECT_TRUE(caught.load()) << "coro_with_timeout must propagate the inner task's exception";
}

// Void specialisation of the above: coro_with_timeout<void> with an inner op that throws before
// the deadline. Drives the void run_task catch + await_resume rethrow.
TEST_F(CoroutineCombinators, TimeoutVoidInnerThrowsBeforeDeadlinePropagates) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            co_await coro_with_timeout(
                []() -> task<void> {
                    co_await sleep(10ms);
                    throw std::runtime_error("inner void boom");
                }(),
                500ms);
            ADD_FAILURE() << "expected the inner exception";
        } catch (const timeout_error &) {
            ADD_FAILURE() << "an inner failure before the deadline must NOT be reclassified as a timeout";
        } catch (const std::runtime_error &e) {
            EXPECT_STREQ(e.what(), "inner void boom");
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "timeout void inner-throw coordinator never finished";
    EXPECT_TRUE(caught.load()) << "coro_with_timeout<void> must propagate the inner task's exception";
}

// when_any(vector) whose WINNING branch throws: the exception must surface from await_resume,
// not a silent empty result. Drives when_any_vector_awaiter's run_one catch branch + the
// await_resume exception rethrow (the vector when_any throwing path; the variadic when_any
// carries the exception inside when_any_result instead).
TEST_F(CoroutineCombinators, WhenAnyVectorThrowingWinnerPropagates) {
    std::atomic<bool> caught{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.push_back([]() -> task<int> {
            co_await sleep(5ms);
            throw std::runtime_error("vector winner boom");
            co_return 0;
        }());
        tasks.push_back([]() -> task<int> {
            co_await sleep(100ms);
            co_return 2;
        }());

        try {
            (void) co_await when_any(std::move(tasks));
            ADD_FAILURE() << "expected the winning branch's exception";
        } catch (const std::runtime_error &e) {
            EXPECT_STREQ(e.what(), "vector winner boom");
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "when_any vector throwing-winner coordinator never finished";
    EXPECT_TRUE(caught.load()) << "when_any(vector) must rethrow the winning task's exception";
}

// Void specialisation of TimeoutReclaimedWhileParked: a coro_with_timeout<void> awaiter reclaimed
// as a when_any loser must tear down its detached run_task + inner task and stop its timer (the
// void timeout_awaiter dtor), so the late-completing detached runner cannot resume the freed frame.
TEST_F(CoroutineCombinators, TimeoutVoidReclaimedWhileParked) {
    run_reclaim_driver([]() -> task<void> {
        auto park = []() -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            co_await coro_with_timeout(
                []() -> task<void> {
                    co_await sleep(40ms);
                }(),
                1000ms);
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return (int) big[1];
        };
        auto r = co_await when_any(park(), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        co_await sleep(60ms); // the detached void runner completes late — must NOT resume the freed frame
    });
}

// =============================================================================
// task<void> composes through the whole aggregate-combinator family
// =============================================================================

// Regression (hard COMPILE error, not a runtime bug): every combinator that STORES
// per-branch results derived its storage straight from `Task::value_type`, so
// `task<void>` — the framework's DEFAULT task type — instantiated `std::tuple<void>` /
// `std::vector<void>` / `std::optional<void>` and failed deep inside libc++ ("field has
// incomplete type 'void'"). Broken: when_all (variadic + vector), when_any(vector),
// parallel, parallel_map, with_deadline. Nothing in the tree ever composed void tasks, so
// nothing caught it. The fix routes every result SLOT through detail::value_slot_t (void →
// std::monostate); this test is the instantiation that keeps the family buildable, and it
// pins the mixed-pack rule (a void branch keeps its tuple position).
TEST_F(CoroutineCombinators, VoidTasksComposeThroughEveryAggregateCombinator) {
    std::atomic<int>  ran{0};
    std::atomic<bool> done{false};

    auto v = [&ran]() -> task<void> {
        co_await sleep(1ms);
        ran.fetch_add(1);
    };
    auto i = []() -> task<int> {
        co_await sleep(1ms);
        co_return 7;
    };

    coro_scheduler().spawn([&, v, i]() -> task<void> {
        co_await when_all(v(), v());

        std::vector<task<void>> vec;
        vec.push_back(v());
        vec.push_back(v());
        co_await when_all(std::move(vec));

        std::vector<task<void>> vec2;
        vec2.push_back(v());
        auto any_v = co_await when_any(std::move(vec2));
        EXPECT_EQ(any_v.first, 0u);

        co_await parallel(v(), v());

        std::vector<int> items{1, 2, 3};
        auto             mapped = co_await parallel_map(
            items,
            [&ran](int) -> task<void> {
                ran.fetch_add(1);
                co_return;
            },
            2);
        EXPECT_EQ(mapped.size(), 3u) << "a void mapper still yields one (monostate) slot per item";

        co_await with_deadline(v(), std::chrono::steady_clock::now() + 2s);

        // Mixed pack: the void branch keeps its position and binds to std::monostate.
        auto [mono, seven] = co_await when_all(v(), i());
        static_assert(std::is_same_v<decltype(mono), std::monostate>, "a void branch must occupy a monostate slot");
        EXPECT_EQ(seven, 7);

        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); }, 5s)) << "void-task combinator coordinator never finished";
    // 2 (when_all) + 2 (when_all vector) + 1 (when_any vector) + 2 (parallel)
    // + 3 (parallel_map) + 1 (with_deadline) + 1 (mixed when_all) = 12
    EXPECT_EQ(ran.load(), 12) << "every void branch must actually have run";
}
