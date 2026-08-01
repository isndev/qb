/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/lockfree/spinlock-basic.cpp
 * @brief `qb::lockfree::SpinLock` (`qb/system/lockfree/spinlock.h`) — state machine + contention.
 *
 * The single-thread state-machine cases (locked / trylock / lock-unlock / spin-count / timed) are
 * pure logic; the one multi-threaded case spawns raw `std::thread`s but NO `qb::Main` / event loop
 * / daemon, so the file stays unit (a real-MT lock-free primitive, consistent with the lockfree
 * threading model). `MutualExclusionUnderContention` is the load-bearing teeth: 8 threads each do
 * 5000 increments of a NON-atomic counter guarded only by the lock — the exact total can only hold
 * if mutual exclusion is real (a broken lock loses increments).
 *
 * De-flaked over the original: the held-lock timed cases no longer rely on a 5ms wall-clock window.
 * A held lock is proven to reject acquisition *deterministically* — `trylock()` and a zero/past
 * deadline both return false with no timing dependence — and the freed lock is re-acquired with a
 * plain `trylock()`, not a timed bound. The single bounded `trylock_for` is kept only to prove the
 * timeout path returns (it cannot hang: the spin loop is bounded by the deadline, the ctest TIMEOUT
 * is the backstop). Added: an already-PAST `trylock_until` deadline (must fail fast, not spin
 * forever — `deadline - now` is negative, so `trylock_for` tries once and returns), and a
 * `std::lock_guard<SpinLock>` smoke test (SpinLock satisfies the BasicLockable contract).
 */

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/system/lockfree/spinlock.h>
#include <qb/system/time.h>

using qb::lockfree::SpinLock;
using namespace std::chrono_literals;

TEST(SpinLock, StartsUnlocked) {
    SpinLock sl;
    EXPECT_FALSE(sl.locked());
}

TEST(SpinLock, TrylockTakesAndBlocks) {
    SpinLock sl;
    EXPECT_TRUE(sl.trylock()); // acquired
    EXPECT_TRUE(sl.locked());
    EXPECT_FALSE(sl.trylock()); // already held
    sl.unlock();
    EXPECT_FALSE(sl.locked());
    EXPECT_TRUE(sl.trylock()); // free again
    sl.unlock();
}

TEST(SpinLock, LockUnlock) {
    SpinLock sl;
    sl.lock();
    EXPECT_TRUE(sl.locked());
    sl.unlock();
    EXPECT_FALSE(sl.locked());
}

TEST(SpinLock, TrylockWithSpinCount) {
    SpinLock sl;
    EXPECT_TRUE(sl.trylock(100)); // free → acquires on first attempt
    EXPECT_FALSE(sl.trylock(50)); // held → exhausts spins, fails
    sl.unlock();
    EXPECT_TRUE(sl.trylock(50)); // free again
    sl.unlock();
}

TEST(SpinLock, TrylockForTimesOutThenSucceeds) {
    SpinLock sl;
    sl.lock();
    // Deterministic: a held lock can NEVER be acquired, regardless of the timeout. A zero timeout
    // proves the timeout path returns false without depending on a wall-clock window elapsing.
    EXPECT_FALSE(sl.trylock_for(qb::duration::zero())) << "held lock must reject a zero-timeout trylock_for";
    EXPECT_FALSE(sl.trylock_for(5ms)) << "held lock must reject even a 5ms trylock_for";
    sl.unlock();
    // Deterministic: once free, a plain (untimed) trylock acquires on the first attempt — no
    // wall-clock success window required.
    EXPECT_TRUE(sl.trylock()) << "freed lock must acquire immediately";
    sl.unlock();
}

TEST(SpinLock, TrylockUntil) {
    SpinLock sl;
    sl.lock();
    // Held lock + a future deadline still fails (the deadline just bounds how long it spins).
    EXPECT_FALSE(sl.trylock_until(qb::mono_now() + 5ms)) << "held lock rejects trylock_until";
    sl.unlock();
    // Freed lock: re-acquire deterministically with a plain trylock.
    EXPECT_TRUE(sl.trylock());
    sl.unlock();
}

TEST(SpinLock, TrylockUntilPastDeadlineFailsFast) {
    SpinLock sl;
    sl.lock();
    // An already-PAST deadline: trylock_until computes `deadline - mono_now()` < 0, so trylock_for
    // tries exactly once and returns. On a HELD lock this must fail fast (and must NOT spin forever
    // on the negative duration). The ctest TIMEOUT is the backstop if this ever regresses to a hang.
    const auto past = qb::mono_now() - 1s;
    EXPECT_FALSE(sl.trylock_until(past)) << "past deadline on a held lock must fail fast";
    sl.unlock();
    // On a FREE lock, even a past deadline still gets its one mandatory attempt and succeeds.
    EXPECT_TRUE(sl.trylock_until(qb::mono_now() - 1s)) << "past deadline still makes one attempt; free lock acquires";
    sl.unlock();
    EXPECT_FALSE(sl.locked());
}

TEST(SpinLock, LockGuardSatisfiesBasicLockable) {
    // SpinLock exposes lock()/unlock(), so it models the standard BasicLockable contract:
    // std::lock_guard must take it on construction and release it on scope exit.
    SpinLock sl;
    EXPECT_FALSE(sl.locked());
    {
        std::lock_guard<SpinLock> guard(sl);
        EXPECT_TRUE(sl.locked()) << "lock_guard must hold the spinlock inside the scope";
        EXPECT_FALSE(sl.trylock()) << "the guard holds it exclusively";
    }
    EXPECT_FALSE(sl.locked()) << "lock_guard must release the spinlock at scope exit";
    // The lock is usable again after the guard released it.
    EXPECT_TRUE(sl.trylock());
    sl.unlock();
}

TEST(SpinLock, MutualExclusionUnderContention) {
    SpinLock                 sl;
    std::uint64_t            counter  = 0; // guarded by sl — no atomic, exclusivity must hold
    constexpr int            kThreads = 8;
    constexpr int            kIters   = 5000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&sl, &counter]() {
            for (int i = 0; i < kIters; ++i) {
                sl.lock();
                ++counter; // critical section
                sl.unlock();
            }
        });
    }
    for (auto &th : threads)
        th.join();
    EXPECT_EQ(counter, static_cast<std::uint64_t>(kThreads) * kIters);
    EXPECT_FALSE(sl.locked());
}

// ---------------------------------------------------------------------------
// The TIMED acquire paths under contention.
//
// `lock()` has always been test-and-test-and-set: on a failed `exchange` it spins on a relaxed
// LOAD, which keeps the lock's cache line shared. `trylock(int64_t)` and `trylock_for()` used to
// skip that and re-issue `exchange` back-to-back — a write RMW that takes the line exclusive on
// every attempt, so waiters ping-pong the line and starve the holder. Measured against the previous
// shape (20k acquisitions/thread): trylock(int64_t) — whose wait loop had nothing at all between
// attempts — is 1.5x faster at 2 threads, ~1.8x at 4, and 4.3x at 8 with a longer critical section.
// trylock_for() had the same shape but already read the monotonic clock every iteration, which was
// incidentally throttling it, so its measured difference is within noise; it is aligned for shape.
//
// A wall-clock threshold would be a flaky test, so these pin CORRECTNESS under the contention that
// exposed the problem — mutual exclusion must hold, every acquisition must be accounted for, and
// the whole thing must finish well inside the ctest timeout (which is the real backstop against a
// regression to the pathological shape: with the RMW storm this case took seconds, not
// milliseconds).
// ---------------------------------------------------------------------------

TEST(SpinLock, TimedAcquirePathsAreMutuallyExclusiveUnderContention) {
    SpinLock                 sl;
    std::uint64_t            counter = 0; // guarded by sl — no atomic, exclusivity must hold
    std::atomic<int>         acquired{0};
    constexpr int            kThreads = 8;
    constexpr int            kIters   = 2000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&sl, &counter, &acquired, t]() {
            for (int i = 0; i < kIters; ++i) {
                // The two budgets are NOT equivalent, and treating them as such made this test
                // flaky (measured: 2 failures in 8 standalone runs, always on the spin path, always
                // at i == 0 — the thundering herd at thread start).
                //
                //   trylock_for(10s)  bounds WALL TIME. Under this workload — 8 threads, a
                //                     `++counter` critical section — 10 s is genuinely unreachable,
                //                     so a false here would be a real defect and is asserted.
                //   trylock(1 << 20)  bounds ATTEMPTS, not time. A TTAS read-spin is ~1 ns, so the
                //                     whole budget is ~1 ms of wall clock; losing every attempt for
                //                     1 ms against 7 contenders is ordinary contention, and
                //                     returning false is exactly what TtasWaitLoopStillHonoursIts-
                //                     Budget requires of it. Retry instead of asserting.
                bool ok;
                if (t % 2 == 0) {
                    ok = sl.trylock_for(std::chrono::seconds{10});
                    ASSERT_TRUE(ok) << "a 10-second WALL-CLOCK budget must acquire: this workload cannot hold the lock that long";
                } else {
                    // Bounded so a genuine livelock still fails the test rather than hanging it:
                    // 4096 x ~1 ms is ~4 s of trying, orders of magnitude past any real contention.
                    ok = false;
                    for (int attempt = 0; attempt < 4096 && !ok; ++attempt)
                        ok = sl.trylock(1 << 20);
                    ASSERT_TRUE(ok) << "the attempt-budgeted path never acquired across 4096 full budgets — that is livelock, not contention";
                }
                ++counter; // critical section
                acquired.fetch_add(1, std::memory_order_relaxed);
                sl.unlock();
            }
        });
    }
    for (auto &th : threads)
        th.join();
    EXPECT_EQ(counter, static_cast<std::uint64_t>(kThreads) * kIters);
    EXPECT_EQ(acquired.load(), kThreads * kIters);
    EXPECT_FALSE(sl.locked());
}

TEST(SpinLock, TtasWaitLoopStillHonoursItsBudget) {
    // The TTAS inner loop must consume the caller's budget, not wait forever: a permanently held
    // lock has to make trylock(spin) and trylock_for() return false rather than spin on the load.
    SpinLock sl;
    sl.lock();
    EXPECT_FALSE(sl.trylock(1000)) << "held lock: the load-spin must exhaust the spin budget and give up";
    EXPECT_FALSE(sl.trylock_for(std::chrono::milliseconds{5})) << "held lock: the load-spin must respect the deadline";
    EXPECT_FALSE(sl.trylock(0)) << "held lock, zero budget: exactly one attempt, then give up";
    sl.unlock();
    EXPECT_TRUE(sl.trylock(0)) << "free lock, zero budget: the one mandatory attempt still acquires";
    sl.unlock();
}
