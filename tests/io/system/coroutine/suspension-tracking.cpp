/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/suspension-tracking.cpp
 * @brief `CoroutineScheduler::dump()` and the opt-in suspension tracking (Huly QB-71) on a real loop.
 *
 * What the scheduler promises, each asserted where it can be observed:
 *  - tracking is off by default, and then the dump still lists the roots, by name, and the coroutines parked on a
 *    loop watcher, without what they wait on;
 *  - on, each awaiter of qb records what its coroutine waits on (sleep, mutex, semaphore, event, latch, channel,
 *    offload, generator), and a coroutine awaiting a task or a generator records its frame, so the dump chains a root
 *    to its leaf;
 *  - an awaitable of your own labels itself with `track_suspension(h, kind)`, or leaves the previous record;
 *  - a record ages, the longest waits come first, a record goes with its frame; turning tracking off drops all;
 *  - a name lives as long as its frame, whether its root is cancelled or completes, tracking on or off;
 *  - a thread counts in the awaiters' gate while it tracks, in the destructors' gate while it tracks or names, and in
 *    neither after its exit: a name alone never opens the awaiters' gate.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine/channel.h>
#include <qb/io/async/coroutine/sync.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;

// A named namespace: these coroutines are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace suspension_tracking_test {

class SuspensionTracking : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        coro_scheduler().set_suspension_tracking(false);
        if (listener::current.has_coro_scheduler()) {
            run_for(5ms);
            listener::current.reset_coro_scheduler();
        }
        listener::current.clear();
    }
};

[[nodiscard]] inline parked_coroutine const *
named(std::vector<parked_coroutine> const &all, std::string_view name) {
    for (auto const &p : all)
        if (p.name == name)
            return &p;
    return nullptr;
}
/// A pointer into a temporary dump would dangle: keep the dump in a local.
parked_coroutine const *named(std::vector<parked_coroutine> &&, std::string_view) = delete;

[[nodiscard]] inline parked_coroutine const *
at(std::vector<parked_coroutine> const &all, void const *frame) {
    for (auto const &p : all)
        if (p.frame == frame)
            return &p;
    return nullptr;
}
parked_coroutine const *at(std::vector<parked_coroutine> &&, void const *) = delete;

/// Whether the dump lists a coroutine by that name / at that frame: safe on a temporary dump. Two names, not two
/// overloads: a string literal converts to `void const *` by a standard conversion, which beats the user-defined one
/// to `std::string_view`, so an overloaded `lists(dump, "x")` would silently compare frames.
[[nodiscard]] inline bool
lists_name(std::vector<parked_coroutine> const &all, std::string_view name) {
    return named(all, name) != nullptr;
}
[[nodiscard]] inline bool
lists_frame(std::vector<parked_coroutine> const &all, void const *frame) {
    return at(all, frame) != nullptr;
}

[[nodiscard]] inline std::string_view
kind_of(parked_coroutine const *p) {
    return p && p->kind ? std::string_view{p->kind} : std::string_view{};
}

/// What the coroutine spawned under `name` waits on at the end of its chain. `spawn(name, callable)` runs the callable
/// in a wrapper that awaits it, so the named root waits on a task and the callable's own coroutine on the awaiter.
[[nodiscard]] inline std::string_view
leaf_kind(std::vector<parked_coroutine> const &all, std::string_view name) {
    auto const *p = named(all, name);
    for (std::size_t depth = 0; p && p->waits_on && depth < all.size(); ++depth) {
        auto const *next = at(all, p->waits_on);
        if (!next)
            break;
        p = next;
    }
    return kind_of(p);
}

/// The kinds along the chain of the coroutine spawned under `name`: the root first, the coroutine actually parked last.
[[nodiscard]] inline std::vector<std::string_view>
chain_kinds(std::vector<parked_coroutine> const &all, std::string_view name) {
    std::vector<std::string_view> out;
    auto const                   *p = named(all, name);
    for (std::size_t depth = 0; p && depth <= all.size(); ++depth) {
        out.push_back(kind_of(p));
        if (!p->waits_on)
            break;
        p = at(all, p->waits_on);
    }
    return out;
}

/// The written dump, for a failure message: who is still parked, and on what.
[[nodiscard]] inline std::string
dump_text() {
    std::ostringstream os;
    coro_scheduler().dump(os);
    return os.str();
}

/// A child the chain test awaits: it parks on the event it is given.
inline task<int>
leaf(async_event *ev) {
    co_await ev->wait();
    co_return 7;
}

/// The root of the chain test: it awaits the leaf.
inline task<void>
root_of_chain(async_event *ev, std::atomic<int> *out) {
    *out = co_await leaf(ev);
}

/// A producer that waits on an event before its value: its consumer parks on next(), the producer on the event.
inline async_generator<int>
gated_numbers(async_event *ev) {
    co_await ev->wait();
    co_yield 1;
}

/// Two values: a consumer that takes the first and parks elsewhere leaves this producer parked at its first co_yield.
inline async_generator<int>
two_numbers() {
    co_yield 1;
    co_yield 2;
}

/// How many coroutines of the dump are parked as `kind`.
[[nodiscard]] inline std::size_t
count_kind(std::vector<parked_coroutine> const &all, std::string_view kind) {
    std::size_t n = 0;
    for (auto const &p : all)
        n += kind_of(&p) == kind;
    return n;
}

/// Parks on a sleep of `d`: one frame size whatever `d`, so a freed frame of it is the next one the pool hands out.
inline task<void>
sleeper(qb::duration d, std::atomic<int> *done) {
    co_await sleep(d);
    ++*done;
}

/// An awaitable of the application's own: parks until resumed by hand, labelled (`label`) or not (null).
struct handmade_wait {
    std::coroutine_handle<> *parked;
    char const              *label;

    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    void
    await_suspend(std::coroutine_handle<> h) noexcept {
        if (label)
            track_suspension(h, label);
        *parked = h;
    }
    void
    await_resume() const noexcept {}
};

/// A one-shot gate a pool thread waits on, released from the test thread.
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
    bool
    wait() {
        std::unique_lock lk(_m);
        return _cv.wait_for(lk, 10s, [this] { return _open; });
    }

private:
    std::mutex              _m;
    std::condition_variable _cv;
    bool                    _open = false;
};

} // namespace suspension_tracking_test

using suspension_tracking_test::at;
using suspension_tracking_test::Gate;
using suspension_tracking_test::kind_of;
using suspension_tracking_test::leaf_kind;
using suspension_tracking_test::lists_frame;
using suspension_tracking_test::lists_name;
using suspension_tracking_test::named;
using suspension_tracking_test::SuspensionTracking;

TEST_F(SuspensionTracking, OffByDefaultTheDumpStillListsTheRootsByName) {
    ASSERT_FALSE(CoroutineScheduler::suspension_tracking()) << "tracking is on before anyone asked for it";
    async_event      ev;
    std::atomic<int> done{0};
    coro_scheduler().spawn("waiter", [&ev, &done]() -> task<void> {
        co_await ev.wait();
        ++done;
    });
    coro_scheduler().spawn("napper", suspension_tracking_test::sleeper(30s, &done));
    run_for(5ms);
    const auto  dump = coro_scheduler().dump();
    auto const *p    = named(dump, "waiter");
    ASSERT_NE(p, nullptr) << "a named root is missing from the dump";
    EXPECT_TRUE(p->root);
    EXPECT_EQ(p->kind, nullptr) << "with tracking off nothing records what a coroutine waits on";
    EXPECT_EQ(p->age.count(), 0);
    auto const *napper = named(dump, "napper");
    ASSERT_NE(napper, nullptr);
    EXPECT_TRUE(napper->root);
    EXPECT_EQ(kind_of(napper), "watcher") << "the scheduler knows a coroutine parked on a loop watcher, tracking or not";

    ev.set();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load() == 1; }));
    EXPECT_FALSE(lists_name(coro_scheduler().dump(), "waiter")) << "a completed root is still in the dump";
}

TEST_F(SuspensionTracking, EachAwaiterOfQbRecordsWhatItsCoroutineWaitsOn) {
    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(true));
    ASSERT_TRUE(CoroutineScheduler::suspension_tracking());

    async_mutex mtx;
    semaphore   sem(1);
    ASSERT_TRUE(sem.try_acquire()); // the test holds the only permit; its release() below hands it to the waiter
    async_event      ev;
    async_latch      latch(1);
    channel<int>     ch(0);
    async_event      release_holder;
    Gate             gate;
    std::atomic<int> done{0};
    std::atomic<int> held{0};

    coro_scheduler().spawn("holder", [&]() -> task<void> {
        co_await mtx.lock();
        ++held;
        co_await release_holder.wait();
        mtx.unlock();
        ++done;
    });
    coro_scheduler().spawn("on-mutex", [&]() -> task<void> {
        co_await mtx.lock();
        mtx.unlock();
        ++done;
    });
    coro_scheduler().spawn("on-semaphore", [&]() -> task<void> {
        co_await sem.acquire();
        ++done;
    });
    coro_scheduler().spawn("on-event", [&]() -> task<void> {
        co_await ev.wait();
        ++done;
    });
    coro_scheduler().spawn("on-latch", [&]() -> task<void> {
        co_await latch.wait();
        ++done;
    });
    coro_scheduler().spawn("on-channel", [&]() -> task<void> {
        (void) co_await ch.recv();
        ++done;
    });
    coro_scheduler().spawn("on-sleep", [&]() -> task<void> {
        co_await sleep(300ms);
        ++done;
    });
    coro_scheduler().spawn("on-offload", [&]() -> task<void> {
        (void) co_await offload([&gate] { return gate.wait(); });
        ++done;
    });
    coro_scheduler().spawn("on-generator", [&]() -> task<void> {
        auto numbers = suspension_tracking_test::gated_numbers(&ev);
        while (auto n = co_await numbers.next())
            (void) n;
        ++done;
    });
    coro_scheduler().spawn("on-event-after-a-value", [&]() -> task<void> {
        auto numbers = suspension_tracking_test::two_numbers();
        (void) co_await numbers.next();
        co_await ev.wait();
        while (auto n = co_await numbers.next())
            (void) n;
        ++done;
    });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return held.load() == 1; }));
    run_for(2ms);

    const auto dump = coro_scheduler().dump();
    EXPECT_EQ(leaf_kind(dump, "holder"), "event");
    EXPECT_EQ(leaf_kind(dump, "on-mutex"), "mutex");
    EXPECT_EQ(leaf_kind(dump, "on-semaphore"), "semaphore");
    EXPECT_EQ(leaf_kind(dump, "on-event"), "event");
    EXPECT_EQ(leaf_kind(dump, "on-latch"), "latch");
    EXPECT_EQ(leaf_kind(dump, "on-channel"), "channel recv");
    EXPECT_EQ(leaf_kind(dump, "on-sleep"), "sleep");
    EXPECT_EQ(leaf_kind(dump, "on-offload"), "offload");
    // the transfer awaiters (a task's, a generator's) record through their tail call: the chain goes through both
    EXPECT_EQ(suspension_tracking_test::chain_kinds(dump, "on-generator"), (std::vector<std::string_view>{"task", "generator next", "event"}))
        << suspension_tracking_test::dump_text();
    // a producer parked at its co_yield, its consumer parked elsewhere: no root leads to it, the dump still lists it
    EXPECT_EQ(leaf_kind(dump, "on-event-after-a-value"), "event");
    EXPECT_EQ(suspension_tracking_test::count_kind(dump, "generator yield"), 1u) << suspension_tracking_test::dump_text();
    auto const *holder = named(dump, "holder");
    ASSERT_NE(holder, nullptr);
    EXPECT_TRUE(holder->root);
    EXPECT_EQ(kind_of(holder), "task") << "the named root is the wrapper that awaits the callable";

    // everyone goes, and with them every record
    release_holder.set();
    sem.release();
    ev.set();
    latch.count_down();
    gate.open();
    coro_scheduler().spawn([&]() -> task<void> { co_await ch.send(1); });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load() == 10; }, 10s))
        << "a parked coroutine never resumed (" << done.load() << " of 10 done):\n"
        << suspension_tracking_test::dump_text();
    run_for(2ms);
    EXPECT_TRUE(coro_scheduler().dump().empty()) << "a resumed and completed coroutine is still in the dump";
}

TEST_F(SuspensionTracking, AChainLeadsFromItsRootToTheLeafAndTheRecordAges) {
    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(true));
    async_event      ev;
    std::atomic<int> out{0};
    coro_scheduler().spawn("chain", suspension_tracking_test::root_of_chain(&ev, &out));
    run_for(5ms);

    const auto  first = coro_scheduler().dump();
    auto const *root  = named(first, "chain");
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(root->root);
    EXPECT_EQ(kind_of(root), "task") << "a coroutine awaiting a task records the task";
    ASSERT_NE(root->waits_on, nullptr) << "the root does not say which coroutine it awaits";
    auto const *child = at(first, root->waits_on);
    ASSERT_NE(child, nullptr) << "the awaited coroutine is not in the dump";
    EXPECT_FALSE(child->root);
    EXPECT_EQ(kind_of(child), "event");

    std::ostringstream text;
    coro_scheduler().dump(text);
    EXPECT_NE(text.str().find("\"chain\""), std::string::npos) << text.str();
    const auto root_at = text.str().find(" task ");
    const auto arrow   = text.str().find(" -> ");
    const auto leaf_at = text.str().find(" event ");
    EXPECT_TRUE(root_at != std::string::npos && arrow != std::string::npos && leaf_at != std::string::npos && root_at < arrow
                && arrow < leaf_at)
        << "the root, then its leaf, are not on one line:\n"
        << text.str();
    EXPECT_NE(text.str().find("(tracking on)"), std::string::npos) << text.str();

    run_for(30ms);
    std::atomic<int> younger_done{0};
    coro_scheduler().spawn("younger", [&ev, &younger_done]() -> task<void> {
        co_await ev.wait();
        ++younger_done;
    });
    run_for(2ms);
    const auto  later = coro_scheduler().dump();
    auto const *aged  = at(later, root->waits_on);
    ASSERT_NE(aged, nullptr);
    EXPECT_GE(aged->age, 25ms) << "the record did not age with the wait";
    auto const *chain   = named(later, "chain");
    auto const *younger = named(later, "younger");
    ASSERT_TRUE(chain && younger);
    EXPECT_LT(chain, younger) << "the dump does not list the longest waits first";

    ev.set();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return out.load() == 7 && younger_done.load() == 1; }));
    run_for(2ms);
    EXPECT_TRUE(coro_scheduler().dump().empty());
}

TEST_F(SuspensionTracking, ACancelledRootTakesItsRecordAndItsNameWithIt) {
    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(true));
    std::atomic<int> done{0};
    coro_scheduler().spawn("sleeper", suspension_tracking_test::sleeper(30s, &done));
    run_for(5ms);
    const auto  before = coro_scheduler().dump();
    auto const *p      = named(before, "sleeper");
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(kind_of(p), "sleep");
    void *const frame = const_cast<void *>(p->frame);

    coro_scheduler().cancel_spawned(std::coroutine_handle<>::from_address(frame));
    EXPECT_FALSE(lists_frame(coro_scheduler().dump(), frame)) << "a destroyed frame left its record behind";

    // the frame pool hands a freed frame to the next coroutine of its size (LIFO): a name left behind would label it
    const auto next = coro_scheduler().spawn_tracked(suspension_tracking_test::sleeper(30s, &done));
    ASSERT_EQ(next.address(), frame) << "the pool did not hand the freed frame back: this case cannot see a stale name";
    run_for(5ms);
    const auto  after  = coro_scheduler().dump();
    auto const *reused = at(after, frame);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->name, "") << "a name outlived its frame";
    coro_scheduler().cancel_spawned(next);
}

TEST_F(SuspensionTracking, ACompletedRootLeavesTheDumpAndTakesItsNameWithIt) {
    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(true));

    // A completed root waits for the drain at the end of the pass to free it: a coroutine resumed after it in the
    // same pass must not see it parked.
    async_event      ev;
    std::atomic<int> finished{0};
    coro_scheduler().spawn("finisher", [&ev, &finished]() -> task<void> {
        co_await ev.wait();
        ++finished;
    });
    run_for(2ms);
    ev.set(); // the finisher is first in the ready queue, the observer after it
    int  finished_when_observed = -1;
    bool listed_when_observed   = true;
    coro_scheduler().spawn([&]() -> task<void> {
        finished_when_observed = finished.load();
        listed_when_observed   = lists_name(coro_scheduler().dump(), "finisher");
        co_return;
    });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return finished_when_observed != -1; }));
    ASSERT_EQ(finished_when_observed, 1) << "the observer ran before the finisher completed: the case proves nothing";
    EXPECT_FALSE(listed_when_observed) << "a completed root is listed before the drain frees it";

    // A name does not need tracking: with it off, the frame's destruction is still seen while a name lives.
    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(false));
    std::atomic<int> done{0};
    coro_scheduler().spawn("brief", suspension_tracking_test::sleeper(1ms, &done));
    const auto  spawned = coro_scheduler().dump();
    auto const *p       = named(spawned, "brief");
    ASSERT_NE(p, nullptr) << "a root spawned and not run yet is missing from the dump";
    void const *const frame = p->frame;
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load() == 1; }));
    run_for(2ms); // the drain that frees a completed root

    const auto next = coro_scheduler().spawn_tracked(suspension_tracking_test::sleeper(30s, &done));
    ASSERT_EQ(next.address(), frame) << "the pool did not hand the freed frame back: this case cannot see a stale name";
    run_for(5ms);
    const auto  after  = coro_scheduler().dump();
    auto const *reused = at(after, frame);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->name, "") << "a name outlived its frame";
    coro_scheduler().cancel_spawned(next);
}

TEST_F(SuspensionTracking, AnAwaitableOfYourOwnLabelsItselfOrLeavesThePreviousRecord) {
    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(true));
    async_event             ev;
    std::coroutine_handle<> labelled;
    std::coroutine_handle<> unlabelled;
    std::atomic<int>        done{0};
    coro_scheduler().spawn("labelled", [&]() -> task<void> {
        co_await suspension_tracking_test::handmade_wait{&labelled, "my queue"};
        ++done;
    });
    coro_scheduler().spawn("unlabelled", [&]() -> task<void> {
        co_await ev.wait();
        co_await suspension_tracking_test::handmade_wait{&unlabelled, nullptr};
        ++done;
    });
    run_for(2ms);
    ev.set();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return labelled && unlabelled; }));

    const auto dump = coro_scheduler().dump();
    EXPECT_EQ(leaf_kind(dump, "labelled"), "my queue");
    EXPECT_EQ(leaf_kind(dump, "unlabelled"), "event") << "an awaitable that does not label itself keeps the last record";

    coro_scheduler().schedule_resume(labelled);
    coro_scheduler().schedule_resume(unlabelled);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load() == 2; }));
}

TEST_F(SuspensionTracking, TurningTrackingOffDropsEveryRecord) {
    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(true));
    async_event      ev;
    std::atomic<int> done{0};
    coro_scheduler().spawn("parked", [&ev, &done]() -> task<void> {
        co_await ev.wait();
        ++done;
    });
    run_for(5ms);
    ASSERT_EQ(leaf_kind(coro_scheduler().dump(), "parked"), "event");

    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(false));
    EXPECT_FALSE(CoroutineScheduler::suspension_tracking());
    const auto  off = coro_scheduler().dump();
    auto const *p   = named(off, "parked");
    ASSERT_NE(p, nullptr) << "the root is listed with tracking off too";
    EXPECT_EQ(p->kind, nullptr) << "a record outlived tracking";

    ev.set();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load() == 1; }));
}

TEST_F(SuspensionTracking, AThreadCountsInTheGatesWhileItTracksOrNamesAndNotAfterItsExit) {
    // The gates in front of the tests count the threads that track (the awaiters') and the threads that track or hold a
    // name (the promise destructors'): zero means one global load and no thread-local access. A thread leaving tracking
    // on must leave both counts at its exit, or every thread would pay the thread-local tests for good; and a name must
    // open the destructors' gate only, or naming a coroutine would tax every suspension of the process.
    ASSERT_FALSE(qb::io::async::detail::suspension_tracking::frames)
        << "this thread already counts in the destructors' gate (a name left by another case): this case proves nothing";
    auto const &tracking = qb::io::async::detail::suspension_tracking::gate.tracking;
    auto const &frames   = qb::io::async::detail::suspension_tracking::gate.frames;
    const int   t0 = tracking.load(), f0 = frames.load();
    int         t_inside = -1, f_inside = -1;
    std::thread([&] {
        if (qb::io::async::detail::set_thread_tracking(true)) {
            t_inside = tracking.load();
            f_inside = frames.load();
        }
        // and exits with tracking on
    }).join();
    EXPECT_EQ(t_inside, t0 + 1) << "a thread that tracks does not count in the awaiters' gate";
    EXPECT_EQ(f_inside, f0 + 1) << "a thread that tracks does not count in the destructors' gate";
    EXPECT_EQ(tracking.load(), t0) << "a thread that exited with tracking on still counts in the awaiters' gate";
    EXPECT_EQ(frames.load(), f0) << "a thread that exited with tracking on still counts in the destructors' gate";

    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(true));
    EXPECT_EQ(tracking.load(), t0 + 1);
    EXPECT_EQ(frames.load(), f0 + 1);
    ASSERT_TRUE(coro_scheduler().set_suspension_tracking(false));
    EXPECT_EQ(tracking.load(), t0) << "turning tracking off left the thread in the awaiters' gate";
    EXPECT_EQ(frames.load(), f0) << "turning tracking off, with no name alive, left the thread in the destructors' gate";

    // a name, tracking off: the destructors must see the frame go, the awaiters have nothing to record
    async_event      ev;
    std::atomic<int> done{0};
    coro_scheduler().spawn("named", [&]() -> task<void> {
        co_await ev.wait();
        ++done;
    });
    run_for(2ms);
    EXPECT_EQ(frames.load(), f0 + 1) << "a thread holding a name does not count in the destructors' gate";
    EXPECT_EQ(tracking.load(), t0) << "a name opened the awaiters' gate";
    ev.set();
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load() == 1; }));
    run_for(2ms); // the drain that frees the completed root, and its name with it
    EXPECT_EQ(frames.load(), f0) << "the thread still counts in the destructors' gate after its last name went";
}
