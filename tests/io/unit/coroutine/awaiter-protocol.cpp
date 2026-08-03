/**
 * @file unit/coroutine/awaiter-protocol.cpp
 * @brief The C++20 awaiter protocol over the qb-io scheduler — the canonical conformance file.
 *
 * This file proves that `qb::io::async`'s scheduler honors the full C++20 awaiter contract for
 * user-defined awaiters: the await_ready → await_suspend → await_resume ordering, every legal
 * await_suspend return form (void, bool, coroutine_handle for symmetric transfer), value-returning and
 * move-only awaiters, awaiter lifetime/destruction, exception propagation out of await_ready /
 * await_suspend / await_resume, and the built-in `sleep()` timer awaiter. Everything runs in-process on
 * the event loop (`qb::io::async::init()` via `reset_async_context()`, `spawn`, `run_ready`, the de-flake
 * pump `pump_until`/`wait_until`) — NO sockets, NO daemon, NO TLS — so this is a pure `unit` test.
 *
 * Hardened over the original test-coroutine-awaiters.cpp:
 *   - the file-local fixed-`run_for` `wait_until` helper is removed in favour of the shared
 *     qb::io::test::pump_until / wait_until (one source of truth; loud bounded timeouts);
 *   - CustomSymmetricTransfer now ACTUALLY drives `symmetric_transfer_awaiter` (await_suspend returns the
 *     target handle) and asserts the resulting execution order, instead of leaving the awaiter
 *     defined-but-unused and testing plain task composition under a misleading name;
 *   - the four immediate-awaiter smoke tests are consolidated into one ordered contract;
 *   - SleepAwaiterIntegration drops the spurious `EXPECT_LT(elapsed, 150ms)` upper bound (the single most
 *     flaky assertion under load / sanitizers) — only the lower bound is a real contract;
 *   - added the missing protocol corners: await_ready() that THROWS, and await_suspend() returning
 *     `false` (the bool overload meaning "resume immediately, do not suspend").
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

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;
using qb::io::test::wait_until;

// =============================================================================
// TEST FIXTURE
// =============================================================================

class CoroutineAwaiterTests : public ::testing::Test {
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

// =============================================================================
// CUSTOM AWAITER IMPLEMENTATIONS
// =============================================================================

/** @brief Awaiter that completes immediately (await_ready == true). */
struct immediate_awaiter {
    bool
    await_ready() const noexcept {
        return true;
    }
    void
    await_suspend(std::coroutine_handle<>) noexcept {}
    void
    await_resume() const noexcept {}
};

/** @brief Awaiter that always suspends and re-schedules itself for immediate resumption. */
struct always_suspend_awaiter {
    bool
    await_ready() const noexcept {
        return false;
    }
    void
    await_suspend(std::coroutine_handle<> h) noexcept {
        coro_scheduler().schedule_resume(h);
    }
    void
    await_resume() const noexcept {}
};

/** @brief Awaiter that returns a value from await_resume. */
template <typename T>
struct value_awaiter {
    T value_;
    explicit value_awaiter(T val)
        : value_(std::move(val)) {}
    bool
    await_ready() const noexcept {
        return true;
    }
    void
    await_suspend(std::coroutine_handle<>) noexcept {}
    T
    await_resume() noexcept {
        return std::move(value_);
    }
};

/** @brief Awaiter that throws from await_resume. */
struct throwing_awaiter {
    bool
    await_ready() const noexcept {
        return true;
    }
    void
    await_suspend(std::coroutine_handle<>) noexcept {}
    void
    await_resume() const {
        throw std::runtime_error("awaiter threw");
    }
};

/** @brief Awaiter that throws from await_ready (before any suspension decision). */
struct ready_throwing_awaiter {
    bool
    await_ready() const {
        throw std::runtime_error("ready threw");
    }
    void
    await_suspend(std::coroutine_handle<>) noexcept {}
    void
    await_resume() const noexcept {}
};

/**
 * @brief Awaiter whose await_suspend returns the bool `false` — "do not suspend, resume immediately".
 *
 * The bool overload of await_suspend lets an awaiter back out of suspension after await_ready has already
 * returned false: returning false resumes the current coroutine without parking it.
 */
struct bool_no_suspend_awaiter {
    bool *suspend_called;
    explicit bool_no_suspend_awaiter(bool *flag)
        : suspend_called(flag) {}
    bool
    await_ready() const noexcept {
        return false;
    }
    bool
    await_suspend(std::coroutine_handle<>) noexcept {
        *suspend_called = true;
        return false; // resume immediately, do not suspend
    }
    void
    await_resume() const noexcept {}
};

/**
 * @brief Symmetric-transfer awaiter: await_suspend returns a target handle so the coroutine machinery
 *        tail-resumes the target without growing the stack.
 */
struct symmetric_transfer_awaiter {
    std::coroutine_handle<> target_;
    explicit symmetric_transfer_awaiter(std::coroutine_handle<> target)
        : target_(target) {}
    bool
    await_ready() const noexcept {
        return false;
    }
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<>) noexcept {
        return target_; // symmetric transfer to the target
    }
    void
    await_resume() const noexcept {}
};

// =============================================================================
// IMMEDIATE-AWAITER PROTOCOL (consolidated)
// =============================================================================

/**
 * @test Immediate awaiter never suspends, in sequence and in a loop
 * @brief await_ready() == true keeps execution synchronous: an immediate awaiter, a chain of them, the
 *        same in a loop, and a value-returning immediate awaiter all run without yielding to the loop.
 *
 * Consolidates the dissolved ImmediateAwaiterDoesntSuspend / MultipleImmediateAwaiters / AwaiterInLoop /
 * ValueAwaiterReturnsValue into one ordered contract driven by a single run_ready() drain.
 */
TEST_F(CoroutineAwaiterTests, ImmediateAwaiterNeverSuspends) {
    std::atomic<int> counter{0};
    std::atomic<int> value_seen{-1};

    auto counter_ptr = &counter;
    auto value_ptr   = &value_seen;
    coro_scheduler().spawn([counter_ptr, value_ptr]() -> task<void> {
        // Single immediate awaiter.
        co_await immediate_awaiter{};
        counter_ptr->fetch_add(1);

        // Chain of immediate awaiters.
        co_await immediate_awaiter{};
        co_await immediate_awaiter{};
        counter_ptr->fetch_add(1);

        // Immediate awaiters in a loop.
        for (int i = 0; i < 5; ++i) {
            co_await immediate_awaiter{};
            counter_ptr->fetch_add(1);
        }

        // Value-returning immediate awaiter.
        int v = co_await value_awaiter<int>{42};
        value_ptr->store(v);
        co_return;
    });

    // No real suspension anywhere: a single drain completes the whole body.
    coro_scheduler().run_ready();

    EXPECT_EQ(counter.load(), 7);     // 1 + 1 + 5
    EXPECT_EQ(value_seen.load(), 42); // value awaiter
    EXPECT_EQ(coro_scheduler().active_count(), 0u);
}

// =============================================================================
// SUSPENDING-AWAITER PROTOCOL
// =============================================================================

/**
 * @test await_ready() == false suspends, then the scheduler resumes
 * @brief Stepping run_ready(1) leaves the coroutine parked after the co_await; a further drive resumes it.
 */
TEST_F(CoroutineAwaiterTests, AlwaysSuspendAwaiterSuspendsThenResumes) {
    std::atomic<int> stage{0};

    auto stage_ptr = &stage;
    coro_scheduler().spawn([stage_ptr]() -> task<void> {
        stage_ptr->store(1);
        co_await always_suspend_awaiter{};
        stage_ptr->store(2);
        co_return;
    });

    // One step: body runs to the co_await, suspends, re-enqueues itself.
    coro_scheduler().run_ready(1);
    EXPECT_EQ(stage.load(), 1);

    // Drive to completion.
    EXPECT_TRUE(pump_until([&] { return stage.load() == 2; })) << "suspended coroutine never resumed";
}

/**
 * @test await_suspend returning bool false resumes immediately without parking
 * @brief The bool overload that returns false means "don't suspend": await_suspend runs, but the
 *        coroutine continues straight into await_resume on the same drain.
 */
TEST_F(CoroutineAwaiterTests, AwaitSuspendReturningFalseResumesImmediately) {
    bool             suspend_called = false;
    std::atomic<int> stage{0};

    auto stage_ptr = &stage;
    coro_scheduler().spawn([stage_ptr, &suspend_called]() -> task<void> {
        stage_ptr->store(1);
        co_await bool_no_suspend_awaiter{&suspend_called};
        stage_ptr->store(2);
        co_return;
    });

    // A single drain must take it all the way to stage 2 — it never actually parked.
    coro_scheduler().run_ready();
    EXPECT_TRUE(suspend_called) << "await_suspend was not invoked";
    EXPECT_EQ(stage.load(), 2) << "false-returning await_suspend wrongly parked the coroutine";
    EXPECT_EQ(coro_scheduler().active_count(), 0u);
}

/**
 * @test Awaiter protocol order: ready → suspend → resume
 * @brief await_ready() is called first, then (on suspension) await_suspend(), then await_resume().
 */
TEST_F(CoroutineAwaiterTests, AwaiterProtocolOrder) {
    struct ProtocolTracker {
        std::vector<std::string> *log;
        explicit ProtocolTracker(std::vector<std::string> *l)
            : log(l) {}
        bool
        await_ready() const noexcept {
            log->push_back("ready");
            return false;
        }
        void
        await_suspend(std::coroutine_handle<> h) noexcept {
            log->push_back("suspend");
            coro_scheduler().schedule_resume(h);
        }
        void
        await_resume() const noexcept {
            log->push_back("resume");
        }
    };

    std::vector<std::string> log;
    std::atomic<bool>        done{false};

    auto log_ptr  = &log;
    auto done_ptr = &done;
    coro_scheduler().spawn([log_ptr, done_ptr]() -> task<void> {
        co_await ProtocolTracker{log_ptr};
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], "ready");
    EXPECT_EQ(log[1], "suspend");
    EXPECT_EQ(log[2], "resume");
}

/**
 * @test await_suspend receives a valid, not-done handle
 * @brief The handle passed into await_suspend identifies the live, suspended coroutine.
 */
TEST_F(CoroutineAwaiterTests, AwaitSuspendReceivesValidHandle) {
    struct HandleChecker {
        bool *ok;
        explicit HandleChecker(bool *o)
            : ok(o) {}
        bool
        await_ready() const noexcept {
            return false;
        }
        void
        await_suspend(std::coroutine_handle<> h) noexcept {
            if (h && !h.done())
                *ok = true;
            coro_scheduler().schedule_resume(h);
        }
        void
        await_resume() const noexcept {}
    };

    bool              handle_ok = false;
    std::atomic<bool> done{false};

    auto done_ptr = &done;
    coro_scheduler().spawn([done_ptr, &handle_ok]() -> task<void> {
        co_await HandleChecker{&handle_ok};
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coroutine never completed";
    EXPECT_TRUE(handle_ok) << "await_suspend received an invalid handle";
}

// =============================================================================
// AWAITER STATE & LIFETIME
// =============================================================================

/**
 * @test Awaiter keeps state across suspend/resume
 * @brief State written in await_suspend is observed in await_resume.
 */
TEST_F(CoroutineAwaiterTests, AwaiterKeepsStateAcrossSuspend) {
    struct StatefulAwaiter {
        int value = 0;
        bool
        await_ready() const noexcept {
            return false;
        }
        void
        await_suspend(std::coroutine_handle<> h) noexcept {
            value = 42; // set during suspension
            coro_scheduler().schedule_resume(h);
        }
        int
        await_resume() noexcept {
            return value; // read after resumption
        }
    };

    std::atomic<int> result{-1};

    auto result_ptr = &result;
    coro_scheduler().spawn([result_ptr]() -> task<void> {
        int val = co_await StatefulAwaiter{};
        result_ptr->store(val);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return result.load() != -1; })) << "stateful awaiter never resumed";
    EXPECT_EQ(result.load(), 42);
}

/**
 * @test Awaiter is destroyed before the statement after the co_await runs
 * @brief The temporary awaiter's lifetime ends at the end of the full co_await expression.
 */
TEST_F(CoroutineAwaiterTests, AwaiterDestroyedAfterAwait) {
    struct TrackedAwaiter {
        std::atomic<bool> *destroyed;
        explicit TrackedAwaiter(std::atomic<bool> *d)
            : destroyed(d) {}
        ~TrackedAwaiter() {
            if (destroyed)
                destroyed->store(true);
        }
        bool
        await_ready() const noexcept {
            return false;
        }
        void
        await_suspend(std::coroutine_handle<> h) noexcept {
            coro_scheduler().schedule_resume(h);
        }
        void
        await_resume() const noexcept {}
    };

    std::atomic<bool> awaiter_destroyed{false};
    std::atomic<bool> after_await{false};

    auto destroyed_ptr = &awaiter_destroyed;
    auto after_ptr     = &after_await;
    coro_scheduler().spawn([destroyed_ptr, after_ptr]() -> task<void> {
        co_await TrackedAwaiter{destroyed_ptr};
        // The awaiter must already be destroyed by the time control reaches here.
        EXPECT_TRUE(destroyed_ptr->load());
        after_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return after_await.load(); })) << "coroutine never completed";
    EXPECT_TRUE(awaiter_destroyed.load());
}

/**
 * @test Move-only awaiter
 * @brief An awaiter holding a move-only member works as a temporary and returns its payload.
 */
TEST_F(CoroutineAwaiterTests, MoveOnlyAwaiter) {
    struct MoveOnlyAwaiter {
        std::unique_ptr<int> data;
        explicit MoveOnlyAwaiter(int val)
            : data(std::make_unique<int>(val)) {}
        MoveOnlyAwaiter(const MoveOnlyAwaiter &) = delete;
        MoveOnlyAwaiter(MoveOnlyAwaiter &&)      = default;
        bool
        await_ready() const noexcept {
            return true;
        }
        void
        await_suspend(std::coroutine_handle<>) noexcept {}
        int
        await_resume() noexcept {
            return *data;
        }
    };

    std::atomic<int> result{-1};

    auto result_ptr = &result;
    coro_scheduler().spawn([result_ptr]() -> task<void> {
        int val = co_await MoveOnlyAwaiter{42};
        result_ptr->store(val);
        co_return;
    });

    coro_scheduler().run_ready();
    EXPECT_EQ(result.load(), 42);
}

// =============================================================================
// SYMMETRIC TRANSFER (drives symmetric_transfer_awaiter)
// =============================================================================

/**
 * @test Custom symmetric-transfer awaiter tail-resumes a target coroutine
 * @brief await_suspend returns a target handle; the machinery transfers control to the target without
 *        recursion. The target then re-schedules the caller, so the full ready → target → caller order is
 *        observable.
 *
 * Replaces the dissolved CustomSymmetricTransfer, which defined `symmetric_transfer_awaiter` but never
 * used it (it co_awaited a plain task and tested ordinary task composition under a misleading name). Here
 * the awaiter is the unit under test: the caller hands its own handle to the target via shared state, the
 * awaiter symmetric-transfers into the target, and the target schedules the caller's resumption.
 */
TEST_F(CoroutineAwaiterTests, CustomSymmetricTransfer) {
    std::vector<int>  order;
    std::atomic<bool> done{false};

    // Shared box so the target coroutine can reach the suspended caller's handle.
    auto caller_handle = std::make_shared<std::coroutine_handle<>>();

    auto order_ptr = &order;
    auto done_ptr  = &done;

    // Target coroutine: records its marker, then re-schedules the caller it transferred from.
    auto make_target = [order_ptr, caller_handle]() -> task<void> {
        order_ptr->push_back(2);
        // Re-schedule the caller that symmetric-transferred into us.
        if (*caller_handle)
            coro_scheduler().schedule_resume(*caller_handle);
        co_return;
    };
    auto target        = make_target();
    auto target_handle = target.handle();

    coro_scheduler().spawn([order_ptr, done_ptr, target_handle, caller_handle]() -> task<void> {
        order_ptr->push_back(1);

        // Custom awaiter whose await_suspend returns the target handle (symmetric transfer).
        struct transfer {
            std::coroutine_handle<>                  target;
            std::shared_ptr<std::coroutine_handle<>> caller_box;
            bool
            await_ready() const noexcept {
                return false;
            }
            std::coroutine_handle<>
            await_suspend(std::coroutine_handle<> caller) noexcept {
                *caller_box = caller; // publish our handle for the target to resume
                return target;        // tail-transfer into the target
            }
            void
            await_resume() const noexcept {}
        };

        co_await transfer{target_handle, caller_handle};

        order_ptr->push_back(3);
        done_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "symmetric-transfer chain never completed";
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1); // caller before the transfer
    EXPECT_EQ(order[1], 2); // target, reached via symmetric transfer
    EXPECT_EQ(order[2], 3); // caller resumed after the target re-scheduled it
}

// =============================================================================
// EXCEPTIONS OUT OF EACH AWAITER ENTRY POINT
// =============================================================================

/**
 * @test Exception thrown from await_resume propagates to the awaiting coroutine
 */
TEST_F(CoroutineAwaiterTests, AwaitResumeExceptionPropagates) {
    std::atomic<bool> caught{false};

    auto caught_ptr = &caught;
    coro_scheduler().spawn([caught_ptr]() -> task<void> {
        try {
            co_await throwing_awaiter{};
        } catch (const std::runtime_error &e) {
            if (std::string(e.what()) == "awaiter threw")
                caught_ptr->store(true);
        }
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return caught.load(); })) << "await_resume exception did not propagate";
}

/**
 * @test Exception thrown from await_ready propagates to the awaiting coroutine
 * @brief await_ready runs in the awaiting coroutine's context; a throw there surfaces at the co_await.
 */
TEST_F(CoroutineAwaiterTests, AwaitReadyExceptionPropagates) {
    std::atomic<bool> caught{false};

    auto caught_ptr = &caught;
    coro_scheduler().spawn([caught_ptr]() -> task<void> {
        try {
            co_await ready_throwing_awaiter{};
        } catch (const std::runtime_error &e) {
            if (std::string(e.what()) == "ready threw")
                caught_ptr->store(true);
        }
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return caught.load(); })) << "await_ready exception did not propagate";
}

/**
 * @test Exception thrown from await_suspend propagates to the awaiting coroutine
 */
TEST_F(CoroutineAwaiterTests, AwaitSuspendExceptionPropagates) {
    struct ThrowInSuspend {
        bool
        await_ready() const noexcept {
            return false;
        }
        void
        await_suspend(std::coroutine_handle<>) {
            throw std::runtime_error("suspend failed");
        }
        void
        await_resume() const noexcept {}
    };

    std::atomic<bool> caught{false};

    auto caught_ptr = &caught;
    coro_scheduler().spawn([caught_ptr]() -> task<void> {
        try {
            co_await ThrowInSuspend{};
        } catch (const std::runtime_error &e) {
            if (std::string(e.what()) == "suspend failed")
                caught_ptr->store(true);
        }
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return caught.load(); })) << "await_suspend exception did not propagate";
}

// =============================================================================
// BUILT-IN sleep() AWAITER
// =============================================================================

/**
 * @test sleep() integrates with libev and waits at least the requested duration
 * @brief Lower-bound only: the coroutine completes and at least 50ms elapsed. No upper bound — a fixed
 *        ceiling is the single most flaky assertion under CI load / sanitizers.
 */
TEST_F(CoroutineAwaiterTests, SleepAwaiterWaitsAtLeastDuration) {
    auto              start = std::chrono::steady_clock::now();
    std::atomic<bool> completed{false};

    auto completed_ptr = &completed;
    coro_scheduler().spawn([completed_ptr]() -> task<void> {
        co_await sleep(50ms);
        completed_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return completed.load(); })) << "sleep coroutine never completed";
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 50ms);
}

/**
 * @test Zero-duration sleep yields then resumes immediately
 * @brief sleep(0ms) is a cooperative yield: it re-enqueues at the back of the ready queue rather than
 *        arming a kernel timer, and resumes promptly.
 */
TEST_F(CoroutineAwaiterTests, ZeroDurationSleepYields) {
    std::atomic<int> stage{0};

    auto stage_ptr = &stage;
    coro_scheduler().spawn([stage_ptr]() -> task<void> {
        stage_ptr->store(1);
        co_await sleep(0ms);
        stage_ptr->store(2);
        co_return;
    });

    EXPECT_TRUE(wait_until([&] { return stage.load() == 2; }, 200ms, 1ms)) << "zero-sleep never resumed";
    EXPECT_EQ(stage.load(), 2);
}

/**
 * @test Sequential sleeps accumulate their durations
 * @brief Three back-to-back 20ms sleeps complete and at least ~60ms elapsed.
 */
TEST_F(CoroutineAwaiterTests, MultipleSleepsAccumulate) {
    auto              start = std::chrono::steady_clock::now();
    std::atomic<bool> completed{false};

    auto completed_ptr = &completed;
    coro_scheduler().spawn([completed_ptr]() -> task<void> {
        co_await sleep(20ms);
        co_await sleep(20ms);
        co_await sleep(20ms);
        completed_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return completed.load(); })) << "sequential sleeps never completed";
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 60ms);
}

/**
 * @test A coroutine can await heterogeneous awaiter kinds in one body
 * @brief Immediate, value-returning, and the built-in sleep awaiter compose; the additive result is exact.
 */
TEST_F(CoroutineAwaiterTests, MixedAwaiterTypes) {
    std::atomic<int> result{0};

    auto result_ptr = &result;
    coro_scheduler().spawn([result_ptr]() -> task<void> {
        co_await immediate_awaiter{};
        result_ptr->fetch_add(1);

        int v = co_await value_awaiter<int>{10};
        result_ptr->fetch_add(v);

        co_await sleep(10ms);
        result_ptr->fetch_add(100);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return result.load() == 111; })) << "mixed awaiters never completed";
    EXPECT_EQ(result.load(), 111); // 1 + 10 + 100
}

// =============================================================================
// BUILT-IN socket_awaiter (qev_io path: await_suspend → io_callback → await_resume)
// =============================================================================
//
// These exercise awaiter.h's socket_awaiter (await_suspend starts qev_io, io_callback fires
// on_event_ready, await_resume stops the watcher) and its destructor scrub. They use a POSIX pipe so
// the fd readiness is fully under test control — no real sockets, still hermetic. Skipped on Windows
// where the fd model differs (the socket_awaiter there takes a uintptr_t handle).
#if !defined(_WIN32)

/**
 * @test wait_readable resumes once the fd has data (socket_awaiter qev_io path)
 * @brief Drives socket_awaiter::await_suspend (qev_io_start), io_callback → on_event_ready (schedule), and
 *        await_resume (qev_io_stop + unregister). The coroutine parks on a pipe read end, stays suspended
 *        until a byte is written, then resumes and reads it — proving the watcher actually fired.
 */
TEST_F(CoroutineAwaiterTests, WaitReadableResumesWhenDataArrives) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0) << "pipe() failed";
    const int read_fd  = fds[0];
    const int write_fd = fds[1];

    std::atomic<int>  stage{0};
    std::atomic<char> byte_seen{0};

    auto stage_ptr = &stage;
    auto byte_ptr  = &byte_seen;
    coro_scheduler().spawn([stage_ptr, byte_ptr, read_fd]() -> task<void> {
        stage_ptr->store(1);             // running, about to park on the read end
        co_await wait_readable(read_fd); // suspends in socket_awaiter::await_suspend (qev_io armed)
        char    c = 0;
        ssize_t n = ::read(read_fd, &c, 1);
        byte_ptr->store(n == 1 ? c : 0);
        stage_ptr->store(2); // resumed only after the fd became readable
        co_return;
    });

    // Step: the coroutine reaches wait_readable and suspends (no data yet → stays at stage 1).
    coro_scheduler().run_ready();
    ASSERT_EQ(stage.load(), 1);
    EXPECT_EQ(coro_scheduler().active_count(), 1u) << "coroutine should be parked on the fd";

    // Make the fd readable; the qev_io watcher fires, schedules the resume, await_resume stops the watcher.
    const char payload = 'Z';
    ASSERT_EQ(::write(write_fd, &payload, 1), 1);

    EXPECT_TRUE(pump_until([&] { return stage.load() == 2; })) << "wait_readable never resumed on data";
    EXPECT_EQ(byte_seen.load(), 'Z') << "resumed coroutine did not read the byte that woke it";
    EXPECT_EQ(coro_scheduler().active_count(), 0u);

    ::close(read_fd);
    ::close(write_fd);
}

/**
 * @test wait_writable resumes immediately on an already-writable fd
 * @brief A fresh pipe's write end is immediately writable, so socket_awaiter's qev_io fires on the next
 *        loop tick — exercising the EV_WRITE branch of the io watcher through to await_resume.
 */
TEST_F(CoroutineAwaiterTests, WaitWritableResumesOnWritableFd) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0) << "pipe() failed";
    const int read_fd  = fds[0];
    const int write_fd = fds[1];

    std::atomic<bool> wrote{false};
    auto              wrote_ptr = &wrote;
    coro_scheduler().spawn([wrote_ptr, write_fd]() -> task<void> {
        co_await wait_writable(write_fd); // pipe write end is writable → resumes promptly
        const char c = 'Q';
        // Bind the result: glibc marks write() __wur under _FORTIFY_SOURCE (on by default in
        // Ubuntu's GCC, off in Debian's), and GCC does NOT accept a `(void)` cast as consuming a
        // warn_unused_result value. The byte either goes into a 64 KiB-capacity pipe or the test's
        // own assertions catch the missing wakeup, so the value is genuinely not actionable here --
        // but it must be bound to compile.
        const auto written = ::write(write_fd, &c, 1);
        (void) written;
        wrote_ptr->store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return wrote.load(); })) << "wait_writable never resumed on a writable fd";
    EXPECT_EQ(coro_scheduler().active_count(), 0u);

    ::close(read_fd);
    ::close(write_fd);
}

/**
 * @test A coroutine parked in socket_awaiter is torn down cleanly when the scheduler is reset
 * @brief Exercises socket_awaiter's destructor scrub path (unschedule + qev_io_stop on a still-armed
 *        watcher): the coroutine parks on a never-readable fd, then destroy_all_suspended() unwinds it.
 *        The watcher is stopped by the awaiter destructor — the body after the await never runs and no
 *        frame leaks.
 */
TEST_F(CoroutineAwaiterTests, SocketAwaiterDestructorStopsArmedWatcher) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0) << "pipe() failed";
    const int read_fd  = fds[0];
    const int write_fd = fds[1];

    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> resumed{false};

    auto resumed_ptr = &resumed;
    coro_scheduler().spawn([resumed_ptr, read_fd]() -> task<void> {
        co_await wait_readable(read_fd); // never becomes readable in this test
        resumed_ptr->store(true);        // must NOT run
        co_return;
    });

    // Park it on the fd.
    coro_scheduler().run_ready();
    ASSERT_EQ(coro_scheduler().active_count(), 1u);

    // Tear it down while the qev_io watcher is still armed: destructor must stop the watcher and the
    // frame must be reclaimed.
    coro_scheduler().destroy_all_suspended();
    EXPECT_EQ(coro_scheduler().active_count(), 0u) << "parked socket awaiter was not torn down";

    qb::io::async::run_for(10ms);
    EXPECT_FALSE(resumed.load()) << "a destroyed socket awaiter coroutine wrongly resumed";

    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "socket awaiter teardown leaked " << (after - baseline) << " frame(s)";

    ::close(read_fd);
    ::close(write_fd);
}

#endif // !_WIN32

// =============================================================================
// BUILT-IN timer_awaiter teardown (destructor scrub while still armed)
// =============================================================================

/**
 * @test A coroutine parked on sleep() is torn down cleanly, stopping the armed qev_timer
 * @brief Exercises timer_awaiter's destructor (unschedule + qev_timer_stop on a still-active watcher,
 *        awaiter.h:361-370): the coroutine parks on a long sleep, then destroy_all_suspended() unwinds it
 *        before the timer fires. The body after the sleep never runs and the frame is reclaimed.
 */
TEST_F(CoroutineAwaiterTests, TimerAwaiterDestructorStopsArmedTimer) {
    const long        baseline = detail::CoroutineFrameAllocator::live_frames;
    std::atomic<bool> resumed{false};

    auto resumed_ptr = &resumed;
    coro_scheduler().spawn([resumed_ptr]() -> task<void> {
        co_await sleep(3000ms); // long timer we will never let fire
        resumed_ptr->store(true);
        co_return;
    });

    // Park on the timer.
    coro_scheduler().run_ready();
    ASSERT_EQ(coro_scheduler().active_count(), 1u);

    // Destroy while the qev_timer is armed: destructor stops the watcher.
    coro_scheduler().destroy_all_suspended();
    EXPECT_EQ(coro_scheduler().active_count(), 0u) << "parked timer awaiter was not torn down";

    qb::io::async::run_for(10ms);
    EXPECT_FALSE(resumed.load()) << "a destroyed timer awaiter coroutine wrongly resumed";

    const long after = detail::CoroutineFrameAllocator::live_frames;
    EXPECT_EQ(after, baseline) << "timer awaiter teardown leaked " << (after - baseline) << " frame(s)";
}

// =============================================================================
// async_awaiter<T> — the callback → co_await bridge
// =============================================================================

/**
 * @test The documented async_awaiter<T> bridge compiles and delivers its result
 * @brief Regression: awaiter_base::await_resume() used to be `virtual void`, which made
 *        async_awaiter<T>'s `T await_resume()` an ill-formed override — every non-void
 *        instantiation was a HARD COMPILE ERROR ("virtual function 'await_resume' has a
 *        different return type"). The type is public, documented API (llm/qb.llm.api.md,
 *        qb/readme/3_qb_io/coroutines.md) but was instantiated nowhere in the tree, so
 *        nothing caught it. This test is the instantiation that keeps it buildable —
 *        for a trivially-copyable result AND a non-trivial one — and pins the contract
 *        that the value handed to the callback is what `co_await` yields.
 */
TEST_F(CoroutineAwaiterTests, AsyncAwaiterBridgesACallbackResult) {
    std::atomic<int>         got_int{0};
    std::atomic<bool>        done{false};
    std::string              got_string;
    std::function<void(int)> deferred_cb; // proves the deferred (not-inline) completion path

    auto *got_int_ptr     = &got_int;
    auto *done_ptr        = &done;
    auto *got_string_ptr  = &got_string;
    auto *deferred_cb_ptr = &deferred_cb;

    coro_scheduler().spawn([got_int_ptr, done_ptr, got_string_ptr, deferred_cb_ptr]() -> task<void> {
        // (a) callback invoked synchronously from inside await_suspend
        got_string_ptr->assign(co_await async_awaiter<std::string>([](auto cb) { cb(std::string("hello")); }));
        // (b) callback stashed and invoked later, from the loop
        got_int_ptr->store(co_await async_awaiter<int>([deferred_cb_ptr](auto cb) { *deferred_cb_ptr = std::move(cb); }));
        done_ptr->store(true);
        co_return;
    });

    // Drive (a); the coroutine then parks inside (b) waiting on the stashed callback.
    EXPECT_TRUE(pump_until([&] { return static_cast<bool>(deferred_cb); })) << "async_awaiter never reached the deferred stage";
    EXPECT_EQ(got_string, "hello") << "synchronously-completed async_awaiter lost its result";
    EXPECT_FALSE(done.load()) << "the coroutine resumed before its deferred callback fired";

    deferred_cb(42); // complete the deferred operation
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "deferred async_awaiter never resumed";
    EXPECT_EQ(got_int.load(), 42) << "deferred async_awaiter delivered the wrong result";
}
