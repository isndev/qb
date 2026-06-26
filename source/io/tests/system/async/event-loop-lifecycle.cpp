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
