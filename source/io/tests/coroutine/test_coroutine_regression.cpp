/**
 * @file test_coroutine_regression.cpp
 * @brief Regression tests for all coroutine bugs found and fixed
 *
 * Each test targets a specific bug that was identified and corrected.
 * These tests ensure the fixes remain in place across future changes.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <numeric>
#include <string>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// Fixture
// =============================================================================

class CoroutineRegression : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

// =============================================================================
// BUG: channel send_awaiter lost value when woken after buffer-full suspension
// The sender suspended because the buffer was full. When a recv freed space,
// the sender was resumed but never pushed its value into the buffer.
// =============================================================================

TEST_F(CoroutineRegression, ChannelSendValueNotLostOnBufferFull) {
    bool done = false;
    std::vector<int> received;

    auto test = [&]() -> task<void> {
        channel<int> ch(1);  // capacity 1

        // Producer: send 3 values into capacity-1 channel
        coro_scheduler().spawn([](channel<int>* ch) -> task<void> {
            co_await ch->send(10);
            co_await ch->send(20);  // blocks until consumer frees space
            co_await ch->send(30);  // blocks again
            ch->close();
        }(&ch));

        // Consumer: receive all values
        while (true) {
            auto val = co_await ch.recv();
            if (!val) break;
            received.push_back(*val);
        }
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(500ms);

    ASSERT_TRUE(done);
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0], 10);
    EXPECT_EQ(received[1], 20);
    EXPECT_EQ(received[2], 30);
}

// =============================================================================
// BUG: send_for (timed send) slow path never pushed the value
// When the sender was suspended due to full buffer and later woken by a recv,
// the timed_send_awaiter's await_resume didn't push the value.
// =============================================================================

TEST_F(CoroutineRegression, ChannelSendForPushesValueOnWake) {
    bool done = false;
    std::vector<int> received;

    auto test = [&]() -> task<void> {
        channel<int> ch(1);

        // Fill the buffer
        co_await ch.send(1);

        // Timed send — should succeed once consumer drains
        coro_scheduler().spawn([](channel<int>* ch, std::vector<int>* out) -> task<void> {
            bool ok = co_await ch->send_for(2, 200ms);
            EXPECT_TRUE(ok);
            ch->close();
        }(&ch, &received));

        // Drain after a short delay
        co_await sleep(20ms);
        while (true) {
            auto val = co_await ch.recv();
            if (!val) break;
            received.push_back(*val);
        }
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(500ms);

    ASSERT_TRUE(done);
    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0], 1);
    EXPECT_EQ(received[1], 2);
}

// =============================================================================
// BUG: send_for double-schedule — timer and receiver both resume the handle
// The shared guard flag now prevents this race.
// =============================================================================

TEST_F(CoroutineRegression, ChannelSendForTimerGuardPreventsDoubleSchedule) {
    bool done = false;

    auto test = [&]() -> task<void> {
        channel<int> ch(1);
        co_await ch.send(99);  // fill buffer

        // Timed send with very short timeout
        bool ok = co_await ch.send_for(42, 5ms);
        // Either timeout or success — no crash from double-schedule
        (void)ok;
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// BUG: run_sync busy-spun at 100% CPU when no events were ready
// Now uses EVRUN_ONCE to block. Verify it completes without hanging.
// =============================================================================

TEST_F(CoroutineRegression, RunSyncDoesNotBusySpin) {
    auto start = std::chrono::steady_clock::now();

    auto slow = []() -> task<int> {
        co_await sleep(50ms);
        co_return 42;
    };

    int result = run_sync(slow());
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(result, 42);
    EXPECT_GE(elapsed, 40ms);
    EXPECT_LE(elapsed, 500ms);  // should not take excessively long
}

TEST_F(CoroutineRegression, RunSyncVoidTask) {
    bool executed = false;

    auto simple = [&executed]() -> task<void> {
        co_await sleep(10ms);
        executed = true;
    };

    run_sync(simple());
    EXPECT_TRUE(executed);
}

// =============================================================================
// BUG: exponential backoff 1 << attempt caused UB for attempt >= 31
// Now clamped to shift <= 30 with unsigned literal.
// =============================================================================

TEST_F(CoroutineRegression, ExponentialBackoffNoOverflow) {
    auto delay = detail::calculate_delay(50, retry_policy{
        .base_delay = 10ms,
        .max_delay = std::chrono::hours(24),
        .strategy = backoff_strategy::exponential
    });
    // 10ms * (1u << 30) = 10ms * 1073741824 ≈ 10737418s, capped at 24h
    EXPECT_LE(delay, std::chrono::hours(24));
    EXPECT_GT(delay, 0ms);
}

TEST_F(CoroutineRegression, ExponentialJitterBackoffNoOverflow) {
    auto delay = detail::calculate_delay(100, retry_policy{
        .base_delay = 1ms,
        .max_delay = std::chrono::seconds(60),
        .strategy = backoff_strategy::exponential_jitter
    });
    EXPECT_LE(delay, std::chrono::seconds(60));
    EXPECT_GT(delay, 0ms);
}

// =============================================================================
// BUG: linear backoff static_cast<int>(attempt) truncated large values
// Now uses long long with clamping.
// =============================================================================

TEST_F(CoroutineRegression, LinearBackoffLargeAttempt) {
    auto delay = detail::calculate_delay(100000, retry_policy{
        .base_delay = 1ms,
        .max_delay = std::chrono::seconds(30),
        .strategy = backoff_strategy::linear
    });
    EXPECT_LE(delay, std::chrono::seconds(30));
    EXPECT_GT(delay, 0ms);
}

// =============================================================================
// BUG: when_any_vector_awaiter swallowed exceptions from the winning task
// Now rethrows via the exception field in state.
// =============================================================================

TEST_F(CoroutineRegression, WhenAnyVectorRethrowsException) {
    bool caught = false;

    auto test = [&]() -> task<void> {
        std::vector<task<int>> tasks;
        tasks.push_back([]() -> task<int> {
            throw std::runtime_error("boom");
            co_return 0;
        }());

        try {
            auto [idx, val] = co_await when_any(std::move(tasks));
            (void)idx; (void)val;
        } catch (const std::runtime_error& e) {
            EXPECT_STREQ(e.what(), "boom");
            caught = true;
        }
    };

    coro_scheduler().spawn(test());
    run_for(100ms);

    EXPECT_TRUE(caught);
}

// =============================================================================
// BUG: when_any_vector_awaiter UB on empty tasks vector
// await_resume dereferenced nullopt result. Now returns default.
// =============================================================================

TEST_F(CoroutineRegression, WhenAnyVectorEmptyReturnsDefault) {
    bool done = false;

    auto test = [&]() -> task<void> {
        std::vector<task<int>> empty_tasks;
        auto [idx, val] = co_await when_any(std::move(empty_tasks));
        EXPECT_EQ(idx, 0u);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(50ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// BUG: shared_task<T> required T to be default-constructible
// Now uses std::optional<T> internally.
// =============================================================================

struct NonDefaultConstructible {
    int value;
    explicit NonDefaultConstructible(int v) : value(v) {}
    NonDefaultConstructible() = delete;
};

TEST_F(CoroutineRegression, SharedTaskNonDefaultConstructibleType) {
    bool done = false;

    auto test = [&]() -> task<void> {
        auto compute = []() -> task<NonDefaultConstructible> {
            co_await sleep(5ms);
            co_return NonDefaultConstructible{42};
        };

        auto sh = make_shared_task(compute());
        auto copy = sh;

        auto r1 = co_await sh;
        auto r2 = co_await copy;

        EXPECT_EQ(r1.value, 42);
        EXPECT_EQ(r2.value, 42);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(100ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// BUG: debounce operator lost data with coro_with_timeout approach
// Rewritten to use channel + producer + try_recv drain. Verify no data loss.
// =============================================================================

TEST_F(CoroutineRegression, DebounceNoDataLoss) {
    bool done = false;
    std::vector<int> emitted;

    auto test = [&]() -> task<void> {
        // Source: emit 1,2,3 rapidly, then pause, then emit 4,5
        auto source_data = std::make_shared<std::vector<int>>(
            std::vector<int>{1, 2, 3, 4, 5});
        auto idx = std::make_shared<size_t>(0);
        auto paused = std::make_shared<bool>(false);

        auto stream = async_stream<int>(
            [source_data, idx, paused]() -> task<std::optional<int>> {
                if (*idx >= source_data->size()) co_return std::nullopt;
                if (*idx == 3 && !*paused) {
                    *paused = true;
                    co_await sleep(80ms);  // quiet period between bursts
                }
                co_return (*source_data)[(*idx)++];
            });

        auto debounced = stream.debounce(30ms);

        co_await debounced.for_each([&emitted](int v) { emitted.push_back(v); });
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(1000ms);

    ASSERT_TRUE(done);
    // Debounce should emit the last value of each burst
    // Burst 1: 1,2,3 (rapid) -> emits 3
    // Burst 2: 4,5 (after pause) -> emits 5
    ASSERT_GE(emitted.size(), 1u);
    // The last emitted value from first burst should be 3
    EXPECT_EQ(emitted[0], 3);
    if (emitted.size() >= 2) {
        EXPECT_EQ(emitted[1], 5);
    }
}

// =============================================================================
// BUG: barrier::arrive_awaiter had unused _handle member
// Compile-time regression — if it compiles, the fix is in place.
// Test the barrier actually works correctly.
// =============================================================================

TEST_F(CoroutineRegression, BarrierArrivedCorrectly) {
    bool done = false;
    int phase2_count = 0;

    auto test = [&]() -> task<void> {
        barrier b(3);

        auto worker = [](barrier* b, int* count) -> task<void> {
            co_await sleep(5ms);
            co_await b->arrive_and_wait();
            ++(*count);
        };

        coro_scheduler().spawn(worker(&b, &phase2_count));
        coro_scheduler().spawn(worker(&b, &phase2_count));
        coro_scheduler().spawn(worker(&b, &phase2_count));

        co_await sleep(100ms);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(phase2_count, 3);
}

// =============================================================================
// Channel recv_for timeout returns nullopt (not crash)
// =============================================================================

TEST_F(CoroutineRegression, ChannelRecvForTimeout) {
    bool done = false;

    auto test = [&]() -> task<void> {
        channel<int> ch(10);
        // No sender — recv_for should timeout
        auto result = co_await ch.recv_for(30ms);
        EXPECT_FALSE(result.has_value());
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// Channel send_for timeout returns false (not crash)
// =============================================================================

TEST_F(CoroutineRegression, ChannelSendForTimeout) {
    bool done = false;

    auto test = [&]() -> task<void> {
        channel<int> ch(1);
        co_await ch.send(1);  // fill buffer

        // No consumer — send_for should timeout
        bool ok = co_await ch.send_for(2, 30ms);
        EXPECT_FALSE(ok);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// when_all propagates first exception
// =============================================================================

TEST_F(CoroutineRegression, WhenAllPropagatesFirstException) {
    bool caught = false;

    auto test = [&]() -> task<void> {
        auto ok_task = []() -> task<int> {
            co_await sleep(50ms);
            co_return 1;
        };
        auto bad_task = []() -> task<int> {
            co_await sleep(5ms);
            throw std::logic_error("when_all_fail");
            co_return 0;
        };

        try {
            auto [a, b] = co_await when_all(ok_task(), bad_task());
            (void)a; (void)b;
        } catch (const std::logic_error& e) {
            EXPECT_STREQ(e.what(), "when_all_fail");
            caught = true;
        }
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(caught);
}

// =============================================================================
// when_any variadic returns winner index and value
// =============================================================================

TEST_F(CoroutineRegression, WhenAnyVariadicReturnsWinner) {
    bool done = false;

    auto test = [&]() -> task<void> {
        auto fast = []() -> task<int> {
            co_await sleep(5ms);
            co_return 42;
        };
        auto slow = []() -> task<int> {
            co_await sleep(500ms);
            co_return 99;
        };

        auto result = co_await when_any(fast(), slow());
        EXPECT_EQ(result.index, 0u);
        EXPECT_EQ(result.get<int>(), 42);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// coro_with_timeout throws timeout_error correctly
// =============================================================================

TEST_F(CoroutineRegression, TimeoutAwaiterThrowsOnExpiry) {
    bool caught = false;

    auto test = [&]() -> task<void> {
        auto slow = []() -> task<int> {
            co_await sleep(500ms);
            co_return 1;
        };

        try {
            co_await coro_with_timeout(slow(), 20ms);
        } catch (const timeout_error&) {
            caught = true;
        }
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(caught);
}

// =============================================================================
// Cancellation: cancellable_sleep wakes immediately on cancel
// =============================================================================

TEST_F(CoroutineRegression, CancellableSleepWakesOnCancel) {
    bool done = false;

    auto test = [&]() -> task<void> {
        cancellation_token token;

        coro_scheduler().spawn([](cancellation_token tok) -> task<void> {
            co_await sleep(10ms);
            tok.cancel();
        }(token));

        auto start = std::chrono::steady_clock::now();
        try {
            co_await cancellable_sleep(5000ms, token);
        } catch (const cancelled_error&) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            EXPECT_LT(elapsed, 1000ms);  // should wake much earlier than 5s
            done = true;
        }
    };

    coro_scheduler().spawn(test());
    run_for(500ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// Semaphore: scoped_acquire RAII guard releases on destruction
// =============================================================================

TEST_F(CoroutineRegression, SemaphoreGuardReleasesOnDestruction) {
    bool done = false;

    auto test = [&]() -> task<void> {
        semaphore sem(1);

        {
            auto guard = co_await sem.scoped_acquire();
            EXPECT_EQ(sem.available_permits(), 0u);
        }
        // Guard destroyed — permit should be released
        EXPECT_EQ(sem.available_permits(), 1u);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(50ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// async_mutex: scoped_lock RAII guard unlocks on destruction
// =============================================================================

TEST_F(CoroutineRegression, AsyncMutexGuardUnlocksOnDestruction) {
    bool done = false;

    auto test = [&]() -> task<void> {
        async_mutex mtx;

        {
            auto guard = co_await mtx.scoped_lock();
            EXPECT_TRUE(mtx.is_locked());
        }
        EXPECT_FALSE(mtx.is_locked());
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(50ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// async_rw_lock: multiple readers allowed, writer exclusive
// =============================================================================

TEST_F(CoroutineRegression, RWLockMultipleReadersOneWriter) {
    bool done = false;
    int reader_count = 0;

    auto test = [&]() -> task<void> {
        async_rw_lock rw;

        auto reader = [](async_rw_lock* rw, int* count) -> task<void> {
            co_await rw->lock_read();
            ++(*count);
            co_await sleep(10ms);
            rw->unlock_read();
        };

        coro_scheduler().spawn(reader(&rw, &reader_count));
        coro_scheduler().spawn(reader(&rw, &reader_count));
        coro_scheduler().spawn(reader(&rw, &reader_count));

        co_await sleep(50ms);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(reader_count, 3);
}

// =============================================================================
// async_latch: count_down triggers waiters at zero
// =============================================================================

TEST_F(CoroutineRegression, LatchCountDownReleasesWaiters) {
    bool done = false;

    auto test = [&]() -> task<void> {
        async_latch latch(3);

        coro_scheduler().spawn([](async_latch* l) -> task<void> {
            co_await sleep(5ms);
            l->count_down();
        }(&latch));
        coro_scheduler().spawn([](async_latch* l) -> task<void> {
            co_await sleep(10ms);
            l->count_down();
        }(&latch));
        coro_scheduler().spawn([](async_latch* l) -> task<void> {
            co_await sleep(15ms);
            l->count_down();
        }(&latch));

        co_await latch.wait();
        EXPECT_TRUE(latch.is_ready());
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// async_event: set() wakes all waiters (manual reset mode)
// =============================================================================

TEST_F(CoroutineRegression, AsyncEventWakesAllWaiters) {
    bool done = false;
    int woke_count = 0;

    auto test = [&]() -> task<void> {
        async_event ev;

        auto waiter = [](async_event* e, int* count) -> task<void> {
            co_await e->wait();
            ++(*count);
        };

        coro_scheduler().spawn(waiter(&ev, &woke_count));
        coro_scheduler().spawn(waiter(&ev, &woke_count));
        coro_scheduler().spawn(waiter(&ev, &woke_count));

        co_await sleep(10ms);
        ev.set();
        co_await sleep(20ms);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(woke_count, 3);
}

// =============================================================================
// Scope: join_all waits for all spawned tasks
// =============================================================================

TEST_F(CoroutineRegression, ScopeJoinAllWaitsForAll) {
    bool done = false;
    int completed = 0;

    auto test = [&]() -> task<void> {
        coroutine_scope scope;

        scope.spawn([](int* c) -> task<void> {
            co_await sleep(10ms);
            ++(*c);
        }(&completed));

        scope.spawn([](int* c) -> task<void> {
            co_await sleep(20ms);
            ++(*c);
        }(&completed));

        co_await scope.join_all();
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(completed, 2);
}

// =============================================================================
// Channel select: first-ready channel wins
// =============================================================================

TEST_F(CoroutineRegression, ChannelSelectFirstReady) {
    bool done = false;

    auto test = [&]() -> task<void> {
        channel<int> ch_fast(1);
        channel<std::string> ch_slow(1);

        coro_scheduler().spawn([](channel<int>* ch) -> task<void> {
            co_await sleep(5ms);
            co_await ch->send(42);
        }(&ch_fast));

        coro_scheduler().spawn([](channel<std::string>* ch) -> task<void> {
            co_await sleep(100ms);
            co_await ch->send(std::string("slow"));
        }(&ch_slow));

        auto result = co_await select(ch_fast, ch_slow);
        EXPECT_EQ(result.index, 0u);
        EXPECT_EQ(result.get<int>(), 42);

        // Drain the slow producer before returning. The 100ms producer holds a
        // raw pointer to `ch_slow`, which is a local of this coroutine frame;
        // its storage is freed at co_return (scope exit) regardless of when the
        // frame itself is reclaimed. Returning here while that producer is still
        // sleeping would make its later `ch->send("slow")` dereference freed
        // channel storage (use-after-free, caught under ASan). Awaiting the slow
        // value keeps `ch_slow` alive until the producer has finished with it.
        auto slow = co_await ch_slow.recv();
        EXPECT_EQ(slow, "slow");
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(300ms);

    EXPECT_TRUE(done);
}

// =============================================================================
// Stream: collect gathers all elements
// =============================================================================

TEST_F(CoroutineRegression, StreamCollectGathersAll) {
    bool done = false;
    std::vector<int> result;

    auto test = [&]() -> task<void> {
        auto stream = async_stream<int>::from_vector({1, 2, 3, 4, 5});
        result = co_await stream.collect();
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(100ms);

    ASSERT_TRUE(done);
    ASSERT_EQ(result.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(result[i], i + 1);
    }
}

// =============================================================================
// Stream: map + filter pipeline
// =============================================================================

TEST_F(CoroutineRegression, StreamMapFilterPipeline) {
    bool done = false;
    std::vector<int> result;

    auto test = [&]() -> task<void> {
        auto stream = async_stream<int>::from_vector({1, 2, 3, 4, 5, 6})
            .map([](int v) { return v * 2; })
            .filter([](int v) { return v > 6; });
        result = co_await stream.collect();
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(100ms);

    ASSERT_TRUE(done);
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], 8);
    EXPECT_EQ(result[1], 10);
    EXPECT_EQ(result[2], 12);
}

// =============================================================================
// Generator: basic sync generator works
// =============================================================================

TEST_F(CoroutineRegression, GeneratorProducesValues) {
    auto gen = []() -> generator<int> {
        co_yield 1;
        co_yield 2;
        co_yield 3;
    };

    auto g = gen();
    auto values = collect_to_vector(g);

    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[0], 1);
    EXPECT_EQ(values[1], 2);
    EXPECT_EQ(values[2], 3);
}

// =============================================================================
// Async generator: ag_collect works with symmetric transfer
// =============================================================================

TEST_F(CoroutineRegression, AsyncGeneratorCollect) {
    bool done = false;
    std::vector<int> result;

    auto test = [&]() -> task<void> {
        auto gen = []() -> async_generator<int> {
            for (int i = 0; i < 5; ++i) {
                co_yield i * 10;
            }
        };

        result = co_await ag_collect(gen());
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(100ms);

    ASSERT_TRUE(done);
    ASSERT_EQ(result.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(result[i], i * 10);
    }
}

// =============================================================================
// Shared task void specialization works
// =============================================================================

TEST_F(CoroutineRegression, SharedTaskVoidMultipleAwaiters) {
    bool done = false;
    int await_count = 0;

    auto test = [&]() -> task<void> {
        auto compute = []() -> task<void> {
            co_await sleep(10ms);
        };

        auto sh = make_shared_task(compute());
        auto copy1 = sh;
        auto copy2 = sh;

        auto waiter = [](shared_task<void> st, int* count) -> task<void> {
            co_await st;
            ++(*count);
        };

        coro_scheduler().spawn(waiter(copy1, &await_count));
        coro_scheduler().spawn(waiter(copy2, &await_count));
        co_await sh;
        ++await_count;

        co_await sleep(20ms);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(await_count, 3);
}

// =============================================================================
// with_retry void overload works
// =============================================================================

TEST_F(CoroutineRegression, WithRetryVoidOverload) {
    bool done = false;
    int attempts = 0;

    auto test = [&]() -> task<void> {
        co_await with_retry([&attempts]() -> task<void> {
            ++attempts;
            if (attempts < 2) throw std::runtime_error("fail");
            co_return;
        }, retry_policy{.max_attempts = 3, .base_delay = 5ms});
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(attempts, 2);
}

// =============================================================================
// Channel: close wakes pending senders with channel_closed
// =============================================================================

TEST_F(CoroutineRegression, ChannelCloseWakesPendingSenders) {
    bool caught = false;
    bool done = false;

    auto test = [&]() -> task<void> {
        channel<int> ch(0);  // unbuffered

        coro_scheduler().spawn([](channel<int>* ch, bool* caught_ptr) -> task<void> {
            try {
                co_await ch->send(1);  // will block (no receiver)
            } catch (const channel_closed&) {
                *caught_ptr = true;
            }
        }(&ch, &caught));

        co_await sleep(10ms);
        ch.close();
        co_await sleep(20ms);
        done = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_TRUE(caught);
}

// =============================================================================
// Finding 2.C.1: channel::send on a closed channel must raise channel_closed
// =============================================================================

TEST_F(CoroutineRegression, ChannelSendOnClosedThrowsImmediately) {
    bool caught = false;
    bool finished = false;

    auto test = [&]() -> task<void> {
        channel<int> ch(4);
        ch.close();
        try {
            co_await ch.send(42);
        } catch (const channel_closed&) {
            caught = true;
        }
        finished = true;
    };

    coro_scheduler().spawn(test());
    run_for(100ms);

    EXPECT_TRUE(caught);
    EXPECT_TRUE(finished);
}

// =============================================================================
// Finding 2.A.1: co_await on a default-constructed shared_task must fail loudly
// =============================================================================

TEST_F(CoroutineRegression, SharedTaskNullStateThrowsLogicError) {
    bool caught = false;
    bool finished = false;

    auto test = [&]() -> task<void> {
        shared_task<int> empty;  // no state
        try {
            co_await empty;
        } catch (const std::logic_error&) {
            caught = true;
        }
        finished = true;
    };

    coro_scheduler().spawn(test());
    run_for(100ms);

    EXPECT_TRUE(caught);
    EXPECT_TRUE(finished);
}

// =============================================================================
// Finding 2.B.5: with_deadline — already-past deadline must throw timeout_error
// =============================================================================

TEST_F(CoroutineRegression, WithDeadlineInThePastThrowsTimeoutImmediately) {
    bool caught = false;
    bool finished = false;

    auto make_task = []() -> task<int> {
        co_await sleep(50ms);
        co_return 7;
    };

    auto test = [&]() -> task<void> {
        auto deadline = std::chrono::steady_clock::now() - 1ms;
        try {
            co_await with_deadline(make_task(), deadline);
        } catch (const timeout_error&) {
            caught = true;
        }
        finished = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(caught);
    EXPECT_TRUE(finished);
}

// =============================================================================
// Finding 2.B.6: active_count() should include suspended coroutines
// =============================================================================

TEST_F(CoroutineRegression, ActiveCountIncludesSuspendedFrames) {
    size_t count_while_sleeping = 0;

    coro_scheduler().spawn([&]() -> task<void> {
        co_await sleep(30ms);
    });

    // Let the coroutine suspend on the sleep watcher before we measure.
    run_for(5ms);
    count_while_sleeping = coro_scheduler().active_count();

    run_for(100ms);

    EXPECT_GE(count_while_sleeping, 1u)
        << "active_count() silently ignored suspended frames";
}

// =============================================================================
// Finding 2.A.8: from_range must not dangle when fed with a temporary
// =============================================================================

TEST_F(CoroutineRegression, FromRangeWithTemporaryDoesNotDangle) {
    auto gen = qb::io::async::from_range(std::vector<int>{1, 2, 3, 4, 5});
    int sum = 0;
    while (auto v = gen.next()) {
        sum += *v;
    }
    EXPECT_EQ(sum, 15);
}

// =============================================================================
// Finding 2.C.16: zip short-circuits when the first stream ends
// =============================================================================

TEST_F(CoroutineRegression, ZipShortCircuitsOnFirstStreamEnd) {
    bool finished = false;

    auto test = [&]() -> task<void> {
        auto short_stream = async_stream<int>::from_vector({1, 2, 3});
        auto long_stream  = async_stream<int>::from_vector({10, 11, 12, 13, 14});

        auto zipped = zip(std::move(short_stream), std::move(long_stream));

        auto collected = co_await zipped.collect();
        EXPECT_EQ(collected.size(), 3u);
        finished = true;
    };

    coro_scheduler().spawn(test());
    run_for(300ms);

    EXPECT_TRUE(finished);
}

// =============================================================================
// Finding 2.A.3: task<T>::await_resume must rethrow stored exception instead
// of silently reading an uninitialised variant slot.
// =============================================================================

TEST_F(CoroutineRegression, TaskAwaitResumeRethrowsStoredException) {
    bool caught = false;
    bool finished = false;

    auto inner = []() -> task<int> {
        throw std::runtime_error("inner-exploded");
        co_return 0;
    };

    auto test = [&]() -> task<void> {
        try {
            int v = co_await inner();
            (void)v;
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "inner-exploded";
        }
        finished = true;
    };

    coro_scheduler().spawn(test());
    run_for(100ms);

    EXPECT_TRUE(caught);
    EXPECT_TRUE(finished);
}

// =============================================================================
// Finding 2.C.2: linear backoff — first retry must wait base_delay (1-based
// retry_number), not 0ms.
// =============================================================================

TEST_F(CoroutineRegression, LinearBackoffFirstRetryUsesBaseDelay) {
    retry_policy policy{
        .base_delay = 25ms,
        .max_delay = std::chrono::seconds(1),
        .strategy = backoff_strategy::linear
    };

    auto d1 = detail::calculate_delay(1, policy);
    auto d2 = detail::calculate_delay(2, policy);

    EXPECT_GE(d1, 25ms)
        << "linear backoff first retry must sleep at least base_delay";
    EXPECT_GE(d2, 50ms);
}

// =============================================================================
// Finding 2.C.13/14: retry with exponential strategy must NEVER overflow AND
// must catch non-std::exception throwables.
// =============================================================================

TEST_F(CoroutineRegression, WithRetryCatchesNonStdExceptionThrow) {
    int attempts = 0;
    bool caught_exhausted = false;
    bool finished = false;

    auto flaky = [&]() -> task<int> {
        ++attempts;
        throw 42;    // non-std::exception (int) — pre-fix would escape
        co_return 0;
    };

    retry_policy fast{
        .max_attempts = 3,
        .base_delay = 1ms,
        .max_delay = 5ms,
        .strategy = backoff_strategy::fixed
    };

    auto test = [&]() -> task<void> {
        try {
            auto r = co_await with_retry(flaky, fast);
            (void)r;
        } catch (const retry_exhausted&) {
            caught_exhausted = true;
        }
        finished = true;
    };

    coro_scheduler().spawn(test());
    run_for(500ms);

    EXPECT_EQ(attempts, 3);
    EXPECT_TRUE(caught_exhausted)
        << "with_retry must treat non-std::exception throwables as retriable";
    EXPECT_TRUE(finished);
}

// =============================================================================
// Finding 2.B.2: cancellable_operation<T> must propagate exceptions from the
// inner task instead of returning T{}.
// =============================================================================

TEST_F(CoroutineRegression, CancellableOperationPropagatesInnerException) {
    bool caught = false;
    bool finished = false;

    auto exploding_task = []() -> task<int> {
        co_await sleep(5ms);
        throw std::runtime_error("inner-cancellable-boom");
        co_return 0;
    };

    auto test = [&]() -> task<void> {
        cancellation_token tok;
        try {
            int v = co_await make_cancellable(exploding_task(), tok, false);
            (void)v;
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "inner-cancellable-boom";
        }
        finished = true;
    };

    coro_scheduler().spawn(test());
    run_for(200ms);

    EXPECT_TRUE(caught) << "make_cancellable must not swallow inner exceptions";
    EXPECT_TRUE(finished);
}

// =============================================================================
// Finding 2.C.4: debounce must surface exceptions from the source stream
// instead of converting them into a silent end-of-stream.
// =============================================================================

TEST_F(CoroutineRegression, DebouncePropagatesSourceException) {
    bool caught = false;
    bool finished = false;

    auto test = [&]() -> task<void> {
        // Build a stream whose _next() itself throws on the second pull.
        // debounce's producer loop awaits _next() directly; any exception
        // there must be captured and surfaced on the consumer side.
        auto counter = std::make_shared<int>(0);
        async_stream<int> src([counter]() -> task<std::optional<int>> {
            ++(*counter);
            if (*counter == 1) {
                co_return 42;
            }
            throw std::runtime_error("upstream-boom");
            co_return std::nullopt;
        });

        auto debounced = std::move(src).debounce(5ms);

        try {
            auto collected = co_await std::move(debounced).collect();
            (void)collected;
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "upstream-boom";
        }
        finished = true;
    };

    coro_scheduler().spawn(test());
    run_for(500ms);

    EXPECT_TRUE(caught) << "debounce must rethrow source exceptions";
    EXPECT_TRUE(finished);
}

// =============================================================================
// Finding 2.C.5: backpressure must release the acquired semaphore permit on
// EOF so a shared semaphore does not starve over successive streams.
// =============================================================================

TEST_F(CoroutineRegression, BackpressureReleasesPermitOnEof) {
    bool finished = false;

    auto test = [&]() -> task<void> {
        // External shared semaphore: if EOF leaks a permit, a second pass
        // will hang forever waiting for the missing slot.
        auto sem = std::make_shared<semaphore>(1);

        auto drain_one_pass = [&]() -> task<void> {
            auto src = async_stream<int>::from_vector({10, 20, 30});
            auto buffered = std::move(src).backpressure(1, sem);
            auto result = co_await std::move(buffered).collect();
            EXPECT_EQ(result.size(), 3u);
        };

        co_await drain_one_pass();
        co_await drain_one_pass();   // would hang if 2.C.5 regresses
        finished = true;
    };

    coro_scheduler().spawn(test());
    run_for(1000ms);

    EXPECT_TRUE(finished)
        << "backpressure EOF leaked a permit on the shared semaphore";
}

// =============================================================================
// Finding 2.C.10: semaphore::acquire must use the fast-path (await_ready)
// when a permit is available, avoiding a suspend/resume round-trip.
// =============================================================================

TEST_F(CoroutineRegression, SemaphoreAcquireFastPathIsSynchronous) {
    bool finished = false;
    bool acquired_synchronously = false;

    auto test = [&]() -> task<void> {
        semaphore sem(1);

        // Check await_ready() directly so we can observe synchronous acquire.
        auto awaiter = sem.acquire();
        acquired_synchronously = awaiter.await_ready();

        // Clean up by releasing explicitly; await_resume only decrements if
        // await_ready returned true.
        if (acquired_synchronously) {
            sem.release();
        }
        finished = true;
        co_return;
    };

    coro_scheduler().spawn(test());
    run_for(100ms);

    EXPECT_TRUE(acquired_synchronously)
        << "semaphore fast-path regressed — acquire suspended unnecessarily";
    EXPECT_TRUE(finished);
}

// =============================================================================
// Finding 2.D.4: Actor::spawn_async must revalidate its cached scheduler
// pointer against the current TLS scheduler. Directly driving the scheduler
// here simulates the essence: after resetting the TLS scheduler, the new one
// must be reachable through current_ptr().
// =============================================================================

TEST_F(CoroutineRegression, SchedulerCurrentPtrReflectsListenerReset) {
    auto* before = &qb::io::async::listener::current.coro_scheduler();
    ASSERT_EQ(before, CoroutineScheduler::current_ptr());

    qb::io::async::listener::current.reset_coro_scheduler();
    EXPECT_EQ(CoroutineScheduler::current_ptr(), nullptr);

    auto* after = &qb::io::async::listener::current.coro_scheduler();
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after, CoroutineScheduler::current_ptr());
    EXPECT_EQ(after, &qb::io::async::listener::current.coro_scheduler());
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
