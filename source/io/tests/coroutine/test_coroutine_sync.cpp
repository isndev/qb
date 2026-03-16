/**
 * @file test_coroutine_sync.cpp
 * @brief Synchronization primitives tests
 *
 * Tests for:
 * - semaphore: limit concurrent operations
 * - async_mutex: mutual exclusion
 * - async_rw_lock: read-write lock
 * - barrier: synchronization point
 *
 * @author qb - C++ Actor Framework
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
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Semaphore limits concurrency
 * @brief Max N concurrent operations
 */
TEST_F(SemaphoreTests, LimitsConcurrency) {
    semaphore sem(2);
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};

    auto worker = [&sem, &concurrent, &max_concurrent]() -> task<void> {
        co_await sem.acquire();

        int current = ++concurrent;
        int expected_max = max_concurrent.load();
        while (current > expected_max &&
               !max_concurrent.compare_exchange_weak(expected_max, current)) {}

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
    semaphore sem(1);
    std::atomic<bool> acquired{false};

    auto worker = [&sem, &acquired]() -> task<void> {
        auto guard = co_await sem.scoped_acquire();
        acquired = true;
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

// =============================================================================
// TEST SUITE: Async Mutex
// =============================================================================

class MutexTests : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Mutex serializes access
 * @brief Critical section protection
 */
TEST_F(MutexTests, SerializesAccess) {
    async_mutex mtx;
    std::atomic<int> counter{0};

    auto worker = [&mtx, &counter]() -> task<void> {
        auto guard = co_await mtx.scoped_lock();
        int val = counter;
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
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Multiple readers
 * @brief Multiple concurrent readers allowed
 */
TEST_F(RWLockTests, MultipleReaders) {
    async_rw_lock rw;
    std::atomic<int> readers{0};
    std::atomic<int> max_readers{0};

    auto reader = [&rw, &readers, &max_readers]() -> task<void> {
        co_await rw.lock_read();

        int current = ++readers;
        int expected_max = max_readers.load();
        while (current > expected_max &&
               !max_readers.compare_exchange_weak(expected_max, current)) {}

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
    async_rw_lock rw;
    std::atomic<bool> writing{false};
    std::atomic<int> readers_during_write{0};

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
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Barrier synchronization
 * @brief Wait for N coroutines
 */
TEST_F(BarrierTests, Synchronization) {
    barrier b(3);
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
    EXPECT_EQ(passed, 0);  // Waiting for 3rd

    coro_scheduler().spawn(worker());
    run_for(50ms);
    EXPECT_EQ(passed, 3);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
