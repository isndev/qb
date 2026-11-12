/**
 * @file qb/tests/io/unit/coroutine/scheduler-teardown-orders.cpp
 * @brief The orders in which a `CoroutineScheduler` destroys NOTHING (Huly QB-1246): after its thread's event loop,
 *        on another thread than the one that built it, from inside one of its own coroutines, and past sixteen
 *        teardown passes of destructors that kept spawning on it. Each is reported on one line that names the order,
 *        and the coroutines it owned -- a root once, whatever it awaits -- are added to
 *        `abandoned_coroutine_frames_total()`.
 *
 * A scheduler destroys every coroutine it owns when it is destroyed while its thread's loop lives
 * (`scheduler-abandoned-frames.cpp` pins that order). In the four orders below destroying would run code that cannot
 * run: a coroutine local's destructor against a thread whose loop is gone, a frame whose watchers sit on another
 * thread's running loop, the frame that is executing -- `exit()` in a coroutine body ends the thread's listener from
 * inside it -- or an unbounded teardown. Then nothing is destroyed: parked, queued and completed frames alike.
 *
 * Every case but the last runs in a CHILD process (a death test, re-executed): an abandoned coroutine is a leak by
 * design, and an exit order is only reachable by exiting. The last -- a reset nested in the teardown it runs -- leaves
 * nothing behind and runs in process. The judge of an exit-order case is an `atexit` handler registered BEFORE
 * the case builds its objects with static storage, so it runs after their destructors, and after the main thread's
 * thread-locals -- `listener::current` among them -- which `exit()` destroys first. It reads the counters, prints its
 * verdict on stderr and ends the child with `std::_Exit`: no exit-time leak check, the abandoned frames being the
 * point. The death test then holds the child's exit code (the judge's verdict) and the scheduler's report line (the
 * order it names). A judge is never the destructor of a static: one that does not return is MSVC's C4722.
 *
 * What the base read (qb 9053b0c7, before the change) is said case by case: there the scheduler abandoned in every
 * order, counted the SUSPENDED set rather than what it owned, and destroyed its queued frames after the loop.
 *
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace scheduler_teardown_orders_test {

// ---- What a child counts, and how it ends -------------------------------------------------------------------------

int         g_locals_destroyed = 0;     ///< coroutine locals destroyed, wherever and whenever
int         g_late_locals      = 0;     ///< ... once the destructors of the objects with static storage had begun
int         g_exit_locals      = 0;     ///< ... once a coroutine body had called `exit()`
bool        g_statics_ending   = false; ///< set by `statics_ending`: the main thread's thread-locals are gone
bool        g_exiting          = false; ///< set by a coroutine body right before it calls `exit()`
std::size_t g_tally0           = 0;     ///< `abandoned_coroutine_frames_total()` when the case started

/// A coroutine local: what the destruction of its frame runs, and when.
struct counted_local {
    counted_local()                                 = default;
    counted_local(counted_local const &)            = delete;
    counted_local &operator=(counted_local const &) = delete;
    ~counted_local() {
        ++g_locals_destroyed;
        if (g_statics_ending)
            ++g_late_locals;
        if (g_exiting)
            ++g_exit_locals;
    }
};

/// Built AFTER every other object with static storage of a case, so destroyed FIRST among them: from its destructor
/// on, the main thread's `listener::current` -- a thread-local, which `exit()` destroys before any static -- is gone.
struct statics_ending {
    statics_ending()                                  = default;
    statics_ending(statics_ending const &)            = delete;
    statics_ending &operator=(statics_ending const &) = delete;
    ~statics_ending() {
        g_statics_ending = true;
    }
};

[[nodiscard]] std::size_t
tally_moved() noexcept {
    return abandoned_coroutine_frames_total() - g_tally0;
}

/// The end of a child: its verdict is its exit code, with no exit-time leak check (the abandoned frames are the point).
[[noreturn]] void
end_child(bool const ok) noexcept {
    std::fflush(stderr);
    std::_Exit(ok ? 0 : 1);
}

/// A precondition of the case did not hold: its verdict would mean nothing.
[[noreturn]] void
setup_failed(char const *const what) noexcept {
    std::fprintf(stderr, "setup failed: %s\n", what);
    std::fflush(stderr);
    std::_Exit(2);
}

/// `exit(0)` from a coroutine body, marked first so that every local destroyed by the teardown it starts is counted.
void
exit_from_coroutine_body() {
    std::fprintf(stderr, "a coroutine body calls std::exit(0)\n");
    std::fflush(stderr);
    g_exiting = true;
    std::exit(0);
}

// ---- The coroutines ------------------------------------------------------------------------------------------------

/// Parks for ever, registered as SUSPENDED with the current scheduler -- the bookkeeping a watcher awaiter does on
/// suspend -- and holding no watcher.
struct park_registered {
    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    void
    await_suspend(std::coroutine_handle<> h) const {
        CoroutineScheduler::current_ptr()->register_suspended(h);
    }
    void
    await_resume() const noexcept {}
};

task<void>
park_registered_for_ever() {
    counted_local local;
    co_await park_registered{};
}

/// Parks on `recv()`, which registers the frame with no scheduler: what the suspended set never saw.
task<void>
recv_for_ever(channel<int> &ch) {
    counted_local local;
    (void) co_await ch.recv();
}

/// Yields once -- back of the current scheduler's ready queue -- then ends.
task<void>
yield_then_end() {
    counted_local local;
    co_await sleep(0ms);
}

task<void>
sleep_for_ever() {
    counted_local local;
    co_await sleep(std::chrono::hours(1));
}

task<void>
ends_at_once() {
    counted_local local;
    co_return;
}

#if !defined(_WIN32)
task<void>
read_one(int const fd) {
    counted_local local;
    co_await wait_readable(fd);
}
#endif

task<void>
recv_then_exit(channel<int> &ch) {
    counted_local local;
    (void) co_await ch.recv(); // suspends: the sender is resumed after it, in the same drain
    exit_from_coroutine_body();
}

task<void>
send_one(channel<int> &ch) {
    co_await ch.send(7); // a receiver is parked: handed over at once, and this coroutine ends
}

task<void>
child_recv(channel<int> &ch, bool const exit_here) {
    counted_local local;
    (void) co_await ch.recv(); // the handle the sender wakes, and the drain resumes, is THIS frame's: a leaf
    if (exit_here)
        exit_from_coroutine_body();
}

task<void>
parent_of(channel<int> &ch, bool const exit_in_child) {
    counted_local local;
    co_await child_recv(ch, exit_in_child);
    exit_from_coroutine_body(); // resumed by its child's end: the handle the drain resumed is a finished frame by now
}

// ---- The scheduler of a case with static storage -------------------------------------------------------------------

/// Built on its first call -- on the thread that makes it -- and destroyed with the statics, after the judge's
/// registration: on the thread that calls `exit()`.
CoroutineScheduler &
static_scheduler() {
    static CoroutineScheduler sched;
    return sched;
}

// ---- T2: after the thread's loop -----------------------------------------------------------------------------------

void
judge_two_schedulers() {
    const std::size_t moved = tally_moved();
    std::fprintf(stderr, "judge: tally moved by %zu (2 expected), coroutine locals destroyed %d (0 expected)\n", moved, g_locals_destroyed);
    end_child(moved == 2 && g_locals_destroyed == 0);
}

/// Two schedulers with static storage, one root each, destroyed after the main thread's `listener::current`.
[[noreturn]] void
two_static_schedulers_outlive_the_loop() {
    listener::current.clear(); // the main thread's listener exists: exit() destroys it before any static
    g_tally0 = abandoned_coroutine_frames_total();
    std::atexit(judge_two_schedulers); // BEFORE the statics below: it runs after their destructors
    static channel<int>       ch;      // built first: destroyed after both schedulers
    static CoroutineScheduler first;
    static CoroutineScheduler second;
    CoroutineScheduler::set_current(&first);
    first.spawn(recv_for_ever(ch)); // a root parked on recv(): registered with no scheduler
    first.run_ready();
    CoroutineScheduler::set_current(&second);
    second.spawn(park_registered_for_ever()); // a root parked on an awaiter that registers it as suspended
    second.run_ready();
    CoroutineScheduler::set_current(nullptr);
    std::exit(3); // the thread-locals, then the statics: the judge decides the exit code
}

/// Asks a scheduler to destroy its coroutines from its own destructor. Built after that scheduler, so destroyed
/// before it -- and, like every object with static storage, after the main thread's `listener::current`.
struct destroys_all_suspended_late {
    CoroutineScheduler &sched;
    explicit destroys_all_suspended_late(CoroutineScheduler &s) noexcept
        : sched(s) {}
    destroys_all_suspended_late(destroys_all_suspended_late const &)            = delete;
    destroys_all_suspended_late &operator=(destroys_all_suspended_late const &) = delete;
    ~destroys_all_suspended_late() {
        sched.destroy_all_suspended();
        std::fprintf(stderr, "destroy_all_suspended() after the thread's loop: coroutine locals destroyed %d\n", g_locals_destroyed);
        std::fflush(stderr);
    }
};

/// A scheduler with static storage whose owner asks it to destroy its coroutines once the thread's loop is gone.
[[noreturn]] void
destroy_all_suspended_is_asked_after_the_loop() {
    listener::current.clear(); // the main thread's listener exists: exit() destroys it before any static
    g_tally0 = abandoned_coroutine_frames_total();
    std::atexit(judge_two_schedulers);               // BEFORE the statics below: it runs after their destructors
    static channel<int>                ch;           // built first: destroyed last
    static CoroutineScheduler          sched;        // two roots, as the two schedulers above hold between them
    static destroys_all_suspended_late owner{sched}; // built last: destroyed first, after the thread's loop
    CoroutineScheduler::set_current(&sched);
    sched.spawn(recv_for_ever(ch));          // parked on recv(): registered with no scheduler
    sched.spawn(park_registered_for_ever()); // parked on an awaiter that registers it as suspended
    sched.run_ready();
    CoroutineScheduler::set_current(nullptr);
    std::exit(3); // the thread-locals, then the statics: the judge decides the exit code
}

#if !defined(_WIN32)
/// A thread_local built BEFORE its thread's `listener::current`: destroyed after it, at the thread's exit.
struct scheduler_outliving_its_thread_loop {
    channel<int>       ch; // declared first: destroyed after the scheduler
    CoroutineScheduler sched;
};

/// The same order on a worker thread, through a thread_local: two roots, one parked on recv(), one registered.
[[noreturn]] void
a_thread_local_scheduler_outlives_its_thread_loop() {
    g_tally0 = abandoned_coroutine_frames_total();
    std::thread worker([] {
        thread_local scheduler_outliving_its_thread_loop holder; // constructed first on this thread
        listener::current.clear();                               // the thread's listener, constructed second
        CoroutineScheduler::set_current(&holder.sched);
        holder.sched.spawn(recv_for_ever(holder.ch));
        holder.sched.spawn(park_registered_for_ever());
        holder.sched.run_ready();
        CoroutineScheduler::set_current(nullptr);
    }); // the thread returns: its listener::current is destroyed, then `holder`
    worker.join();
    const std::size_t moved = tally_moved();
    std::fprintf(stderr, "judge: tally moved by %zu (2 expected), coroutine locals destroyed %d (0 expected)\n", moved, g_locals_destroyed);
    end_child(moved == 2 && g_locals_destroyed == 0);
}
#endif

// ---- F2: what a scheduler holds QUEUED after the loop --------------------------------------------------------------

void
judge_queued_and_parked() {
    const std::size_t moved = tally_moved();
    std::fprintf(stderr, "judge: coroutine locals destroyed after the loop %d (0 expected), tally moved by %zu (2 expected)\n", g_late_locals,
                 moved);
    end_child(g_late_locals == 0 && moved == 2);
}

/// A scheduler with static storage owns one root sitting in its READY queue (it yielded) and one parked on recv().
[[noreturn]] void
a_static_scheduler_holds_a_queued_frame_after_the_loop() {
    listener::current.clear();
    g_tally0 = abandoned_coroutine_frames_total();
    std::atexit(judge_queued_and_parked);
    static channel<int>   ch; // built before the scheduler: destroyed after it
    CoroutineScheduler   &sched = static_scheduler();
    static statics_ending marker;
    CoroutineScheduler::set_current(&sched);
    sched.spawn(yield_then_end());
    sched.spawn(recv_for_ever(ch));
    sched.run_ready(2); // each runs once: one is back in the ready queue, one is parked on recv()
    if (sched.pending_count() != 1)
        setup_failed("the yielding root is not queued");
    CoroutineScheduler::set_current(nullptr);
    std::exit(3);
}

#if !defined(_WIN32)
void
judge_fired_io_frame() {
    const std::size_t moved = tally_moved();
    std::fprintf(stderr, "judge: coroutine locals destroyed after the loop %d (0 expected), tally moved by %zu (1 expected)\n", g_late_locals,
                 moved);
    end_child(g_late_locals == 0 && moved == 1);
}

/// A scheduler with static storage owns a root whose io watcher FIRED: the frame sits in its ready queue, its awaiter
/// still holding the watcher it armed on the main thread's loop.
[[noreturn]] void
a_static_scheduler_holds_a_frame_whose_watcher_fired() {
    listener::current.clear();
    g_tally0 = abandoned_coroutine_frames_total();
    std::atexit(judge_fired_io_frame);
    CoroutineScheduler   &sched = static_scheduler();
    static statics_ending marker;
    int                   sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || ::write(sv[1], "x", 1) != 1)
        setup_failed("socketpair");
    CoroutineScheduler::set_current(&sched);
    sched.spawn(read_one(sv[0]));
    sched.run_ready();                   // parks: its ev_io is started on the main thread's loop
    listener::current.run(EVRUN_NOWAIT); // the watcher fires: the frame is queued in the CURRENT scheduler
    if (sched.pending_count() != 1)
        setup_failed("the fired frame is not queued");
    CoroutineScheduler::set_current(nullptr);
    std::exit(3);
}
#endif

// ---- Change 2: on another thread -----------------------------------------------------------------------------------

void
judge_foreign_thread() {
    const std::size_t moved = tally_moved();
    std::fprintf(stderr, "judge: coroutine locals destroyed %d (0 expected), tally moved by %zu (1 expected)\n", g_locals_destroyed, moved);
    end_child(g_locals_destroyed == 0 && moved == 1);
}

/// A scheduler with static storage is BUILT on a worker thread, which keeps it and its loop alive; the main thread
/// exits, so the statics -- that scheduler among them -- are destroyed on the main thread.
[[noreturn]] void
a_static_scheduler_built_on_a_worker_goes_on_the_main_thread() {
    g_tally0 = abandoned_coroutine_frames_total();
    std::atexit(judge_foreign_thread);             // before the worker builds the scheduler
    auto *const parked = new std::promise<void>(); // leaked on purpose, as `hold`: the worker outlives every static
    auto *const hold   = new std::promise<void>(); // never set: the worker stays alive, with its loop, until _Exit
    std::thread worker([parked, hold] {
        listener::current.clear();
        CoroutineScheduler &sched = static_scheduler(); // BUILT here, on the worker
        CoroutineScheduler::set_current(&sched);
        sched.spawn(sleep_for_ever());
        sched.run_ready(); // parks on a timer of the worker's loop
        parked->set_value();
        hold->get_future().wait();
    });
    parked->get_future().wait();
    worker.detach();
    std::exit(3);
}

// ---- F6 and change 7a: from inside one of its own coroutines -------------------------------------------------------

void
judge_exit_from_body_two() {
    const std::size_t moved = tally_moved();
    std::fprintf(stderr, "judge: coroutine locals destroyed after exit() %d (0 expected), tally moved by %zu (2 expected)\n", g_exit_locals,
                 moved);
    end_child(g_exit_locals == 0 && moved == 2);
}

void
judge_exit_from_body_three() {
    const std::size_t moved = tally_moved();
    std::fprintf(stderr, "judge: coroutine locals destroyed after exit() %d (0 expected), tally moved by %zu (3 expected)\n", g_exit_locals,
                 moved);
    end_child(g_exit_locals == 0 && moved == 3);
}

/// A root spawned on the thread's scheduler receives from a channel with static storage, then calls `exit(0)` from
/// its body: the drain of that scheduler is on the stack when exit() destroys the thread's listener.
[[noreturn]] void
exit_from_a_coroutine_body() {
    listener::current.clear();
    g_tally0 = abandoned_coroutine_frames_total();
    std::atexit(judge_exit_from_body_two);
    static channel<int> ch; // outlives the main thread's listener
    coro_scheduler().spawn(recv_then_exit(ch));
    coro_scheduler().spawn(send_one(ch));
    coro_scheduler().run_ready(); // the receiver parks, the sender hands it 7 and ends, the receiver exits
    setup_failed("no coroutine exited");
}

/// The same with a parent and its child: `exit_in_child` exits from the leaf the drain resumed, else from the parent
/// that leaf's end resumed. A bystander sleeps beside them.
[[noreturn]] void
exit_from_a_nested_coroutine(bool const exit_in_child) {
    listener::current.clear();
    g_tally0 = abandoned_coroutine_frames_total();
    std::atexit(judge_exit_from_body_three);
    static channel<int> ch;
    coro_scheduler().spawn(parent_of(ch, exit_in_child));
    coro_scheduler().spawn(sleep_for_ever());
    coro_scheduler().spawn(send_one(ch));
    coro_scheduler().run_ready();
    setup_failed("no coroutine exited");
}

// ---- Change 4: the cleanups of tasks with static storage after an exit from a coroutine body -----------------------

int g_task_locals   = 0; ///< locals of the two roots driven by hand
int g_helper_locals = 0; ///< locals of the operations their combinators run (tasks the combinators own)

struct task_local {
    task_local()                              = default;
    task_local(task_local const &)            = delete;
    task_local &operator=(task_local const &) = delete;
    ~task_local() {
        ++g_task_locals;
    }
};
struct helper_local {
    helper_local()                                = default;
    helper_local(helper_local const &)            = delete;
    helper_local &operator=(helper_local const &) = delete;
    ~helper_local() {
        ++g_helper_locals;
    }
};

task<int>
helper_sleeps(qb::duration const d, int const v) {
    helper_local local;
    co_await sleep(d);
    co_return v;
}

task<void>
helper_sleeps_void(qb::duration const d) {
    helper_local local;
    co_await sleep(d);
}

task<void>
any_of_two() {
    task_local local;
    (void) co_await when_any(helper_sleeps(std::chrono::hours(1), 1), helper_sleeps(std::chrono::hours(2), 2));
}

task<void>
timed_out_later() {
    task_local local;
    try {
        co_await coro_with_timeout(helper_sleeps_void(std::chrono::hours(2)), std::chrono::hours(1));
    } catch (...) {
    }
}

task<void>
exits_at_once() {
    exit_from_coroutine_body();
    co_return;
}

void
judge_static_tasks_after_exit() {
    const std::size_t moved = tally_moved();
    std::fprintf(stderr,
                 "judge: locals of the tasks driven by hand destroyed %d (2 expected), tally moved by %zu (4 "
                 "expected); locals of the operations their combinators own destroyed %d\n",
                 g_task_locals, moved, g_helper_locals);
    end_child(g_task_locals == 2 && moved == 4);
}

/// Two tasks with static storage, driven by hand, parked on `when_any` and on `coro_with_timeout`. The frames those
/// combinators spawn to run their operations -- two branches, one runner -- are the thread's scheduler's; the
/// operations themselves are tasks the combinators own. A coroutine of that scheduler then calls `exit(0)`: the
/// scheduler abandons its four roots, and the tasks driven by hand are destroyed with the statics, after the loop --
/// their operations with them, by the combinators that own them -- while their destructors' cleanups of the spawned
/// frames (`cancel_spawned`) find no scheduler and are dropped: a cleanup asked of `coro_scheduler()` would stop the
/// process by name.
[[noreturn]] void
exit_with_static_tasks_parked_on_helpers() {
    listener::current.clear();
    g_tally0 = abandoned_coroutine_frames_total();
    std::atexit(judge_static_tasks_after_exit);
    static task<void> any_task;
    static task<void> timeout_task;
    any_task = any_of_two();
    any_task.handle().resume(); // parks on when_any: two branch frames spawned on the thread's scheduler
    timeout_task = timed_out_later();
    timeout_task.handle().resume(); // parks on coro_with_timeout: its runner spawned there, a timer of its own
    coro_scheduler().spawn(exits_at_once());
    coro_scheduler().run_ready(); // the branches and the runner park on their timers, then the last one exits
    setup_failed("no coroutine exited");
}

// ---- Change 1: destructors that keep spawning ----------------------------------------------------------------------

CoroutineScheduler *g_dying   = nullptr;
int                 g_respawn = 0;

task<void> respawner();

/// A coroutine local whose destructor spawns one more `respawner` on the dying scheduler and pumps it, so the new
/// one parks: every teardown pass leaves one behind. 100 000 generations is "for ever" -- the child stops there, so
/// that an unbounded teardown fails the case instead of hanging it.
struct respawn_in_dtor {
    respawn_in_dtor()                                   = default;
    respawn_in_dtor(respawn_in_dtor const &)            = delete;
    respawn_in_dtor &operator=(respawn_in_dtor const &) = delete;
    ~respawn_in_dtor() {
        if (++g_respawn > 100000) {
            std::fprintf(stderr, "run-away: 100000 generations respawned and the teardown did not end\n");
            std::fflush(stderr);
            std::_Exit(9);
        }
        g_dying->spawn(respawner());
        g_dying->run_ready();
    }
};

task<void>
respawner() {
    respawn_in_dtor r;
    co_await sleep(std::chrono::hours(1));
}

/// A scheduler owned directly, destroyed while the thread's loop lives: each frame it destroys respawns on it.
[[noreturn]] void
destructors_keep_spawning_on_a_dying_scheduler() {
    listener::current.clear();
    g_tally0 = abandoned_coroutine_frames_total();
    {
        CoroutineScheduler sched;
        g_dying = &sched;
        CoroutineScheduler::set_current(&sched);
        sched.spawn(respawner());
        sched.run_ready(); // parks on its timer
    } // ~CoroutineScheduler
    CoroutineScheduler::set_current(nullptr);
    const std::size_t moved = tally_moved();
    std::fprintf(stderr,
                 "judge: the destructor returned after %d generation(s) (16 expected), tally moved by %zu (1 "
                 "expected)\n",
                 g_respawn, moved);
    end_child(g_respawn == 16 && moved == 1);
}

// ---- F10: a static destructor finishes a spawned coroutine ---------------------------------------------------------

/// Resumes, from its destructor, a coroutine spawned on a scheduler with static storage and never run.
struct finisher {
    std::coroutine_handle<> handle{};
    finisher()                            = default;
    finisher(finisher const &)            = delete;
    finisher &operator=(finisher const &) = delete;
    ~finisher() {
        if (handle)
            handle.resume();
    }
};

void
judge_late_finish() {
    const std::size_t moved = tally_moved();
    const bool        bound = CoroutineScheduler::current_ptr() != nullptr;
    std::fprintf(stderr,
                 "judge: the coroutine ran to its end %d (1 expected), tally moved by %zu (1 expected), current "
                 "scheduler %s (none expected)\n",
                 g_locals_destroyed, moved, bound ? "bound" : "none");
    end_child(g_locals_destroyed == 1 && moved == 1 && !bound);
}

[[noreturn]] void
a_static_destructor_finishes_a_spawned_coroutine() {
    listener::current.clear();
    g_tally0 = abandoned_coroutine_frames_total();
    std::atexit(judge_late_finish);
    CoroutineScheduler &sched = static_scheduler(); // built before the finisher: destroyed after it
    static finisher     f;
    f.handle = sched.spawn_tracked(ends_at_once()); // spawned, never run: the finisher's destructor runs it
    std::exit(3);
}

// ---- Coroutines that never ran, whose parameters spawn again -------------------------------------------------------

CoroutineScheduler *g_spawn_target        = nullptr; ///< where a respawning parameter spawns; null: `coro_scheduler()`
int                 g_param_generations   = 0;       ///< frames destroyed whose parameter spawned one more
int                 g_resets_returned     = 0;       ///< nested `reset_coro_scheduler()` calls that returned
long                g_nested_reset_live0  = 0;
int                 g_nested_reset_locals = 0;

struct respawning_parameter;
task<void> never_runs(respawning_parameter p);

/// A coroutine parameter. The frame's copy is moved from the argument, so only that copy is armed: destroyed with a
/// frame that never ran, it spawns one more such coroutine -- never run either -- on the target scheduler. 100 000
/// generations is "for ever": the child stops there, so that an unbounded teardown fails the case instead of hanging.
struct respawning_parameter {
    bool armed             = true;
    respawning_parameter() = default;
    respawning_parameter(respawning_parameter &&other) noexcept
        : armed(std::exchange(other.armed, false)) {}
    respawning_parameter(respawning_parameter const &)            = delete;
    respawning_parameter &operator=(respawning_parameter const &) = delete;
    respawning_parameter &operator=(respawning_parameter &&)      = delete;
    ~respawning_parameter() {
        if (!armed)
            return;
        if (++g_param_generations > 100000) {
            std::fprintf(stderr, "run-away: 100000 generations respawned and the teardown did not end\n");
            std::fflush(stderr);
            std::_Exit(9);
        }
        CoroutineScheduler &target = g_spawn_target ? *g_spawn_target : coro_scheduler();
        target.spawn(never_runs(respawning_parameter{}));
    }
};

task<void>
never_runs(respawning_parameter p) {
    (void) p;
    co_return;
}

/// Each teardown pass destroys two generations: the owned root (`destroy_all_suspended()`), then the one its
/// parameter queued meanwhile (the drain of what is queued when it begins); the one queued last is left. Sixteen
/// passes: 32 destroyed, one abandoned.
[[noreturn]] void
judge_respawning_parameters() {
    const std::size_t moved = tally_moved();
    std::fprintf(stderr,
                 "judge: the teardown returned after destroying %d generation(s) (32 expected: two a pass), tally moved "
                 "by %zu (1 expected)\n",
                 g_param_generations, moved);
    end_child(g_param_generations == 32 && moved == 1);
}

/// A scheduler owned directly, with one spawned coroutine that never ran, destroyed while the thread's loop lives.
[[noreturn]] void
never_run_coroutines_respawn_on_a_dying_scheduler() {
    listener::current.clear();
    g_tally0 = abandoned_coroutine_frames_total();
    {
        CoroutineScheduler sched;
        g_spawn_target = &sched;
        sched.spawn(never_runs(respawning_parameter{})); // queued, never run
    } // ~CoroutineScheduler
    judge_respawning_parameters();
}

/// The same chain on the thread's scheduler, through `coro_scheduler()`, torn down by `reset_coro_scheduler()`.
[[noreturn]] void
never_run_coroutines_respawn_on_the_scheduler_being_reset() {
    listener::current.clear();
    g_tally0       = abandoned_coroutine_frames_total();
    g_spawn_target = nullptr;
    coro_scheduler().spawn(never_runs(respawning_parameter{})); // queued, never run
    listener::current.reset_coro_scheduler();
    judge_respawning_parameters();
}

/// A coroutine local whose destructor resets the thread's scheduler: run by the reset that is destroying it.
struct resets_from_its_destructor {
    resets_from_its_destructor()                                              = default;
    resets_from_its_destructor(resets_from_its_destructor const &)            = delete;
    resets_from_its_destructor &operator=(resets_from_its_destructor const &) = delete;
    ~resets_from_its_destructor() {
        ++g_nested_reset_locals;
        listener::current.reset_coro_scheduler();
        ++g_resets_returned;
    }
};

task<void>
parked_resetting(channel<int> &ch) {
    resets_from_its_destructor local;
    (void) co_await ch.recv();
}

} // namespace scheduler_teardown_orders_test

using namespace scheduler_teardown_orders_test;

/**
 * @test T2. Two schedulers with static storage, destroyed after the main thread's `listener::current`, abandon the
 *       roots they own and count each once: one parked on `recv()`, which registers nothing, and one on an awaiter
 *       that registers it as suspended -- the tally moves by exactly two, neither frame's local is destroyed, and
 *       the line names the order. Base (g++ and clang, sanitized and Release): the tally moved by one -- it counted
 *       the suspended set, so the `recv()` root was never counted -- and the line named no order.
 */
TEST(SchedulerTeardownOrdersDeathTest, ASchedulerDestroyedAfterItsLoopAbandonsItsRootsAndCountsThem) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(two_static_schedulers_outlive_the_loop(), ::testing::ExitedWithCode(0), "destroyed AFTER its thread's event loop");
}

/**
 * @test `destroy_all_suspended()` asked of a scheduler AFTER its thread's loop -- from the destructor of an object
 *       with static storage -- destroys nothing, as the destructor does in that order: both roots are left to the
 *       scheduler's destructor, which abandons them, counts them and names the order. Until the fix, and on the
 *       base: it destroyed both on a thread that can run nothing any more -- two locals destroyed, nothing counted,
 *       no line.
 */
TEST(SchedulerTeardownOrdersDeathTest, DestroyAllSuspendedAskedAfterTheLoopDestroysNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(destroy_all_suspended_is_asked_after_the_loop(), ::testing::ExitedWithCode(0), "destroyed AFTER its thread's event loop");
}

#if !defined(_WIN32)
/**
 * @test T2, the thread_local order (Linux and macOS: MSVC builds every thread's `listener::current` at the thread's
 *       start, before any thread_local of a function). A scheduler held by a thread_local built before its thread's
 *       listener is destroyed after it, at the thread's exit: both roots abandoned and counted, none destroyed.
 *       Base: the tally moved by one, as above.
 */
TEST(SchedulerTeardownOrdersDeathTest, AThreadLocalSchedulerBuiltBeforeItsThreadsListenerAbandonsItsRoots) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_thread_local_scheduler_outlives_its_thread_loop(), ::testing::ExitedWithCode(0), "destroyed AFTER its thread's event loop");
}
#endif

/**
 * @test F2. A scheduler destroyed after the loop destroys NOTHING it holds, queued frames included: a root sitting in
 *       its ready queue and one parked on `recv()` are both abandoned (tally +2) and no coroutine local is destroyed
 *       after the loop. Base: the destructor's drain destroyed the queued frame after the loop (one local) and the
 *       tally did not move.
 */
TEST(SchedulerTeardownOrdersDeathTest, AFrameQueuedInAStaticSchedulerIsNotDestroyedAfterTheLoop) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_scheduler_holds_a_queued_frame_after_the_loop(), ::testing::ExitedWithCode(0),
                "destroyed AFTER its thread's event loop");
}

#if !defined(_WIN32)
/**
 * @test F2, with a watcher: the queued frame's io watcher fired on the main thread's loop before the exit. Nothing
 *       is destroyed after the loop, the tally moves by one. Base: the destructor's drain destroyed the frame, whose
 *       awaiter stopped its watcher on the freed loop -- heap-use-after-free in `ev_io_stop` (g++ and clang under
 *       AddressSanitizer), SIGSEGV in a g++ Release build.
 */
TEST(SchedulerTeardownOrdersDeathTest, AFrameWhoseWatcherFiredIsNotDestroyedAfterTheLoop) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_scheduler_holds_a_frame_whose_watcher_fired(), ::testing::ExitedWithCode(0),
                "destroyed AFTER its thread's event loop");
}
#endif

/**
 * @test Change 2. A scheduler with static storage built on a worker thread -- alive, with its loop -- is destroyed
 *       by `exit()` on the main thread: it destroys nothing (its coroutine waits on the worker's loop) and the line
 *       says ON ANOTHER THREAD. Base: abandoned too (tally +1, nothing destroyed), but the line named no order.
 */
TEST(SchedulerTeardownOrdersDeathTest, ASchedulerDestroyedOnAnotherThreadAbandonsItsRoots) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_scheduler_built_on_a_worker_goes_on_the_main_thread(), ::testing::ExitedWithCode(0),
                "destroyed ON ANOTHER THREAD than the one that built it");
}

/**
 * @test F6. `exit(0)` from a coroutine body, under its scheduler's `run_ready()` drain: the thread's scheduler,
 *       destroyed from inside one of its own coroutines, destroys nothing -- no local of the executing frame or of
 *       the finished sender -- and says so; the channel with static storage is then destroyed with no report.
 *       Base: the listener's teardown destroyed the executing frame under it, then `~channel` read it --
 *       heap-use-after-free under AddressSanitizer (g++ and clang); in a g++ Release build one local destroyed after
 *       the exit and nothing counted.
 */
TEST(SchedulerTeardownOrdersDeathTest, ExitFromACoroutineBodyDestroysNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exit_from_a_coroutine_body(), ::testing::ExitedWithCode(0), "destroyed FROM INSIDE one of its own coroutines");
}

/**
 * @test Change 7a. The drain resumes the LEAF a waker queued, never its root: `exit(0)` from a child awaited by a
 *       spawned root destroys nothing (three roots abandoned: the parent, the sleeping bystander, the finished
 *       sender). Base: heap-use-after-free in `~channel` (g++ and clang); in a g++ Release build three locals
 *       destroyed after the exit.
 */
TEST(SchedulerTeardownOrdersDeathTest, ExitFromANestedCoroutineDestroysNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exit_from_a_nested_coroutine(true), ::testing::ExitedWithCode(0), "destroyed FROM INSIDE one of its own coroutines");
}

/**
 * @test Change 7a. `exit(0)` from the parent its child's end resumed: the handle the drain resumed is a finished
 *       frame by then, so excluding "the resumed frame" could not work; nothing is destroyed. Base:
 *       heap-use-after-free (in `coroutine_handle::done()` with g++, in `~task` with clang), SIGSEGV in a g++ Release
 *       build.
 */
TEST(SchedulerTeardownOrdersDeathTest, ExitFromAParentResumedByItsChildDestroysNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exit_from_a_nested_coroutine(false), ::testing::ExitedWithCode(0), "destroyed FROM INSIDE one of its own coroutines");
}

/**
 * @test Change 4. After `exit(0)` from a coroutine body, two tasks with static storage parked on `when_any` and on
 *       `coro_with_timeout` are destroyed by their tasks, after the loop: the four coroutines the thread's scheduler
 *       owned (two branches, a runner, the one that exited) are abandoned and counted, the tasks' cleanups of them
 *       are dropped, and the child reaches its judge. Base: heap-use-after-free in `ev_timer_stop` (g++ and clang);
 *       in a g++ Release build no report, nothing counted.
 */
TEST(SchedulerTeardownOrdersDeathTest, ExitDropsTheCleanupsOfStaticTasksParkedOnHelpers) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exit_with_static_tasks_parked_on_helpers(), ::testing::ExitedWithCode(0), "destroyed FROM INSIDE one of its own coroutines");
}

/**
 * @test Change 1. A coroutine local whose destructor spawns on the dying scheduler and pumps it at every pass: the
 *       teardown is bounded -- the destructor returns after sixteen passes, the one coroutine left is abandoned and
 *       counted, and the line says its destructors kept spawning. An unbounded repeat stops at the run-away guard
 *       (exit 9). Base: the destructor destroyed no parked frame (no generation), counted the one it held as
 *       suspended, and the line named no order.
 */
TEST(SchedulerTeardownOrdersDeathTest, DestructorsThatKeepSpawningEndTheTeardownAfterSixteenPasses) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(destructors_keep_spawning_on_a_dying_scheduler(), ::testing::ExitedWithCode(0),
                "destroyed while its coroutines' destructors kept spawning on it");
}

/**
 * @test F10. A static destructor resumes, after the loop, a coroutine that only finishes: it runs to its end, the
 *       hand-over of its finished frame is dropped -- the frame stays with the scheduler that owns it, which counts
 *       it (tally +1) -- and no scheduler is bound. Base: a scheduler was built inside the destroyed listener to take
 *       the frame (current bound) and the tally did not move.
 */
TEST(SchedulerTeardownOrdersDeathTest, AStaticDestructorThatFinishesASpawnedCoroutineLeavesItsFrameToItsScheduler) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_destructor_finishes_a_spawned_coroutine(), ::testing::ExitedWithCode(0), "destroyed AFTER its thread's event loop");
}

/**
 * @test A spawned coroutine that never ran, whose parameter's destructor spawns another such coroutine on the dying
 *       scheduler: each drain of the teardown takes only what is queued when it begins, so the sixteen passes bound
 *       the chain -- two generations a pass, 32 destroyed -- and the one left is abandoned, counted, and reported
 *       with the line of the destructors that kept spawning. Base: the destructor's drain ran until the queue was
 *       empty, which it never was -- the child reached the run-away guard (100 000 generations, exit 9), g++ and
 *       clang sanitized and Release, MSVC Release.
 */
TEST(SchedulerTeardownOrdersDeathTest, NeverRunCoroutinesWhoseParametersRespawnEndTheTeardownAfterSixteenPasses) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(never_run_coroutines_respawn_on_a_dying_scheduler(), ::testing::ExitedWithCode(0),
                "destroyed while its coroutines' destructors kept spawning on it");
}

/**
 * @test The same chain on the thread's scheduler, each generation spawned through `coro_scheduler()` -- the dying
 *       scheduler, which the thread's record still names -- torn down by `listener::current.reset_coro_scheduler()`:
 *       the reset returns after 32 generations, the last one abandoned and reported. Base (every build): the reset
 *       returned after two generations, nothing counted -- the chain moved on to a scheduler `coro_scheduler()` built
 *       in its place meanwhile.
 */
TEST(SchedulerTeardownOrdersDeathTest, NeverRunCoroutinesWhoseParametersRespawnEndAResetAfterSixteenPasses) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(never_run_coroutines_respawn_on_the_scheduler_being_reset(), ::testing::ExitedWithCode(0),
                "destroyed while its coroutines' destructors kept spawning on it");
}

/**
 * @test A coroutine local whose destructor calls `listener::current.reset_coro_scheduler()`, run by the reset that is
 *       destroying the thread's scheduler: the nested reset finds the scheduler dying and does nothing, the outer one
 *       completes -- the frame destroyed once, nothing left, nothing reported. In process: nothing leaks. Base: the
 *       nested reset freed the scheduler under the outer one's `destroy_all_suspended()` -- heap-use-after-free there
 *       under AddressSanitizer (g++ and clang); unseen in a g++ Release build and on MSVC Release, where the freed
 *       scheduler is read in silence and the case passes.
 */
TEST(SchedulerTeardownOrders, AResetFromInsideTheTeardownItRunsDoesNothing) {
    listener::current.clear();
    g_resets_returned        = 0;
    g_nested_reset_locals    = 0;
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    g_nested_reset_live0     = detail::CoroutineFrameAllocator::live_frames;
    {
        channel<int> ch; // outlives the frame parked on it
        coro_scheduler().spawn(parked_resetting(ch));
        (void) coro_scheduler().run_ready(); // parks on recv()
        ASSERT_EQ(g_nested_reset_locals, 0) << "the coroutine did not park";
        listener::current.reset_coro_scheduler(); // destroys the frame, whose local resets again
        EXPECT_EQ(g_nested_reset_locals, 1) << "the frame was not destroyed exactly once";
        EXPECT_EQ(g_resets_returned, 1) << "the nested reset did not return";
        EXPECT_FALSE(listener::current.has_coro_scheduler()) << "a scheduler was left, or built, by the nested reset";
    }
    EXPECT_EQ(detail::CoroutineFrameAllocator::live_frames, g_nested_reset_live0) << "a frame was left behind";
    EXPECT_EQ(abandoned_coroutine_frames_total(), tally0) << "the reset reported an abandon";
    listener::current.clear();
}
