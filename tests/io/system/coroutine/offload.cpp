/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/offload.cpp
 * @brief `co_await offload(fn, args...)` (qb/io/async/coroutine/offload.h, Huly QB-69) on a real loop.
 *
 * What the header promises, each asserted where it can be observed (the pool's lazy start and its sizing are in
 * offload-pool.cpp, alone in its binary: they need a process nothing has offloaded from, and ctest shuffles the cases):
 *  - the call runs on a pool thread, the coroutine resumes on the awaiting thread, values cross
 *    (a move-only result, copied and moved arguments), an exception is rethrown by `co_await`;
 *  - the callable and its arguments die on the pool thread, the result on the awaiting loop;
 *  - a frame destroyed while its call runs is never resumed, and its result dies on the loop;
 *  - a thousand concurrent offloads each resume exactly once, and the loop has no work after the
 *    last (the port's watcher is stopped);
 *  - a thread that exits with an offload in flight is safe (the port closed, the late result
 *    dropped on the pool thread);
 *  - the blocking `async::run()` returns only once the offload completed.
 * The wake path is the loop backend's, so this binary is on the per-backend matrix
 * (system/CMakeLists.txt, QB_IO_BACKEND_SENSITIVE_TESTS).
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

// A named namespace: these types are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace offload_test {

/// A one-shot gate a pool thread can wait on, released from the test thread.
class Gate {
public:
    void
    open() {
        {
            std::lock_guard lk(_m);
            _open = true;
        }
        _cv.notify_all();
    }
    /// @return Whether the gate opened within `limit` (a pool thread never waits forever).
    bool
    wait(std::chrono::milliseconds limit = 5s) {
        std::unique_lock lk(_m);
        return _cv.wait_for(lk, limit, [this] { return _open; });
    }

private:
    std::mutex              _m;
    std::condition_variable _cv;
    bool                    _open = false;
};

/// Where a `ThreadStamp` died: written by whichever thread destroys it, read by the test thread.
using DeathPlace = std::shared_ptr<std::atomic<std::thread::id>>;

[[nodiscard]] inline DeathPlace
death_place() {
    return std::make_shared<std::atomic<std::thread::id>>();
}

/// Records the thread on which the LIVE instance -- not a moved-from one -- is destroyed.
struct ThreadStamp {
    DeathPlace died_on;
    explicit ThreadStamp(DeathPlace where)
        : died_on(std::move(where)) {}
    ThreadStamp(ThreadStamp &&o) noexcept
        : died_on(std::move(o.died_on)) {}
    ThreadStamp(const ThreadStamp &o)
        : died_on(o.died_on) {}
    ~ThreadStamp() {
        if (died_on)
            died_on->store(std::this_thread::get_id());
    }
};

class Offload : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (listener::current.has_coro_scheduler()) {
            run_for(5ms);
            listener::current.reset_coro_scheduler();
        }
        listener::current.clear();
    }
};

} // namespace offload_test

using offload_test::death_place;
using offload_test::Gate;
using offload_test::Offload;
using offload_test::ThreadStamp;

// ---------------------------------------------------------------------------
// Where things run, and what crosses
// ---------------------------------------------------------------------------

TEST_F(Offload, TheCallRunsOnAPoolThreadAndTheCoroutineResumesOnTheAwaitingThread) {
    const auto        loop_thread = std::this_thread::get_id();
    std::thread::id   call_thread{};
    std::thread::id   resume_thread{};
    std::atomic<int>  result{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        result        = co_await offload([&call_thread] {
            call_thread = std::this_thread::get_id();
            return 42;
        });
        resume_thread = std::this_thread::get_id();
        done          = true;
    });

    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "the offload never resumed its coroutine";
    EXPECT_EQ(result.load(), 42);
    EXPECT_NE(call_thread, std::thread::id{});
    EXPECT_NE(call_thread, loop_thread) << "the call ran on the awaiting thread";
    EXPECT_EQ(resume_thread, loop_thread) << "the coroutine resumed off its loop";
}

TEST_F(Offload, VoidMoveOnlyResultAndArgumentsCross) {
    std::atomic<int>     side{0};
    std::unique_ptr<int> moved_out;
    std::string          seen;
    std::atomic<bool>    done{false};

    std::string original = "copied at the call";
    coro_scheduler().spawn([&]() -> task<void> {
        co_await offload([&side] { side = 7; });                                                             // void
        moved_out    = co_await offload([](std::unique_ptr<int> p) { return p; }, std::make_unique<int>(9)); // move-only in and out
        auto pending = offload([](std::string s) { return s + "!"; }, original);                             // copied HERE, at the call
        original     = "changed after the call";
        seen         = co_await pending;
        done         = true;
    });

    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "the offloads never completed";
    EXPECT_EQ(side.load(), 7);
    ASSERT_TRUE(moved_out);
    EXPECT_EQ(*moved_out, 9);
    EXPECT_EQ(seen, "copied at the call!") << "the argument was not copied at the call";
}

TEST_F(Offload, AnExceptionIsRethrownByCoAwaitOnTheAwaitingThread) {
    const auto        loop_thread = std::this_thread::get_id();
    std::string       what;
    std::thread::id   caught_on{};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        try {
            (void) co_await offload([]() -> int { throw std::runtime_error("from the pool"); });
        } catch (std::runtime_error const &e) {
            what      = e.what();
            caught_on = std::this_thread::get_id();
        }
        done = true;
    });

    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "the throwing offload never resumed its coroutine";
    EXPECT_EQ(what, "from the pool");
    EXPECT_EQ(caught_on, loop_thread);
}

TEST_F(Offload, TheCallableDiesOnThePoolThreadAndTheResultOnTheLoop) {
    const auto        loop_thread  = std::this_thread::get_id();
    auto              callable_end = death_place();
    auto              result_end   = death_place();
    std::thread::id   call_thread{};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        {
            auto r = co_await offload([stamp = ThreadStamp{callable_end}, result_end, &call_thread]() {
                call_thread = std::this_thread::get_id();
                return ThreadStamp{result_end};
            });
            (void) r;
        } // the result dies here, on the loop
        done = true;
    });

    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); })) << "the offload never resumed its coroutine";
    EXPECT_EQ(callable_end->load(), call_thread) << "the callable was not destroyed on the pool thread that ran it";
    EXPECT_EQ(result_end->load(), loop_thread) << "the result was not destroyed on the awaiting loop";
}

// ---------------------------------------------------------------------------
// A frame destroyed while its call runs
// ---------------------------------------------------------------------------

TEST_F(Offload, AnAbandonedFrameIsNeverResumedAndItsResultDiesOnTheLoop) {
    const auto        loop_thread = std::this_thread::get_id();
    auto              result_end  = death_place();
    Gate              release;
    std::atomic<bool> started{false};
    std::atomic<bool> resumed{false};
    const auto        discarded_before = current_offload_stats().discarded;

    coro_scheduler().spawn([&]() -> task<void> {
        (void) co_await offload([&release, &started, result_end] {
            started = true;
            (void) release.wait();
            return ThreadStamp{result_end};
        });
        resumed = true;
    });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return started.load(); })) << "the call never started";
    ASSERT_TRUE(listener::current.has_work()) << "a loop with an offload in flight reports no work";

    // Destroy the suspended frame while its call is still running, then let the call finish.
    listener::current.reset_coro_scheduler();
    release.open();

    ASSERT_TRUE(qb::io::test::pump_until([&] { return current_offload_stats().discarded == discarded_before + 1; }))
        << "the abandoned offload's completion was never drained";
    EXPECT_FALSE(resumed.load()) << "a destroyed frame was resumed";
    EXPECT_EQ(result_end->load(), loop_thread) << "the abandoned result was not destroyed on the awaiting loop";
    EXPECT_TRUE(qb::io::test::pump_until([] { return !listener::current.has_work(); }))
        << "the loop still has work once the abandoned offload was drained";
}

// ---------------------------------------------------------------------------
// Many at once
// ---------------------------------------------------------------------------

TEST_F(Offload, AThousandConcurrentOffloadsEachResumeExactlyOnceAndLeaveNoWork) {
    constexpr int     n = 1000;
    std::vector<int>  resumes(n, 0);
    std::atomic<int>  done{0};
    std::atomic<long> sum{0};

    for (int i = 0; i < n; ++i)
        coro_scheduler().spawn([i, &resumes, &done, &sum]() -> task<void> {
            const int v = co_await offload([](int x) { return x * 2; }, i);
            ++resumes[i];
            sum.fetch_add(v);
            done.fetch_add(1);
        });

    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load() == n; }, 10s)) << done.load() << " of " << n << " resumed";
    for (int i = 0; i < n; ++i)
        ASSERT_EQ(resumes[i], 1) << "coroutine " << i << " resumed " << resumes[i] << " times";
    EXPECT_EQ(sum.load(), static_cast<long>(n) * (n - 1)) << "a result was lost or crossed to another coroutine";
    EXPECT_TRUE(qb::io::test::pump_until([] { return !listener::current.has_work(); }))
        << "the loop still has work after the last offload: the port's watcher was left started";
}

// ---------------------------------------------------------------------------
// A thread that exits with an offload in flight
// ---------------------------------------------------------------------------

TEST_F(Offload, AThreadExitingWithAnOffloadInFlightIsSafe) {
    auto              result_end = death_place();
    std::thread::id   call_thread{};
    Gate              release;
    std::atomic<bool> started{false};
    std::atomic<bool> resumed{false};

    std::thread owner([&] {
        coro_scheduler().spawn([&]() -> task<void> {
            (void) co_await offload([&] {
                call_thread = std::this_thread::get_id();
                started     = true;
                (void) release.wait();
                return ThreadStamp{result_end};
            });
            resumed = true;
        });
        (void) qb::io::test::pump_until([&] { return started.load(); });
        // Exit with the call still running: the port closes, the listener (and its loop) go.
    });
    owner.join();
    ASSERT_TRUE(started.load()) << "the call never started on the exiting thread";

    const auto completed_before = current_offload_stats().completed;
    const auto discarded_before = current_offload_stats().discarded;
    release.open();
    // The pool thread finishes the call, finds the port closed and drops the job itself.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (current_offload_stats().completed == completed_before && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_GT(current_offload_stats().completed, completed_before) << "the call never completed";
    while (result_end->load() == std::thread::id{} && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    EXPECT_FALSE(resumed.load()) << "a frame of an exited thread was resumed";
    EXPECT_EQ(result_end->load(), call_thread) << "with its loop gone, the result is dropped on the pool thread that made it";
    EXPECT_EQ(current_offload_stats().discarded, discarded_before + 1) << "the dropped result was not counted as discarded";
}

// ---------------------------------------------------------------------------
// The blocking run
// ---------------------------------------------------------------------------

TEST_F(Offload, TheBlockingRunReturnsOnlyOnceTheOffloadCompleted) {
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        (void) co_await offload([] {
            std::this_thread::sleep_for(50ms);
            return 0;
        });
        done = true;
    });
    run(EVRUN_NOWAIT); // starts the coroutine: its offload is now in flight
    ASSERT_FALSE(done.load());
    ASSERT_TRUE(listener::current.has_work()) << "a loop with an offload in flight reports no work";

    run(); // blocks while the port's watcher is referenced, i.e. until the completion is drained
    EXPECT_TRUE(done.load()) << "the blocking run returned before the offload completed";
    EXPECT_FALSE(listener::current.has_work());
}
