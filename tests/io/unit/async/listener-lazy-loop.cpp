/**
 * @file unit/async/listener-lazy-loop.cpp
 * @brief `listener::current` builds its event loop on the FIRST USE THAT NEEDS ONE, never before: a thread that uses
 *        nothing of qb builds no loop, on every platform (Huly QB-1246).
 *
 * Until 3.4 the thread's listener built its loop in its constructor, and MSVC constructs every thread_local of a
 * module at the start of each of its threads (`__dyn_tls_init` on `DLL_THREAD_ATTACH`): every thread of a Windows
 * process that linked qb -- a thread pool's, a library's, one that never heard of qb -- built an event loop and its
 * backend at its start, and destroyed them at its exit. On Linux and macOS a thread_local is constructed on its
 * first use, so a thread that never touched the listener built nothing then either; the cases below pin it on every
 * platform all the same.
 *
 * HOW IT IS SEEN, with no API of its own: `ev_set_allocator()` routes every allocation the loop makes through a hook
 * of this file, installed once while the process starts -- before any loop of this process exists -- and as
 * compatible with `realloc`/`free` as the loop's own default, so a block allocated before it and freed through it is
 * fine. The hook counts, per thread, the fresh blocks libev allocates, and keeps the ones the thread still holds:
 * "no loop" is no block at all, and the loop a first use built is a block of its thread that is still alive, the one
 * `listener::current.loop()` and the thread's record (`detail::thread_loop()`) both name -- never "the first block of
 * the thread", which under `QB_EV_BACKEND` is the throwaway loop the listener builds first to probe the backend.
 * Every case runs on a fresh `std::thread`: the main thread of this binary never touches the listener.
 *
 * WHAT IS PINNED
 *   (a) a thread that uses nothing of qb allocates nothing through libev, at its start or at its exit (red on the
 *       base on MSVC only: every thread built a loop at its start);
 *   (b) `has_work()`, `run(EVRUN_NOWAIT)`, `defer()` and the drain that runs it, `clear()`, `size()` and
 *       `coro_scheduler()` -- a coroutine spawned and run to a park on nothing -- build no loop and leave the
 *       record's loop null (red on the base on every platform: the first touch of the listener built the loop);
 *       and each first use that needs a loop -- `loop()`, `registerEvent()` (an `async::callback` with a delay),
 *       `co_await sleep()`, `arm_wake()`, `run_once_for()`, `backend()`, `set_io_poll_interval()` -- builds it,
 *       ONCE, on its thread, and the record names it (the base built it once too, in the constructor);
 *   (c) a `listener` of one's own builds its loop in its constructor, and stays out of the thread's record;
 *   (d) a callback deferred before the loop exists is kept, and runs on the next `run()` once the loop is built;
 *   (e) a thread whose listener never built a loop ends cleanly: its listener's destructor builds none, drops the
 *       deferred callback, destroys the parked coroutine, and reports nothing.
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
#include <coroutine>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/ev/ev.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace listener_lazy_loop_test {

std::atomic<long> g_blocks{0};  ///< fresh blocks libev allocated, every thread together
thread_local long t_blocks = 0; ///< ... on this thread

// The blocks libev allocated on this thread and has not freed yet: a loop built here and alive is one of them.
constexpr int      kLiveSlots = 256;
thread_local void *t_live[kLiveSlots]{};
thread_local int   t_live_count    = 0;
thread_local bool  t_live_overflow = false; ///< more blocks alive than slots: `alive_here()` cannot answer

void
remember(void *const block) noexcept {
    if (t_live_count < kLiveSlots)
        t_live[t_live_count++] = block;
    else
        t_live_overflow = true;
}

void
forget(void *const block) noexcept {
    for (int i = 0; i < t_live_count; ++i)
        if (t_live[i] == block) {
            t_live[i] = t_live[--t_live_count];
            return;
        }
}

/// `block` is one libev allocated on this thread and still holds.
[[nodiscard]] bool
alive_here(void const *const block) noexcept {
    for (int i = 0; i < t_live_count; ++i)
        if (t_live[i] == block)
            return true;
    return false;
}

/// The loop's allocator, counting: `realloc` and `free` exactly as libev's default (`ev_realloc_emul`) uses them.
void *
counting_realloc(void *const ptr, long const size) noexcept {
    if (size > 0) {
        bool const fresh = ptr == nullptr;
        if (!fresh)
            forget(ptr); // before `realloc`, which may free it: the block it returns is remembered instead
        void *const block = std::realloc(ptr, static_cast<std::size_t>(size));
        if (block) {
            if (fresh) {
                ++t_blocks;
                g_blocks.fetch_add(1, std::memory_order_relaxed);
            }
            remember(block);
        }
        return block; // null: libev aborts on an allocation that failed
    }
    if (size == 0) {
        if (ptr)
            forget(ptr);
        std::free(ptr);
    }
    return nullptr;
}

/// Installed while this binary's objects with static storage are initialised: before `main`, before any loop.
struct allocator_hook {
    allocator_hook() noexcept {
        ev_set_allocator(&counting_realloc);
    }
};
allocator_hook const g_hook;

/// The loop the thread's record names: null until the thread's listener has built one.
[[nodiscard]] struct ev_loop *
recorded_loop() noexcept {
    return detail::thread_loop().raw;
}

[[nodiscard]] struct ev_loop *
loop_of(listener const &l) {
    return static_cast<struct ev_loop *>(l.loop());
}

/// Parks for ever and registers nothing: no watcher, no loop.
struct park_plain {
    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    void
    await_suspend(std::coroutine_handle<>) const noexcept {}
    void
    await_resume() const noexcept {}
};

/// Counts its own destruction, once (a move leaves the source with nothing to count).
class counted {
public:
    explicit counted(int &count) noexcept
        : _count(&count) {}
    counted(counted &&other) noexcept
        : _count(std::exchange(other._count, nullptr)) {}
    counted(counted const &)            = delete;
    counted &operator=(counted const &) = delete;
    counted &operator=(counted &&)      = delete;
    ~counted() {
        if (_count)
            ++*_count;
    }

private:
    int *_count;
};

task<void>
parks_on_nothing(bool *const started, int *const gone) {
    counted local{*gone};
    *started = true;
    co_await park_plain{};
}

task<void>
sleeps_an_hour(bool *const started) {
    *started = true;
    co_await sleep(1h);
}

/// What a first use left on its thread: read there, asserted here.
struct first_use_report {
    long            before      = -1;    ///< blocks on the thread before the use
    long            after       = -1;    ///< ... after it
    long            after_again = -1;    ///< ... after asking for the loop and its backend again
    bool            alive_here  = false; ///< the listener's loop is a block libev allocated on the thread, alive
    bool            overflow    = false;
    struct ev_loop *recorded    = nullptr;
    struct ev_loop *loop        = nullptr;
};

template <typename Use>
[[nodiscard]] first_use_report
first_use_on_a_fresh_thread(Use use) {
    first_use_report r;
    std::thread([&] {
        r.before = t_blocks;
        use();
        r.after      = t_blocks;
        r.recorded   = recorded_loop();
        r.loop       = loop_of(listener::current);
        r.alive_here = alive_here(r.loop);
        r.overflow   = t_live_overflow;
        (void) listener::current.backend();
        r.after_again = t_blocks;
    }).join();
    return r;
}

void
expect_built_once(first_use_report const &r, char const *const use) {
    EXPECT_EQ(r.before, 0) << use << ": the fresh thread had allocated through libev before its first use";
    EXPECT_GT(r.after, 0) << use << " built no loop";
    ASSERT_NE(r.loop, nullptr) << use;
    ASSERT_FALSE(r.overflow) << use << ": more blocks alive on the thread than this file keeps";
    EXPECT_TRUE(r.alive_here) << use << ": the listener's loop is not a loop libev built on its thread and still holds";
    EXPECT_EQ(r.recorded, r.loop) << use << ": the thread's record does not name the loop its listener built";
    EXPECT_EQ(r.after_again, r.after) << use << ": asking for the loop again allocated more -- a second loop";
}

} // namespace listener_lazy_loop_test

using namespace listener_lazy_loop_test;

/**
 * @test (a) A `std::thread` that uses nothing of qb allocates nothing through libev -- at its start, in its body, at
 *       its exit. On the base this was red on MSVC: the thread's `listener::current`, constructed at the thread's
 *       start with every other thread_local of the module, built a loop there. On Linux and macOS the base built
 *       nothing for such a thread either (a thread_local is constructed on first use); the case pins it on every
 *       platform.
 */
TEST(ListenerLazyLoop, AThreadThatUsesNothingOfQbBuildsNoLoop) {
    long const before = g_blocks.load();
    long       seen   = -1;
    std::thread([&] { seen = t_blocks; }).join();
    EXPECT_EQ(seen, 0) << "a thread that uses nothing of qb built a loop at its start";
    EXPECT_EQ(g_blocks.load() - before, 0) << "a thread that uses nothing of qb allocated through libev";
}

/**
 * @test (b) What needs no loop builds none. On a fresh thread: `has_work()`, `run(EVRUN_NOWAIT)`, `defer()` and the
 *       `run()` that drains it, `clear()`, `size()`, and `coro_scheduler()` with a coroutine spawned and run to a
 *       park on nothing, then a `run()` with that scheduler -- after each, nothing allocated through libev and the
 *       record's loop null; at the thread's exit, still nothing. On the base the first of them built the loop (the
 *       first touch of the listener constructed it).
 */
TEST(ListenerLazyLoop, WhatNeedsNoLoopBuildsNone) {
    struct step {
        char const     *what;
        long            blocks;
        struct ev_loop *recorded;
    };
    long const        before  = g_blocks.load();
    bool              drained = false;
    bool              started = false;
    int               gone    = 0;
    std::vector<step> steps;
    std::thread([&] {
        auto const mark = [&](char const *what) {
            steps.push_back({what, t_blocks, recorded_loop()});
        };
        (void) listener::current.has_work();
        mark("has_work()");
        listener::current.run(EVRUN_NOWAIT);
        mark("run(EVRUN_NOWAIT)");
        listener::current.defer([&drained] { drained = true; });
        mark("defer()");
        listener::current.run(EVRUN_NOWAIT);
        mark("the run() that drains a deferred callback");
        listener::current.clear();
        mark("clear()");
        (void) listener::current.size();
        mark("size()");
        coro_scheduler().spawn(parks_on_nothing(&started, &gone));
        coro_scheduler().run_ready();
        mark("coro_scheduler(): a coroutine spawned and run to a park on nothing");
        listener::current.run(EVRUN_NOWAIT);
        mark("run(EVRUN_NOWAIT) with a scheduler");
    }).join();
    for (step const &s : steps) {
        EXPECT_EQ(s.blocks, 0) << s.what << " built a loop";
        EXPECT_EQ(s.recorded, nullptr) << s.what << ": the thread's record names a loop";
    }
    EXPECT_TRUE(drained) << "the deferred callback must run on the next run(), loop or not";
    EXPECT_TRUE(started) << "the coroutine must run to its park";
    EXPECT_EQ(gone, 1) << "the parked coroutine must be destroyed with the thread's listener";
    EXPECT_EQ(g_blocks.load() - before, 0) << "the thread allocated through libev, at its exit if not before";
}

/**
 * @test (b) `listener::current.loop()` builds the thread's loop, once; the record names it.
 */
TEST(ListenerLazyLoop, LoopBuildsIt) {
    expect_built_once(first_use_on_a_fresh_thread([] { (void) listener::current.loop(); }), "loop()");
}

/**
 * @test (b) `registerEvent()`, through an `async::callback` with a delay, builds the thread's loop, once (the callback
 *       is dropped, never run, with the thread's listener).
 */
TEST(ListenerLazyLoop, RegisterEventBuildsIt) {
    bool ran = false;
    expect_built_once(first_use_on_a_fresh_thread([&ran] { callback([&ran] { ran = true; }, 1h); }),
                      "registerEvent() (an async::callback with a delay)");
    EXPECT_FALSE(ran);
}

/**
 * @test (b) `co_await sleep()` in a coroutine builds the thread's loop -- the scheduler it runs on built none -- once.
 */
TEST(ListenerLazyLoop, ASleepingCoroutineBuildsIt) {
    bool started = false;
    expect_built_once(first_use_on_a_fresh_thread([&started] {
                          coro_scheduler().spawn(sleeps_an_hour(&started));
                          coro_scheduler().run_ready(); // runs to its sleep: a timer on the thread's loop
                      }),
                      "co_await sleep()");
    EXPECT_TRUE(started);
}

/**
 * @test (b) `arm_wake()` builds the thread's loop, once.
 */
TEST(ListenerLazyLoop, ArmWakeBuildsIt) {
    expect_built_once(first_use_on_a_fresh_thread([] { listener::current.arm_wake(); }), "arm_wake()");
}

/**
 * @test (b) `run_once_for()` builds the thread's loop, once (a non-positive cap: a turn that does not block).
 */
TEST(ListenerLazyLoop, RunOnceForBuildsIt) {
    auto const one_turn = [] {
        (void) listener::current.run_once_for(qb::duration::zero());
    };
    expect_built_once(first_use_on_a_fresh_thread(one_turn), "run_once_for()");
}

/**
 * @test (b) `backend()` builds the thread's loop, once: the backend is the loop's.
 */
TEST(ListenerLazyLoop, BackendBuildsIt) {
    expect_built_once(first_use_on_a_fresh_thread([] { (void) listener::current.backend(); }), "backend()");
}

/**
 * @test (b) `set_io_poll_interval()` builds the thread's loop, once: the interval is kept in the CPU counter's ticks,
 *       and the counter is calibrated where the loop is built.
 */
TEST(ListenerLazyLoop, SetIoPollIntervalBuildsIt) {
    expect_built_once(first_use_on_a_fresh_thread([] { listener::current.set_io_poll_interval(std::chrono::milliseconds(1)); }),
                      "set_io_poll_interval()");
}

/**
 * @test (c) A `listener` of one's own builds its loop in its constructor, as before, and stays out of the thread's
 *       record: the record is about the thread's own listener, the loop its coroutines wait on.
 */
TEST(ListenerLazyLoop, AListenerOfOnesOwnBuildsItsLoopInItsConstructor) {
    long            blocks   = -1;
    bool            built    = false;
    struct ev_loop *loop     = nullptr;
    struct ev_loop *recorded = nullptr;
    std::thread([&] {
        listener own;
        blocks   = t_blocks;
        loop     = loop_of(own);
        built    = alive_here(loop);
        recorded = recorded_loop();
    }).join();
    EXPECT_GT(blocks, 0) << "a listener of one's own built no loop in its constructor";
    EXPECT_NE(loop, nullptr);
    EXPECT_TRUE(built) << "the listener's loop is not a loop libev built on its thread and still holds";
    EXPECT_EQ(recorded, nullptr) << "a listener of one's own entered the thread's record";
}

/**
 * @test (d) A callback deferred while the thread's listener has no loop is kept when the loop is built, and runs on
 *       the next `run()`, once.
 */
TEST(ListenerLazyLoop, ACallbackDeferredBeforeTheLoopRunsOnTheNextRun) {
    long blocks_at_defer = -1;
    int  ran_before_run  = -1;
    int  ran             = 0;
    std::thread([&] {
        listener::current.defer([&ran] { ++ran; });
        blocks_at_defer = t_blocks;
        (void) listener::current.loop(); // the loop is built now, with the callback queued
        ran_before_run = ran;
        listener::current.run(EVRUN_NOWAIT);
        listener::current.run(EVRUN_NOWAIT);
    }).join();
    EXPECT_EQ(blocks_at_defer, 0) << "defer() built a loop";
    EXPECT_EQ(ran_before_run, 0) << "building the loop ran the deferred callback";
    EXPECT_EQ(ran, 1) << "the callback deferred before the loop existed must run on the next run(), once";
}

/**
 * @test (e) A thread whose listener never built a loop ends cleanly: the listener's destructor, at the thread's exit,
 *       builds no loop, drops the deferred callback it holds (its closure released, never run), destroys the
 *       coroutine parked on its scheduler, and reports nothing. On the base the listener's first touch built a loop.
 */
TEST(ListenerLazyLoop, AnUnbuiltListenerEndsCleanlyAtItsThreadsExit) {
    long const               before  = g_blocks.load();
    std::size_t const        tally0  = abandoned_coroutine_frames_total();
    auto                     token   = std::make_shared<int>(0);
    std::weak_ptr<int> const watched = token;
    bool                     ran     = false;
    bool                     started = false;
    int                      gone    = 0;
    std::thread([&] {
        (void) listener::current.has_work();
        listener::current.defer([held = std::move(token), &ran] { ran = held != nullptr; });
        coro_scheduler().spawn(parks_on_nothing(&started, &gone));
        coro_scheduler().run_ready();
    }).join(); // ~listener at the thread's exit, its loop never built
    EXPECT_EQ(g_blocks.load() - before, 0) << "the thread's listener built a loop, at the thread's exit if not before";
    EXPECT_FALSE(ran) << "a deferred callback ran at the thread's exit";
    EXPECT_TRUE(watched.expired()) << "the dropped callback's captured state was never released";
    EXPECT_TRUE(started);
    EXPECT_EQ(gone, 1) << "the parked coroutine must be destroyed with the thread's listener";
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0);
}
