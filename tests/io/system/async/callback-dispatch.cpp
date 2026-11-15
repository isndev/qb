/**
 * @file system/async/callback-dispatch.cpp
 * @brief `qb::io::async::callback()` — immediate vs deferred dispatch on the event loop.
 *
 * `async::callback(func[, timeout])` (qb/io/async/io.h) is the everyday way to defer a callable onto
 * the qb-io loop. It has exactly two behaviours, both proven here against a real (socket-free) libev
 * loop, so this is a SYSTEM test (it needs `qb::io::async::init()` and pumps the loop, but opens no
 * descriptor):
 *
 *   - a non-positive timeout (`<= 0`) runs the callable SYNCHRONOUSLY, in-call, before `callback()`
 *     returns — no loop iteration is required;
 *   - a positive timeout schedules a self-deleting `Timeout<F>` that fires exactly once when the loop
 *     reaches the deadline, and not before — owning its own copy of a named callable (Huly QB-972).
 *
 * `async::defer(func)` is pinned beside them: the next turn, FIFO, one generation per call, served from
 * inside a blocking run -- and a loop pumped from inside a deferred callback (`run_sync()`, `run_until()`,
 * a nested `run()`) drains the other deferred callbacks, in queue order, never the ones executing on the
 * stack (Huly QB-1246). Those cases connect over an empty address list, which fails without a descriptor.
 *
 * Restructured from the dissolved system/test-async-io.cpp (CallbackImmediateExecution,
 * CallbackScheduledExecution). The hand-rolled `for(i){run(EVRUN_ONCE);sleep_for()}` poll is replaced
 * by the shared deadline-bounded `qb::io::test::pump_until`, the deferred case additionally asserts the
 * callback does NOT fire before the loop runs (the inline/deferred distinction is the whole contract),
 * and a fires-exactly-once guard is added. No file-local main() (shared gtest_main).
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

namespace {

class CallbackDispatchTest : public ::testing::Test {
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

} // namespace

// =============================================================================
// Immediate dispatch (non-positive timeout) — runs inline, no loop needed
// =============================================================================

TEST_F(CallbackDispatchTest, ZeroTimeoutRunsInline) {
    bool executed = false;
    // Zero timeout: the callable runs before callback() returns — observable
    // without ever calling async::run().
    async::callback([&executed]() { executed = true; }, qb::duration::zero());
    EXPECT_TRUE(executed) << "callback(zero) did not run inline";
}

TEST_F(CallbackDispatchTest, NoArgOverloadRunsInline) {
    bool executed = false;
    // The single-argument overload is unconditionally synchronous.
    async::callback([&executed]() { executed = true; });
    EXPECT_TRUE(executed) << "callback(func) did not run inline";
}

TEST_F(CallbackDispatchTest, NegativeTimeoutRunsInline) {
    bool executed = false;
    async::callback([&executed]() { executed = true; }, -5s);
    EXPECT_TRUE(executed) << "callback(negative) did not run inline";
}

// =============================================================================
// Deferred dispatch (positive timeout) — fires once via the loop, not before
// =============================================================================

TEST_F(CallbackDispatchTest, ScheduledCallbackFiresViaLoopAndNotBefore) {
    std::atomic<int> fired{0};
    async::callback([&fired]() { fired.fetch_add(1); }, 50ms);

    // The deferred callback must NOT have run synchronously inside callback().
    EXPECT_EQ(fired.load(), 0) << "a deferred callback fired before the loop ran";

    EXPECT_TRUE(pump_until([&] { return fired.load() == 1; })) << "deferred callback never fired";

    // And it must fire exactly once — pump further to prove no re-fire.
    EXPECT_FALSE(pump_until([&] { return fired.load() > 1; }, 100ms)) << "deferred callback fired more than once";
    EXPECT_EQ(fired.load(), 1);
}

// A NAMED callable handed to the timed overload is copied into the timer. `callback()` used to
// instantiate `Timeout<L&>` for an lvalue, whose member is a REFERENCE to the caller's object: the
// timer then fired through a dangling reference once the caller's callable was gone (Huly QB-972).
// Here the callable is the only owner of `state` besides the local, so a reference-holding timer is
// seen at once: both owners die with the scope and the weak pointer expires before anything fires.
TEST_F(CallbackDispatchTest, ScheduledCallbackOwnsACopyOfANamedCallable) {
    std::atomic<int>   fired{0};
    std::weak_ptr<int> state_alive;
    {
        auto state  = std::make_shared<int>(42);
        state_alive = state;
        auto named  = [state, &fired]() {
            fired.fetch_add(*state == 42 ? 1 : 100);
        };
        async::callback(named, 20ms); // an lvalue: the timer must take its own copy
    } // `named` and `state` are gone; only the timer's copy can keep `*state` alive
    ASSERT_FALSE(state_alive.expired()) << "the pending timer must own a copy of the callable and its captures";

    EXPECT_TRUE(pump_until([&] { return fired.load() != 0; })) << "the timed callback never fired";
    EXPECT_EQ(fired.load(), 1) << "the timer's copy saw the wrong captured state";
    EXPECT_TRUE(state_alive.expired()) << "the self-deleting timer must release its copy once it has fired";
}

// =============================================================================
// defer() — next loop turn (after the current dispatch unwinds), no timer
// =============================================================================

TEST_F(CallbackDispatchTest, DeferDoesNotRunInline) {
    bool executed = false;
    async::defer([&executed]() { executed = true; });
    // Unlike callback(fn) (inline), defer(fn) does NOT run before the call returns.
    EXPECT_FALSE(executed) << "defer(fn) ran inline — it must wait for the loop turn";
    EXPECT_TRUE(pump_until([&] { return executed; })) << "deferred callback never ran";
}

TEST_F(CallbackDispatchTest, DeferRunsInFifoOrder) {
    std::vector<int> order;
    async::defer([&] { order.push_back(1); });
    async::defer([&] { order.push_back(2); });
    async::defer([&] { order.push_back(3); });
    EXPECT_TRUE(order.empty()) << "no deferred callback may run before the loop turns";
    EXPECT_TRUE(pump_until([&] { return order.size() == 3; }));
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST_F(CallbackDispatchTest, DeferRunsAfterTheDispatchingHandlerUnwinds) {
    // A defer() issued from inside a running handler must execute only AFTER that
    // handler returns — the re-entrancy-break contract. Drive a timer callback
    // (itself a loop dispatch) that records 1, then defers a record-2.
    std::vector<int> order;
    async::callback(
        [&] {
            order.push_back(1);
            async::defer([&] { order.push_back(2); });
            order.push_back(3); // enclosing handler is still executing
        },
        1ms);
    EXPECT_TRUE(pump_until([&] { return order.size() == 3; }));
    EXPECT_EQ(order, (std::vector<int>{1, 3, 2})) << "deferred work must run after the handler that queued it";
}

namespace {

/// A raw watcher that is only ever FED, the way `disconnect()` feeds an io watcher between two passes:
/// libev invokes it on entry to the next `ev_run`, before that call's iteration.
struct FedWatcher {
    ev_timer         w{};
    std::atomic<int> stage{0};
    FedWatcher() {
        ev_timer_init(&w, &FedWatcher::cb, 0., 0.);
        w.data = this;
    }
    // Out of the loop's pending queue on every way out, a failed assertion included: a watcher still pending when
    // its storage goes is read by the next pass, and written by the loop when it is destroyed (Huly QB-1246).
    ~FedWatcher() {
        ev_timer_stop(async::listener::current.loop(), &w);
    }
    FedWatcher(FedWatcher const &)            = delete;
    FedWatcher &operator=(FedWatcher const &) = delete;
    static void
    cb(struct ev_loop *, ev_timer *w, int) {
        auto *self = static_cast<FedWatcher *>(w->data);
        async::defer([self] {
            self->stage.store(1);
            async::defer([self] { self->stage.store(2); });
        });
    }
};

} // namespace

TEST_F(CallbackDispatchTest, DeferFromWithinDeferWaitsForTheNextTurn) {
    // One public `run()` call is one turn, and a defer queued by a deferred callback lands on the
    // NEXT one. Drive the PUBLIC `listener::run(EVRUN_NOWAIT)`, from the two places a watcher's
    // handler runs inside it: the loop's iteration (a timer), and the loop's ENTRY (a fed watcher,
    // invoked before the iteration). The nested defer ran in the call that ran its parent both ways
    // -- after the loop returned, in `run()`'s own drain, and in the iteration after an entry drain,
    // through the in-loop drain's zero-delay re-arm (Huly QB-448). This test drove the raw loop until
    // then, which skips `run()`'s drain, so it could not see either.
    {
        std::atomic<int> stage{0};
        async::callback(
            [&] {
                async::defer([&] {
                    stage.store(1);
                    async::defer([&] { stage.store(2); });
                });
            },
            1ms);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (stage.load() == 0 && std::chrono::steady_clock::now() < deadline)
            async::listener::current.run(EVRUN_NOWAIT);
        ASSERT_EQ(stage.load(), 1) << "the run() call that ran the deferred callback also ran the defer it queued: one public call "
                                      "advanced two generations";
        while (stage.load() != 2 && std::chrono::steady_clock::now() < deadline)
            async::listener::current.run(EVRUN_NOWAIT);
        EXPECT_EQ(stage.load(), 2) << "the nested defer never ran on a later turn";
    }
    {
        FedWatcher fed;
        auto      *loop = static_cast<struct ev_loop *>(async::listener::current.loop());
        ev_feed_event(loop, &fed.w, EV_CUSTOM);
        async::listener::current.run(EVRUN_NOWAIT);
        ASSERT_EQ(fed.stage.load(), 1) << "a handler run on entry to the loop: the run() call that ran its deferred callback also ran "
                                          "the defer that callback queued";
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (fed.stage.load() != 2 && std::chrono::steady_clock::now() < deadline)
            async::listener::current.run(EVRUN_NOWAIT);
        EXPECT_EQ(fed.stage.load(), 2) << "the nested defer never ran on a later turn";
    }
}

// Regression: the deferred queue used to be drained ONLY after `_loop.run()` returned.
// A blocking `async::run()` -- the documented standalone main loop ("run forever") --
// never returns while any watcher is active, so a defer() posted from a handler was
// starved indefinitely. That is reachable from framework code on a public API path:
// tcp::connector::deliver_failure_deferred() (a synchronously-failing connect) and the
// qbm-http/1.1 reconnect both defer. The queue must now also be serviced from INSIDE the
// loop, at the tail of the current turn.
//
// Observable contract: with a long-lived watcher keeping the loop parked, the deferred
// callback must run BEFORE a later real timer -- not after run() finally returns. The
// watchdog timer always breaks the loop, so this test fails loudly instead of hanging.
TEST_F(CallbackDispatchTest, DeferIsDrainedFromInsideABlockingRun) {
    std::vector<int> order; // 1 = deferred callback, 2 = watchdog timer

    async::callback([] {}, 1h); // a never-firing watcher keeps ev_run parked
    async::callback(
        [&order] {
            order.push_back(2);
            async::break_parent(); // guarantees run() returns even if the defer starves
        },
        200ms);
    async::defer([&order] { order.push_back(1); });

    async::run(); // blocking run — the documented standalone loop

    EXPECT_EQ(order, (std::vector<int>{1, 2})) << "defer() must be drained inside the blocking run, not only after it returns";
}

// The other half of the blocking contract (Huly QB-448): under a blocking run the in-loop drain
// serves the queue, and a defer queued by a deferred callback is re-armed for the loop's next
// iteration. A blocking call that returns before that re-arm fires (EVRUN_ONCE) leaves it to the next
// call -- its own drain after the loop does not take it -- and a non-blocking call then runs it: the
// re-arm fires there, the hook stands down (a non-blocking turn drains at its tail), the tail drain
// takes it.
TEST_F(CallbackDispatchTest, ABlockingTurnLeavesANestedDeferToTheNextCall) {
    std::atomic<int> stage{0};
    async::callback(
        [&] {
            async::defer([&] {
                stage.store(1);
                async::defer([&] { stage.store(2); });
            });
        },
        1ms);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (stage.load() == 0 && std::chrono::steady_clock::now() < deadline)
        async::listener::current.run(EVRUN_ONCE); // blocks until the timer fires
    ASSERT_EQ(stage.load(), 1) << "the blocking run() call that ran the deferred callback also ran the defer it queued";
    EXPECT_TRUE(async::listener::current.has_deferred()) << "the nested defer was lost";
    while (stage.load() != 2 && std::chrono::steady_clock::now() < deadline)
        async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(stage.load(), 2) << "the nested defer never ran on a later call";
}

TEST_F(CallbackDispatchTest, PendingDeferIsDroppedOnListenerClear) {
    // Teardown drops pending deferred callbacks WITHOUT running them; each captured
    // shared_ptr is released (the std::function destructor runs), so `ran` stays false
    // and the captured target frees — a strong capture keeps its target alive exactly
    // until fire-or-drop, leak-free.
    auto               alive = std::make_shared<int>(0);
    std::weak_ptr<int> weak  = alive;
    bool               ran   = false;
    async::defer([alive, &ran]() { ran = true; });
    alive.reset(); // the deferred callback now holds the only strong ref
    EXPECT_FALSE(weak.expired()) << "capture must keep the target alive until fire-or-drop";
    async::listener::current.clear(); // teardown drops the queue
    EXPECT_FALSE(ran) << "a dropped defer must NOT run at teardown";
    EXPECT_TRUE(weak.expired()) << "dropping the defer must release its captured state";
}

// =============================================================================
// A loop pumped from inside a deferred callback (Huly QB-1246)
// =============================================================================
//
// A deferred callback that pumps the loop -- `run_sync()`, `run_until()`, a nested `run()` -- runs turns of its
// own, and their drains take every deferred callback but the ones executing on the stack, in queue order: the
// rule of GLib and Asio. Until 3.4 the drain refused to nest, so such a pump drained nothing -- a completion
// delivered through `defer()` (the connector's immediate failure) never ran, and the pump spun at full CPU until a
// timer ended it, or for ever when none was armed. Every case that pumps until something happens has a watchdog
// (a timer, or `run_sync()`'s own timeout), so a red is an assertion, never a hang.

namespace callback_dispatch_test {

/// The fixture of the nested-drain cases: some spawn coroutines (`run_sync()`), whose frames are drained before the
/// loop is cleared. A named namespace: a coroutine the cases spawn names nothing of theirs, but the rule is the file's
/// (qb/scripts/check-coro-fixture-linkage.py).
class NestedDrain : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        if (async::listener::current.has_coro_scheduler()) {
            async::run_for(5ms);
            async::listener::current.reset_coro_scheduler();
        }
        async::listener::current.clear();
    }
};

/// A connect over no address at all: the connector fails at once and delivers that failure through `defer()`
/// (`tcp::connector::run()`), on every system, without a descriptor. Whether a socket came back.
async::task<bool>
connect_over_no_address() {
    auto socket = co_await async::tcp::connect(std::vector<endpoint>{}, "nowhere");
    co_return socket.has_value();
}

/// Ends every `ev_run` on the stack: a nested pump's and the blocking run's around it.
void
break_every_run() {
    async::listener::current.loop().break_loop(ev::ALL);
}

} // namespace callback_dispatch_test

using callback_dispatch_test::break_every_run;
using callback_dispatch_test::connect_over_no_address;
using callback_dispatch_test::NestedDrain;

// `defer([] { run_sync(connect_to(nowhere)); })` -- the shape of a reconnect written as a deferred callback.
TEST_F(NestedDrain, ARunSyncInADeferredCallbackHearsAFailureDeliveredThroughDefer) {
    bool ran = false, heard = false, connected = true, timed_out = false;
    async::defer([&] {
        ran = true;
        try {
            connected = async::run_sync(async::coro_with_timeout(connect_over_no_address(), 2s));
            heard     = true;
        } catch (async::timeout_error const &) {
            timed_out = true;
        }
    });
    async::listener::current.run(EVRUN_NOWAIT);
    ASSERT_TRUE(ran) << "the deferred callback never ran";
    EXPECT_FALSE(timed_out) << "run_sync() in a deferred callback never ran the connector's deferred failure: its pump drained no "
                               "deferred callback and spun until the 2 s timeout";
    EXPECT_TRUE(heard);
    EXPECT_FALSE(connected) << "a connect over no address returned a socket";
}

// The same under a blocking `async::run()`: the deferred callback runs from the in-loop drain, inside the loop.
TEST_F(NestedDrain, ARunSyncInADeferredCallbackUnderABlockingRunHearsAFailureDeliveredThroughDefer) {
    bool       ran = false, heard = false, connected = true, timed_out = false, watchdog = false;
    auto const guard = async::scoped_callback(
        [&] {
            watchdog = true;
            break_every_run();
        },
        5s);
    async::defer([&] {
        ran = true;
        try {
            connected = async::run_sync(async::coro_with_timeout(connect_over_no_address(), 2s));
            heard     = true;
        } catch (async::timeout_error const &) {
            timed_out = true;
        }
        break_every_run();
    });
    async::run(); // the watchdog is a live watcher: this blocks, and the in-loop drain runs the callback
    ASSERT_TRUE(ran) << "the in-loop drain never ran the deferred callback";
    EXPECT_FALSE(watchdog) << "the blocking run outlived its deferred callback";
    EXPECT_FALSE(timed_out) << "run_sync() in a deferred callback the in-loop drain ran never ran the connector's deferred failure";
    EXPECT_TRUE(heard);
    EXPECT_FALSE(connected);
}

// `run_until(flag)` in a deferred callback, the flag cleared by a callback that one defers before pumping.
TEST_F(NestedDrain, ARunUntilInADeferredCallbackRunsTheChildThatEndsIt) {
    bool       pumping = true, child_ran = false, watchdog = false, returned = false;
    auto const guard = async::scoped_callback(
        [&] {
            watchdog = true;
            pumping  = false;
        },
        2s);
    async::defer([&] {
        async::defer([&] {
            child_ran = true;
            pumping   = false;
        });
        async::run_until(pumping);
        returned = true;
    });
    async::listener::current.run(EVRUN_NOWAIT);
    ASSERT_TRUE(returned) << "the deferred callback never ran";
    EXPECT_FALSE(watchdog) << "run_until() in a deferred callback drained no deferred callback: only the 2 s watchdog ended it";
    EXPECT_TRUE(child_ran);
}

// The flag is cleared by a SIBLING queued after the pumping callback -- what separates the rule from a nested drain
// that would take only what was deferred after it began.
TEST_F(NestedDrain, ARunUntilInADeferredCallbackRunsTheSiblingQueuedAfterIt) {
    bool       pumping = true, sibling_ran = false, watchdog = false, returned = false;
    auto const guard = async::scoped_callback(
        [&] {
            watchdog = true;
            pumping  = false;
        },
        2s);
    async::defer([&] {
        async::run_until(pumping);
        returned = true;
    });
    async::defer([&] {
        sibling_ran = true;
        pumping     = false;
    });
    async::listener::current.run(EVRUN_NOWAIT);
    ASSERT_TRUE(returned) << "the deferred callback never ran";
    EXPECT_FALSE(watchdog) << "run_until() in a deferred callback never ran the sibling queued behind it: only the 2 s watchdog ended it";
    EXPECT_TRUE(sibling_ran);
}

// The queue order holds across the nesting. A nested drain takes what was queued when it began -- the outer
// drain's rest, then what the pumping callback deferred -- and a callback deferred in a nested drain waits for the
// next nested pass.
TEST_F(NestedDrain, ANestedDrainKeepsTheQueueOrder) {
    std::vector<std::string> order;
    async::defer([&] { order.push_back("A"); });
    async::defer([&] {
        order.push_back("X begins");
        async::defer([&] { order.push_back("D, deferred by X"); });
        async::listener::current.run(EVRUN_NOWAIT);
        order.push_back("X between its passes");
        async::listener::current.run(EVRUN_NOWAIT);
        order.push_back("X ends");
    });
    async::defer([&] {
        order.push_back("B");
        async::defer([&] { order.push_back("E, deferred by B"); });
    });
    async::defer([&] { order.push_back("C"); });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(order,
              (std::vector<std::string>{"A", "X begins", "B", "C", "D, deferred by X", "X between its passes", "E, deferred by B", "X ends"}));
    EXPECT_FALSE(async::listener::current.has_deferred()) << "something was left queued";
}

// No stealing. What a nested pass deferred is the next turn's for the drain that encloses it: that drain stops at
// the bound it took when it began, whatever the nested drains took past it.
TEST_F(NestedDrain, TheEnclosingDrainDoesNotRunWhatANestedPassDeferred) {
    std::vector<std::string> order;
    async::defer([&] {
        order.push_back("X begins");
        async::listener::current.run(EVRUN_NOWAIT);
        order.push_back("X ends");
    });
    async::defer([&] {
        order.push_back("Y");
        async::defer([&] { order.push_back("child of Y"); });
    });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(order, (std::vector<std::string>{"X begins", "Y", "X ends"}))
        << "the nested pass in X did not run Y, or the drain around X ran the child Y deferred in it";
    EXPECT_TRUE(async::listener::current.has_deferred()) << "the child of Y was lost";
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(order, (std::vector<std::string>{"X begins", "Y", "X ends", "child of Y"})) << "the child of Y never ran on a later turn";
}

// A deferred callback that runs a BLOCKING loop -- `async::run()` with a live watcher -- is a level of its own:
// the loop's in-loop drain serves both the callback deferred before it and the one a watcher inside it defers.
TEST_F(NestedDrain, ABlockingRunInADeferredCallbackDrainsWhatItDeferredAndWhatItsWatchersDefer) {
    std::vector<std::string> order;
    bool                     watchdog = false, returned = false;
    auto const               guard = async::scoped_callback(
        [&] {
            watchdog = true;
            break_every_run();
        },
        2s);
    std::unique_ptr<async::ScopedTimeout<std::function<void()>>> inner_timer;
    async::defer([&] {
        async::defer([&] { order.push_back("deferred before the blocking run"); });
        inner_timer = async::scoped_callback(std::function<void()>([&] {
                                                 async::defer([&] {
                                                     order.push_back("deferred by a watcher inside it");
                                                     async::break_parent();
                                                 });
                                             }),
                                             1ms);
        async::run(); // the watchdog keeps it blocking
        returned = true;
    });
    async::listener::current.run(EVRUN_NOWAIT);
    ASSERT_TRUE(returned) << "the deferred callback never ran";
    EXPECT_FALSE(watchdog) << "async::run() in a deferred callback never drained the callbacks deferred in it: only the 2 s watchdog "
                              "ended it";
    EXPECT_EQ(order, (std::vector<std::string>{"deferred before the blocking run", "deferred by a watcher inside it"}));
}

// The documented recursion: a deferred callback whose blocking run's drain runs the next one, and so on. Each level is a
// blocking run of its own, and the count of those on the stack must not wrap: at 256 levels an 8-bit count read 0 there,
// the in-loop drain stood down, and the next level never ran.
TEST_F(NestedDrain, BlockingRunsInDeferredCallbacksNestPast256Levels) {
    constexpr int kLevels        = 300;
    int           deepest        = 0;
    int           watchdog_level = 0; ///< the level the watchdog found stuck, 0 when it never fired
    auto const    guard          = async::scoped_callback(
        [&] {
            watchdog_level = deepest;
            break_every_run();
        },
        5s);
    std::function<void(int)> level = [&](int const depth) {
        deepest = depth;
        if (depth == kLevels) {
            break_every_run();
            return;
        }
        async::defer([&level, depth] { level(depth + 1); });
        async::run(); // a blocking level: its in-loop drain runs the next one
    };
    async::defer([&level] { level(1); });
    async::run();
    EXPECT_EQ(watchdog_level, 0) << "the blocking run at level " << watchdog_level
                                 << " never drained the next level: only the watchdog ended it";
    EXPECT_EQ(deepest, kLevels);
}

// A callback that throws in a nested drain is contained there; the pumping callback goes on, and every later
// callback runs once, in order.
TEST_F(NestedDrain, ACallbackThatThrowsInANestedDrainIsContained) {
    std::vector<std::string> order;
    async::defer([&] {
        order.push_back("X begins");
        async::defer([&] {
            order.push_back("thrower");
            throw std::runtime_error("a deferred callback that throws");
        });
        async::defer([&] { order.push_back("after the thrower"); });
        async::defer([&] {
            order.push_back("a non-std thrower");
            throw 42; // what a drain contains is not always a std::exception
        });
        async::defer([&] { order.push_back("after the non-std thrower"); });
        async::listener::current.run(EVRUN_NOWAIT);
        order.push_back("X ends");
    });
    async::defer([&] { order.push_back("B"); });
    async::listener::current.run(EVRUN_NOWAIT);
    for (int i = 0; i < 3; ++i)
        async::listener::current.run(EVRUN_NOWAIT); // nothing runs twice
    EXPECT_EQ(order, (std::vector<std::string>{
                         "X begins", "B", "thrower", "after the thrower", "a non-std thrower", "after the non-std thrower", "X ends"
                     }));
    EXPECT_FALSE(async::listener::current.has_deferred());
}

// A nested turn drains at ITS tail, after the watchers it ran, as any turn does: a callback deferred in a pumping
// callback runs after a timer due in that pump's turn, never at the turn's entry before it.
TEST_F(NestedDrain, ANestedTurnDrainsAfterTheWatchersItRan) {
    std::vector<std::string>                                     order;
    bool                                                         watchdog = false;
    std::unique_ptr<async::ScopedTimeout<std::function<void()>>> due_timer;
    auto const                                                   guard = async::scoped_callback(
        [&] {
            watchdog = true;
            break_every_run();
        },
        2s);
    async::defer([&] {
        async::defer([&] { order.push_back("drained by the first pass"); });
        async::listener::current.run(EVRUN_NOWAIT);
        due_timer            = async::scoped_callback(std::function<void()>([&] { order.push_back("timer"); }), 1ms);
        const auto due_after = std::chrono::steady_clock::now() + 5ms;
        while (std::chrono::steady_clock::now() < due_after) {
        } // the timer is due before the second pass begins
        async::defer([&] { order.push_back("deferred before the second pass"); });
        async::listener::current.run(EVRUN_NOWAIT);
        order.push_back("X ends");
        break_every_run();
    });
    async::run(); // the deferred callback runs from the in-loop drain of a blocking run
    EXPECT_FALSE(watchdog);
    EXPECT_EQ(order, (std::vector<std::string>{"drained by the first pass", "timer", "deferred before the second pass", "X ends"}))
        << "the second nested pass drained its deferred callback before the timer it ran, or not at all";
}
