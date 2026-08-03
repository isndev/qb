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
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
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

// A with_timeout subject whose real expiry handler counts how many times it has
// actually fired. We push its deadline out once (via updateTimeout) after the
// timer has already been armed, forcing the internal `on(event::timer&)` to take
// its RESCHEDULE arm (io.h: after > 0 -> re-arm, don't dispatch) before the real
// fire eventually lands.
class ReschedulableTimer : public async::with_timeout<ReschedulableTimer> {
public:
    std::atomic<int> fires{0};

    explicit ReschedulableTimer(qb::duration timeout)
        : with_timeout(timeout) {}

    void
    on(async::event::timer const &) {
        fires.fetch_add(1);
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
// A fire-and-forget async::callback may tear down its own loop from inside its
// body. clear() must not destroy the Timeout that is currently mid-invoke — the
// Timeout reclaims itself via `delete this` when its on() returns; destroying it
// in clear() too is a double-free (regression: ASan SEGV in invoke()).
// =============================================================================
TEST_F(EventLoopLifecycleTest, ClearFromInsideFiringCallbackIsSafe) {
    std::atomic<bool> fired{false};
    async::callback(
        [&fired] {
            fired.store(true);
            async::listener::current.clear(); // destroys the firing Timeout — must not double-free
        },
        1ms);
    // pump_until drives the loop the production way (run_for) until the timer actually fires —
    // a raw EVRUN_NOWAIT spin would not wait for the 1 ms delay and could finish first. The
    // double-free regression would crash here (ASan SEGV in invoke()) rather than fail an assert.
    EXPECT_TRUE(pump_until([&] { return fired.load(); })) << "callback never fired";
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

    // run_once() (EVRUN_ONCE) blocks for one event block and the one-shot timer is
    // the only armed watcher, so a single call normally dispatches it. But
    // EVRUN_ONCE can return on a spurious backend wakeup before the system-timer-
    // quantised 1ms timer actually expires — far more likely under heavy CPU load
    // (e.g. a full parallel test sweep). Pump until the timer is genuinely
    // dispatched so the test asserts the real contract ("an armed timer IS
    // dispatched and counted") instead of racing OS timer granularity. The bound
    // keeps a true never-dispatch failure loud (fired stays false → the assert fires).
    std::size_t invoked = 0;
    for (int i = 0; i < 1000 && !fired.load(); ++i)
        invoked += async::run_once();
    EXPECT_TRUE(fired.load()) << "run_once() did not dispatch the armed timer";
    EXPECT_GE(invoked, 1u) << "run_once() reported zero invoked events";
}

// async::run_until(flag) must pump EVRUN_NOWAIT passes until the flag flips.
TEST_F(EventLoopLifecycleTest, RunUntilPumpsUntilFlagFlips) {
    bool             keep_going = true;
    std::atomic<int> ticks{0};

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
// The dispatch locus contains an exception thrown by a watcher handler
// =============================================================================

// Regression: listener::on() invoked the handler unguarded, so a throwing user
// handler -- `on(event::disconnected&&)`, `on(event::pending_read&&)`, an ev::stat
// observer, any of them reachable from application code that merely allocates --
// unwound straight through qev's `qev_invoke_pending`/`qev_run`. libev is built as
// C (qb/include/qb/vendor/qev, LANGUAGES C), so that is UB: it skips libev's epilogue
// (`--loop_depth`, the `loop_done` reset that re-arms a broken loop) and has no
// unwind info at all on MSVC. It also stranded `listener::_dispatch_top` on a
// destroyed stack frame, corrupting the re-entrancy guard clear() reads. The loop
// must contain it instead, exactly as Timeout::on and the defer drain already do.
TEST_F(EventLoopLifecycleTest, HandlerExceptionIsContainedAtTheDispatchLocus) {
    struct Thrower {
        bool entered = false;
        void
        on(async::event::timer &) {
            entered = true;
            throw std::runtime_error("handler boom");
        }
    } thrower;

    auto &watcher = async::listener::current.registerEvent<async::event::timer>(thrower);
    // start(0.) — an ALREADY-EXPIRED one-shot, which libev dispatches on the next loop iteration.
    // A 1 ms timer pumped with EVRUN_NOWAIT (which does NOT block) was a timing trap: 2000 non-
    // blocking iterations complete in well under a millisecond on a fast host, so the timer never
    // expired and the handler never ran. It only passed because earlier tests in the same binary
    // burned enough wall time first — run this case in isolation and it failed deterministically.
    watcher.start(0.);

    // A pending loop-owned callback so the post-throw clear() below exercises the
    // `_destroy_owner` branch (the one that consults `_dispatch_top`).
    bool ran = false;
    async::callback([&ran]() { ran = true; }, 30s);

    // The throw must NOT escape the event loop. Bound the pump by wall clock as well as by
    // iterations so a regression that stops dispatching cannot hang the suite.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    EXPECT_NO_THROW({
        while (!thrower.entered && std::chrono::steady_clock::now() < deadline)
            async::run(EVRUN_NOWAIT);
    });
    EXPECT_TRUE(thrower.entered) << "the throwing handler never ran";

    // The loop is still healthy and the registry is still coherent afterwards.
    EXPECT_NO_THROW(async::run(EVRUN_NOWAIT));
    EXPECT_NO_THROW(async::listener::current.clear());
    EXPECT_EQ(async::listener::current.size(), 0u) << "clear() must still reclaim every watcher after a contained throw";
    EXPECT_FALSE(ran);

    // `Thrower` is a bare stack object, not an `async::base`, so nothing unregisters on its
    // behalf. That matters because `clear()` deliberately only DETACHES a user-owned watcher —
    // deleting it there would dangle the `_async_event` reference an owning `async::base` still
    // holds — and leaves reclamation to the owner's destructor. This test owns the registration,
    // so this test must release it. Caught by LSan on Linux (macOS ASan has no leak detector, so
    // the suite was green there); the wrapper, not the product, was at fault.
    async::listener::current.unregisterEvent(watcher._interface);
}

// =============================================================================
// clear(): a destroyed owner may unregister ANOTHER still-linked watcher
// =============================================================================

// Regression (use-after-free): clear() used to cache `cur->_list_next` and then
// run `destroy(owner)`, which executes arbitrary user code -- the async::callback
// closure's captured state. Here that state is the last shared_ptr to a live
// FlagTimer, so its ~with_timeout re-enters unregisterEvent() on the very node the
// cached `next` pointed at and frees it; the loop then called `stop()` on freed
// memory (SEGV: the RegisteredKernelEvent freelist had already overwritten the
// vptr). It also drove _registered_count (zeroed up-front) below zero to SIZE_MAX.
//
// Registration order is load-bearing: the registry is a prepend list, so the
// FlagTimer (built first) must sit immediately AFTER the Timeout (built last) for
// the cached-next to be the node the destructor frees.
TEST_F(EventLoopLifecycleTest, ClearSurvivesOwnerDestructorUnregisteringAnotherWatcher) {
    const auto baseline = async::listener::current.size();

    {
        auto victim = std::make_shared<FlagTimer>(30s); // registers the watcher that follows
        async::callback([victim]() { FAIL() << "callback must not fire"; }, 30s);
        EXPECT_EQ(async::listener::current.size(), baseline + 2) << "expected one watcher for the timer and one for the pending callback";
    } // the closure now holds the ONLY reference to `victim`

    // clear() destroys the loop-owned Timeout -> ~closure -> ~FlagTimer ->
    // unregisterEvent(next node). Must not touch freed memory, and must leave an
    // exact (non-underflowed) count.
    EXPECT_NO_THROW(async::listener::current.clear());
    EXPECT_EQ(async::listener::current.size(), 0u) << "clear() must reclaim both watchers and keep the count exact";

    // The loop is still healthy afterwards.
    EXPECT_NO_THROW(async::run(EVRUN_NOWAIT));
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
            EXPECT_EQ(async::listener::current.size(), baseline + 2) << "two live timers should register two watchers (round " << round << ")";
        } // both destructors run here: unregister + free back to the pool
        EXPECT_EQ(async::listener::current.size(), baseline)
            << "destroying both timers should restore the registry size (round " << round << ")";
    }

    // The loop is still healthy after the churn.
    EXPECT_NO_THROW(async::run(EVRUN_NOWAIT));
}

// =============================================================================
// with_timeout::on reschedule arm: a mid-flight updateTimeout() defers the fire
// =============================================================================

// When the libev timer expires but `_last_activity` was refreshed more recently
// than `_timeout` ago, with_timeout::on must NOT dispatch the derived handler;
// it re-arms the watcher for the remaining slice (io.h `after > 0` branch). We
// drive that by bumping the deadline once after the first arm, then proving the
// real handler still fires exactly once after the extended deadline.
TEST_F(EventLoopLifecycleTest, WithTimeoutReschedulesWhenActivityIsRecent) {
    ReschedulableTimer timer(40ms);

    // Let ~half the original window elapse, then record fresh activity. The next
    // libev expiry (at the original 40ms mark) now sees `_last_activity` only
    // ~20ms old against a 40ms timeout => after ~= 20ms > 0 => RESCHEDULE.
    // A never-true predicate pumps the loop for the full budget; the false return
    // is expected and must be consumed (the helper is [[nodiscard]]).
    const bool half_elapsed = pump_until([&] { return false; }, 20ms, 5ms);
    EXPECT_FALSE(half_elapsed) << "spacer pump unexpectedly satisfied";
    timer.updateTimeout();

    // The derived handler must still be pending right after we pushed it out.
    EXPECT_EQ(timer.fires.load(), 0) << "handler dispatched before the extended deadline";

    // It must eventually fire exactly once, after the re-armed slice elapses.
    EXPECT_TRUE(pump_until([&] { return timer.fires.load() >= 1; })) << "rescheduled with_timeout handler never fired";
    EXPECT_EQ(timer.fires.load(), 1) << "rescheduled handler fired more than once";
}

// =============================================================================
// Timeout<F> immediate-fire ctor branch (timeout <= 0): runs func now, self-deletes
// =============================================================================

// `async::callback(f, 0)` short-circuits in the free function before ever building
// a Timeout, so the Timeout ctor's `timeout <= 0` arm (run func inline, set
// _delete_only, start(0.)) is only reachable by constructing Timeout directly.
// The object is loop-owned and self-deletes on its next (immediate) expiry, with
// _delete_only suppressing a second func() call.
TEST_F(EventLoopLifecycleTest, TimeoutZeroDurationFiresInlineThenSelfDeletes) {
    const auto baseline = async::listener::current.size();

    std::atomic<int> calls{0};
    auto             f = [&calls]() {
        calls.fetch_add(1);
    };

    // Direct construction with a zero duration: func() runs once right here, then
    // a 0s one-shot is armed so the object reclaims itself on the next loop pass.
    new async::Timeout<decltype(f)>(std::move(f), qb::duration::zero());
    EXPECT_EQ(calls.load(), 1) << "zero-duration Timeout must invoke func inline at construction";
    EXPECT_EQ(async::listener::current.size(), baseline + 1) << "the pending self-delete watcher should be registered until it fires";

    // Pump once: the armed 0s timer expires, on() sees _delete_only and deletes
    // this WITHOUT re-invoking func.
    EXPECT_TRUE(pump_until([&] { return async::listener::current.size() == baseline; })) << "zero-duration Timeout never reclaimed itself";
    EXPECT_EQ(calls.load(), 1) << "_delete_only must suppress a second func() call on the self-delete pass";
}

// =============================================================================
// Timeout<F>::on swallows an exception thrown by the user callback
// =============================================================================

// A normal (positive-duration) Timeout whose func throws when the timer fires
// must not propagate: Timeout::on wraps func() in try/catch and still deletes
// itself, leaving the loop usable. Exercises the catch arm + the self-delete.
TEST_F(EventLoopLifecycleTest, TimeoutOnSwallowsThrowingCallback) {
    const auto baseline = async::listener::current.size();

    std::atomic<bool> entered{false};
    async::callback(
        [&entered]() {
            entered.store(true);
            throw std::runtime_error("callback boom");
        },
        1ms);
    EXPECT_EQ(async::listener::current.size(), baseline + 1);

    // Pumping must not let the exception escape the loop, and the Timeout must
    // still self-delete (registry returns to baseline).
    EXPECT_NO_THROW({
        EXPECT_TRUE(pump_until([&] { return entered.load() && async::listener::current.size() == baseline; }))
            << "throwing callback never fired or never reclaimed its watcher";
    });
    EXPECT_TRUE(entered.load());
    EXPECT_EQ(async::listener::current.size(), baseline);

    // The loop is still healthy after swallowing the throw.
    EXPECT_NO_THROW(async::run(EVRUN_NOWAIT));
}

// =============================================================================
// listener::_resolve_backend_flags(): QB_EV_BACKEND env-var resolution
// =============================================================================

namespace {

// Portable setenv/unsetenv — the POSIX names are not declared by MSVC's <stdlib.h>.
inline void
set_env(const char *name, const char *value) {
#if defined(_WIN32)
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}
inline void
unset_env(const char *name) {
#if defined(_WIN32)
    ::_putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

// Save/restore QB_EV_BACKEND around a construction so we never perturb sibling
// tests or parallel ctest workers (the var is read once per listener ctor).
struct ScopedEnv {
    std::string name;
    bool        had_old;
    std::string old_value;

    explicit ScopedEnv(const char *n, const char *value)
        : name(n) {
        const char *cur = std::getenv(n);
        had_old         = cur != nullptr;
        if (had_old)
            old_value = cur;
        if (value)
            set_env(n, value);
        else
            unset_env(n);
    }
    ~ScopedEnv() {
        if (had_old)
            set_env(name.c_str(), old_value.c_str());
        else
            unset_env(name.c_str());
    }
};

} // namespace

// A known, built-in, runtime-available backend must drive _resolve_backend_flags
// through: table match -> not AUTO -> supported -> probe loop_new succeeds -> return
// the requested flag. "select" is built into libev on POSIX, but the qb ev fork
// compiles it OUT on Windows (no <sys/select.h> → EV_USE_SELECT 0), where wepoll/epoll
// drives the loop. The contract is the same on both: a KNOWN+SUPPORTED backend is
// honoured; a KNOWN-but-unavailable one degrades to a valid auto backend (never
// "unknown", never a throw). Assert against what the platform actually supports
// (qev_supported_backends) so this verifies the contract rather than assuming select
// is universal. We build a throwaway listener on a fresh thread (so listener::current
// is untouched) and assert the resolved backend.
TEST_F(EventLoopLifecycleTest, ResolveBackendFlagsHonoursKnownSupportedBackend) {
    const bool select_supported = (qev_supported_backends() & EVBACKEND_SELECT) != 0;

    std::atomic<unsigned int> chosen{0};
    std::thread               t([&chosen] {
        ScopedEnv       env("QB_EV_BACKEND", "select");
        async::listener probe; // ctor calls _resolve_backend_flags("select")
        chosen.store(probe.backend());
    });
    t.join();

    if (select_supported) {
        EXPECT_EQ(chosen.load(), static_cast<unsigned int>(EVBACKEND_SELECT))
            << "QB_EV_BACKEND=select must pin the libev select backend where it is built in";
    } else {
        // select compiled out (Windows): the contract degrades to a valid auto backend.
        EXPECT_NE(chosen.load(), 0u);
        EXPECT_STRNE(async::listener::backend_name(chosen.load()), "unknown")
            << "QB_EV_BACKEND=select with select unavailable must resolve to a valid auto backend";
    }
}

// An unrecognised name must degrade to EVFLAG_AUTO (the `!known` arm) without
// throwing, yielding a real, non-"unknown" auto-selected backend.
TEST_F(EventLoopLifecycleTest, ResolveBackendFlagsUnknownNameFallsBackToAuto) {
    std::atomic<unsigned int> chosen{0};
    std::thread               t([&chosen] {
        ScopedEnv       env("QB_EV_BACKEND", "no_such_backend_xyz");
        async::listener probe;
        chosen.store(probe.backend());
    });
    t.join();

    EXPECT_STRNE(async::listener::backend_name(chosen.load()), "unknown")
        << "an unknown QB_EV_BACKEND must still resolve to a valid auto backend";
}

// The explicit "auto" token must take the early `req == EVFLAG_AUTO` return arm.
TEST_F(EventLoopLifecycleTest, ResolveBackendFlagsAutoTokenSelectsAuto) {
    std::atomic<unsigned int> chosen{0};
    std::thread               t([&chosen] {
        ScopedEnv       env("QB_EV_BACKEND", "auto");
        async::listener probe;
        chosen.store(probe.backend());
    });
    t.join();

    EXPECT_STRNE(async::listener::backend_name(chosen.load()), "unknown") << "QB_EV_BACKEND=auto must resolve to a valid auto backend";
}
