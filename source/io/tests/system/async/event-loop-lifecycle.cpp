/**
 * @file system/async/event-loop-lifecycle.cpp
 * @brief Per-thread event-loop lifecycle: init / re-init / run / clear of `qb::io::async::listener`.
 *
 * These cases pin the lifecycle of the thread-local qb-io event loop (`qb::io::async::init()`,
 * `async::run()`, and `async::listener::current.clear()` from qb/io/async/listener.h). They drive a real
 * (socket-free) libev loop, so they are SYSTEM tests. Contracts proven:
 *
 *   - a freshly `init()`-ed loop runs without throwing in both NOWAIT and ONCE modes, and re-`init()`
 *     is idempotent;
 *   - re-initialising the loop and arming a fresh timer afterwards still dispatches it (the loop is
 *     usable across a re-init);
 *   - `listener::current.clear()` detaches every live async object safely and drives the registered
 *     watcher count to zero, and is idempotent (clearing an already-empty listener is a no-op);
 *   - several worker threads can each `init()` and `run()` their own independent loop concurrently.
 *
 * Restructured from the dissolved system/test-async-io.cpp (EventLoopAlive, EventLoopReinitialization,
 * AsyncInitCleanupThreads, ClearDetachesLiveAsyncObjectsSafely). The reinit case's hand-rolled poll is
 * replaced by `qb::io::test::pump_until`; the watcher-count assertions use `listener::current.size()`
 * directly. No file-local main().
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
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace {

class EventLoopLifecycleTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        async::listener::current.clear();
    }
};

// A trivial with_timeout subject used to register / observe a live watcher.
class FlagTimer : public async::with_timeout<FlagTimer> {
public:
    std::atomic<bool> triggered{false};

    explicit FlagTimer(qb::duration timeout)
        : with_timeout(timeout) {}

    void
    on(async::event::timer const &) {
        triggered.store(true);
    }
};

} // namespace

// =============================================================================
// A fresh loop runs without throwing; re-init is idempotent
// =============================================================================

TEST_F(EventLoopLifecycleTest, FreshLoopRunsAndReInitIsSafe) {
    EXPECT_NO_THROW({
        async::run(EVRUN_NOWAIT);
        async::run(EVRUN_ONCE);
    });
    EXPECT_NO_THROW(async::init());
    EXPECT_NO_THROW(async::run(EVRUN_NOWAIT));
}

// =============================================================================
// The loop stays usable across a re-init: a timer armed afterwards fires
// =============================================================================

TEST_F(EventLoopLifecycleTest, TimerArmedAfterReInitStillFires) {
    // Arm + run a first timer, then re-init and arm a second.
    std::atomic<bool> first{false};
    async::callback([&first]() { first.store(true); }, 30ms);
    async::run(EVRUN_NOWAIT);

    async::init(); // re-initialise the loop

    std::atomic<bool> second{false};
    async::callback([&second]() { second.store(true); }, 30ms);

    EXPECT_TRUE(pump_until([&] { return second.load(); })) << "a timer armed after re-init never fired";
}

// =============================================================================
// clear() detaches live watchers and drives the count to zero (idempotent)
// =============================================================================

TEST_F(EventLoopLifecycleTest, ClearDetachesLiveWatchersAndIsIdempotent) {
    const auto baseline = async::listener::current.size();

    {
        FlagTimer timer(1s);
        EXPECT_EQ(async::listener::current.size(), baseline + 1) << "constructing a timer should register one watcher";

        async::listener::current.clear();
        EXPECT_EQ(async::listener::current.size(), 0u) << "clear() must detach every live watcher";

        // Idempotent: clearing an already-empty listener is a no-op.
        EXPECT_NO_THROW(async::listener::current.clear());
        EXPECT_EQ(async::listener::current.size(), 0u);
    }

    // The timer's destructor running against an already-cleared listener is safe.
    EXPECT_EQ(async::listener::current.size(), 0u);
}

// =============================================================================
// Several threads each init + run their own independent loop
// =============================================================================

TEST_F(EventLoopLifecycleTest, ConcurrentPerThreadInitAndRun) {
    constexpr int kThreads = 4;

    std::atomic<int>         init_ok{0};
    std::atomic<int>         run_ok{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&init_ok, &run_ok] {
            async::init();
            init_ok.fetch_add(1);
            async::run(EVRUN_NOWAIT);
            run_ok.fetch_add(1);
            // Loop is reclaimed when the thread terminates.
        });
    }

    for (auto &th : threads)
        th.join();

    EXPECT_EQ(init_ok.load(), kThreads);
    EXPECT_EQ(run_ok.load(), kThreads);
}

// =============================================================================
// run() overloads: run_once / run_until / break_one / break_parent
// =============================================================================

// async::run_once() must dispatch a single armed timer and report its invocation.
TEST_F(EventLoopLifecycleTest, RunOnceDispatchesAnArmedTimer) {
    std::atomic<bool> fired{false};
    async::callback([&fired]() { fired.store(true); }, 1ms);

    // run_once() waits for at least one event block; the one-shot timer is the
    // only armed watcher, so it must fire and be reported as invoked.
    const std::size_t invoked = async::run_once();
    EXPECT_TRUE(fired.load()) << "run_once() did not dispatch the armed timer";
    EXPECT_GE(invoked, 1u) << "run_once() reported zero invoked events";
}

// async::run_until(flag) must pump EVRUN_NOWAIT passes until the flag flips.
TEST_F(EventLoopLifecycleTest, RunUntilPumpsUntilFlagFlips) {
    bool              keep_going = true;
    std::atomic<int>  ticks{0};

    // A timer that flips the loop-control flag once it has fired a few times.
    // Re-arm by scheduling a fresh callback each tick so run_until keeps spinning.
    std::function<void()> tick = [&]() {
        if (ticks.fetch_add(1) + 1 >= 3) {
            keep_going = false;
            return;
        }
        async::callback(tick, 1ms);
    };
    async::callback(tick, 1ms);

    const std::size_t invoked = async::run_until(keep_going);
    EXPECT_FALSE(keep_going) << "run_until() returned while its flag was still true";
    EXPECT_GE(ticks.load(), 3) << "the re-arming timer chain did not run to completion";
    EXPECT_GE(invoked, 3u) << "run_until() under-counted invoked events";
}

// break_one()/break_parent() must make a default (blocking) run() return.
TEST_F(EventLoopLifecycleTest, BreakOneMakesBlockingRunReturn) {
    // Arm a timer whose handler breaks the loop from inside the run() cycle.
    std::atomic<bool> fired{false};
    async::callback(
        [&fired]() {
            fired.store(true);
            async::break_parent(); // request the current run() to break
        },
        1ms);

    // A default blocking run() would otherwise wait on the timer; the handler's
    // break_parent() must let it return promptly after the timer fires.
    EXPECT_NO_THROW(async::listener::current.run(0));
    EXPECT_TRUE(fired.load()) << "the timer that calls break_parent() never fired";
}

// break_one() on an idle listener is a harmless no-op (no active run cycle).
TEST_F(EventLoopLifecycleTest, BreakOneOnIdleListenerIsHarmless) {
    EXPECT_NO_THROW(async::listener::current.break_one());
    EXPECT_NO_THROW(async::break_parent());
    // The loop is still usable afterwards.
    EXPECT_NO_THROW(async::run(EVRUN_NOWAIT));
}

// =============================================================================
// Backend introspection: backend() / backend_name()
// =============================================================================

TEST_F(EventLoopLifecycleTest, BackendReportsAKnownName) {
    const unsigned int be = async::listener::current.backend();
    // The auto-selected backend must be a real, non-"unknown" one.
    const char *name = async::listener::backend_name(be);
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "unknown") << "live listener reported an unknown backend (raw=" << be << ")";
}

TEST_F(EventLoopLifecycleTest, BackendNameMapsEveryKnownBackend) {
    // Exhaustively drive the switch in backend_name() over every named case.
    EXPECT_STREQ(async::listener::backend_name(EVBACKEND_SELECT), "select");
    EXPECT_STREQ(async::listener::backend_name(EVBACKEND_POLL), "poll");
    EXPECT_STREQ(async::listener::backend_name(EVBACKEND_EPOLL), "epoll");
    EXPECT_STREQ(async::listener::backend_name(EVBACKEND_KQUEUE), "kqueue");
    EXPECT_STREQ(async::listener::backend_name(EVBACKEND_PORT), "port");
    EXPECT_STREQ(async::listener::backend_name(EVBACKEND_LINUXAIO), "linuxaio");
    EXPECT_STREQ(async::listener::backend_name(EVBACKEND_IOURING), "iouring");
    // An unmapped flag value hits the default arm.
    EXPECT_STREQ(async::listener::backend_name(0u), "unknown");
}

// =============================================================================
// Event accounting: total_events_processed accumulates across runs
// =============================================================================

TEST_F(EventLoopLifecycleTest, TotalEventsProcessedAccumulates) {
    const std::size_t before = async::listener::current.total_events_processed();

    std::atomic<bool> a{false};
    async::callback([&a]() { a.store(true); }, 1ms);
    EXPECT_TRUE(pump_until([&] { return a.load(); }));

    const std::size_t mid = async::listener::current.total_events_processed();
    EXPECT_GT(mid, before) << "processing a timer must advance the lifetime counter";

    std::atomic<bool> b{false};
    async::callback([&b]() { b.store(true); }, 1ms);
    EXPECT_TRUE(pump_until([&] { return b.load(); }));

    // Lifetime counter is monotonic and never reset between runs.
    EXPECT_GT(async::listener::current.total_events_processed(), mid);
}

// =============================================================================
// unregisterEvent(nullptr) is a guarded no-op
// =============================================================================

TEST_F(EventLoopLifecycleTest, UnregisterNullEventIsNoOp) {
    const auto baseline = async::listener::current.size();
    EXPECT_NO_THROW(async::listener::current.unregisterEvent(nullptr));
    EXPECT_EQ(async::listener::current.size(), baseline) << "unregistering nullptr must not touch the registry";
}

// =============================================================================
// clear() destroys a loop-owned, never-fired async::callback (owner-destroy branch)
// =============================================================================

TEST_F(EventLoopLifecycleTest, ClearDestroysPendingLoopOwnedCallback) {
    const auto baseline = async::listener::current.size();

    bool ran = false;
    // A long-timeout callback registers a self-deleting Timeout owner that the
    // loop owns; it will NOT fire within the test window.
    async::callback([&ran]() { ran = true; }, 10s);
    EXPECT_EQ(async::listener::current.size(), baseline + 1) << "a pending callback should register exactly one watcher";

    // clear() must take the _destroy_owner branch: it destroys the loop-owned
    // Timeout (which re-enters unregisterEvent via the _detached_by_clear path)
    // and drives the registry to zero -- without ever invoking the user callback.
    async::listener::current.clear();
    EXPECT_EQ(async::listener::current.size(), 0u) << "clear() must reclaim the pending loop-owned callback";
    EXPECT_FALSE(ran) << "clear() must not invoke a callback whose timer never fired";

    // Pumping afterwards must not resurrect the destroyed callback.
    async::run(EVRUN_NOWAIT);
    EXPECT_FALSE(ran);
    EXPECT_EQ(async::listener::current.size(), 0u);
}

// =============================================================================
// Watcher allocator: register/unregister churn reuses the thread-local freelist
// =============================================================================

TEST_F(EventLoopLifecycleTest, RegisterUnregisterChurnIsStableAndSafe) {
    const auto baseline = async::listener::current.size();

    // Repeatedly construct and destroy timers. Each construction allocates a
    // RegisteredKernelEvent (or reuses a freelist block from a prior destroy);
    // each destruction frees it back to the pool. Drives the operator new
    // freelist-hit path and the operator delete pool-push path.
    for (int round = 0; round < 8; ++round) {
        {
            FlagTimer t1(1s);
            FlagTimer t2(1s);
            EXPECT_EQ(async::listener::current.size(), baseline + 2)
                << "two live timers should register two watchers (round " << round << ")";
        } // both destructors run here: unregister + free back to the pool
        EXPECT_EQ(async::listener::current.size(), baseline)
            << "destroying both timers should restore the registry size (round " << round << ")";
    }

    // The loop is still healthy after the churn.
    EXPECT_NO_THROW(async::run(EVRUN_NOWAIT));
}
