/**
 * @file unit/coroutine/scheduler-abandoned-frames.cpp
 * @brief A `CoroutineScheduler` destroyed while its thread's event loop lives DESTROYS every coroutine it owns --
 *        whatever it is parked on, whatever its coroutines' destructors spawn, wake or cancel on it -- and moves
 *        no tally; a frame a task driven by hand owns is that task's, and is never reported (Huly QB-84, QB-1246).
 *
 * Until 3.4 the destructor abandoned every suspended frame (Finding 2.B.8: "their watchers still reference them"):
 * one line on `qb::io::cerr`, the frame counted in `abandoned_coroutine_frames_total()`, and the frame leaked --
 * its locals never destroyed, its chain never freed, a mutex waiter left queued to run on under whichever
 * scheduler drained the mutex next. The destruction of a frame stops its watchers (each awaiter's destructor
 * does), so that reason held for one order only: a scheduler destroyed after its thread's loop. That order and
 * the other two in which destroying would run code that cannot run (on another thread, from inside one of its
 * own coroutines) still abandon; they are pinned in child processes by scheduler-teardown-orders.cpp, since an
 * abandoned coroutine is a leak by design. Everything here runs in THIS process with LeakSanitizer on in the
 * sanitize lane: nothing may leak, and nothing does.
 *
 * WHAT IS PINNED
 *   - T1c: a frame a task driven by hand owns, registered as suspended with a directly-owned scheduler, is not
 *     the scheduler's: it is neither destroyed nor counted when that scheduler goes, and its task destroys it
 *     afterwards (the base counted it: +1).
 *   - T1, one case per family of what a root can be parked on (the families of the design's probe, plus a timed
 *     send): a root spawned on a directly-owned scheduler -- made current, run to its park -- is destroyed with
 *     it while the thread's loop lives. Its local's destructor has run, nothing it waited on wakes it afterwards,
 *     the tally does not move, and once the helpers some families spawn on the LISTENER's scheduler are gone too
 *     (`when_any` / `when_all` branches, a timeout's runner, a scope's worker, a shared task's runner),
 *     `detail::CoroutineFrameAllocator::live_frames` is back where it started. The base abandoned every family:
 *     the local never destroyed, the tally +0, +1 or +2 depending on what the root awaited.
 *   - T1b: a mutex waiter destroyed with its scheduler does not run to its end when the mutex is released (the
 *     base left it queued in the mutex, and the next unlock ran it under the listener's scheduler).
 *   - The re-entrant teardowns, loop alive: a coroutine local's destructor that spawns on the dying scheduler,
 *     that spawns AND pumps it (the new coroutine parks after the pass that ran the destructor took its
 *     snapshot), that releases a mutex guard waking a sibling, that cancels a sibling -- everything destroyed,
 *     the tally +0. The base abandoned them all.
 *   - A frame parked on a TEMPORARY listener's loop, destroyed after that listener: by its scheduler (the base
 *     abandoned it, tally +1 and a leak) and by its task (the base stopped the frame's timer on the freed loop:
 *     a heap-use-after-free in `ev_timer_stop` under AddressSanitizer). `ev_loop_destroy` detaches the watchers
 *     still on a loop it destroys (E5), so the late stop returns before it reads that loop.
 *   - The paths the framework drives itself (the listener's teardown, a teardown while another scheduler is
 *     current, a clean teardown) report nothing, as before.
 *
 * "Reports nothing" is read on the tally: `report_abandoned_coroutine_frames()` adds the count it reports and
 * prints its one ABANDONED line in the same call, so a tally that does not move is a line that was not printed.
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

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstring>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::reset_async_context;

namespace scheduler_abandoned_frames_test {

int g_root_gone     = 0; ///< locals of the root coroutines destroyed
int g_other_gone    = 0; ///< locals of the second coroutine of a case (a sibling, a mutex waiter) destroyed
int g_param_gone    = 0; ///< by-value parameters of a coroutine spawned from a destructor destroyed
int g_sleepers_made = 0; ///< locals of the helper coroutines (the families' `sleeper`) constructed
int g_sleepers_gone = 0; ///< ... and destroyed
int g_completed     = 0; ///< coroutines that ran to their end

/// Counts its own destruction into `*count`, once: a move leaves the source with nothing to count, so a
/// coroutine's by-value parameter -- moved into its frame -- is counted when the frame goes, never at the call.
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

struct sleeper_local {
    sleeper_local() noexcept {
        ++g_sleepers_made;
    }
    sleeper_local(sleeper_local const &)            = delete;
    sleeper_local &operator=(sleeper_local const &) = delete;
    ~sleeper_local() {
        ++g_sleepers_gone;
    }
};

[[nodiscard]] long
live() noexcept {
    return detail::CoroutineFrameAllocator::live_frames;
}

/// Parks the coroutine for ever, registered as SUSPENDED with the current scheduler -- the bookkeeping a
/// watcher awaiter does on suspend -- and holding no watcher.
struct park_registered {
    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    void
    await_suspend(std::coroutine_handle<> h) const noexcept {
        CoroutineScheduler::current_ptr()->register_suspended(h);
    }
    void
    await_resume() const noexcept {}
};

/// Parks the coroutine for ever and registers nothing anywhere: only the scheduler that owns the root knows it.
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

task<void>
park_for_ever() {
    counted local{g_root_gone};
    co_await park_registered{};
}

task<void>
finish_at_once() {
    co_return;
}

task<void>
sleep_for_ever() {
    co_await sleep(5000ms);
}

/// Holds a task parked on a timer, then yields: spawned as is (no closure wrapper), its own frame is the one queued.
task<void>
hold_a_parked_task_then_yield() {
    task<void> parked = sleep_for_ever();
    parked.handle().resume(); // parks on its timer, registered with the current scheduler, owned by this frame
    co_await sleep(0ms);      // back of the current scheduler's ready queue
}

// ---- T1: the families ------------------------------------------------------------------------------------------

using chan = std::shared_ptr<channel<int>>;

/// What a root waits on and the test keeps: released after the root's scheduler is gone, to see whether anything
/// wakes the root then.
struct kept {
    chan                         a, b;
    std::shared_ptr<async_mutex> mutex;
    bool                         locked = false;
    cancellation_token           token;
#if !defined(_WIN32)
    int pair[2]{-1, -1}; ///< a socket pair nothing is ever written to: [0] never readable
    int full[2]{-1, -1}; ///< a socket pair whose [0] has its send buffer full: never writable
#endif

    kept()                        = default;
    kept(kept const &)            = delete;
    kept &operator=(kept const &) = delete;
    ~kept() {
        close_fds();
    }

    void
    release() {
        if (locked) {
            locked = false;
            mutex->unlock();
        }
        a.reset();
        b.reset();
        mutex.reset();
    }
    void
    close_fds() noexcept {
#if !defined(_WIN32)
        for (int *const fds : {pair, full})
            for (int i = 0; i < 2; ++i)
                if (fds[i] >= 0)
                    ::close(std::exchange(fds[i], -1));
#endif
    }
};

task<void>
sleeper(qb::duration const d) {
    sleeper_local local;
    co_await sleep(d);
    ++g_completed;
}

task<void>
f_park() {
    counted local{g_root_gone};
    co_await park_registered{};
    ++g_completed;
}
task<void>
f_plain() {
    counted local{g_root_gone};
    co_await park_plain{};
    ++g_completed;
}
task<void>
f_sleep() {
    counted local{g_root_gone};
    co_await sleep(5s);
    ++g_completed;
}
#if !defined(_WIN32)
task<void>
f_io_r(int const fd) {
    counted local{g_root_gone};
    co_await wait_readable(fd);
    ++g_completed;
}
task<void>
f_io_w(int const fd) {
    counted local{g_root_gone};
    co_await wait_writable(fd);
    ++g_completed;
}
#endif
task<void>
f_recv(chan ch) {
    counted local{g_root_gone};
    (void) co_await ch->recv();
    ++g_completed;
}
task<void>
f_recv_for(chan ch) {
    counted local{g_root_gone};
    (void) co_await ch->recv_for(5s);
    ++g_completed;
}
task<void>
f_send(chan ch) {
    counted local{g_root_gone};
    try {
        co_await ch->send(1);
    } catch (channel_closed const &) {
    }
    ++g_completed;
}
task<void>
f_send_for(chan ch) {
    counted local{g_root_gone};
    (void) co_await ch->send_for(1, 5s);
    ++g_completed;
}
task<void>
f_select(chan a, chan b) {
    counted local{g_root_gone};
    (void) co_await select(*a, *b);
    ++g_completed;
}
task<void>
f_when_any() {
    counted local{g_root_gone};
    (void) co_await when_any(sleeper(5s), sleeper(6s));
    ++g_completed;
}
task<void>
f_when_all() {
    counted local{g_root_gone};
    (void) co_await when_all(sleeper(5s), sleeper(6s));
    ++g_completed;
}
task<void>
f_timeout() {
    counted local{g_root_gone};
    try {
        co_await coro_with_timeout(sleeper(5s), 4s);
    } catch (...) {
    }
    ++g_completed;
}
task<void>
f_mutex(std::shared_ptr<async_mutex> m) {
    counted local{g_root_gone};
    co_await m->lock();
    m->unlock();
    ++g_completed;
}
task<void>
f_nested() {
    counted local{g_root_gone};
    co_await sleeper(5s);
    ++g_completed;
}
task<void>
f_csleep(cancellation_token token) {
    counted local{g_root_gone};
    (void) co_await cancellable_sleep(5s, token);
    ++g_completed;
}
task<void>
f_scope() {
    counted         local{g_root_gone};
    coroutine_scope scope;
    scope.spawn(sleeper(5s));
    co_await scope.join_all();
    ++g_completed;
}
task<void>
f_mcancel(cancellation_token token) {
    counted local{g_root_gone};
    try {
        co_await make_cancellable(sleeper(5s), token);
    } catch (...) {
    }
    ++g_completed;
}
task<void>
f_deadline() {
    counted local{g_root_gone};
    try {
        co_await with_deadline(sleeper(5s), std::chrono::steady_clock::now() + 4s);
    } catch (...) {
    }
    ++g_completed;
}
task<void>
f_scope_for() {
    counted         local{g_root_gone};
    coroutine_scope scope;
    scope.spawn(sleeper(5s));
    (void) co_await scope.join_all_for(4s);
    ++g_completed;
}
task<void>
f_shared() {
    counted local{g_root_gone};
    auto    shared = make_shared_task(sleeper(5s));
    co_await shared;
    ++g_completed;
}

enum class family {
    park,
    plain,
    sleep,
#if !defined(_WIN32)
    io_r,
    io_w,
#endif
    recv,
    recv_own,
    recv_for,
    send,
    send_for,
    select,
    when_any,
    when_all,
    timeout,
    mutex,
    nested,
    csleep,
    scope,
    mcancel,
    deadline,
    scope_for,
    shared,
    count, ///< not a family: how many this platform has
};

[[nodiscard]] char const *
name_of(family const which) {
    switch (which) {
        case family::park:
            return "park";
        case family::plain:
            return "plain";
        case family::sleep:
            return "sleep";
#if !defined(_WIN32)
        case family::io_r:
            return "io_r";
        case family::io_w:
            return "io_w";
#endif
        case family::recv:
            return "recv";
        case family::recv_own:
            return "recv_own";
        case family::recv_for:
            return "recv_for";
        case family::send:
            return "send";
        case family::send_for:
            return "send_for";
        case family::select:
            return "select";
        case family::when_any:
            return "when_any";
        case family::when_all:
            return "when_all";
        case family::timeout:
            return "timeout";
        case family::mutex:
            return "mutex";
        case family::nested:
            return "nested";
        case family::csleep:
            return "csleep";
        case family::scope:
            return "scope";
        case family::mcancel:
            return "mcancel";
        case family::deadline:
            return "deadline";
        case family::scope_for:
            return "scope_for";
        case family::shared:
            return "shared";
        case family::count:
            break;
    }
    return "unknown";
}

/// How a failure names the family it ran.
void
PrintTo(family const which, std::ostream *const os) {
    *os << name_of(which);
}

[[nodiscard]] std::vector<family>
all_families() {
    std::vector<family> all;
    for (int i = 0; i < static_cast<int>(family::count); ++i)
        all.push_back(static_cast<family>(i));
    return all;
}

#if !defined(_WIN32)
[[nodiscard]] bool
make_fds(kept &k) {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, k.pair) != 0 || ::socketpair(AF_UNIX, SOCK_STREAM, 0, k.full) != 0)
        return false;
    if (::fcntl(k.full[0], F_SETFL, ::fcntl(k.full[0], F_GETFL) | O_NONBLOCK) != 0)
        return false;
    char block[4096];
    std::memset(block, 'x', sizeof block);
    while (::write(k.full[0], block, sizeof block) > 0) {
    }
    return true; // full[0] is not writable until its peer reads, which never happens
}
#endif

[[nodiscard]] task<void>
make(family const which, kept &k) {
    switch (which) {
        case family::park:
            return f_park();
        case family::plain:
            return f_plain();
        case family::sleep:
            return f_sleep();
#if !defined(_WIN32)
        case family::io_r:
            return f_io_r(k.pair[0]);
        case family::io_w:
            return f_io_w(k.full[0]);
#endif
        case family::recv:
            k.a = std::make_shared<channel<int>>(0);
            return f_recv(k.a);
        case family::recv_own: // the channel lives in the frame alone
            return f_recv(std::make_shared<channel<int>>(0));
        case family::recv_for:
            k.a = std::make_shared<channel<int>>(0);
            return f_recv_for(k.a);
        case family::send: // capacity 0 and no receiver: the sender parks
            k.a = std::make_shared<channel<int>>(0);
            return f_send(k.a);
        case family::send_for:
            k.a = std::make_shared<channel<int>>(0);
            return f_send_for(k.a);
        case family::select:
            k.a = std::make_shared<channel<int>>(0);
            k.b = std::make_shared<channel<int>>(0);
            return f_select(k.a, k.b);
        case family::when_any:
            return f_when_any();
        case family::when_all:
            return f_when_all();
        case family::timeout:
            return f_timeout();
        case family::mutex: // held by the test: the root parks in its waiter list
            k.mutex  = std::make_shared<async_mutex>();
            k.locked = k.mutex->try_lock();
            return f_mutex(k.mutex);
        case family::nested:
            return f_nested();
        case family::csleep:
            return f_csleep(k.token);
        case family::scope:
            return f_scope();
        case family::mcancel:
            return f_mcancel(k.token);
        case family::deadline:
            return f_deadline();
        case family::scope_for:
            return f_scope_for();
        case family::shared:
            return f_shared();
        case family::count:
            break;
    }
    return f_park();
}

class SchedulerAbandonedFrames : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
        g_root_gone = g_other_gone = g_param_gone = 0;
        g_sleepers_made = g_sleepers_gone = 0;
        g_completed                       = 0;
    }
    void
    TearDown() override {
        if (listener::current.has_coro_scheduler()) {
            run_for(5ms);
            listener::current.reset_coro_scheduler();
        }
        listener::current.clear();
    }

    /// A few non-blocking passes of the thread's loop: whatever a release woke is resumed in the first one.
    static void
    pump() {
        for (int pass = 0; pass < 4; ++pass)
            listener::current.run(EVRUN_NOWAIT);
    }
};

class SchedulerDestroysWhatItOwns
    : public SchedulerAbandonedFrames
    , public ::testing::WithParamInterface<family> {};

// ---- the re-entrant teardowns ----------------------------------------------------------------------------------

task<void>
recv_counted(channel<int> &ch, counted param) {
    counted local{g_other_gone};
    (void) co_await ch.recv();
    ++g_completed;
    (void) param;
}

/// A coroutine local whose destructor spawns on the scheduler being destroyed -- and, for `pump`, runs it, so the
/// new coroutine starts and parks on `ch` after the teardown pass that destroyed this local took its snapshot.
class spawns_when_destroyed {
public:
    spawns_when_destroyed(CoroutineScheduler &dying, channel<int> &ch, bool const pump) noexcept
        : _dying(&dying)
        , _ch(&ch)
        , _pump(pump) {}
    spawns_when_destroyed(spawns_when_destroyed &&other) noexcept
        : _dying(std::exchange(other._dying, nullptr))
        , _ch(other._ch)
        , _pump(other._pump) {}
    spawns_when_destroyed(spawns_when_destroyed const &)            = delete;
    spawns_when_destroyed &operator=(spawns_when_destroyed const &) = delete;
    spawns_when_destroyed &operator=(spawns_when_destroyed &&)      = delete;
    ~spawns_when_destroyed() {
        if (!_dying)
            return;
        _dying->spawn(recv_counted(*_ch, counted{g_param_gone}));
        if (_pump)
            _dying->run_ready();
    }

private:
    CoroutineScheduler *_dying;
    channel<int>       *_ch;
    bool                _pump;
};

task<void>
parked_with(spawns_when_destroyed guard) {
    counted local{g_root_gone};
    co_await sleep(5s);
    ++g_completed;
    (void) guard;
}

task<void>
hold_lock_and_sleep(async_mutex &m) {
    counted local{g_root_gone};
    auto    held = co_await m.scoped_lock();
    co_await sleep(5s);
    ++g_completed;
}

task<void>
wait_lock(async_mutex &m) {
    counted local{g_other_gone};
    auto    held = co_await m.scoped_lock();
    ++g_completed;
}

/// A coroutine local whose destructor cancels a sibling spawned on the same scheduler (`cancel_spawned()`), with
/// the sibling's handle read when it runs: the test learns it after both are spawned.
class cancels_when_destroyed {
public:
    cancels_when_destroyed(CoroutineScheduler &dying, std::coroutine_handle<> const &sibling) noexcept
        : _dying(&dying)
        , _sibling(&sibling) {}
    cancels_when_destroyed(cancels_when_destroyed const &)            = delete;
    cancels_when_destroyed &operator=(cancels_when_destroyed const &) = delete;
    ~cancels_when_destroyed() {
        _dying->cancel_spawned(*_sibling);
    }

private:
    CoroutineScheduler            *_dying;
    std::coroutine_handle<> const *_sibling;
};

task<void>
parked_cancelling(CoroutineScheduler *dying, std::coroutine_handle<> const *sibling) {
    cancels_when_destroyed cancel{*dying, *sibling};
    counted                local{g_root_gone};
    co_await sleep(5s);
    ++g_completed;
}

task<void>
sibling_sleeping() {
    counted local{g_other_gone};
    co_await sleep(5s);
    ++g_completed;
}

// ---- a temporary listener's loop -------------------------------------------------------------------------------

/// Parks on a timer armed on the loop it is given -- an awaiter built by hand on a loop of one's choosing, as
/// awaiter.h documents it.
task<void>
sleep_on(ev::loop_ref const loop) {
    counted local{g_root_gone};
    co_await timer_awaiter{5s, loop};
    ++g_completed;
}

} // namespace scheduler_abandoned_frames_test

using namespace scheduler_abandoned_frames_test;

/**
 * @test T1c. A frame a task driven by hand owns, parked on a directly-owned scheduler -- registered there as
 *       suspended, exactly what a watcher awaiter does -- is not that scheduler's: its teardown neither destroys
 *       nor counts it, and the task destroys it afterwards, once (no leak, no double free). On the base the
 *       teardown counted it: tally +1.
 */
TEST_F(SchedulerAbandonedFrames, AFrameItsTaskOwnsIsNotReported) {
    const std::size_t before = abandoned_coroutine_frames_total();
    {
        task<void> parked = park_for_ever();
        {
            CoroutineScheduler sched; // owned here, not by the listener
            CoroutineScheduler::set_current(&sched);
            parked.handle().resume(); // runs to the park: registered as suspended with `sched`, owned by `parked`
            EXPECT_EQ(sched.active_count(), 1u) << "the parked frame must be registered as suspended";
        } // ~CoroutineScheduler: the frame is its task's
        CoroutineScheduler::set_current(nullptr);
        EXPECT_EQ(g_root_gone, 0) << "a scheduler destroyed a frame it does not own";
        EXPECT_EQ(abandoned_coroutine_frames_total(), before) << "a frame its task owns was reported as abandoned";
    } // its task destroys it
    EXPECT_EQ(g_root_gone, 1) << "the task must destroy its frame, once";
    EXPECT_EQ(abandoned_coroutine_frames_total(), before) << "destroying the frame afterwards must report nothing";
}

/**
 * @test A clean teardown -- every frame completed -- reports nothing.
 */
TEST_F(SchedulerAbandonedFrames, ACleanTeardownReportsNothing) {
    const std::size_t before = abandoned_coroutine_frames_total();
    {
        task<void> done = finish_at_once();
        {
            CoroutineScheduler sched;
            CoroutineScheduler::set_current(&sched);
            done.handle().resume(); // completes: never suspended
            EXPECT_TRUE(done.handle().done());
        }
        CoroutineScheduler::set_current(nullptr);
    }
    EXPECT_EQ(abandoned_coroutine_frames_total(), before) << "a teardown with nothing suspended must not move the tally";
}

/**
 * @test T1, one case per family. A root spawned on a directly-owned scheduler, made current and run to its park
 *       (the helpers its awaitable spawns on the LISTENER's scheduler run to theirs too, with that scheduler still
 *       current, as a coroutine's would), is DESTROYED when the scheduler is destroyed while the thread's loop
 *       lives: its local's destructor has run, the tally has not moved, and releasing what it waited on -- a
 *       channel, a mutex -- wakes nothing. Once the listener's own scheduler is gone as well (with the helpers it
 *       owns), `live_frames` is back to its baseline and every helper's local is destroyed. On the base every
 *       family was abandoned: its local never destroyed, the tally moved by 0, 1 or 2, and the mutex waiter ran
 *       to its end on the next unlock.
 */
TEST_P(SchedulerDestroysWhatItOwns, ARootParkedOnAnyAwaitableIsDestroyedWithItsScheduler) {
    auto *const       lsched = &coro_scheduler(); // the listener's, current
    const long        live0  = live();
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    kept              k;
#if !defined(_WIN32)
    ASSERT_TRUE(make_fds(k)) << "socketpair()";
#endif
    {
        CoroutineScheduler sched;
        CoroutineScheduler::set_current(&sched);
        sched.spawn(make(GetParam(), k));
        sched.run_ready();   // the root runs to its park
        lsched->run_ready(); // the helpers spawned on the listener's scheduler run to theirs, `sched` current
        EXPECT_EQ(g_root_gone, 0) << "the root must be parked before its scheduler goes";
        EXPECT_EQ(g_completed, 0);
    } // ~CoroutineScheduler, the thread's loop alive: it destroys what it owns (and unbinds itself)
    CoroutineScheduler::set_current(lsched);
    EXPECT_EQ(g_root_gone, 1) << "the root's local must be destroyed with its scheduler";
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0) << "a scheduler destroyed while its loop lives abandoned a coroutine";

    k.release(); // a channel's destructor, a mutex's unlock: nothing may be woken by them now
    pump();
    EXPECT_EQ(g_completed, 0) << "a coroutine destroyed with its scheduler ran on";

    listener::current.reset_coro_scheduler(); // the listener's helpers go with the listener's scheduler
    EXPECT_EQ(g_sleepers_gone, g_sleepers_made) << "a helper's local was never destroyed";
    EXPECT_EQ(live(), live0) << "a coroutine frame is still allocated";
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0);
    k.close_fds();
}

INSTANTIATE_TEST_SUITE_P(Families, SchedulerDestroysWhatItOwns, ::testing::ValuesIn(all_families()),
                         [](::testing::TestParamInfo<family> const &info) { return name_of(info.param); });

/**
 * @test T1b. A coroutine parked on an `async_mutex` the test holds is destroyed with its scheduler: it leaves the
 *       mutex's waiter list, and releasing the mutex then runs nothing -- the mutex is simply free. On the base the
 *       abandoned waiter stayed in the list: the unlock handed it the lock and queued it on the listener's
 *       scheduler, which ran it to its end (and it unlocked the mutex behind it).
 */
TEST_F(SchedulerAbandonedFrames, AMutexWaiterDestroyedWithItsSchedulerDoesNotRunWhenTheMutexIsReleased) {
    auto *const lsched = &coro_scheduler();
    auto        m      = std::make_shared<async_mutex>();
    ASSERT_TRUE(m->try_lock());
    {
        CoroutineScheduler sched;
        CoroutineScheduler::set_current(&sched);
        sched.spawn(f_mutex(m));
        sched.run_ready();
        ASSERT_EQ(m->waiters_count(), 1u) << "the coroutine must be parked on the mutex";
    }
    CoroutineScheduler::set_current(lsched);
    EXPECT_EQ(g_root_gone, 1) << "the waiter must be destroyed with its scheduler";
    EXPECT_EQ(m->waiters_count(), 0u) << "a destroyed waiter must leave the mutex's list";
    m->unlock();
    pump();
    EXPECT_EQ(g_completed, 0) << "the unlock ran a coroutine whose scheduler is gone";
    EXPECT_FALSE(m->is_locked()) << "with no waiter, an unlock frees the mutex";
}

/**
 * @test A coroutine local's destructor SPAWNS on the scheduler being destroyed (loop alive): the new coroutine,
 *       never started, is destroyed by the same teardown -- its by-value parameter with it -- and the tally does not
 *       move. On the base the parked root was abandoned (its local never destroyed, so nothing was spawned either).
 */
TEST_F(SchedulerAbandonedFrames, ADestructorThatSpawnsOnTheDyingSchedulerLeavesNothing) {
    auto *const       lsched = &coro_scheduler();
    const long        live0  = live();
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    channel<int>      ch;
    {
        CoroutineScheduler sched;
        CoroutineScheduler::set_current(&sched);
        sched.spawn(parked_with(spawns_when_destroyed{sched, ch, false}));
        sched.run_ready();
        ASSERT_EQ(g_param_gone, 0);
    }
    CoroutineScheduler::set_current(lsched);
    EXPECT_EQ(g_root_gone, 1) << "the parked root must be destroyed";
    EXPECT_EQ(g_param_gone, 1) << "the coroutine its destructor spawned must be destroyed by the same teardown";
    EXPECT_EQ(g_other_gone, 0) << "the spawned coroutine was never started";
    EXPECT_EQ(live(), live0);
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0);
    ch.close();
    pump();
    EXPECT_EQ(g_completed, 0);
}

/**
 * @test A coroutine local's destructor spawns on the dying scheduler AND pumps it: the new coroutine starts and
 *       parks on a channel, outside the snapshot of the teardown pass that ran the destructor. A second pass
 *       destroys it; the tally does not move, and closing the channel afterwards wakes nothing. On the base the
 *       root was abandoned.
 */
TEST_F(SchedulerAbandonedFrames, ADestructorThatSpawnsAndPumpsTheDyingSchedulerLeavesNothing) {
    auto *const       lsched = &coro_scheduler();
    const long        live0  = live();
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    channel<int>      ch;
    {
        CoroutineScheduler sched;
        CoroutineScheduler::set_current(&sched);
        sched.spawn(parked_with(spawns_when_destroyed{sched, ch, true}));
        sched.run_ready();
    }
    CoroutineScheduler::set_current(lsched);
    EXPECT_EQ(g_root_gone, 1) << "the parked root must be destroyed";
    EXPECT_EQ(g_other_gone, 1) << "the coroutine the destructor started must be destroyed, by a second pass";
    EXPECT_EQ(g_param_gone, 1);
    EXPECT_EQ(live(), live0);
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0);
    ch.close();
    pump();
    EXPECT_EQ(g_completed, 0) << "closing the channel ran a coroutine whose scheduler is gone";
}

/**
 * @test The destruction of one frame WAKES a sibling the same scheduler owns: the holder of an `async_mutex` is
 *       destroyed, its guard unlocks, and the unlock hands the mutex to the sibling parked on it. Both are
 *       destroyed, neither runs on, the mutex ends free and the tally does not move. On the base both were
 *       abandoned, and the mutex stayed locked by a frame that never ran again.
 */
TEST_F(SchedulerAbandonedFrames, ADestructionThatWakesASiblingLeavesNothing) {
    auto *const       lsched = &coro_scheduler();
    const long        live0  = live();
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    async_mutex       m;
    {
        CoroutineScheduler sched;
        CoroutineScheduler::set_current(&sched);
        sched.spawn(hold_lock_and_sleep(m));
        sched.spawn(wait_lock(m));
        sched.run_ready(); // the holder takes the mutex and sleeps; the waiter parks on the mutex
        ASSERT_TRUE(m.is_locked());
        ASSERT_EQ(m.waiters_count(), 1u);
    }
    CoroutineScheduler::set_current(lsched);
    EXPECT_EQ(g_root_gone, 1) << "the holder must be destroyed";
    EXPECT_EQ(g_other_gone, 1) << "the woken sibling must be destroyed";
    pump();
    EXPECT_EQ(g_completed, 0);
    EXPECT_FALSE(m.is_locked()) << "the mutex must be free once both frames are gone";
    EXPECT_EQ(live(), live0);
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0);
}

/**
 * @test A coroutine local's destructor CANCELS a sibling of the dying scheduler (`cancel_spawned()`): whichever the
 *       teardown reaches first, both are destroyed once, and the tally does not move. On the base both were
 *       abandoned (tally +2).
 */
TEST_F(SchedulerAbandonedFrames, ADestructorThatCancelsASiblingLeavesNothing) {
    auto *const             lsched = &coro_scheduler();
    const long              live0  = live();
    const std::size_t       tally0 = abandoned_coroutine_frames_total();
    std::coroutine_handle<> sibling{};
    {
        CoroutineScheduler sched;
        CoroutineScheduler::set_current(&sched);
        sched.spawn(parked_cancelling(&sched, &sibling));
        sibling = sched.spawn_tracked(sibling_sleeping());
        sched.run_ready();
    }
    CoroutineScheduler::set_current(lsched);
    EXPECT_EQ(g_root_gone, 1) << "the cancelling root must be destroyed";
    EXPECT_EQ(g_other_gone, 1) << "the sibling must be destroyed, once";
    EXPECT_EQ(live(), live0);
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0);
}

/**
 * @test A directly-owned scheduler owns a frame parked on the timer of a TEMPORARY listener's loop; the temporary
 *       listener is destroyed first, then the scheduler, while the thread's own loop lives. The scheduler destroys
 *       the frame, and the frame's timer stop finds a watcher `ev_loop_destroy` detached: no read of the freed
 *       loop, the tally +0, nothing leaked. On the base the scheduler abandoned the frame (tally +1, and the frame
 *       leaked).
 */
TEST_F(SchedulerAbandonedFrames, ASchedulerDestroyedAfterATemporaryLoopItsFrameWaitedOnDestroysTheFrame) {
    auto *const       lsched = &coro_scheduler();
    const long        live0  = live();
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    {
        CoroutineScheduler sched;
        CoroutineScheduler::set_current(&sched);
        {
            listener tmp; // a second listener on this thread, with a loop of its own; it creates no scheduler
            sched.spawn(sleep_on(tmp.loop()));
            sched.run_ready();
            ASSERT_EQ(g_root_gone, 0);
        } // ~listener: its loop is destroyed with the frame's timer still started on it
    }
    CoroutineScheduler::set_current(lsched);
    EXPECT_EQ(g_root_gone, 1) << "the frame must be destroyed with its scheduler";
    EXPECT_EQ(g_completed, 0);
    EXPECT_EQ(live(), live0);
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0);
}

/**
 * @test The same frame, owned by a task driven by hand, destroyed after the temporary listener: its timer stop
 *       returns on the detached watcher. On the base it read and wrote the freed loop -- a heap-use-after-free in
 *       `ev_timer_stop` under AddressSanitizer.
 */
TEST_F(SchedulerAbandonedFrames, ATaskDestroyedAfterATemporaryLoopItsFrameWaitedOnStopsNothingOnIt) {
    const long        live0  = live();
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    {
        task<void> parked;
        {
            listener tmp;
            parked = sleep_on(tmp.loop());
            parked.handle().resume(); // parks on tmp's loop
            ASSERT_FALSE(parked.handle().done());
        } // ~listener: its loop is destroyed with the frame's timer still started on it
    } // ~task: the frame is destroyed, its timer stopped after its loop
    EXPECT_EQ(g_root_gone, 1);
    EXPECT_EQ(g_completed, 0);
    EXPECT_EQ(live(), live0);
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0);
}

/**
 * @test The listener's teardown DESTROYS a parked frame it owns, so it reports nothing: the report is for a
 *       scheduler destroyed in a refused order, never for the lifecycle the framework drives itself.
 */
TEST_F(SchedulerAbandonedFrames, ListenerTeardownDestroysRatherThanAbandons) {
    const std::size_t before = abandoned_coroutine_frames_total();
    coro_scheduler().spawn(park_for_ever()); // owned by the listener's scheduler
    run_for(1ms);                            // the spawned frame runs to its park
    EXPECT_EQ(coro_scheduler().active_count(), 1u);
    listener::current.reset_coro_scheduler(); // ~CoroutineScheduler, the loop alive: it destroys what it owns
    EXPECT_EQ(g_root_gone, 1);
    EXPECT_EQ(abandoned_coroutine_frames_total(), before) << "a frame the teardown destroyed is not an abandoned one";
}

/**
 * @test A scheduler destroyed while ANOTHER one is current scrubs what its own frames leave behind: a spawned frame it
 *       destroys at its teardown holds a task parked on a timer, registered with it, and that registration goes with the
 *       task -- the teardown reports nothing (Huly QB-1220: the awaiter reaches the scheduler it registered with while
 *       that one lives, and the teardown is the destroying scheduler that ~task scrubs).
 */
TEST_F(SchedulerAbandonedFrames, ATeardownWhileAnotherSchedulerIsCurrentLeavesNothingBehind) {
    auto *const       listener_scheduler = &coro_scheduler();
    const std::size_t before             = abandoned_coroutine_frames_total();
    {
        CoroutineScheduler sched;
        {
            struct rebind { // `sched` current while it runs, the listener's again however this block ends
                ~rebind() {
                    CoroutineScheduler::set_current(listener_scheduler_);
                }
                CoroutineScheduler *listener_scheduler_;
            } const restore{listener_scheduler};
            CoroutineScheduler::set_current(&sched);
            sched.spawn(hold_a_parked_task_then_yield()); // owned by `sched`
            sched.run_ready(1);                           // one resume: it runs to its yield, queued in `sched` again
            ASSERT_EQ(sched.pending_count(), 1u);
            ASSERT_EQ(sched.active_count(), 2u);
        }
    } // ~CoroutineScheduler: destroys the queued spawned frame, and with it the task parked on its timer
    EXPECT_EQ(CoroutineScheduler::current_ptr(), listener_scheduler);
    EXPECT_EQ(abandoned_coroutine_frames_total(), before) << "the registration of a frame the teardown destroyed was left behind";
}
