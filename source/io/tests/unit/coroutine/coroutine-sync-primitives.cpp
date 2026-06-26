/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/coroutine-sync-primitives.cpp
 * @brief The coroutine synchronization primitives — semaphore / mutex / rw-lock / barrier / event / latch.
 *
 * Covers every primitive declared in qb/io/async/coroutine/sync.h plus the `with_semaphore` /
 * `with_lock` helpers: `semaphore` (acquire/try_acquire/release/scoped_acquire, the
 * cancellation-token-aware `acquire(token)` retract path, and permit accounting),
 * `async_mutex` (lock/scoped_lock/try_lock/unlock/is_locked + a CONTENDED waiters_count),
 * `async_rw_lock` (read/write locks with a PROVEN reader↔writer exclusion), `barrier`
 * (arrive/reset), `async_event` (manual + auto reset, plus a cancellation path), and
 * `async_latch` (count_down/wait/arrive_and_wait).
 *
 * Restructured over the original test-coroutine-sync.cpp:
 *   - the seven byte-identical per-primitive fixtures are collapsed into one
 *     `CoroutineSyncPrimitives` base (SetUp = `reset_async_context()`);
 *   - the formerly VACUOUS `RWLockScopedReadWriteLock` now PROVES exclusion: a writer is held
 *     while a second writer must wait, and only proceeds once the first releases;
 *   - the formerly-uncontended `AsyncMutexWaitersCount` now drives a real contended path so
 *     `waiters_count()` is observed at a non-zero value;
 *   - a cancellation-token path is added on `async_event.wait()` (a parked wait cancelled via
 *     `when_any(ev.wait(), check_cancelled(token))`) alongside the existing semaphore cancel
 *     trio;
 *   - the duplicate `IsSetReflectsState` (subsumed by `AsyncEventBasicState`) is dropped;
 *   - every spawned-body test gates on a real `done` flag through `qb::io::test::pump_until`
 *     instead of a blind `run_for(Nms)`; the file-local `main()` is removed (shared gtest_main).
 */

#include <atomic>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

// One shared base for every primitive fixture (replaces the 7 boilerplate clones).
class CoroutineSyncPrimitives : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        // Some primitive tests intentionally leave a coroutine parked on a held lock /
        // never-set event; drain then destroy suspended frames so they do not leak into the
        // next test.
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// =============================================================================
// Semaphore
// =============================================================================

TEST_F(CoroutineSyncPrimitives, SemaphoreLimitsConcurrency) {
    semaphore         sem(2);
    std::atomic<int>  concurrent{0};
    std::atomic<int>  max_concurrent{0};
    std::atomic<int>  finished{0};
    constexpr int     N = 5;

    auto worker = [&]() -> task<void> {
        co_await sem.acquire();
        int current      = ++concurrent;
        int expected_max = max_concurrent.load();
        while (current > expected_max && !max_concurrent.compare_exchange_weak(expected_max, current)) {
        }
        co_await sleep(20ms);
        --concurrent;
        sem.release();
        finished.fetch_add(1);
    };

    for (int i = 0; i < N; ++i)
        coro_scheduler().spawn(worker());

    EXPECT_TRUE(pump_until([&] { return finished.load() == N; })) << "not all semaphore workers completed";
    EXPECT_LE(max_concurrent.load(), 2) << "semaphore(2) must cap concurrency at 2";
    EXPECT_EQ(sem.available_permits(), 2u);
}

TEST_F(CoroutineSyncPrimitives, SemaphoreTryAcquire) {
    semaphore sem(1);

    EXPECT_TRUE(sem.try_acquire());
    EXPECT_FALSE(sem.try_acquire());

    sem.release();
    EXPECT_TRUE(sem.try_acquire());
}

TEST_F(CoroutineSyncPrimitives, SemaphoreScopedAcquireAutoReleases) {
    semaphore         sem(1);
    std::atomic<bool> acquired{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto guard = co_await sem.scoped_acquire();
        acquired.store(true);
        done.store(true);
        // guard releases on scope exit
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "scoped_acquire coroutine never ran";
    EXPECT_TRUE(acquired.load());
    EXPECT_EQ(sem.available_permits(), sem.total_permits()) << "the guard must restore the permit on destruction";
}

TEST_F(CoroutineSyncPrimitives, SemaphoreReleaseIsCappedAtTotalPermits) {
    semaphore sem(2);
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_FALSE(sem.try_acquire());
    sem.release();
    sem.release();
    sem.release(); // over-release — must be a no-op
    sem.release();
    EXPECT_EQ(sem.available_permits(), 2u);
    EXPECT_EQ(sem.total_permits(), 2u);
}

TEST_F(CoroutineSyncPrimitives, SemaphoreAvailableAndTotalPermits) {
    semaphore sem(5);
    EXPECT_EQ(sem.total_permits(), 5u);
    EXPECT_EQ(sem.available_permits(), 5u);
    EXPECT_TRUE(sem.try_acquire());
    EXPECT_EQ(sem.available_permits(), 4u);
    EXPECT_EQ(sem.total_permits(), 5u);
    sem.release();
    EXPECT_EQ(sem.available_permits(), 5u);
}

TEST_F(CoroutineSyncPrimitives, SemaphoreWithHelperReturnsResultAndRestoresPermit) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        semaphore sem(1);
        auto      result = co_await with_semaphore(sem, []() { return 42; });
        EXPECT_EQ(result, 42);
        EXPECT_EQ(sem.available_permits(), 1u);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "with_semaphore never ran";
}

// --- Semaphore cancellation (acquire(token) retract path) -------------------

TEST_F(CoroutineSyncPrimitives, SemaphoreCancelAcquireWakesAndRetracts) {
    semaphore          sem(1);
    cancellation_token token;
    std::atomic<bool>  cancelled{false};
    std::atomic<bool>  acquired{false};
    std::atomic<bool>  parked{false};

    EXPECT_TRUE(sem.try_acquire()); // hold the only permit so the waiter must park

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        try {
            co_await sem.acquire(token); // parks (no permit)
            acquired.store(true);
        } catch (const cancelled_error &) {
            cancelled.store(true);
        }
    });

    EXPECT_TRUE(pump_until([&] { return parked.load(); })) << "waiter never started";
    EXPECT_FALSE(acquired.load());
    EXPECT_FALSE(cancelled.load());

    token.cancel(); // retract the parked acquirer
    EXPECT_TRUE(pump_until([&] { return cancelled.load(); })) << "cancel never woke the parked acquirer";
    EXPECT_FALSE(acquired.load());

    // The cancelled waiter must NOT have consumed a permit: releasing the holder restores it.
    sem.release();
    EXPECT_EQ(sem.available_permits(), 1u);
    EXPECT_TRUE(sem.try_acquire());
}

TEST_F(CoroutineSyncPrimitives, SemaphoreCancelledWaiterSkippedNextServed) {
    semaphore          sem(1);
    cancellation_token tok_a;
    std::atomic<bool>  a_parked{false};
    std::atomic<bool>  a_cancelled{false};
    std::atomic<bool>  b_acquired{false};

    EXPECT_TRUE(sem.try_acquire()); // hold the permit

    coro_scheduler().spawn([&]() -> task<void> {
        a_parked.store(true);
        try {
            co_await sem.acquire(tok_a);
        } catch (const cancelled_error &) {
            a_cancelled.store(true);
        }
    });
    EXPECT_TRUE(pump_until([&] { return a_parked.load(); }));

    coro_scheduler().spawn([&]() -> task<void> {
        co_await sem.acquire(); // plain waiter, queued behind A
        b_acquired.store(true);
        sem.release();
    });
    qb::io::async::run_for(5ms);

    tok_a.cancel(); // A retracts
    EXPECT_TRUE(pump_until([&] { return a_cancelled.load(); }));
    sem.release(); // hand the permit to the next live waiter → B (not the retracted A)

    EXPECT_TRUE(pump_until([&] { return b_acquired.load(); })) << "the live waiter B was never served after A cancelled";
    EXPECT_TRUE(a_cancelled.load());
}

TEST_F(CoroutineSyncPrimitives, SemaphoreGrantedBeatsRacingCancel) {
    semaphore          sem(1);
    cancellation_token token;
    std::atomic<bool>  acquired{false};
    std::atomic<bool>  cancelled{false};
    std::atomic<bool>  parked{false};

    EXPECT_TRUE(sem.try_acquire()); // hold the permit

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        try {
            co_await sem.acquire(token);
            acquired.store(true);
            sem.release();
        } catch (const cancelled_error &) {
            cancelled.store(true);
        }
    });
    EXPECT_TRUE(pump_until([&] { return parked.load(); }));

    // Synchronously hand the permit, THEN cancel — the grant must win (cancel is a no-op).
    sem.release();
    token.cancel();

    EXPECT_TRUE(pump_until([&] { return acquired.load() || cancelled.load(); }));
    EXPECT_TRUE(acquired.load()) << "the granted permit must be honoured";
    EXPECT_FALSE(cancelled.load()) << "a racing cancel must not steal an already-granted permit";
}

// =============================================================================
// Async Mutex
// =============================================================================

TEST_F(CoroutineSyncPrimitives, MutexSerializesAccess) {
    async_mutex      mtx;
    std::atomic<int> counter{0};
    std::atomic<int> finished{0};
    constexpr int    N = 5;

    auto worker = [&]() -> task<void> {
        auto guard = co_await mtx.scoped_lock();
        int  val   = counter.load();
        co_await sleep(5ms); // window for a lost update if the mutex did not serialize
        counter.store(val + 1);
        finished.fetch_add(1);
    };

    for (int i = 0; i < N; ++i)
        coro_scheduler().spawn(worker());

    EXPECT_TRUE(pump_until([&] { return finished.load() == N; })) << "not all mutex workers completed";
    EXPECT_EQ(counter.load(), N) << "no lost updates -> the mutex serialized every critical section";
}

TEST_F(CoroutineSyncPrimitives, MutexTryLock) {
    async_mutex mtx;

    EXPECT_TRUE(mtx.try_lock());
    EXPECT_FALSE(mtx.try_lock());
    EXPECT_TRUE(mtx.is_locked());

    mtx.unlock();
    EXPECT_FALSE(mtx.is_locked());
}

TEST_F(CoroutineSyncPrimitives, MutexWaitersCountReflectsContention) {
    // Drives a CONTENDED path so waiters_count() is observed non-zero (the original asserted
    // it == 0 in a single-coroutine context where no waiter could ever exist).
    async_mutex       mtx;
    std::atomic<bool> holder_acquired{false};
    std::atomic<bool> contender_parked{false};
    std::atomic<bool> contender_acquired{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await mtx.lock();
        holder_acquired.store(true);
        co_await sleep(50ms); // hold long enough for the contender to queue
        mtx.unlock();
    });
    EXPECT_TRUE(pump_until([&] { return holder_acquired.load(); }));

    coro_scheduler().spawn([&]() -> task<void> {
        contender_parked.store(true);
        co_await mtx.lock(); // must park behind the holder
        contender_acquired.store(true);
        mtx.unlock();
    });

    // Once the contender has parked behind the held lock, waiters_count() must be 1.
    EXPECT_TRUE(pump_until([&] { return mtx.waiters_count() == 1u; })) << "contended waiter never registered in waiters_count()";
    EXPECT_FALSE(contender_acquired.load()) << "the contender must still be parked while the holder owns the lock";

    EXPECT_TRUE(pump_until([&] { return contender_acquired.load(); })) << "the contender never acquired after release";
    EXPECT_EQ(mtx.waiters_count(), 0u);
    EXPECT_FALSE(mtx.is_locked());
}

TEST_F(CoroutineSyncPrimitives, MutexWithHelperReturnsResultAndUnlocks) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        async_mutex mtx;
        auto        result = co_await with_lock(mtx, []() { return std::string("hello"); });
        EXPECT_EQ(result, "hello");
        EXPECT_FALSE(mtx.is_locked());
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "with_lock never ran";
}

// =============================================================================
// Read-Write Lock
// =============================================================================

TEST_F(CoroutineSyncPrimitives, RWLockAllowsMultipleConcurrentReaders) {
    async_rw_lock    rw;
    std::atomic<int> readers{0};
    std::atomic<int> max_readers{0};
    std::atomic<int> finished{0};
    constexpr int    N = 5;

    auto reader = [&]() -> task<void> {
        co_await rw.lock_read();
        int current      = ++readers;
        int expected_max = max_readers.load();
        while (current > expected_max && !max_readers.compare_exchange_weak(expected_max, current)) {
        }
        co_await sleep(20ms);
        --readers;
        rw.unlock_read();
        finished.fetch_add(1);
    };

    for (int i = 0; i < N; ++i)
        coro_scheduler().spawn(reader());

    EXPECT_TRUE(pump_until([&] { return finished.load() == N; })) << "not all readers completed";
    EXPECT_GT(max_readers.load(), 1) << "an rw-lock must admit multiple concurrent readers";
}

TEST_F(CoroutineSyncPrimitives, RWLockWriterExcludesReaders) {
    async_rw_lock     rw;
    std::atomic<bool> writing{false};
    std::atomic<int>  readers_during_write{0};
    std::atomic<int>  finished{0};
    constexpr int     N = 3;

    auto writer = [&]() -> task<void> {
        co_await rw.lock_write();
        writing.store(true);
        co_await sleep(30ms);
        writing.store(false);
        rw.unlock_write();
    };
    auto reader = [&]() -> task<void> {
        co_await rw.lock_read();
        if (writing.load())
            readers_during_write.fetch_add(1);
        co_await sleep(5ms);
        rw.unlock_read();
        finished.fetch_add(1);
    };

    coro_scheduler().spawn(writer());
    for (int i = 0; i < N; ++i)
        coro_scheduler().spawn(reader());

    EXPECT_TRUE(pump_until([&] { return finished.load() == N; })) << "not all readers completed";
    EXPECT_EQ(readers_during_write.load(), 0) << "no reader may hold the lock while the writer is writing";
}

TEST_F(CoroutineSyncPrimitives, RWLockScopedGuardsProveWriterExclusion) {
    // The original RWLockScopedReadWriteLock only acquired scoped guards in empty blocks and
    // asserted done==true — it never proved exclusion. Here a held write guard must block a
    // second writer until the guard is released.
    async_rw_lock     rw;
    std::atomic<bool> first_writer_holding{false};
    std::atomic<bool> second_writer_acquired{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        {
            // Read guard then write guard, exercising both scoped paths.
            { auto rg = co_await rw.scoped_read_lock(); }
            auto wg = co_await rw.scoped_write_lock();
            first_writer_holding.store(true);

            // While the write guard is held, a second writer must NOT acquire.
            coro_scheduler().spawn([&]() -> task<void> {
                co_await rw.lock_write();
                second_writer_acquired.store(true);
                rw.unlock_write();
            });
            co_await sleep(30ms);
            EXPECT_FALSE(second_writer_acquired.load()) << "a second writer must wait while the write guard is held";
            // wg releases here on scope exit, admitting the second writer.
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load() && second_writer_acquired.load(); }))
        << "the second writer was never admitted after the guard released";
    EXPECT_TRUE(first_writer_holding.load());
}

// =============================================================================
// Barrier
// =============================================================================

TEST_F(CoroutineSyncPrimitives, BarrierReleasesAllOnLastArrival) {
    barrier          b(3);
    std::atomic<int> arrived{0};
    std::atomic<int> passed{0};

    auto worker = [&]() -> task<void> {
        arrived.fetch_add(1);
        co_await b.arrive_and_wait();
        passed.fetch_add(1);
    };

    coro_scheduler().spawn(worker());
    coro_scheduler().spawn(worker());

    EXPECT_TRUE(pump_until([&] { return arrived.load() == 2; })) << "two workers never arrived";
    EXPECT_EQ(passed.load(), 0) << "the barrier must hold until all 3 arrive";

    coro_scheduler().spawn(worker());
    EXPECT_TRUE(pump_until([&] { return passed.load() == 3; })) << "the barrier never released after the 3rd arrival";
}

TEST_F(CoroutineSyncPrimitives, BarrierResetAndReuseAcrossPhases) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        barrier b(2);
        int     phase = 0;

        auto participant = [&b, &phase]() -> task<void> {
            co_await b.arrive_and_wait();
            phase = 1;
        };

        coro_scheduler().spawn(participant());
        coro_scheduler().spawn(participant());
        co_await sleep(40ms);
        EXPECT_EQ(phase, 1);

        b.reset();
        phase = 0;
        coro_scheduler().spawn(participant());
        coro_scheduler().spawn(participant());
        co_await sleep(40ms);
        EXPECT_EQ(phase, 1);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "barrier reset/reuse never finished";
}

// =============================================================================
// async_event
// =============================================================================

TEST_F(CoroutineSyncPrimitives, EventManualResetWakesAllWaiters) {
    async_event      ev;
    std::atomic<int> woken{0};

    auto waiter = [&]() -> task<void> {
        co_await ev.wait();
        woken.fetch_add(1);
    };
    coro_scheduler().spawn(waiter());
    coro_scheduler().spawn(waiter());
    coro_scheduler().spawn(waiter());

    qb::io::async::run_for(10ms);
    EXPECT_EQ(woken.load(), 0) << "no waiter may wake before set()";

    ev.set();
    EXPECT_TRUE(pump_until([&] { return woken.load() == 3; })) << "manual-reset set() must wake every waiter";
}

TEST_F(CoroutineSyncPrimitives, EventManualResetNewWaiterPassesThrough) {
    async_event ev;
    ev.set();
    std::atomic<bool> woken{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await ev.wait(); // already set -> returns immediately
        woken.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return woken.load(); })) << "a waiter on an already-set manual event must pass through";
}

TEST_F(CoroutineSyncPrimitives, EventManualResetClearsSignal) {
    async_event ev;
    ev.set();
    ev.reset();
    std::atomic<bool> woken{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await ev.wait();
        woken.store(true);
    });
    qb::io::async::run_for(10ms);
    EXPECT_FALSE(woken.load()) << "reset() must clear the signal";

    ev.set();
    EXPECT_TRUE(pump_until([&] { return woken.load(); })) << "the waiter never woke after the second set()";
}

TEST_F(CoroutineSyncPrimitives, EventAutoResetWakesOneWaiterPerSet) {
    async_event      ev(/*auto_reset=*/true);
    std::atomic<int> woken{0};

    auto waiter = [&]() -> task<void> {
        co_await ev.wait();
        woken.fetch_add(1);
    };
    coro_scheduler().spawn(waiter());
    coro_scheduler().spawn(waiter());
    qb::io::async::run_for(10ms);
    EXPECT_EQ(woken.load(), 0);

    ev.set(); // wakes exactly one
    EXPECT_TRUE(pump_until([&] { return woken.load() == 1; })) << "auto-reset set() must wake exactly one waiter";
    qb::io::async::run_for(10ms);
    EXPECT_EQ(woken.load(), 1) << "auto-reset must not wake a second waiter on a single set()";

    ev.set(); // wakes the second
    EXPECT_TRUE(pump_until([&] { return woken.load() == 2; })) << "the second set() never woke the second waiter";
}

TEST_F(CoroutineSyncPrimitives, EventAutoResetStoresSignalWhenNoWaiter) {
    async_event ev(/*auto_reset=*/true);
    ev.set(); // signal stored
    std::atomic<bool> woken{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await ev.wait(); // consumes the stored signal
        woken.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return woken.load(); })) << "a late waiter never consumed the stored auto-reset signal";
    EXPECT_FALSE(ev.is_set()) << "the stored signal must be consumed";
}

TEST_F(CoroutineSyncPrimitives, EventBasicState) {
    async_event evt;
    EXPECT_EQ(evt.waiters_count(), 0u);
    EXPECT_FALSE(evt.is_set());
    evt.set();
    EXPECT_TRUE(evt.is_set());
    evt.reset();
    EXPECT_FALSE(evt.is_set());
}

TEST_F(CoroutineSyncPrimitives, EventWaitIsCancellableViaCheckCancelled) {
    // Cancellation path on async_event.wait(): the event is NEVER set, so the only way the
    // coroutine can make progress is the cancellation branch. when_any resolves to whichever
    // branch completes first; cancelling the token completes the check_cancelled branch and
    // unblocks the otherwise-permanently-parked ev.wait(). The result must carry index 1
    // (the cancellation branch won) — proving the event wait was the one still pending.
    async_event        ev; // never set
    cancellation_token token;
    std::atomic<bool>  parked{false};
    std::atomic<bool>  done{false};
    std::atomic<size_t> winner{99};

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        auto result = co_await when_any(
            [&ev]() -> task<void> { co_await ev.wait(); }(),
            [token]() -> task<void> { co_await check_cancelled(token); }());
        winner.store(result.index);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return parked.load(); })) << "event waiter never started";
    EXPECT_FALSE(done.load()) << "the wait must still be parked before cancel (the event is never set)";

    token.cancel();
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "cancelling the token never unblocked the parked event wait";
    EXPECT_EQ(winner.load(), 1u) << "the cancellation branch must win — the never-set event wait stays pending";
}

TEST_F(CoroutineSyncPrimitives, ProducerConsumerEventHandshake) {
    async_event       ready;
    async_event       done;
    std::string       message;
    std::atomic<bool> finished{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await sleep(5ms);
        message = "hello";
        ready.set();
        co_await done.wait();
        finished.store(true);
    });
    coro_scheduler().spawn([&]() -> task<void> {
        co_await ready.wait();
        EXPECT_EQ(message, "hello");
        message += " world";
        done.set();
    });

    EXPECT_TRUE(pump_until([&] { return finished.load(); })) << "the event handshake never completed";
    EXPECT_EQ(message, "hello world");
}

// =============================================================================
// async_latch
// =============================================================================

TEST_F(CoroutineSyncPrimitives, LatchReleasesWaitersAtZero) {
    async_latch      latch(3);
    std::atomic<int> woken{0};

    auto waiter = [&]() -> task<void> {
        co_await latch.wait();
        woken.fetch_add(1);
    };
    coro_scheduler().spawn(waiter());
    coro_scheduler().spawn(waiter());
    qb::io::async::run_for(10ms);
    EXPECT_EQ(woken.load(), 0);

    latch.count_down();
    qb::io::async::run_for(10ms);
    EXPECT_EQ(woken.load(), 0);
    latch.count_down();
    qb::io::async::run_for(10ms);
    EXPECT_EQ(woken.load(), 0);
    latch.count_down(); // reaches 0 -> releases all
    EXPECT_TRUE(pump_until([&] { return woken.load() == 2; })) << "the latch never released its waiters at zero";
}

TEST_F(CoroutineSyncPrimitives, LatchAlreadyZeroPassesImmediately) {
    async_latch       latch(0); // already done
    std::atomic<bool> woken{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await latch.wait();
        woken.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return woken.load(); })) << "a waiter on an already-zero latch must pass through";
}

TEST_F(CoroutineSyncPrimitives, LatchArriveAndWait) {
    async_latch      latch(2);
    std::atomic<int> woken{0};

    auto worker = [&]() -> task<void> {
        co_await latch.arrive_and_wait();
        woken.fetch_add(1);
    };
    coro_scheduler().spawn(worker());
    coro_scheduler().spawn(worker());

    EXPECT_TRUE(pump_until([&] { return woken.load() == 2; })) << "arrive_and_wait never released both workers";
}

TEST_F(CoroutineSyncPrimitives, LatchCountDownByN) {
    async_latch       latch(10);
    std::atomic<bool> woken{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await latch.wait();
        woken.store(true);
    });
    qb::io::async::run_for(10ms);
    EXPECT_FALSE(woken.load());

    latch.count_down(10);
    EXPECT_TRUE(pump_until([&] { return woken.load(); })) << "count_down(N) to zero never released the waiter";
}

TEST_F(CoroutineSyncPrimitives, LatchExtraCountDownPastZeroIsSafe) {
    async_latch       latch(1);
    std::atomic<bool> woken{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await latch.wait();
        woken.store(true);
    });
    latch.count_down(); // 0 -> releases
    latch.count_down(); // no-op
    latch.count_down(); // no-op
    EXPECT_TRUE(pump_until([&] { return woken.load(); })) << "extra count_down past zero must be a safe no-op";
}
