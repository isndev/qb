/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/async/listener-defer-nested.cpp
 * @brief A deferred callback that runs the loop opens a new turn inside its own: the callbacks queued behind it, and
 *        those queued meanwhile, run there (Huly QB-1246).
 *
 * Until 3.4 the deferred queue was drained by one drain at a time: a `run()` called from inside a deferred callback
 * found the drain guard set and drained nothing. So a callback that WAITED for something by pumping the loop --
 * `run_sync()` of an awaitable, `run_until()` of a flag, a module's `await()` -- waited for ever when what it waited
 * for arrived through `defer()`, which is how qb-io itself delivers a connect refused at once
 * (`tcp::connector::deliver_failure_deferred()`): the pump found the queue not empty, so it never parked, and the queue
 * never emptied -- a loop spinning at full speed.
 *
 * What a turn is now (the semantics of GLib's and Asio's nested loops, and of Qt's):
 *
 *  - a drain runs the callbacks queued BEFORE it began, in order;
 *  - a callback being drained that runs the loop opens a NESTED turn: its drain takes everything queued at that
 *    moment -- what is left of the enclosing drain's own snapshot included -- except the callbacks executing;
 *  - the enclosing drain, when its callback returns, runs only what is LEFT of its own snapshot: never a callback
 *    queued after it began, whoever took the others;
 *  - a callback deferred by a drained callback is the next turn's, as before (Huly QB-448);
 *  - `clear()` drops the queue and ends EVERY drain on the stack (Huly QB-276);
 *  - under a blocking run, nothing queued waits on the poll.
 *
 * Every pump below is bounded by a deadline: against the listener as it was these cases FAIL, they do not hang.
 */

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

namespace listener_defer_nested_test {

using namespace std::chrono_literals;
namespace async = qb::io::async;

/// NOWAIT turns of the thread's loop until `done()` or the budget is spent. Whether `done()` was reached.
template <typename Done>
bool
pump(Done done, std::chrono::milliseconds const budget = 500ms) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done() && std::chrono::steady_clock::now() < deadline)
        async::listener::current.run(EVRUN_NOWAIT);
    return done();
}

class ListenerDeferNested : public ::testing::Test {
protected:
    void
    SetUp() override {
        async::init();
        async::listener::current.clear();
    }
    void
    TearDown() override {
        async::listener::current.clear();
    }
};

/// What an awaitable below waits for: completed once, by whoever gets there first.
struct Completion {
    std::coroutine_handle<> waiter{};
    bool                    done = false;

    void
    complete() {
        if (!done && waiter) {
            done = true;
            waiter.resume();
        }
    }
};

/// An awaitable that a DEFERRED callback completes: what a connect refused at once is to the coroutine awaiting it.
struct completed_by_a_deferred_callback {
    Completion &completion;

    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    void
    await_suspend(std::coroutine_handle<> const waiter) const {
        completion.waiter = waiter;
        async::defer([&completion = completion] { completion.complete(); });
    }
    void
    await_resume() const noexcept {}
};

// --- 1. the defect: a callback that waits by pumping the loop ---------------------------------------------------

TEST_F(ListenerDeferNested, ACallbackThatPumpsTheLoopRunsTheCallbacksQueuedBehindIt) {
    bool behind  = false;
    bool reached = false;
    async::defer([&] { reached = pump([&] { return behind; }); });
    async::defer([&] { behind = true; });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_TRUE(reached) << "a deferred callback pumped the loop for its whole budget and the callback queued behind it never ran";
    EXPECT_TRUE(behind);
}

TEST_F(ListenerDeferNested, ACallbackThatPumpsTheLoopRunsWhatAHandlerDefersMeanwhile) {
    // The connect-refused shape: what the callback waits for is deferred by a HANDLER the pump itself dispatches.
    bool delivered = false;
    bool reached   = false;
    async::defer([&] {
        async::callback([&] { async::defer([&] { delivered = true; }); }, 1ms);
        reached = pump([&] { return delivered; }, 2s);
    });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_TRUE(reached) << "the pump dispatched the handler, and what that handler deferred never ran inside it";
}

TEST_F(ListenerDeferNested, RunSyncFromADeferredCallbackReturnsWhenADeferredCallbackCompletesIt) {
    // `defer([]{ run_sync(connect(refused)); })`, without the network: run_sync() pumps, the awaitable is completed
    // by a deferred callback. It span for ever: here a watchdog timer completes the awaitable after 2 s, so the case
    // fails instead of hanging.
    Completion completion;
    int        answer  = 0;
    bool       expired = false;
    async::defer([&] {
        async::callback(
            [&] {
                expired = !completion.done;
                completion.complete();
            },
            2s);
        answer = async::run_sync([](Completion &c) -> async::task<int> {
            co_await completed_by_a_deferred_callback{c};
            co_return 42;
        }(completion));
    });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(answer, 42);
    EXPECT_FALSE(expired) << "run_sync() pumped for 2 s from a deferred callback: the deferred completion never ran inside it";
}

// --- 2. what a nested turn takes, and what it leaves ------------------------------------------------------------

TEST_F(ListenerDeferNested, ANestedTurnTakesTheRestOfTheSnapshotInOrderAndTheOuterDrainRunsNoneOfItAgain) {
    std::vector<int> order;
    async::defer([&] {
        order.push_back(1);
        async::listener::current.run(EVRUN_NOWAIT); // ONE nested turn
        order.push_back(4);
    });
    async::defer([&] { order.push_back(2); });
    async::defer([&] { order.push_back(3); });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4})) << "the nested turn takes what is queued, in queue order, once";
    EXPECT_FALSE(async::listener::current.has_deferred());
}

TEST_F(ListenerDeferNested, TheOuterDrainNeverRunsACallbackQueuedAfterItBegan) {
    // The nested turn runs B and C; C defers D. The outer drain counted three callbacks when it began and has run
    // one: a bound that counted ITS OWN pops would now take D, a callback of a later generation, in the same call.
    std::vector<int> order;
    async::defer([&] {
        order.push_back(1);
        async::listener::current.run(EVRUN_NOWAIT);
    });
    async::defer([&] { order.push_back(2); });
    async::defer([&] {
        order.push_back(3);
        async::defer([&] { order.push_back(9); });
    });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3})) << "the outer drain ran a callback deferred after it began";
    EXPECT_TRUE(async::listener::current.has_deferred()) << "the callback deferred inside the nested turn was lost";
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 9}));
}

TEST_F(ListenerDeferNested, ACallbackDeferredInsideANestedTurnIsNotRunByThatTurn) {
    // One generation a turn holds for the nested turn too: it took B, and what B defers is the next turn's.
    std::vector<int> order;
    async::defer([&] {
        order.push_back(1);
        async::listener::current.run(EVRUN_NOWAIT);
        order.push_back(3);                         // the nested turn is over: it ran 2, not 9
        async::listener::current.run(EVRUN_NOWAIT); // a second nested turn takes 9
        order.push_back(4);
    });
    async::defer([&] {
        order.push_back(2);
        async::defer([&] { order.push_back(9); });
    });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 9, 4}));
}

TEST_F(ListenerDeferNested, AClearFromANestedTurnEndsEveryDrainOnTheStack) {
    // [A, B, C, D]: A opens a nested turn, which runs B; B clears. C and D are dropped -- the nested drain and the
    // outer one both end there -- and what B defers after its clear() is the next turn's (Huly QB-276).
    std::vector<int> order;
    async::defer([&] {
        order.push_back(1);
        async::listener::current.run(EVRUN_NOWAIT);
        order.push_back(3);
    });
    async::defer([&] {
        order.push_back(2);
        async::listener::current.clear();
        async::defer([&] { order.push_back(9); });
    });
    async::defer([&] { order.push_back(7); }); // dropped by the clear
    async::defer([&] { order.push_back(8); }); // dropped by the clear
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3})) << "a drain on the stack went on after a clear(), or took what was deferred after it";
    EXPECT_TRUE(async::listener::current.has_deferred());
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 9}));
}

// --- 3. blocking runs --------------------------------------------------------------------------------------------

TEST_F(ListenerDeferNested, ABlockingTurnOpenedFromADeferredCallbackDoesNotWaitOnThePollWithACallbackQueued) {
    // A timer keeps the loop able to block, and ends the wait of run(EVRUN_ONCE) after 300 ms. The callback queued
    // behind the one that opens that blocking turn must run when the turn BEGINS -- handed to the in-loop drain on
    // entry -- not after the poll has waited for the timer.
    bool timer_fired     = false;
    bool ran             = false;
    bool ran_before_poll = false;
    async::callback([&] { timer_fired = true; }, 300ms);
    async::defer([&] { async::listener::current.run(EVRUN_ONCE); });
    async::defer([&] {
        ran             = true;
        ran_before_poll = !timer_fired;
    });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_TRUE(ran);
    EXPECT_TRUE(ran_before_poll) << "run(EVRUN_ONCE) called from a deferred callback waited in the poll with a callback queued";
}

TEST_F(ListenerDeferNested, UnderABlockingRunACallbackThatPumpsRunsWhatAHandlerDefersMeanwhile) {
    // The standalone main loop (`async::run()`): the hook drains the callback, which pumps.
    bool delivered = false;
    bool reached   = false;
    async::callback([] { async::break_parent(); }, 3s); // the watchdog: the case fails, it does not hang
    async::defer([&] {
        async::callback([&] { async::defer([&] { delivered = true; }); }, 1ms);
        reached = pump([&] { return delivered; }, 1s);
        async::break_parent();
    });
    async::run();
    EXPECT_TRUE(reached) << "under a blocking run, a deferred callback pumped the loop and what a handler deferred never ran inside it";
}

TEST_F(ListenerDeferNested, ABlockingTurnOpenedFromAHookDrainedCallbackRunsWhatItsHandlersDefer) {
    // `async::run()` drains A through its in-loop hook; A opens a blocking turn of its own, whose timer handler
    // defers Y. Y runs inside that turn, at its tail -- not after A has returned to the outer loop.
    bool y_ran        = false;
    bool y_ran_inside = false;
    async::callback([] { async::break_parent(); }, 3s); // the watchdog
    async::defer([&] {
        async::callback([&] { async::defer([&] { y_ran = true; }); }, 1ms);
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!y_ran && std::chrono::steady_clock::now() < deadline)
            async::listener::current.run_once_for(50ms);
        y_ran_inside = y_ran;
        async::break_parent();
    });
    async::run();
    EXPECT_TRUE(y_ran_inside) << "a blocking turn opened from a hook-drained callback did not drain what its own handlers deferred";
}

TEST_F(ListenerDeferNested, ABlockingTurnOpenedFromAHookDrainedCallbackTakesWhatIsQueuedBehindIt) {
    // The same hand-over when the callback that opens the blocking turn is drained by a blocking loop's own hook:
    // what is left of the HOOK's snapshot is the new turn's on entry, it does not wait for that turn's poll.
    bool timer_fired     = false;
    bool ran             = false;
    bool ran_before_poll = false;
    async::callback([] { async::break_parent(); }, 3s); // the watchdog
    async::defer([&] {
        async::callback([&] { timer_fired = true; }, 300ms); // the bound of the turn's wait
        async::listener::current.run(EVRUN_ONCE);
        async::break_parent();
    });
    async::defer([&] {
        ran             = true;
        ran_before_poll = !timer_fired;
    });
    async::run();
    EXPECT_TRUE(ran);
    EXPECT_TRUE(ran_before_poll) << "run(EVRUN_ONCE) opened from a hook-drained callback waited in the poll with a callback queued";
}

// In the two cases below the timer that stands for "the poll waited" is armed BY the handler whose deferred callback
// must not wait: it cannot come due in that handler's own iteration, however late the host runs it.

TEST_F(ListenerDeferNested, UnderABlockingRunWhatATailDrainedCallbackDefersIsNotLeftToWaitOnThePoll) {
    // A HANDLER of the blocking loop (not a deferred callback) pumps one turn: it runs Y at its tail, and Y defers X.
    // X is the next turn's; the handler pumping no further, X must still run before the loop blocks -- here before
    // the later timer, never after the watchdog.
    std::vector<int> order;
    async::callback([] { async::break_parent(); }, 3s); // the watchdog
    async::callback(
        [&] {
            async::callback(
                [&] {
                    order.push_back(1);
                    async::break_parent();
                },
                200ms);
            async::defer([&] { async::defer([&] { order.push_back(0); }); });
            async::listener::current.run(EVRUN_NOWAIT); // one nested turn: runs the outer callback, queues the inner
        },
        1ms);
    async::run();
    EXPECT_EQ(order, (std::vector<int>{0, 1})) << "a callback deferred from a nested turn waited on the blocking loop's poll";
}

TEST_F(ListenerDeferNested, UnderABlockingRunAHandlersDeferIsServedAfterTheInLoopDrainHasRun) {
    // The in-loop drain has run once and returned. A HANDLER of the same loop that defers afterwards is served in its
    // own iteration: nothing the drain kept while it ran may read that handler as one of its callbacks, and leave
    // what it defers to wait on the poll.
    std::vector<int> order;
    async::callback([] { async::break_parent(); }, 3s); // the watchdog
    async::defer([&] { order.push_back(1); });          // drained by the hook, in the loop's first iteration
    async::callback(
        [&] {
            async::callback(
                [&] {
                    order.push_back(3);
                    async::break_parent();
                },
                200ms);
            async::defer([&] { order.push_back(2); });
        },
        20ms);
    async::run();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}))
        << "a handler's defer waited on the blocking loop's poll after the in-loop drain had run once";
}

TEST_F(ListenerDeferNested, AClearUnderABlockingRunArmsNoDrainForWhatItDrops) {
    // clear() holds the queue: what a destructor it runs defers is dropped with the rest. Under a blocking run
    // `defer()` feeds the in-loop drain -- but not then: fed, the drain would find the queue held, re-arm itself
    // for it at every one of clear()'s passes, and stay armed on a loop that clear() leaves with nothing to do.
    // Two shapes: the destructor defers; and it defers, then opens a blocking turn of its own, which must not hand
    // the held queue to the drain on entry either.
    struct DefersWhenReleased {
        bool const opens_a_blocking_turn;
        explicit DefersWhenReleased(bool const opens) noexcept
            : opens_a_blocking_turn(opens) {}
        DefersWhenReleased(DefersWhenReleased const &)            = delete;
        DefersWhenReleased &operator=(DefersWhenReleased const &) = delete;
        ~DefersWhenReleased() {
            async::defer([] {});
            if (opens_a_blocking_turn) {
                async::callback([] {}, 20ms); // a watcher, so the turn may block; and the bound of its wait
                async::listener::current.run(EVRUN_ONCE);
            }
        }
    };
    for (const bool opens_a_blocking_turn : {false, true}) {
        bool cleared       = false;
        bool idle_after_it = false;
        async::callback([] { async::break_parent(); }, 3s); // the watchdog; clear() takes it away with the rest
        async::callback(
            [&] {
                // A timer the loop owns, whose closure holds the object: clear() destroys it after it has dropped
                // the queue and stopped the drain's watcher, and the destructor defers.
                async::callback([held = std::make_shared<DefersWhenReleased>(opens_a_blocking_turn)] {}, 1h);
                async::listener::current.clear();
                cleared       = true;
                idle_after_it = !async::listener::current.has_work();
            },
            1ms);
        async::run(); // returns once clear() has left the loop without a watcher
        EXPECT_TRUE(cleared) << "opens_a_blocking_turn=" << opens_a_blocking_turn;
        EXPECT_TRUE(idle_after_it) << "clear() left the in-loop drain armed for a callback it had dropped; opens_a_blocking_turn="
                                   << opens_a_blocking_turn;
        EXPECT_FALSE(async::listener::current.has_deferred());
        async::listener::current.clear();
    }
}

/// Opens a run that only a break ends, in which a timer handler defers the callback that breaks it. Whether that
/// callback ran before the watchdog had to end the wait.
bool
a_run_ended_by_what_its_handler_defers() {
    struct State {
        bool served   = false;
        bool watchdog = false;
    };
    // Shared, not on this frame: the watchdog stays armed after the run it guards has returned.
    const auto state = std::make_shared<State>();
    async::callback(
        [state] {
            if (!state->served) {
                state->watchdog = true;
                async::break_parent();
            }
        },
        500ms);
    async::callback(
        [state] {
            async::defer([state] {
                state->served = true;
                async::break_parent();
            });
        },
        1ms);
    async::listener::current.run(); // blocking, until a break
    return state->served && !state->watchdog;
}

TEST_F(ListenerDeferNested, ARunOnlyABreakEndsOpenedFromADrainedCallbackServesWhatItsHandlersDefer) {
    // Such a run never reaches a tail drain: what its handlers defer is served by the in-loop drain or not at all.
    // `defer()` used to skip that drain's feed whenever a drain was on the stack -- which it is here, twice over: the
    // callback that opens the run is being drained by a non-blocking turn's tail, then by a blocking loop's hook.
    bool from_a_tail_drain = false;
    async::defer([&] { from_a_tail_drain = a_run_ended_by_what_its_handler_defers(); });
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_TRUE(from_a_tail_drain) << "opened from a callback of a non-blocking turn, the run waited for its watchdog";

    bool from_the_hook = false;
    async::callback([] { async::break_parent(); }, 3s); // the outer run's watchdog
    async::defer([&] {
        from_the_hook = a_run_ended_by_what_its_handler_defers();
        async::break_parent();
    });
    async::run();
    EXPECT_TRUE(from_the_hook) << "opened from a callback of a blocking loop's own drain, the run waited for its watchdog";
}

// --- 4. many levels ----------------------------------------------------------------------------------------------

TEST_F(ListenerDeferNested, DrainsNestAsDeepAsTheCallbacksPump) {
    // Each level defers the next and pumps until the innermost has run: 300 nested turns, more than a byte counts.
    constexpr int kDepth = 300;
    int           depth  = 0;
    int           peak   = 0;
    bool          bottom = false;
    struct Level {
        int  &depth;
        int  &peak;
        bool &bottom;
        void
        operator()() const {
            peak = ++depth > peak ? depth : peak;
            if (depth == kDepth) {
                bottom = true;
            } else {
                async::defer(Level{depth, peak, bottom});
                (void) pump([this] { return bottom; }, 5s);
            }
            --depth;
        }
    };
    async::defer(Level{depth, peak, bottom});
    async::listener::current.run(EVRUN_NOWAIT);
    EXPECT_TRUE(bottom);
    EXPECT_EQ(peak, kDepth);
    EXPECT_EQ(depth, 0);
}

/// One level of BlockingTurnsNestToTheirTwoHundredAndFiftySixth: a timer HANDLER that opens a blocking turn of its own.
struct BlockingLevel {
    static constexpr int kBlockingTurns = 256; ///< blocking turns on the stack when the last one opens

    int  &turns;       ///< blocking turns on the stack
    bool &served;      ///< the innermost turn ran what its handler deferred
    bool &served_late; ///< ... only once its watchdog had ended the wait

    void
    operator()() const {
        if (turns + 1 < kBlockingTurns) {
            ++turns;
            async::callback(*this, 1ms);
            async::listener::current.run(EVRUN_ONCE); // returns when the level below has unwound
            return;
        }
        // The 256th: a run that only a break ends. Its handler defers the callback that breaks it; the in-loop
        // drain must serve it at the tail of that iteration.
        ++turns;
        bool watchdog = false;
        async::callback(
            [&watchdog] {
                watchdog = true;
                async::break_parent();
            },
            500ms);
        async::callback(
            [this, &watchdog] {
                async::defer([this, &watchdog] {
                    served      = true;
                    served_late = watchdog;
                    async::break_parent();
                });
            },
            1ms);
        async::listener::current.run(); // blocking, until a break
    }
};

TEST_F(ListenerDeferNested, BlockingTurnsNestToTheirTwoHundredAndFiftySixth) {
    // The count of blocking runs on the stack was a byte. With 256 of them it read zero: `defer()` did not feed the
    // in-loop drain and the drain stood down, so in the 256th -- a run only a break ends -- a callback deferred by a
    // handler waited for the poll to be ended by something else.
    int  turns       = 1; // this case's own, below
    bool served      = false;
    bool served_late = true;
    async::callback(BlockingLevel{turns, served, served_late}, 1ms);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!served && std::chrono::steady_clock::now() < deadline)
        async::listener::current.run(EVRUN_ONCE);
    ASSERT_EQ(turns, BlockingLevel::kBlockingTurns);
    EXPECT_TRUE(served);
    EXPECT_FALSE(served_late) << "with 256 blocking turns on the stack a handler's deferred callback waited on the poll";
}

// --- 5. a break asked while a pump is running --------------------------------------------------------------------
// The loop keeps a break for the `ev_run` in progress only, and a pump begins one at every pass. A break is for the
// innermost run that only a break ends -- `run()` with no flag -- and that run keeps it: asked while a callback or a
// handler of the run pumps, or before one pumps in the same iteration, it ends the run once the pumping one has
// returned. Before a nested turn drained the queue, a callback queued behind a pumping one ran after it, in the main
// loop's own drain, and its break ended the main loop; inside the pump it would be lost at the pump's next pass, and
// the main loop would never end. The other orders below were lost before 3.4 too.

/// `async::run()` under a 2 s watchdog, `arrange()` having queued what the case is about. Whether something other
/// than the watchdog ended the run. The watchdog ends EVERY run on the stack (the loop's own `ev::ALL`): a case whose
/// inner run it had to end fails, it does not go on to hang on the outer one.
template <typename Arrange>
[[nodiscard]] bool
the_main_loop_ends_before_its_watchdog(Arrange arrange) {
    const auto watchdog = std::make_shared<bool>(false);
    async::callback(
        [watchdog] {
            *watchdog = true;
            async::listener::current.loop().break_loop(ev::ALL);
        },
        2s);
    arrange();
    async::run();
    return !*watchdog;
}

/// A raw watcher of the thread's loop, never started: fed, it is pending at the next `ev_run`'s ENTRY, where the
/// loop invokes what is pending before it does anything else.
struct FedAtTheNextEntry {
    ev::timer             watcher;
    std::function<void()> body;

    explicit FedAtTheNextEntry(std::function<void()> what)
        : watcher(async::listener::current.loop())
        , body(std::move(what)) {
        watcher.set<FedAtTheNextEntry, &FedAtTheNextEntry::on>(this);
    }
    FedAtTheNextEntry(FedAtTheNextEntry const &)            = delete;
    FedAtTheNextEntry &operator=(FedAtTheNextEntry const &) = delete;

    void
    on(ev::timer &, int) {
        body();
    }
};

/// Pumps the thread's loop the documented way until a timer of its own says `wait` has elapsed.
void
pump_for(std::chrono::milliseconds const wait) {
    bool waiting = true;
    async::callback([&waiting] { waiting = false; }, wait);
    async::run_until(waiting);
}

TEST_F(ListenerDeferNested, ABreakAskedByACallbackQueuedBehindAPumpingOneEndsTheMainLoop) {
    // B runs inside A's pump, between two of its passes.
    bool b_ran = false;
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
        async::defer([] { pump_for(100ms); }); // A
        async::defer([&b_ran] {                // B
            b_ran = true;
            async::break_parent();
        });
    })) << "the break of a callback queued behind a pumping one was lost: only the watchdog ended the main loop";
    EXPECT_TRUE(b_ran);
}

TEST_F(ListenerDeferNested, ABreakAskedByACallbackQueuedBehindARunSyncEndsTheMainLoop) {
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::defer([] {
            async::run_sync([]() -> async::task<void> {
                co_await async::sleep(100ms);
            }());
        });
        async::defer([] { async::break_parent(); });
    })) << "the break of a callback queued behind run_sync() was lost";
}

TEST_F(ListenerDeferNested, ABreakAskedBeforeALaterCallbackOfTheSameDrainPumpsEndsTheMainLoop) {
    // The other order: the break is asked first, and the callback behind it pumps. Lost before 3.4 too.
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::defer([] { async::break_parent(); });
        async::defer([] { pump_for(50ms); });
    })) << "a break asked before a later callback of the same drain pumped was lost";
}

TEST_F(ListenerDeferNested, ABreakAskedByAHandlerUnderAPumpEndsTheMainLoop) {
    // Whoever runs the pump -- a deferred callback, a watcher's handler -- and whoever asks under it: a handler here.
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::defer([] {
            async::callback([] { async::break_parent(); }, 10ms);
            pump_for(60ms);
        });
    })) << "a break asked by a handler under a deferred callback's pump was lost";
    async::listener::current.clear();
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::callback(
            [] {
                async::callback([] { async::break_parent(); }, 10ms);
                pump_for(60ms);
            },
            1ms);
    })) << "a break asked by a handler under a handler's pump was lost";
}

TEST_F(ListenerDeferNested, ABreakAskedByAHandlerBeforeACallbackOfTheSameIterationPumpsEndsTheMainLoop) {
    // No pump is running when the break is asked: the callback the handler deferred is drained at the tail of the
    // same iteration, and pumps.
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::callback(
            [] {
                async::defer([] { pump_for(50ms); });
                async::break_parent();
            },
            1ms);
    })) << "a break asked by a handler was lost to the pump of a callback drained in the same iteration";
}

TEST_F(ListenerDeferNested, ABreakAskedUnderAPumpOfACallbackThatAHandlersPumpDrainsEndsTheMainLoop) {
    // Two pumps deep, the outer one a handler's: H pumps; its pump drains C1, which pumps; C1's pump drains C2, which
    // asks. The break outlives C1's pump, and H's.
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::callback(
            [] {                                             // H
                async::defer([] { pump_for(30ms); });        // C1
                async::defer([] { async::break_parent(); }); // C2
                pump_for(80ms);
            },
            1ms);
    })) << "a break asked two pumps deep under a handler was lost";
}

TEST_F(ListenerDeferNested, ABreakAskedByTheHandlerThatEndsThePumpInTheSamePassEndsTheMainLoop) {
    // The pass that hears the break is the pump's last: the loop does not run again before the pump returns, but the
    // pass that was in progress when the break was asked has ended, and taken it.
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::defer([] {
            bool waiting = true;
            async::callback(
                [&waiting] {
                    async::break_parent();
                    waiting = false;
                },
                10ms);
            async::run_until(waiting);
        });
    })) << "a break asked in the last pass of a pump was lost";
}

TEST_F(ListenerDeferNested, ABreakAskedByACoroutineUnderRunSyncEndsTheMainLoop) {
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::defer([] {
            async::run_sync([]() -> async::task<void> {
                async::break_parent();
                co_await async::sleep(50ms);
            }());
        });
    })) << "a coroutine's break under run_sync() was lost";
}

TEST_F(ListenerDeferNested, EveryPumpOfTheLibraryRelaysABreakAskedUnderIt) {
    // The pump is a watcher's handler's each time -- nothing of the deferred queue is below the request -- and the
    // break is asked by a timer under it. run_until() is the case above.
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::callback(
            [] {
                async::callback([] { async::break_parent(); }, 10ms);
                async::run_sync([]() -> async::task<void> {
                    co_await async::sleep(50ms);
                }());
            },
            1ms);
    })) << "run_sync()";
    async::listener::current.clear();
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::callback(
            [] {
                async::callback([] { async::break_parent(); }, 10ms);
                (void) async::run_sync([]() -> async::task<int> {
                    co_await async::sleep(50ms);
                    co_return 7;
                }());
            },
            1ms);
    })) << "run_sync() of an awaitable that returns a value";
    async::listener::current.clear();
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::callback(
            [] {
                async::callback([] { async::break_parent(); }, 10ms);
                async::run_for(50ms);
            },
            1ms);
    })) << "run_for()";
    async::listener::current.clear();
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::callback(
            [] {
                async::callback([] { async::break_parent(); }, 10ms);
                const auto deadline = std::chrono::steady_clock::now() + 50ms;
                while (std::chrono::steady_clock::now() < deadline)
                    async::run(EVRUN_NOWAIT);
            },
            1ms);
    })) << "async::run(EVRUN_NOWAIT)";
    async::listener::current.clear();
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::callback(
            [] {
                async::callback([] { async::break_parent(); }, 10ms);
                async::callback([] {}, 50ms); // ends the wait of the second turn below
                async::run_once();            // the turn that hears the break
                async::run_once();            // a turn after it: the loop has forgotten
            },
            1ms);
    })) << "async::run_once()";
}

/// Asks the break at every pass of a pump it keeps busy: a timer it arms again, due at once, so that no pass is idle
/// and the pump never parks. Silent once the pump has returned -- the flag is shared: the last timer armed outlives
/// the handler that pumped.
struct AsksAtEveryPass {
    std::shared_ptr<bool const> pumping;

    void
    operator()() const {
        if (!*pumping)
            return;
        async::break_parent();
        async::callback(*this, 1ns);
    }
};

TEST_F(ListenerDeferNested, TheLastPassOfAPumpOfTheLibraryNeedsItsRelay) {
    // The break is asked in the pump's LAST pass, a non-blocking one: no park follows it to say it again, no deferred
    // callback is left to drain. The pump is a watcher's handler's, so nothing of the deferred queue is below it
    // either. Only the pump's own `relay_break()` hands the break to the run.
    enum class Pump { run_until, run_sync_of_void, run_sync_of_a_value, a_pass_of_async_run, run_once_for_with_work_queued, run_for };
    for (const Pump pump :
         {Pump::run_until, Pump::run_sync_of_void, Pump::run_sync_of_a_value, Pump::a_pass_of_async_run, Pump::run_once_for_with_work_queued,
          Pump::run_for}) {
        EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([pump] {
            async::callback(
                [pump] {
                    if (pump == Pump::run_for) {
                        const auto pumping = std::make_shared<bool>(true);
                        async::callback(AsksAtEveryPass{pumping}, 1ns);
                        async::run_for(20ms);
                        *pumping = false;
                        return;
                    }
                    bool waiting = true;
                    async::defer([&waiting] { // drained at the entry of the pump's first pass
                        async::break_parent();
                        waiting = false;
                    });
                    switch (pump) {
                        case Pump::run_until:
                            async::run_until(waiting);
                            break;
                        case Pump::run_sync_of_void:
                            async::run_sync([]() -> async::task<void> {
                                co_return;
                            }());
                            break;
                        case Pump::run_sync_of_a_value:
                            (void) async::run_sync([]() -> async::task<int> {
                                co_return 7;
                            }());
                            break;
                        case Pump::a_pass_of_async_run:
                            async::run(EVRUN_NOWAIT);
                            break;
                        case Pump::run_once_for_with_work_queued:
                            async::listener::current.run_once_for(50ms);
                            break;
                        case Pump::run_for:
                            break;
                    }
                },
                1ms);
            async::callback([] {}, 1s); // the run has something to wait for: it does not end for want of a watcher
        })) << "the break asked in the last pass was lost; pump="
            << static_cast<int>(pump);
        async::listener::current.clear();
    }
}

TEST_F(ListenerDeferNested, ABreakAskedBeforeAHandlerOpensARunOfItsOwnOutlivesThatRun) {
    // A watcher's handler asks the main loop's break, then opens a run of its own with the member `run()`: no pump of
    // the library, no drain of the main loop below it. The inner run keeps a request of its own; when it ends, the
    // main loop's is handed back and said again.
    bool inner_ended_by_its_own = false;
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&inner_ended_by_its_own] {
        async::callback(
            [&inner_ended_by_its_own] {
                async::break_parent(); // the main loop's
                bool own = false;
                async::callback(
                    [&own] {
                        own = true;
                        async::break_parent();
                    },
                    20ms);
                async::listener::current.run();
                inner_ended_by_its_own = own;
            },
            1ms);
    })) << "the main loop's break did not outlive the run its handler opened";
    EXPECT_TRUE(inner_ended_by_its_own);
}

TEST_F(ListenerDeferNested, ABreakOfEveryLoopAskedRightAfterAKeptBreakInsideAPumpsPassIsLeftAsItIs) {
    // In ONE pass of a pump under an inner run: `break_parent()`, kept for that run, then the loop's own
    // `break_loop(ev::ALL)`. The pass ends holding ALL -- only a break of one run goes with the run that ends -- and
    // the pump's relay finds a break held: it says nothing over it, and both runs end.
    bool              inner_returned = false;
    FedAtTheNextEntry asker([] {
        async::break_parent();
        async::listener::current.loop().break_loop(ev::ALL);
    });
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
        async::callback(
            [&] { // a handler of the main loop
                async::callback(
                    [&asker] { // a handler of the inner run
                        asker.watcher.feed_event(EV_TIMER);
                        async::run(EVRUN_NOWAIT); // one pass of a pump: `asker` runs at its entry
                    },
                    5ms);
                async::listener::current.run();
                inner_returned = true;
            },
            1ms);
    })) << "a break of every loop asked right after a kept break was narrowed to the inner run";
    EXPECT_TRUE(inner_returned);
}

TEST_F(ListenerDeferNested, AKeptBreakOutlivesAPumpsPassThatEndsBeforeItPolls) {
    // A pass ended at its ENTRY by the loop's own break: it clears what the loop held, takes its own break with it,
    // and never polls -- nothing the loop counts has moved. The loop holds no break any more: the relay says it.
    FedAtTheNextEntry ender([] { async::listener::current.loop().break_loop(); });
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&ender] {
        async::callback(
            [&ender] {
                async::break_parent(); // kept for the main loop
                ender.watcher.feed_event(EV_TIMER);
                async::run(EVRUN_NOWAIT);
            },
            1ms);
        async::callback([] {}, 1s);
    })) << "a kept break was lost to a pump's pass that ended before it polled";
}

/// Has a callback run at the ENTRY of every pass of a pump under the main loop, and counts what that pump did: a
/// callback deferred at the main loop's depth under a blocking run feeds the in-loop drain, which is pending at the
/// entry of the next pass, before it polls.
struct FedAtEveryPass {
    std::function<void()>                 callback;
    async::async_event                    ran{true}; ///< auto-reset: one wake per callback run
    bool                                  stop              = false;
    bool                                  ended             = false;   ///< `feeds_every_pass()` returned: no pass is fed any more
    bool const                           *waiting           = nullptr; ///< the pump's condition, while the pump runs
    std::chrono::steady_clock::time_point due               = std::chrono::steady_clock::time_point::max(); ///< the pump's timer
    std::size_t                           fed_while_waiting = 0; ///< callbacks run at a pass's entry, the pump waiting
    std::size_t                           late              = 0; ///< passes ended past `due`, the pump still waiting: none polled
    std::size_t                           away              = 0; ///< resumes away from the main loop's depth: the shape is broken

    /// Defers the callback, for the in-loop drain to run at the next pass's entry.
    static void
    feed(std::shared_ptr<FedAtEveryPass> const &self) {
        async::defer([self] {
            if (self->waiting && *self->waiting)
                ++self->fed_while_waiting;
            if (!self->stop)
                self->callback();
            self->ran.set();
        });
    }
};

/// Feeds the pass after the one whose entry ran the callback deferred last -- from the pump's tail, between those
/// two passes, at the main loop's depth -- until told to stop or until `give_up`. It is resumed ONCE per pass: by
/// the event that callback set, which the ready queue serves at the tail of that pass. (A `yield_or_cancel()` loop is
/// resumed again in the same drain, up to the turn's cap of 65536 resumes: a pass then cost tens of milliseconds in
/// a debug build, a time no pass of the pump itself takes.) The first pass is fed before the pump begins.
async::task<void>
feeds_every_pass(std::shared_ptr<FedAtEveryPass> fed, std::chrono::steady_clock::time_point give_up) {
    for (;;) {
        co_await fed->ran.wait(); // the callback deferred last has run: this is the tail of the pass that ran it
        if (fed->stop || std::chrono::steady_clock::now() >= give_up)
            break;
        if (async::listener::current.loop().depth() != 1) {
            ++fed->away;
            break;
        }
        if (fed->waiting && *fed->waiting && std::chrono::steady_clock::now() > fed->due)
            ++fed->late;
        FedAtEveryPass::feed(fed);
    }
    fed->ended = true;
}

TEST_F(ListenerDeferNested, AKeptBreakDoesNotKeepAPumpUnderItsRunFromPolling) {
    // The in-loop drain, fed between two passes of a pump, is the first thing the next pass invokes: at its entry,
    // before it polls. A kept break said again THERE ends the pass before its poll; with the drain fed at every pass
    // the pump never polls again, and the timer it waits for never fires. Five callbacks for that drain to run, one
    // for each point that says a kept break again: one that does nothing -- the drain itself says nothing there --
    // and four that end the way those points do: a pump of the library with nothing left to wait for, the relay a pump
    // written by hand ends with, an `EVRUN_ONCE` turn and a run of its own, both ended at their entry by a break of
    // their own. Under a pass of another pump none of them says the kept break. The case counts passes, never time: a
    // pass that ends past the timer's due time with the pump still waiting is a pass that did not poll -- one at most
    // ends between the due time and the poll that fires the timer, two when the two clocks are read apart -- and a
    // pump that never polls again makes one at every pass until the feeding gives up, after 1.5 s: the case fails
    // then, before its watchdog; it does not hang.
    using clock = std::chrono::steady_clock;
    enum class Drained {
        nothing,
        a_pump_of_the_library_with_nothing_to_wait_for,
        the_relay_of_a_pump_written_by_hand,
        an_evrun_once_turn_ended_at_its_entry,
        a_run_of_its_own_ended_at_its_entry
    };
    for (const Drained drained :
         {Drained::nothing, Drained::a_pump_of_the_library_with_nothing_to_wait_for, Drained::the_relay_of_a_pump_written_by_hand,
          Drained::an_evrun_once_turn_ended_at_its_entry, Drained::a_run_of_its_own_ended_at_its_entry}) {
        const auto      fed = std::make_shared<FedAtEveryPass>();
        clock::duration took{};
        bool            fed_until_it_fired = false;
        EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
            async::callback(
                [&] {                      // a handler of the main loop
                    async::break_parent(); // kept: the main loop ends when this handler returns
                    std::function<void()> callback = [] {
                    };
                    switch (drained) {
                        case Drained::nothing:
                            break;
                        case Drained::a_pump_of_the_library_with_nothing_to_wait_for:
                            callback = [] {
                                const bool waiting = false;
                                async::run_until(waiting);
                            };
                            break;
                        case Drained::the_relay_of_a_pump_written_by_hand:
                            callback = [] {
                                async::listener::current.relay_break();
                            };
                            break;
                        case Drained::an_evrun_once_turn_ended_at_its_entry:
                            callback = [] {
                                async::defer([] { async::break_parent(); }); // drained at the turn's entry: ends it there
                                async::run_once();
                            };
                            break;
                        case Drained::a_run_of_its_own_ended_at_its_entry:
                            callback = [] {
                                async::defer([] { async::break_parent(); }); // the inner run's own, drained at its entry
                                async::listener::current.run();
                            };
                            break;
                    }
                    bool waiting  = true;
                    fed->callback = std::move(callback);
                    fed->waiting  = &waiting;
                    async::coro_scheduler().spawn(feeds_every_pass(fed, clock::now() + 1500ms));
                    FedAtEveryPass::feed(fed); // the first pass
                    const auto armed_at = clock::now();
                    fed->due            = armed_at + 22ms; // the timer's 20 ms, and 2 for two clocks read apart
                    async::callback([&waiting] { waiting = false; }, 20ms);
                    async::run_until(waiting);
                    took               = clock::now() - armed_at;
                    fed_until_it_fired = !fed->ended;
                    fed->waiting       = nullptr;
                    fed->stop          = true;
                },
                1ms);
        })) << "drained="
            << static_cast<int>(drained);
        EXPECT_LE(fed->late, 2u) << "a pump under a run that kept a break did not poll while the in-loop drain was fed between its "
                                    "passes: "
                                 << fed->late << " passes ended past its timer's due time, which fired after "
                                 << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                                 << " ms; drained=" << static_cast<int>(drained);
        EXPECT_TRUE(fed_until_it_fired) << "the timer fired only once the feeding had ended: the pump starved while it was "
                                           "fed, or the pass that fired it was not fed; drained="
                                        << static_cast<int>(drained);
        EXPECT_GE(fed->fed_while_waiting, 1u) << "no pass was fed while the pump waited; drained=" << static_cast<int>(drained);
        EXPECT_EQ(fed->away, 0u) << "the feeding was resumed away from the main loop's depth; drained=" << static_cast<int>(drained);
        EXPECT_TRUE(pump([&] { return fed->ended; }, 2s)) << "the coroutine of this case did not end; drained=" << static_cast<int>(drained);
        async::listener::current.clear();
    }
}

TEST_F(ListenerDeferNested, ABreakOfEveryLoopAskedBeforeABreakOfOneRunIsLeftAsItIs) {
    // The other order: `break_loop(ev::ALL)` first, then `break_parent()`, in one handler of an inner run. The loop
    // holds a break of every run: asking one of the inner run alone does not write over it.
    bool inner_returned = false;
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&inner_returned] {
        async::callback(
            [&inner_returned] { // a handler of the main loop
                async::callback(
                    [] { // a handler of the inner run
                        async::listener::current.loop().break_loop(ev::ALL);
                        async::break_parent();
                    },
                    5ms);
                async::listener::current.run();
                inner_returned = true;
            },
            1ms);
    })) << "a break of every loop was narrowed to the inner run by the break_parent() that followed it";
    EXPECT_TRUE(inner_returned);
}

TEST_F(ListenerDeferNested, AKeptBreakOutlivesAClearThatRunsTheLoop) {
    // clear() runs passes of its own to let the loop forget what it detaches: they make the loop forget a kept break
    // too, and clear() says it again. Only watchers the listener does not own can keep the run going after a clear():
    // a raw one here, started, and another, fed, that makes clear()'s first pass run the loop.
    bool              raw_fired = false;
    FedAtTheNextEntry fed([] {});
    FedAtTheNextEntry keeper([&raw_fired] { raw_fired = true; });
    keeper.watcher.start(0.4, 0.);
    async::callback(
        [&fed] {
            async::callback([] {}, 10s); // something registered: clear() runs its passes
            async::break_parent();       // the main loop's, kept
            fed.watcher.feed_event(EV_TIMER);
            async::listener::current.clear();
        },
        1ms);
    async::run();
    keeper.watcher.stop();
    EXPECT_FALSE(raw_fired) << "a break kept for the main loop was lost to the passes clear() runs";
}

TEST_F(ListenerDeferNested, APumpWrittenByHandRelaysABreakWithRelayBreak) {
    // Passes of the member `run(EVRUN_NOWAIT)`, the call a core's own pass makes: nothing is said again for them.
    // Under a deferred callback the in-loop drain says it when the callback has returned; under a watcher's handler
    // the pump ends with `relay_break()`.
    const auto pump_by_hand = [](std::chrono::milliseconds const wait) {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (std::chrono::steady_clock::now() < deadline)
            async::listener::current.run(EVRUN_NOWAIT);
    };
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
        async::defer([&] {
            async::callback([] { async::break_parent(); }, 10ms);
            pump_by_hand(50ms);
        });
    })) << "under a deferred callback's pump written by hand";
    async::listener::current.clear();
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
        async::callback(
            [&] {
                async::callback([] { async::break_parent(); }, 10ms);
                pump_by_hand(50ms);
                async::listener::current.relay_break();
            },
            1ms);
    })) << "under a handler's pump written by hand, which ends with relay_break()";
    async::listener::current.clear();
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
        async::defer([&] {
            async::callback([] { async::break_parent(); }, 10ms);
            async::listener::current.run(); // a run of its own first, ended by that break: the main loop's depth again
            async::callback([] { async::break_parent(); }, 10ms);
            pump_by_hand(50ms);
        });
    })) << "under a deferred callback's pump written by hand, after a run that callback had opened";
}

TEST_F(ListenerDeferNested, ABreakOfEveryLoopAskedAfterAKeptBreakIsLeftAsItIs) {
    // `loop().break_loop(ev::ALL)` is the loop's own call: asked behind a `break_parent()` in the same drain, with
    // no pump between them, it is what the loop holds -- saying the kept break again there would narrow it to the
    // inner run.
    bool inner_returned = false;
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&inner_returned] {
        async::callback(
            [&inner_returned] { // a handler of the main loop
                async::defer([] { async::break_parent(); });
                async::defer([] { async::listener::current.loop().break_loop(ev::ALL); });
                async::listener::current.run(); // its drain runs both
                inner_returned = true;
            },
            1ms);
    })) << "a break of every loop asked after a kept break ended the inner run only";
    EXPECT_TRUE(inner_returned);
}

TEST_F(ListenerDeferNested, ABreakAskedByWhatAPumpsHandlerDefersEndsTheMainLoop) {
    // The asking callback is drained by one of the pump's own parks -- an EVRUN_ONCE turn, a blocking one: that turn
    // is not the run the break is for.
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {
        async::defer([] {
            async::callback([] { async::defer([] { async::break_parent(); }); }, 10ms);
            pump_for(60ms);
        });
    })) << "a break asked by a callback that a pump's own park drained ended that park and nothing else";
}

TEST_F(ListenerDeferNested, ABreakEndsTheInnermostRunThatOnlyABreakEndsAndNoOther) {
    // A run opened from a callback of the main loop and ended by its own break: the main loop goes on. Two shapes --
    // the break asked straight from one of the inner run's handlers; and asked under a pump that one of them runs,
    // where the INNER run keeps it, and no other.
    for (const bool asked_under_a_pump : {false, true}) {
        bool inner_returned = false;
        bool main_went_on   = false;
        EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
            async::defer([&, asked_under_a_pump] {
                if (asked_under_a_pump)
                    async::callback(
                        [] {
                            async::callback([] { async::break_parent(); }, 10ms);
                            pump_for(40ms);
                        },
                        1ms);
                else
                    async::callback([] { async::break_parent(); }, 10ms);
                async::listener::current.run(); // until that break, and only it
                inner_returned = true;
                async::callback(
                    [&main_went_on] {
                        main_went_on = true;
                        async::break_parent();
                    },
                    50ms);
            });
        })) << "asked_under_a_pump="
            << asked_under_a_pump;
        EXPECT_TRUE(inner_returned) << "asked_under_a_pump=" << asked_under_a_pump;
        EXPECT_TRUE(main_went_on) << "the break of a run opened from a callback ended the main loop with it; asked_under_a_pump="
                                  << asked_under_a_pump;
        async::listener::current.clear();
    }
}

TEST_F(ListenerDeferNested, ABreakAskedForTheMainLoopOutlivesARunOpenedAfterIt) {
    // B asks the main loop to end; A, behind it in the same drain, opens a run of its own, which its own break ends.
    // B's break is the main loop's still: kept across the inner run, not spent on it.
    bool inner_ended_by_its_own = false;
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
        async::defer([] { async::break_parent(); }); // B
        async::defer([&] {                           // A
            bool own = false;
            async::callback([] { async::defer([] {}); }, 5ms); // the inner run drains a callback before its own break
            async::callback(
                [&own] {
                    own = true;
                    async::break_parent();
                },
                30ms);
            async::listener::current.run();
            inner_ended_by_its_own = own;
        });
    })) << "a break asked for the main loop was lost to a run opened after it";
    EXPECT_TRUE(inner_ended_by_its_own) << "the main loop's break ended the run opened after it, before that run's own";
}

TEST_F(ListenerDeferNested, ABreakIsKeptForTheMainLoopAfterARunOpenedFromACallbackHasEnded) {
    // A opens a run of its own, which its own break ends; then, back in the main loop's drain, A pumps, and a handler
    // asks the main loop to end under that pump. The inner run is over: the drains that count are the main loop's again.
    bool inner_returned = false;
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
        async::defer([&inner_returned] {
            async::callback([] { async::break_parent(); }, 10ms);
            async::listener::current.run();
            inner_returned = true;
            async::callback([] { async::break_parent(); }, 10ms);
            pump_for(60ms);
        });
    })) << "after a run opened from a callback had ended, a break asked under that callback's pump was lost";
    EXPECT_TRUE(inner_returned);
}

TEST_F(ListenerDeferNested, ACallbackDrainedByARunOpenedFromAnotherAsksItsBreakOfThatRun) {
    // A break names no run: it is for the innermost one that only a break ends when it is asked. B, queued behind A,
    // is drained by the run A opens -- handed to it on entry, as everything queued is -- and ends THAT run. Before a
    // nested turn drained the queue B ran after A had returned, and ended the main loop.
    bool inner_ended_by_b = false;
    bool b_ran            = false;
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([&] {
        async::defer([&] { // A
            bool own_timer = false;
            async::callback(
                [&own_timer] {
                    own_timer = true;
                    async::break_parent();
                },
                300ms);
            async::listener::current.run();
            inner_ended_by_b = b_ran && !own_timer;
            async::break_parent(); // the main loop's, asked from its own drain
        });
        async::defer([&b_ran] { // B
            b_ran = true;
            async::break_parent();
        });
    }));
    EXPECT_TRUE(inner_ended_by_b);
}

TEST_F(ListenerDeferNested, ABreakWithNoRunToEndIsNotKeptForALaterOne) {
    // With no run that only a break ends on the stack a break ends the pass in progress and nothing else, as before:
    // it is not held against the next run().
    async::defer([] { async::break_parent(); });
    async::listener::current.run(EVRUN_NOWAIT);
    bool timer_fired = false;
    async::callback(
        [&timer_fired] {
            timer_fired = true;
            async::break_parent();
        },
        30ms);
    EXPECT_TRUE(the_main_loop_ends_before_its_watchdog([] {}));
    EXPECT_TRUE(timer_fired) << "a break asked before the run began ended it at once";
}

// --- 6. a listener destroyed by a callback it is draining -------------------------------------------------------
// The drain is a member function of that listener, and so is the run() under it: when the callback RETURNS there is
// nothing left for them to read -- they read freed memory until 3.4 (the drain's count, the queue, the scheduler). It
// is found out on the drain's own frame, and the process stops on a line that says what happened. A callback that
// ENDS the process never returns: it is not stopped, and exits with the status it asked for.

[[noreturn]] void
a_callback_destroys_its_listener_and_returns() {
    auto own = std::make_unique<async::listener>();
    own->defer([&own] { own.reset(); });
    own->defer([] {});
    own->run(EVRUN_NOWAIT);
    std::_Exit(0); // not reached: the drain stopped the process
}

[[noreturn]] void
what_a_callback_captured_destroys_the_listener() {
    // The closure is the listener's last owner: the listener goes when the drain releases the closure, after its body.
    struct Owner {
        async::listener *const owned;
        explicit Owner(async::listener *const l) noexcept
            : owned(l) {}
        Owner(Owner const &)            = delete;
        Owner &operator=(Owner const &) = delete;
        ~Owner() {
            delete owned;
        }
    };
    auto *const raw = new async::listener;
    raw->defer([owner = std::make_shared<Owner>(raw)] {});
    raw->run(EVRUN_NOWAIT);
    std::_Exit(0); // not reached
}

[[noreturn]] void
a_callback_of_a_nested_turn_destroys_the_listener() {
    // Two drains on the stack: both are marked, and the innermost says it first.
    auto own = std::make_unique<async::listener>();
    own->defer([&own] { own->run(EVRUN_NOWAIT); });
    own->defer([&own] { own.reset(); });
    own->run(EVRUN_NOWAIT);
    std::_Exit(0); // not reached
}

[[noreturn]] void
a_callback_destroys_its_listener_under_a_blocking_run() {
    // The in-loop drain's callback, under `async::run()`: the run below it is a blocking one, the loop's own frames
    // are on the stack.
    async::init();
    async::callback([] {}, 1h); // a watcher, so the loop can block
    async::defer([] { async::listener::current.~listener(); });
    async::run();
    std::_Exit(0); // not reached
}

[[noreturn]] void
a_callback_ends_the_process() {
    async::init();
    async::defer([] { std::exit(7); }); // the thread's listener is destroyed under this callback, which never returns
    async::listener::current.run(EVRUN_NOWAIT);
    std::_Exit(0); // not reached
}

TEST(ListenerDeferNestedDeathTest, AListenerDestroyedByACallbackItIsDrainingStopsOnALineThatSaysSo) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(a_callback_destroys_its_listener_and_returns(), "a listener was destroyed by one of its own deferred callbacks");
    EXPECT_DEATH(what_a_callback_captured_destroys_the_listener(), "a listener was destroyed by one of its own deferred callbacks");
    EXPECT_DEATH(a_callback_of_a_nested_turn_destroys_the_listener(), "a listener was destroyed by one of its own deferred callbacks");
    EXPECT_DEATH(a_callback_destroys_its_listener_under_a_blocking_run(), "a listener was destroyed by one of its own deferred callbacks");
}

TEST(ListenerDeferNestedDeathTest, ACallbackThatEndsTheProcessIsNotStopped) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_callback_ends_the_process(), ::testing::ExitedWithCode(7), "");
}

} // namespace listener_defer_nested_test
