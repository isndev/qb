/**
 * @file qb/io/tests/coroutine/test-coroutine-sync.cpp
 * @brief Coroutine synchronization primitive tests
 *
 * This file contains tests for coroutine synchronization primitives, including
 * semaphores, async_mutex, async_rw_lock, barriers, async_event, async_latch, scoped
 * helpers, wait counts, reset behavior, and producer-consumer handshakes.
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

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Semaphore
// =============================================================================

class SemaphoreTests : public ::testing::Test {
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

/**
 * @test Semaphore limits concurrency
 * @brief Max N concurrent operations
 */
TEST_F(SemaphoreTests, LimitsConcurrency) {
    semaphore        sem(2);
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};

    auto worker = [&sem, &concurrent, &max_concurrent]() -> task<void> {
        co_await sem.acquire();

        int current      = ++concurrent;
        int expected_max = max_concurrent.load();
        while (current > expected_max && !max_concurrent.compare_exchange_weak(expected_max, current)) {
        }

        co_await sleep(20ms);
        --concurrent;
        sem.release();
    };

    // Spawn 5 workers, only 2 should run concurrently
    for (int i = 0; i < 5; ++i) {
        coro_scheduler().spawn(worker());
    }

    run_for(200ms);

    EXPECT_LE(max_concurrent, 2);
    EXPECT_EQ(sem.available_permits(), 2);
}

/**
 * @test Try acquire
 * @brief Non-blocking acquire
 */
TEST_F(SemaphoreTests, TryAcquire) {
    semaphore sem(1);

    EXPECT_TRUE(sem.try_acquire());
    EXPECT_FALSE(sem.try_acquire());

    sem.release();
    EXPECT_TRUE(sem.try_acquire());
}

/**
 * @test Scoped acquire
 * @brief RAII style acquire
 */
TEST_F(SemaphoreTests, ScopedAcquire) {
    semaphore         sem(1);
    std::atomic<bool> acquired{false};

    auto worker = [&sem, &acquired]() -> task<void> {
        auto guard = co_await sem.scoped_acquire();
        acquired   = true;
        // Auto-release on scope exit
    };

    auto t = worker();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);

    EXPECT_TRUE(acquired);
    // Permit returns when guard is destroyed (release is capped at total_permits)
    EXPECT_EQ(sem.available_permits(), sem.total_permits());
}

/**
 * @test Release cap
 * @brief release() never increases available beyond total permits
 */
TEST_F(SemaphoreTests, ReleaseCap) {
    semaphore sem(2);
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_FALSE(sem.try_acquire());
    sem.release();
    sem.release();
    // Over-release: should be capped at 2
    sem.release();
    sem.release();
    EXPECT_EQ(sem.available_permits(), 2u);
    EXPECT_EQ(sem.total_permits(), 2u);
}

/**
 * @test Cancellable acquire wakes and throws on cancel, retracting its claim.
 */
TEST_F(SemaphoreTests, CancelAcquireWakesAndRetracts) {
    semaphore          sem(1);
    cancellation_token token;
    std::atomic<bool>  cancelled{false};
    std::atomic<bool>  acquired{false};

    EXPECT_TRUE(sem.try_acquire()); // hold the only permit so the waiter must park

    auto waiter = [&sem, &token, &cancelled, &acquired]() -> task<void> {
        try {
            co_await sem.acquire(token); // parks (no permit)
            acquired = true;
        } catch (const cancelled_error &) {
            cancelled = true;
        }
    };
    coro_scheduler().spawn(waiter());
    run_for(20ms);
    EXPECT_FALSE(acquired);
    EXPECT_FALSE(cancelled);

    token.cancel(); // retract the parked acquirer
    run_for(20ms);
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(acquired);

    // The cancelled waiter must NOT have consumed a permit: releasing the holder restores it fully.
    sem.release();
    EXPECT_EQ(sem.available_permits(), 1u);
    EXPECT_TRUE(sem.try_acquire()); // permit is genuinely free
}

/**
 * @test A cancelled waiter is skipped so release() serves the next waiter.
 */
TEST_F(SemaphoreTests, CancelledWaiterSkippedNextServed) {
    semaphore          sem(1);
    cancellation_token tok_a;
    std::atomic<bool>  a_cancelled{false};
    std::atomic<bool>  b_acquired{false};

    EXPECT_TRUE(sem.try_acquire()); // hold the permit

    auto a = [&sem, &tok_a, &a_cancelled]() -> task<void> {
        try {
            co_await sem.acquire(tok_a);
        } catch (const cancelled_error &) {
            a_cancelled = true;
        }
    };
    auto b = [&sem, &b_acquired]() -> task<void> {
        co_await sem.acquire(); // plain waiter, queued behind A
        b_acquired = true;
        sem.release();
    };
    coro_scheduler().spawn(a());
    run_for(5ms);
    coro_scheduler().spawn(b());
    run_for(5ms);

    tok_a.cancel(); // A retracts
    run_for(5ms);
    sem.release();  // hand the permit to the next live waiter → B (not the retracted A)
    run_for(20ms);

    EXPECT_TRUE(a_cancelled);
    EXPECT_TRUE(b_acquired);
}

/**
 * @test A granted permit beats a racing cancel (no throw, no leak).
 */
TEST_F(SemaphoreTests, GrantedBeatsRacingCancel) {
    semaphore          sem(1);
    cancellation_token token;
    std::atomic<bool>  acquired{false};
    std::atomic<bool>  cancelled{false};

    EXPECT_TRUE(sem.try_acquire()); // hold the permit

    auto waiter = [&sem, &token, &acquired, &cancelled]() -> task<void> {
        try {
            co_await sem.acquire(token);
            acquired = true;
            sem.release();
        } catch (const cancelled_error &) {
            cancelled = true;
        }
    };
    coro_scheduler().spawn(waiter());
    run_for(5ms); // waiter is parked

    // Synchronously: hand the permit, THEN cancel — the grant must win (cancel is a no-op).
    sem.release();
    token.cancel();
    run_for(20ms);

    EXPECT_TRUE(acquired);   // honoured the granted permit
    EXPECT_FALSE(cancelled); // racing cancel did not steal it
}

// =============================================================================
// TEST SUITE: Async Mutex
// =============================================================================

class MutexTests : public ::testing::Test {
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

/**
 * @test Mutex serializes access
 * @brief Critical section protection
 */
TEST_F(MutexTests, SerializesAccess) {
    async_mutex      mtx;
    std::atomic<int> counter{0};

    auto worker = [&mtx, &counter]() -> task<void> {
        auto guard = co_await mtx.scoped_lock();
        int  val   = counter;
        co_await sleep(5ms);
        counter = val + 1;
    };

    for (int i = 0; i < 5; ++i) {
        coro_scheduler().spawn(worker());
    }

    run_for(200ms);

    EXPECT_EQ(counter, 5);
}

/**
 * @test Try lock
 * @brief Non-blocking lock attempt
 */
TEST_F(MutexTests, TryLock) {
    async_mutex mtx;

    EXPECT_TRUE(mtx.try_lock());
    EXPECT_FALSE(mtx.try_lock());
    EXPECT_TRUE(mtx.is_locked());

    mtx.unlock();
    EXPECT_FALSE(mtx.is_locked());
}

// =============================================================================
// TEST SUITE: Read-Write Lock
// =============================================================================

class RWLockTests : public ::testing::Test {
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

/**
 * @test Multiple readers
 * @brief Multiple concurrent readers allowed
 */
TEST_F(RWLockTests, MultipleReaders) {
    async_rw_lock    rw;
    std::atomic<int> readers{0};
    std::atomic<int> max_readers{0};

    auto reader = [&rw, &readers, &max_readers]() -> task<void> {
        co_await rw.lock_read();

        int current      = ++readers;
        int expected_max = max_readers.load();
        while (current > expected_max && !max_readers.compare_exchange_weak(expected_max, current)) {
        }

        co_await sleep(20ms);
        --readers;
        rw.unlock_read();
    };

    for (int i = 0; i < 5; ++i) {
        coro_scheduler().spawn(reader());
    }

    run_for(100ms);

    EXPECT_GT(max_readers, 1);
}

/**
 * @test Writer excludes readers
 * @brief Single writer, no concurrent readers
 */
TEST_F(RWLockTests, WriterExcludesReaders) {
    async_rw_lock     rw;
    std::atomic<bool> writing{false};
    std::atomic<int>  readers_during_write{0};

    auto writer = [&rw, &writing]() -> task<void> {
        co_await rw.lock_write();
        writing = true;
        co_await sleep(30ms);
        writing = false;
        rw.unlock_write();
    };

    auto reader = [&rw, &writing, &readers_during_write]() -> task<void> {
        co_await rw.lock_read();
        if (writing) {
            readers_during_write++;
        }
        co_await sleep(5ms);
        rw.unlock_read();
    };

    coro_scheduler().spawn(writer());
    for (int i = 0; i < 3; ++i) {
        coro_scheduler().spawn(reader());
    }

    run_for(100ms);

    EXPECT_EQ(readers_during_write, 0);
}

// =============================================================================
// TEST SUITE: Barrier
// =============================================================================

class BarrierTests : public ::testing::Test {
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

/**
 * @test Barrier synchronization
 * @brief Wait for N coroutines
 */
TEST_F(BarrierTests, Synchronization) {
    barrier          b(3);
    std::atomic<int> arrived{0};
    std::atomic<int> passed{0};

    auto worker = [&b, &arrived, &passed]() -> task<void> {
        arrived++;
        co_await b.arrive_and_wait();
        passed++;
    };

    coro_scheduler().spawn(worker());
    coro_scheduler().spawn(worker());

    run_for(50ms);
    EXPECT_EQ(arrived, 2);
    EXPECT_EQ(passed, 0); // Waiting for 3rd

    coro_scheduler().spawn(worker());
    run_for(50ms);
    EXPECT_EQ(passed, 3);
}

// =============================================================================
// TEST SUITE: async_event
// =============================================================================

class AsyncEventTests : public ::testing::Test {
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

TEST_F(AsyncEventTests, ManualReset_WakesAllWaiters) {
    async_event ev;
    int         woken = 0;

    auto waiter = [&ev, &woken]() -> task<void> {
        co_await ev.wait();
        ++woken;
    };

    coro_scheduler().spawn(waiter());
    coro_scheduler().spawn(waiter());
    coro_scheduler().spawn(waiter());
    run_for(10ms);
    EXPECT_EQ(woken, 0); // not set yet

    ev.set();
    run_for(10ms);
    EXPECT_EQ(woken, 3);
}

TEST_F(AsyncEventTests, ManualReset_NewWaiterPassesThrough) {
    async_event ev;
    ev.set();
    int woken = 0;

    auto late_waiter = [&ev, &woken]() -> task<void> {
        co_await ev.wait(); // event already set → returns immediately
        ++woken;
    };
    coro_scheduler().spawn(late_waiter());
    run_for(10ms);
    EXPECT_EQ(woken, 1);
}

TEST_F(AsyncEventTests, ManualReset_ResetClearsSignal) {
    async_event ev;
    ev.set();
    ev.reset();
    int woken = 0;

    auto waiter = [&ev, &woken]() -> task<void> {
        co_await ev.wait();
        ++woken;
    };
    coro_scheduler().spawn(waiter());
    run_for(10ms);
    EXPECT_EQ(woken, 0); // cleared

    ev.set();
    run_for(10ms);
    EXPECT_EQ(woken, 1);
}

TEST_F(AsyncEventTests, AutoReset_WakesOneWaiter) {
    async_event ev(/*auto_reset=*/true);
    int         woken = 0;

    auto waiter = [&ev, &woken]() -> task<void> {
        co_await ev.wait();
        ++woken;
    };
    coro_scheduler().spawn(waiter());
    coro_scheduler().spawn(waiter());
    run_for(10ms);
    EXPECT_EQ(woken, 0);

    ev.set(); // wakes exactly one
    run_for(10ms);
    EXPECT_EQ(woken, 1);

    ev.set(); // wakes the second
    run_for(10ms);
    EXPECT_EQ(woken, 2);
}

TEST_F(AsyncEventTests, AutoReset_SignalStoredWhenNoWaiter) {
    async_event ev(/*auto_reset=*/true);
    ev.set(); // signal stored
    int woken = 0;

    auto late = [&ev, &woken]() -> task<void> {
        co_await ev.wait(); // consumes stored signal
        ++woken;
    };
    coro_scheduler().spawn(late());
    run_for(10ms);
    EXPECT_EQ(woken, 1);
    EXPECT_FALSE(ev.is_set()); // consumed
}

TEST_F(AsyncEventTests, IsSetReflectsState) {
    async_event ev;
    EXPECT_FALSE(ev.is_set());
    ev.set();
    EXPECT_TRUE(ev.is_set());
    ev.reset();
    EXPECT_FALSE(ev.is_set());
}

TEST_F(AsyncEventTests, ProducerConsumerHandshake) {
    async_event ready;
    async_event done;
    std::string message;

    auto producer = [&ready, &done, &message]() -> task<void> {
        co_await sleep(5ms);
        message = "hello";
        ready.set();
        co_await done.wait();
    };

    auto consumer = [&ready, &done, &message]() -> task<void> {
        co_await ready.wait();
        EXPECT_EQ(message, "hello");
        message += " world";
        done.set();
    };

    coro_scheduler().spawn(producer());
    coro_scheduler().spawn(consumer());
    run_for(50ms);
    EXPECT_EQ(message, "hello world");
}

// =============================================================================
// TEST SUITE: async_latch
// =============================================================================

class AsyncLatchTests : public ::testing::Test {
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

TEST_F(AsyncLatchTests, BasicCountdown) {
    async_latch latch(3);
    int         woken = 0;

    auto waiter = [&latch, &woken]() -> task<void> {
        co_await latch.wait();
        ++woken;
    };
    coro_scheduler().spawn(waiter());
    coro_scheduler().spawn(waiter());
    run_for(10ms);
    EXPECT_EQ(woken, 0);

    latch.count_down();
    run_for(10ms);
    EXPECT_EQ(woken, 0);
    latch.count_down();
    run_for(10ms);
    EXPECT_EQ(woken, 0);
    latch.count_down(); // reaches 0
    run_for(10ms);
    EXPECT_EQ(woken, 2);
}

TEST_F(AsyncLatchTests, AlreadyZeroPassesImmediately) {
    async_latch latch(0); // already done
    int         woken = 0;

    auto waiter = [&latch, &woken]() -> task<void> {
        co_await latch.wait();
        ++woken;
    };
    coro_scheduler().spawn(waiter());
    run_for(10ms);
    EXPECT_EQ(woken, 1);
}

TEST_F(AsyncLatchTests, ArriveAndWait) {
    async_latch latch(2);
    int         woken = 0;

    auto worker = [&latch, &woken]() -> task<void> {
        co_await latch.arrive_and_wait();
        ++woken;
    };
    coro_scheduler().spawn(worker());
    coro_scheduler().spawn(worker());
    run_for(50ms);
    EXPECT_EQ(woken, 2);
}

TEST_F(AsyncLatchTests, CountDownByN) {
    async_latch latch(10);
    int         woken = 0;

    auto waiter = [&latch, &woken]() -> task<void> {
        co_await latch.wait();
        ++woken;
    };
    coro_scheduler().spawn(waiter());
    run_for(10ms);
    EXPECT_EQ(woken, 0);

    latch.count_down(10);
    run_for(10ms);
    EXPECT_EQ(woken, 1);
}

TEST_F(AsyncLatchTests, ExtraCountDownIsSafe) {
    async_latch latch(1);
    int         woken = 0;

    auto waiter = [&latch, &woken]() -> task<void> {
        co_await latch.wait();
        ++woken;
    };
    coro_scheduler().spawn(waiter());
    latch.count_down(); // 0
    latch.count_down(); // no-op
    latch.count_down(); // no-op
    run_for(10ms);
    EXPECT_EQ(woken, 1);
}

// =============================================================================
// TEST SUITE: Advanced Sync APIs
// =============================================================================

class SyncAdvancedTests : public ::testing::Test {
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

TEST_F(SyncAdvancedTests, WithSemaphoreHelper) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        semaphore sem(1);
        auto      result = co_await with_semaphore(sem, []() { return 42; });
        EXPECT_EQ(result, 42);
        EXPECT_EQ(sem.available_permits(), 1u);
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(SyncAdvancedTests, WithLockHelper) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        async_mutex mtx;
        auto        result = co_await with_lock(mtx, []() { return std::string("hello"); });
        EXPECT_EQ(result, "hello");
        EXPECT_FALSE(mtx.is_locked());
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(SyncAdvancedTests, BarrierResetAndReuse) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        barrier b(2);
        int     phase = 0;

        auto participant = [&b, &phase](int id) -> task<void> {
            co_await b.arrive_and_wait();
            phase = 1;
        };

        coro_scheduler().spawn(participant(1));
        coro_scheduler().spawn(participant(2));
        co_await sleep(50ms);
        EXPECT_EQ(phase, 1);

        b.reset();
        phase = 0;

        coro_scheduler().spawn(participant(3));
        coro_scheduler().spawn(participant(4));
        co_await sleep(50ms);
        EXPECT_EQ(phase, 1);

        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(SyncAdvancedTests, SemaphoreAvailableAndTotalPermits) {
    semaphore sem(5);
    EXPECT_EQ(sem.total_permits(), 5u);
    EXPECT_EQ(sem.available_permits(), 5u);
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_EQ(sem.available_permits(), 4u);
    EXPECT_EQ(sem.total_permits(), 5u);
    sem.release();
    EXPECT_EQ(sem.available_permits(), 5u);
}

TEST_F(SyncAdvancedTests, AsyncMutexWaitersCount) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        async_mutex mtx;
        EXPECT_EQ(mtx.waiters_count(), 0u);
        EXPECT_FALSE(mtx.is_locked());
        co_await mtx.lock();
        EXPECT_TRUE(mtx.is_locked());
        EXPECT_EQ(mtx.waiters_count(), 0u);
        mtx.unlock();
        EXPECT_FALSE(mtx.is_locked());
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(SyncAdvancedTests, AsyncEventBasicState) {
    async_event evt;
    EXPECT_EQ(evt.waiters_count(), 0u);
    EXPECT_FALSE(evt.is_set());
    evt.set();
    EXPECT_TRUE(evt.is_set());
    evt.reset();
    EXPECT_FALSE(evt.is_set());
}

TEST_F(SyncAdvancedTests, RWLockScopedReadWriteLock) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        async_rw_lock rw;
        {
            auto rg = co_await rw.scoped_read_lock();
        }
        {
            auto wg = co_await rw.scoped_write_lock();
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
