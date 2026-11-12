/**
 * @file qb/tests/io/unit/async/listener-after-loop.cpp
 * @brief What reaches a thread's `listener::current` after its destructor (Huly QB-1246): from the destructor of an
 *        object with static storage -- the statics are destroyed after the main thread's thread-locals -- or of a
 *        thread_local built before the listener. A wake or a hand-over is DROPPED, a query answers "nothing", a
 *        request that needs the loop STOPS the process on one line that names it, and a coroutine frame or an io
 *        object destroyed there stops no watcher on the freed loop.
 *
 * Until 3.4 every such reach read the destroyed listener: a wake bound a scheduler built inside it (since 3.3.0),
 * `sleep()` armed a timer on its freed loop (a crash in `ev_now_update`), a frame parked on a watcher stopped it on
 * the freed loop (heap-use-after-free; in a g++ Release build a static task parked on `wait_readable()` was a SIGSEGV
 * at exit), and the rest read freed members in silence -- the listener's storage is the thread's, still mapped, so
 * only a sanitizer that knows the object is dead can see those reads.
 *
 * That sanitizer is the lane guard: in an AddressSanitizer build the thread's listener is POISONED once its
 * destructor has completed (`QB_LISTENER_ASAN`). Several cases below are silent on the base without it and red only
 * under it; each says which. Its own control is here too: a read of a member that is not guarded on purpose
 * (`has_work()`, on a core's every pass) must be reported, and threads built one after another must never see a dead
 * thread's poison.
 *
 * Every case runs in a CHILD process (a death test, re-executed). A named stop is `EXPECT_DEATH` on the line. A drop
 * or a "nothing" ends the child through a JUDGE: an `atexit` handler registered BEFORE the case builds its objects
 * with static storage, so it runs after their destructors; it reads what they recorded, prints its verdict on stderr
 * and ends the child with `std::_Exit` (no exit-time leak check: a frame driven by hand and never destroyed is part
 * of several cases). A judge is never the destructor of a static: one that does not return is MSVC's C4722.
 *
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

// An AddressSanitizer build, detected as `listener.h` detects it for the lane guard: gcc and MSVC define
// __SANITIZE_ADDRESS__, clang answers __has_feature. Detected HERE, not read from `QB_LISTENER_ASAN`, so that the
// guard's control exists, and fails, in a sanitized build of a tree without the guard.
#if defined(__SANITIZE_ADDRESS__)
#define LISTENER_AFTER_LOOP_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LISTENER_AFTER_LOOP_ASAN 1
#endif
#endif

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace listener_after_loop_test {

// ---- What a child records, and how it ends -------------------------------------------------------------------------

int  g_root_locals    = 0;     ///< locals of the root of a case destroyed
int  g_root_late      = 0;     ///< ... once the destructors of the objects with static storage had begun
int  g_resumed        = 0;     ///< coroutines of a case that went past their suspension
bool g_statics_ending = false; ///< set by `statics_ending`: the main thread's thread-locals are gone
bool g_returned       = false; ///< the request a static destructor made returned to it

/// A local of the root coroutine of a case.
struct root_local {
    root_local()                              = default;
    root_local(root_local const &)            = delete;
    root_local &operator=(root_local const &) = delete;
    ~root_local() {
        ++g_root_locals;
        if (g_statics_ending)
            ++g_root_late;
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

/// The end of a child: its verdict is its exit code.
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

/// Drives the frames a combinator spawned on the thread's scheduler to their own park, if it has any.
void
drain_thread_scheduler() {
    if (listener::current.has_coro_scheduler())
        (void) listener::current.coro_scheduler().run_ready();
}

// ---- The coroutines ------------------------------------------------------------------------------------------------

task<void>
recv_on(channel<int> &ch) {
    root_local local;
    (void) co_await ch.recv();
    ++g_resumed;
}

task<void>
lock_on(async_mutex &m) {
    root_local local;
    co_await m.lock();
    ++g_resumed;
    m.unlock();
}

task<void>
cancellable_sleeper(cancellation_token token) {
    root_local local;
    (void) co_await cancellable_sleep(std::chrono::hours(1), token);
    ++g_resumed;
}

task<void>
sleep_for_ever() {
    root_local local;
    co_await sleep(std::chrono::hours(1));
    ++g_resumed;
}

task<void>
yield_then_end() {
    root_local local;
    co_await sleep(0ms);
    ++g_resumed;
}

/// Yields with `yield_or_cancel()`: an awaiter that asks the CURRENT scheduler to queue it, and no loop.
task<void>
yield_to_the_scheduler() {
    root_local local;
    co_await yield_or_cancel(cancellation_token{});
    ++g_resumed;
}

task<void>
ends_at_once() {
    root_local local;
    co_return;
}

/// A helper or a nested child: no local of the root.
task<void>
plain_sleep(qb::duration const d) {
    co_await sleep(d);
}

task<int>
plain_sleep_int(qb::duration const d, int const v) {
    co_await sleep(d);
    co_return v;
}

// ---- T3 and its kin: a wake after the loop binds nothing -----------------------------------------------------------

void
judge_nothing_bound() {
    const bool bound = CoroutineScheduler::current_ptr() != nullptr;
    std::fprintf(stderr, "judge: current scheduler %s (none expected), coroutines resumed %d (0 expected)\n", bound ? "bound" : "none",
                 g_resumed);
    end_child(!bound && g_resumed == 0);
}

/// A channel with static storage, a receiver parked on it -- a task driven by hand and never destroyed -- and the
/// channel's destructor closes it at exit, after the main thread's listener.
[[noreturn]] void
a_static_channel_closes_on_a_parked_receiver() {
    listener::current.clear();
    std::atexit(judge_nothing_bound);
    static channel<int> ch;
    auto *const         receiver = new task<void>(recv_on(ch)); // never destroyed: still parked at exit
    receiver->handle().resume();
    std::exit(3);
}

channel<int> *g_heap_channel = nullptr;

/// The same wake from an `atexit` handler: it runs after the thread-locals, and before the judge, registered first.
[[noreturn]] void
an_atexit_handler_closes_a_channel_with_a_receiver() {
    listener::current.clear();
    std::atexit(judge_nothing_bound);
    g_heap_channel       = new channel<int>();
    auto *const receiver = new task<void>(recv_on(*g_heap_channel)); // never destroyed
    receiver->handle().resume();
    std::atexit([] { g_heap_channel->close(); });
    std::exit(3);
}

cancellation_token &
static_token() {
    static cancellation_token token;
    return token;
}

/// Cancels, from its destructor, the token a task with static storage waits on.
struct canceller {
    canceller()                             = default;
    canceller(canceller const &)            = delete;
    canceller &operator=(canceller const &) = delete;
    ~canceller() {
        static_token().cancel();
    }
};

/// A static destructor cancels, after the loop, the token a task with static storage (driven by hand) waits on in
/// `cancellable_sleep`: the cancel hook is a cleanup and a wake.
[[noreturn]] void
a_static_destructor_cancels_a_token_a_static_task_waits_on() {
    listener::current.clear();
    std::atexit(judge_nothing_bound);
    (void) static_token();     // built first: destroyed after the task
    static task<void> sleeper; // destroyed after the canceller ran
    static canceller  cancel_at_exit;
    sleeper = cancellable_sleeper(static_token());
    sleeper.handle().resume();
    std::exit(3);
}

#if !defined(_WIN32)
CoroutineScheduler *g_tls_current_after = nullptr;
bool                g_tls_closed        = false;

/// A thread_local built BEFORE its thread's `listener::current`: destroyed after it, at the thread's exit. Its
/// channel is closed there, on a parked receiver, and it records what that wake bound.
struct channel_outliving_its_thread_loop {
    std::unique_ptr<channel<int>> ch;
    channel_outliving_its_thread_loop()                                                     = default;
    channel_outliving_its_thread_loop(channel_outliving_its_thread_loop const &)            = delete;
    channel_outliving_its_thread_loop &operator=(channel_outliving_its_thread_loop const &) = delete;
    ~channel_outliving_its_thread_loop() {
        ch.reset(); // closes it: its receiver is woken, after this thread's listener
        g_tls_current_after = CoroutineScheduler::current_ptr();
        g_tls_closed        = true;
    }
};

[[noreturn]] void
a_thread_local_channel_closes_after_its_thread_loop() {
    std::thread worker([] {
        thread_local channel_outliving_its_thread_loop holder; // constructed before this thread's listener
        listener::current.clear();
        (void) coro_scheduler(); // the thread's scheduler, bound: its listener's destructor unbinds it
        holder.ch            = std::make_unique<channel<int>>();
        auto *const receiver = new task<void>(recv_on(*holder.ch)); // never destroyed
        receiver->handle().resume();
    });
    worker.join();
    std::fprintf(stderr, "judge: channel closed %d, then current scheduler %s (none expected), coroutines resumed %d\n",
                 static_cast<int>(g_tls_closed), g_tls_current_after ? "bound" : "none", g_resumed);
    end_child(g_tls_closed && g_tls_current_after == nullptr && g_resumed == 0);
}
#endif

// ---- The controls: no listener, no loop -----------------------------------------------------------------------------

void
judge_static_receiver() {
    const bool bound = CoroutineScheduler::current_ptr() != nullptr;
    std::fprintf(stderr, "judge: the receiver's frame destroyed %d (1 expected), current scheduler %s (none expected)\n", g_root_locals,
                 bound ? "bound" : "none");
    end_child(g_root_locals == 1 && !bound);
}

/// A task with static storage parked on `recv()` of a channel with static storage built before it: the task goes
/// first, its awaiter leaves the channel's list, and the channel goes with nobody to wake.
[[noreturn]] void
a_static_task_parked_on_recv() {
    listener::current.clear();
    std::atexit(judge_static_receiver);
    static channel<int> ch;
    static task<void>   receiver;
    receiver = recv_on(ch);
    receiver.handle().resume();
    std::exit(3);
}

/// A mutex with static storage, held, with a waiter -- a task driven by hand and never destroyed -- parked on it.
[[noreturn]] void
a_static_mutex_with_a_parked_waiter() {
    listener::current.clear();
    std::atexit(judge_nothing_bound);
    static async_mutex m;
    if (!m.try_lock())
        setup_failed("try_lock");
    auto *const waiter = new task<void>(lock_on(m)); // never destroyed
    waiter->handle().resume();
    std::exit(3);
}

// ---- T4: a task with static storage, per watcher family ------------------------------------------------------------

enum class family {
    sleep,
#if !defined(_WIN32)
    wait_readable,
#endif
    recv_for,
    send_for,
    coro_with_timeout,
    when_any,
    when_all,
    make_cancellable,
    with_deadline,
    nested,
};

constexpr family kEveryFamily[] = {
    family::sleep,
#if !defined(_WIN32)
    family::wait_readable,
#endif
    family::recv_for,      family::send_for,         family::coro_with_timeout, family::when_any,
    family::when_all,      family::make_cancellable, family::with_deadline,     family::nested,
};

std::string
family_name(::testing::TestParamInfo<family> const &info) {
    switch (info.param) {
        case family::sleep:
            return "sleep";
#if !defined(_WIN32)
        case family::wait_readable:
            return "wait_readable";
#endif
        case family::recv_for:
            return "recv_for";
        case family::send_for:
            return "send_for";
        case family::coro_with_timeout:
            return "coro_with_timeout";
        case family::when_any:
            return "when_any";
        case family::when_all:
            return "when_all";
        case family::make_cancellable:
            return "make_cancellable";
        case family::with_deadline:
            return "with_deadline";
        case family::nested:
            return "nested";
    }
    return "unknown";
}

/// What a task waits on: built before it, so destroyed after it.
struct kept {
    channel<int>       ch{0}; // capacity 0: a send with no receiver parks
    cancellation_token token;
#if !defined(_WIN32)
    int pair[2] = {-1, -1};
#endif
};

task<void>
root_sleep() {
    root_local local;
    co_await sleep(std::chrono::hours(1));
}

#if !defined(_WIN32)
task<void>
root_wait_readable(int const fd) {
    root_local local;
    co_await wait_readable(fd);
}
#endif

task<void>
root_recv_for(channel<int> &ch) {
    root_local local;
    (void) co_await ch.recv_for(std::chrono::hours(1));
}

task<void>
root_send_for(channel<int> &ch) {
    root_local local;
    (void) co_await ch.send_for(1, std::chrono::hours(1));
}

task<void>
root_coro_with_timeout() {
    root_local local;
    try {
        co_await coro_with_timeout(plain_sleep(std::chrono::hours(2)), std::chrono::hours(1));
    } catch (...) {
    }
}

task<void>
root_when_any() {
    root_local local;
    (void) co_await when_any(plain_sleep_int(std::chrono::hours(1), 1), plain_sleep_int(std::chrono::hours(2), 2));
}

task<void>
root_when_all() {
    root_local local;
    (void) co_await when_all(plain_sleep_int(std::chrono::hours(1), 1), plain_sleep_int(std::chrono::hours(2), 2));
}

task<void>
root_make_cancellable(cancellation_token token) {
    root_local local;
    try {
        co_await make_cancellable(plain_sleep(std::chrono::hours(1)), std::move(token));
    } catch (...) {
    }
}

task<void>
root_with_deadline() {
    root_local local;
    try {
        co_await with_deadline(plain_sleep(std::chrono::hours(2)), std::chrono::steady_clock::now() + std::chrono::hours(1));
    } catch (...) {
    }
}

task<void>
root_nested() {
    root_local local;
    co_await plain_sleep(std::chrono::hours(1)); // a child task the root owns, parked on a timer
}

task<void>
make_root(family const f, kept &k) {
    switch (f) {
        case family::sleep:
            return root_sleep();
#if !defined(_WIN32)
        case family::wait_readable:
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, k.pair) != 0)
                setup_failed("socketpair");
            return root_wait_readable(k.pair[0]);
#endif
        case family::recv_for:
            return root_recv_for(k.ch);
        case family::send_for:
            return root_send_for(k.ch);
        case family::coro_with_timeout:
            return root_coro_with_timeout();
        case family::when_any:
            return root_when_any();
        case family::when_all:
            return root_when_all();
        case family::make_cancellable:
            return root_make_cancellable(k.token);
        case family::with_deadline:
            return root_with_deadline();
        case family::nested:
            return root_nested();
    }
    setup_failed("unknown family");
}

void
judge_static_task() {
    std::fprintf(stderr, "judge: the task's frame destroyed after the loop %d (1 expected)\n", g_root_late);
    end_child(g_root_locals == 1 && g_root_late == 1);
}

/// A task with static storage, driven by hand, parked on one watcher family; what it waits on outlives it.
[[noreturn]] void
a_static_task_parked_on(family const f) {
    listener::current.clear();
    std::atexit(judge_static_task);
    static kept           k; // built before the task: destroyed after it
    static task<void>     t;
    static statics_ending marker;
    t = make_root(f, k);
    t.handle().resume();
    drain_thread_scheduler(); // what a combinator spawned parks on its own watcher too
    if (t.done())
        setup_failed("the task did not park");
    std::exit(3);
}

class ListenerAfterLoopFamilyDeathTest : public ::testing::TestWithParam<family> {};

// ---- T5: an io object ----------------------------------------------------------------------------------------------

int g_io_destroyed = 0;

/// An io object: a registered `ev_timer` on the thread's listener, through `async::base`.
struct idle_timer : with_timeout<idle_timer> {
    idle_timer()
        : with_timeout<idle_timer>(std::chrono::hours(1)) {}
    idle_timer(idle_timer const &)            = delete;
    idle_timer &operator=(idle_timer const &) = delete;
    ~idle_timer() {
        ++g_io_destroyed;
    }
    void
    on(event::timer &) noexcept {}
};

void
judge_io_object() {
    std::fprintf(stderr, "judge: the io object destroyed after the loop %d (1 expected)\n", g_io_destroyed);
    end_child(g_io_destroyed == 1);
}

[[noreturn]] void
an_io_object_with_static_storage() {
    listener::current.clear();
    std::atexit(judge_io_object);
    static idle_timer t; // registered on the main thread's listener, which goes first
    std::exit(3);
}

// ---- The requests a static destructor makes after the loop ---------------------------------------------------------

void (*g_request)() = nullptr;

/// Makes `g_request` from its destructor: after the main thread's `listener::current`.
struct requester {
    requester()                             = default;
    requester(requester const &)            = delete;
    requester &operator=(requester const &) = delete;
    ~requester() {
        g_request();
    }
};

/// The main thread's listener and its loop exist; `setup` runs while they live; `request` is made by a static
/// destructor after they are gone; `judge`, when there is one, reads the outcome after every static.
[[noreturn]] void
request_after_the_loop(void (*const setup)(), void (*const request)(), void (*const judge)()) {
    (void) listener::current.loop(); // exit() destroys the listener and its loop before any static
    if (judge)
        std::atexit(judge);
    g_request = request;
    static requester at_exit;
    if (setup)
        setup();
    std::exit(0);
}

// The doors: each request below needs the loop or the thread's scheduler, and stops by name.

void
request_a_callback() {
    qb::io::async::callback([] {}, 10s);
}

void
request_run() {
    (void) qb::io::async::run(EVRUN_NOWAIT);
}

void
request_the_loop() {
    (void) listener::current.loop();
}

void
request_run_once() {
    (void) run_once();
}

void
request_run_until() {
    const bool keep_running = false;
    (void) run_until(keep_running);
}

void
request_run_for() {
    run_for(1ms);
}

void
request_a_spawn() {
    coro_scheduler().spawn(ends_at_once());
}

/// Resumes, from its destructor, a coroutine never started: it runs after the loop, to its first suspension.
struct resumer {
    task<void> t;
    resumer()                           = default;
    resumer(resumer const &)            = delete;
    resumer &operator=(resumer const &) = delete;
    ~resumer() {
        if (t && !t.done())
            t.handle().resume();
        (void) t.detach(); // the frame is left: this is about the resume, not about its destruction
    }
};

/// A static destructor resumes a coroutine that goes to `co_await sleep(...)`.
[[noreturn]] void
a_static_destructor_resumes_a_coroutine_that_sleeps() {
    (void) listener::current.loop();
    static resumer r;
    r.t = sleep_for_ever();
    std::exit(0);
}

/// The same, with a scheduler with static storage left bound as the current one: the scheduler's doors are not asked,
/// the loop's is.
[[noreturn]] void
a_static_destructor_resumes_a_coroutine_with_a_scheduler_bound() {
    (void) listener::current.loop();
    static CoroutineScheduler sched; // built before the resumer: destroyed after it
    static resumer            r;
    r.t = sleep_for_ever();
    CoroutineScheduler::set_current(&sched); // and LEFT bound: the listener's destructor unbinds only its own
    std::exit(0);
}

/// Resumes, from its destructor, a coroutine spawned on a scheduler with static storage and never run.
struct handle_resumer {
    std::coroutine_handle<> handle{};
    handle_resumer()                                  = default;
    handle_resumer(handle_resumer const &)            = delete;
    handle_resumer &operator=(handle_resumer const &) = delete;
    ~handle_resumer() {
        if (handle)
            handle.resume();
    }
};

/// A static destructor resumes a spawned coroutine that only yields (`sleep(0ms)`).
[[noreturn]] void
a_static_destructor_resumes_a_coroutine_that_yields() {
    (void) listener::current.loop();
    static CoroutineScheduler sched; // built before the resumer: destroyed after it
    static handle_resumer     r;
    r.handle = sched.spawn_tracked(yield_then_end()); // spawned, never run: the destructor runs it
    std::exit(0);
}

/// The same with `yield_or_cancel()`: no scheduler is bound, so its suspension asks for the thread's.
[[noreturn]] void
a_static_destructor_resumes_a_coroutine_that_yields_to_its_scheduler() {
    (void) listener::current.loop();
    static CoroutineScheduler sched; // built before the resumer: destroyed after it
    static handle_resumer     r;
    r.handle = sched.spawn_tracked(yield_to_the_scheduler()); // spawned, never run, never bound as current
    std::exit(0);
}

// The drops and the "nothing" answers.

void
judge_returned() {
    std::fprintf(stderr, "judge: the request after the loop returned %d (1 expected)\n", static_cast<int>(g_returned));
    end_child(g_returned);
}

int g_closure_ran      = 0;
int g_closure_released = 0;

void
request_a_defer() {
    std::shared_ptr<int> token(new int(0), [](int *p) {
        delete p;
        ++g_closure_released;
    });
    qb::io::async::defer([token = std::move(token)] { ++g_closure_ran; });
    g_returned = true;
}

void
judge_defer() {
    std::fprintf(stderr, "judge: the deferred closure ran %d (0 expected), released %d (1 expected)\n", g_closure_ran, g_closure_released);
    end_child(g_returned && g_closure_ran == 0 && g_closure_released == 1);
}

void
request_a_clear() {
    listener::current.clear();
    g_returned = true;
}

void
request_a_break() {
    break_parent();
    g_returned = true;
}

void
request_a_poll_interval() {
    listener::current.set_io_poll_interval(1ms);
    g_returned = true;
}

int g_answer = -1;

void
make_the_thread_scheduler() {
    (void) coro_scheduler();
}

void
ask_for_a_scheduler() {
    g_answer = listener::current.has_coro_scheduler() ? 1 : 0;
}

void
judge_no_scheduler() {
    std::fprintf(stderr, "judge: has_coro_scheduler() answered %d (0 expected)\n", g_answer);
    end_child(g_answer == 0);
}

std::size_t g_processed_before = 0;
std::size_t g_processed_after  = 1;

void
process_one_event() {
    bool waiting = true;
    qb::io::async::callback([&waiting] { waiting = false; }, 1ms);
    (void) run_until(waiting);
    g_processed_before = listener::current.total_events_processed();
}

void
ask_for_the_events_processed() {
    g_processed_after = listener::current.total_events_processed();
}

void
judge_no_events() {
    std::fprintf(stderr,
                 "judge: events processed while the loop lived %zu (1 or more expected), after it %zu (0 "
                 "expected)\n",
                 g_processed_before, g_processed_after);
    end_child(g_processed_before > 0 && g_processed_after == 0);
}

/// The owner of a registered timer: what `registerEvent` dispatches to.
struct timer_owner {
    void
    on(event::timer &) noexcept {}
};
timer_owner             g_timer_owner;
IRegisteredKernelEvent *g_wrapper = nullptr;

void
register_a_timer() {
    auto &timer = listener::current.registerEvent<event::timer>(g_timer_owner);
    timer.start(3600.);
    g_wrapper = timer._interface; // `clear()`, in the listener's destructor, detaches it and leaves it to its owner
}

void
unregister_the_detached_timer() {
    listener::current.unregisterEvent(g_wrapper);
    g_returned = true;
}

// The census of the other members: each query is asked while the loop lives, its answer made non-zero by the setup,
// then from a static destructor once the listener is gone.

/// `dispatch_timing()`, as one number: zero exactly when every field of the stats is zero.
std::uint64_t
dispatch_timing_fields() {
    const qb::TimingStats t = listener::current.dispatch_timing();
    return t.count + static_cast<std::uint64_t>(t.total.count() != 0) + static_cast<std::uint64_t>(t.last.count() != 0)
           + static_cast<std::uint64_t>(t.max.count() != 0) + static_cast<std::uint64_t>(t.recent_max.count() != 0);
}

void
time_one_dispatch() {
    listener::current.set_dispatch_timing(true);
    process_one_event();
}

void
defer_nothing() {
    listener::current.defer([] {});
}

void
arm_the_wake() {
    listener::current.arm_wake();
}

void
poll_every_millisecond() {
    listener::current.set_io_poll_interval(1ms);
}

/// One query: what makes its answer non-zero while the loop lives, and the question.
struct census_query {
    char const *name;
    void (*setup)();
    std::uint64_t (*ask)();
};

const census_query kQueries[] = {
    {"size", register_a_timer, [] { return static_cast<std::uint64_t>(listener::current.size()); }},
    {"has_deferred", defer_nothing, [] { return static_cast<std::uint64_t>(listener::current.has_deferred()); }},
    {"is_wake_armed", arm_the_wake, [] { return static_cast<std::uint64_t>(listener::current.is_wake_armed()); }},
    {"io_poll_interval_ticks", poll_every_millisecond, [] { return listener::current.io_poll_interval_ticks(); }},
    {"backend", nullptr, [] { return static_cast<std::uint64_t>(listener::current.backend()); }},
    {"dispatch_timing", time_one_dispatch, dispatch_timing_fields},
};

census_query const *g_query         = nullptr;
std::uint64_t       g_answer_before = 0;
std::uint64_t       g_answer_after  = 1;

void
set_the_query_up() {
    if (g_query->setup)
        g_query->setup();
    g_answer_before = g_query->ask();
}

void
ask_the_query() {
    g_answer_after = g_query->ask();
}

void
judge_the_query() {
    std::fprintf(stderr, "judge: %s answered %llu while the loop lived, %llu after it (0 expected)\n", g_query->name,
                 static_cast<unsigned long long>(g_answer_before), static_cast<unsigned long long>(g_answer_after));
    end_child(g_answer_before != 0 && g_answer_after == 0);
}

[[noreturn]] void
a_query_after_the_loop(census_query const &q) {
    g_query = &q;
    request_after_the_loop(set_the_query_up, ask_the_query, judge_the_query);
}

class ListenerAfterLoopQueryDeathTest : public ::testing::TestWithParam<census_query> {};

std::string
query_name(::testing::TestParamInfo<census_query> const &info) {
    return info.param.name;
}

void
request_a_disarm() {
    listener::current.disarm_wake();
    g_returned = true;
}

void
request_a_recycle() {
    listener::current.recycle_coro_scheduler();
    g_returned = true;
}

void
request_a_reset() {
    listener::current.reset_coro_scheduler();
    g_returned = true;
}

void
request_dispatch_timing() {
    listener::current.set_dispatch_timing(true);
    g_returned = true;
}

void
request_an_armed_wake() {
    listener::current.arm_wake();
}

// ---- T6: the lane guard's own control ------------------------------------------------------------------------------

#ifdef LISTENER_AFTER_LOOP_ASAN
/// Reads, from its destructor, a member a core calls on every pass and that is not guarded on purpose.
struct has_work_reader {
    has_work_reader()                                   = default;
    has_work_reader(has_work_reader const &)            = delete;
    has_work_reader &operator=(has_work_reader const &) = delete;
    ~has_work_reader() {
        std::fprintf(stderr, "a static destructor reads listener::current.has_work() after its destructor\n");
        std::fflush(stderr);
        const bool work = listener::current.has_work();
        std::fprintf(stderr, "it read %d and nothing reported it\n", static_cast<int>(work));
    }
};

[[noreturn]] void
read_the_listener_after_its_destructor() {
    listener::current.clear();
    static has_work_reader reader;
    std::exit(0);
}

void
sleep_parked_unwatched() {
    coro_scheduler().spawn(plain_sleep(std::chrono::hours(1)));
    (void) coro_scheduler().run_ready(); // parked on a timer: its scheduler destroys it at the thread's exit
}

/// Threads built one after another, each with its own `listener::current` poisoned at its exit: 300 in a row, then
/// 50 rounds of 4 alive together, each of those with a coroutine parked when it exits.
[[noreturn]] void
threads_one_after_another() {
    long sum = 0;
    for (int i = 0; i < 300; ++i) {
        std::thread th([&sum] {
            listener::current.clear();
            sum += listener::current.has_coro_scheduler() ? 1 : 2;
        });
        th.join();
    }
    for (int round = 0; round < 50; ++round) {
        long                     part[4] = {0, 0, 0, 0};
        std::vector<std::thread> threads;
        for (int k = 0; k < 4; ++k)
            threads.emplace_back([&part, k] {
                sleep_parked_unwatched();
                part[k] = listener::current.has_coro_scheduler() ? 1 : 2;
            });
        for (auto &th : threads)
            th.join();
        sum += part[0] + part[1] + part[2] + part[3];
    }
    std::fprintf(stderr, "judge: 300 threads one after the other and 50 rounds of 4: sum %ld (800 expected)\n", sum);
    end_child(sum == 800);
}
#endif

} // namespace listener_after_loop_test

using namespace listener_after_loop_test;

// ======================================================================================================== T3: wakes

/**
 * @test T3. A channel with static storage, closed by its destructor at exit on a parked receiver, wakes nobody and
 *       binds nothing: the wake is dropped, no scheduler is bound, the receiver never runs. Base (g++ and clang,
 *       sanitized and Release): a scheduler was built inside the destroyed listener and bound, the wake queued in it
 *       (since 3.3.0) -- the judge read `current` bound.
 */
TEST(ListenerAfterLoopDeathTest, AStaticChannelClosedAtExitWakesNobodyAndBindsNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_channel_closes_on_a_parked_receiver(), ::testing::ExitedWithCode(0), "judge: current scheduler none");
}

/**
 * @test The same wake from an `atexit` handler, after the thread-locals: dropped, nothing bound. Base: `current`
 *       bound, as above.
 */
TEST(ListenerAfterLoopDeathTest, AnAtexitHandlerThatClosesAChannelBindsNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(an_atexit_handler_closes_a_channel_with_a_receiver(), ::testing::ExitedWithCode(0), "judge: current scheduler none");
}

/**
 * @test A static destructor cancels, after the loop, the token a task with static storage waits on: the cancel hook's
 *       wake is dropped and nothing is bound. Base: `current` bound.
 */
TEST(ListenerAfterLoopDeathTest, ACancelAfterTheLoopBindsNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_destructor_cancels_a_token_a_static_task_waits_on(), ::testing::ExitedWithCode(0), "judge: current scheduler none");
}

#if !defined(_WIN32)
/**
 * @test The thread_local order (Linux and macOS: MSVC builds every thread's `listener::current` at the thread's
 *       start): a channel held by a thread_local built before its thread's listener closes after it, at the thread's
 *       exit; the wake binds nothing on that thread. Base: `current` bound on that thread.
 */
TEST(ListenerAfterLoopDeathTest, AThreadLocalChannelClosedAfterItsThreadsLoopBindsNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_thread_local_channel_closes_after_its_thread_loop(), ::testing::ExitedWithCode(0), "then current scheduler none");
}
#endif

/**
 * @test A control: a task with static storage parked on `recv()` of a channel built before it reaches no listener and
 *       no loop -- its frame is destroyed by its task, the channel then has nobody to wake. Green on the base as on
 *       the tip, the lane guard on or off: what would make it red is a touch of the listener, which the guard reports.
 */
TEST(ListenerAfterLoopDeathTest, AStaticTaskParkedOnRecvReachesNoListener) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_task_parked_on_recv(), ::testing::ExitedWithCode(0), "judge: the receiver's frame destroyed 1");
}

/**
 * @test A control: a mutex with static storage destroyed with a waiter parked on it wakes nobody -- green on the base
 *       too, the lane guard on or off.
 */
TEST(ListenerAfterLoopDeathTest, AStaticMutexWithAParkedWaiterWakesNobody) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_mutex_with_a_parked_waiter(), ::testing::ExitedWithCode(0), "judge: current scheduler none");
}

// ========================================================================================= T4: frames, T5: io objects

/**
 * @test T4. A task with static storage, driven by hand and parked on one watcher family, is destroyed after the main
 *       thread's listener by its task: its awaiter stops a watcher that the destroyed loop detached, so nothing
 *       touches freed memory and the child reaches its judge. Base: heap-use-after-free in every family under
 *       AddressSanitizer, g++ and clang (`ev_io_stop` for `wait_readable`, `ev_timer_stop` for the nine others). In a
 *       g++ Release build of the base the `wait_readable` family is a SIGSEGV at exit; the nine others read the freed
 *       loop in silence and pass there.
 */
TEST_P(ListenerAfterLoopFamilyDeathTest, ATaskWithStaticStorageParkedOnItIsDestroyedCleanAfterTheLoop) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_static_task_parked_on(GetParam()), ::testing::ExitedWithCode(0), "judge: the task's frame destroyed after the loop 1");
}

INSTANTIATE_TEST_SUITE_P(EveryWatcherFamily, ListenerAfterLoopFamilyDeathTest, ::testing::ValuesIn(kEveryFamily), family_name);

/**
 * @test T5. An io object with static storage is destroyed after the loop: its registration, which `clear()` detached
 *       in the listener's destructor, is released without reaching the destroyed listener. Green on the base, which
 *       read the destroyed listener's list in silence (its storage is the thread's, still mapped); red on the base
 *       with the lane guard laid over it alone: use-after-poison in `listener::_unlink` (g++ `-O0` and clang).
 */
TEST(ListenerAfterLoopDeathTest, AnIoObjectWithStaticStorageIsReleasedAfterTheLoop) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(an_io_object_with_static_storage(), ::testing::ExitedWithCode(0), "judge: the io object destroyed after the loop 1");
}

// =============================================================================================== the doors: a request

/**
 * @test A static destructor resumes a coroutine that goes to `co_await sleep(...)`: the loop is asked for, and the
 *       request stops by name. Base: the destroyed listener handed out a null loop -- UBSan's member access within
 *       a null `ev_loop` in `ev_now_update` (g++ and clang), a crash with no line in a g++ Release build.
 */
TEST(ListenerAfterLoopDeathTest, ACoroutineThatSleepsAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(a_static_destructor_resumes_a_coroutine_that_sleeps(),
                 "qb: listener::current\\.loop\\(\\) was requested after that thread's event loop");
}

/**
 * @test The same with a scheduler with static storage left bound as the current one: the loop's door stops it.
 *       Base: as above, a null `ev_loop` under UBSan, a crash with no line in Release.
 */
TEST(ListenerAfterLoopDeathTest, ACoroutineThatSleepsWithAStaticSchedulerBoundStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(a_static_destructor_resumes_a_coroutine_with_a_scheduler_bound(),
                 "qb: listener::current\\.loop\\(\\) was requested after that thread's event loop");
}

/**
 * @test A static destructor resumes a spawned coroutine that only yields with `sleep(0ms)`: `sleep()` asks for the
 *       loop before the coroutine suspends, and that door stops it, on its own request text. Base: exit 0
 *       everywhere, the yield queued in a scheduler built inside the destroyed listener.
 */
TEST(ListenerAfterLoopDeathTest, ACoroutineThatYieldsAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(a_static_destructor_resumes_a_coroutine_that_yields(),
                 "qb: listener::current\\.loop\\(\\) was requested after that thread's event loop");
}

/**
 * @test The other door of a coroutine's own suspension: one that yields with `yield_or_cancel()` -- no loop asked
 *       for, no scheduler bound -- asks for the thread's scheduler (`detail::bind_thread_scheduler()`), which stops
 *       it on ITS request text: the door behind it, the member `coro_scheduler()`, would stop it on another line.
 *       Base: exit 0 everywhere, the yield queued in a scheduler built inside the destroyed listener.
 */
TEST(ListenerAfterLoopDeathTest, ACoroutineThatYieldsToItsSchedulerAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(a_static_destructor_resumes_a_coroutine_that_yields_to_its_scheduler(),
                 "qb: its thread's scheduler \\(a coroutine suspending, yielding or spawning\\) was requested after that "
                 "thread's event loop");
}

/**
 * @test `coro_scheduler().spawn(...)` from a static destructor: the thread's scheduler is asked for, and stops.
 *       Base: exit 0 everywhere, `current` bound to a scheduler built inside the destroyed listener.
 */
TEST(ListenerAfterLoopDeathTest, ASpawnAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(request_after_the_loop(nullptr, request_a_spawn, nullptr),
                 "qb: coro_scheduler\\(\\) was requested after that thread's event loop");
}

/**
 * @test `async::callback(fn, 10s)` from a static destructor registers a timer: stops by name. Base: a null `ev_loop`
 *       under UBSan; in a g++ Release build glibc aborted on a corrupted heap ("malloc(): unsorted double linked list
 *       corrupted").
 */
TEST(ListenerAfterLoopDeathTest, ACallbackAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(request_after_the_loop(nullptr, request_a_callback, nullptr),
                 "qb: listener::current\\.registerEvent\\(\\) .*was requested after that thread's event loop");
}

/**
 * @test `async::run(EVRUN_NOWAIT)` from a static destructor: stops by name. Base: heap-use-after-free in
 *       `listener::_loop_has_pending()` (the freed loop's counters); exit 0 in a g++ Release build.
 */
TEST(ListenerAfterLoopDeathTest, ARunAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(request_after_the_loop(nullptr, request_run, nullptr), "qb: async::run\\(\\) was requested after that thread's event loop");
}

/**
 * @test `listener::current.loop()` from a static destructor: stops by name. Base: exit 0 everywhere (a null loop
 *       handed out).
 */
TEST(ListenerAfterLoopDeathTest, TheLoopAskedForAfterItStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(request_after_the_loop(nullptr, request_the_loop, nullptr),
                 "qb: listener::current\\.loop\\(\\) was requested after that thread's event loop");
}

/**
 * @test `async::run_once()` from a static destructor: stops by name. Base: heap-use-after-free in
 *       `listener::_loop_has_pending()`; exit 0 in a g++ Release build.
 */
TEST(ListenerAfterLoopDeathTest, ARunOnceAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(request_after_the_loop(nullptr, request_run_once, nullptr),
                 "qb: async::run_once\\(\\) was requested after that thread's event loop");
}

/**
 * @test `async::run_until(...)` from a static destructor -- even with nothing to wait for: stops by name. Base: exit
 *       0 everywhere.
 */
TEST(ListenerAfterLoopDeathTest, ARunUntilAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(request_after_the_loop(nullptr, request_run_until, nullptr),
                 "qb: async::run_until\\(\\) was requested after that thread's event loop");
}

/**
 * @test `run_for(...)` from a static destructor: stops by name. Base: heap-use-after-free in
 *       `listener::_loop_has_pending()`; a crash with no line in a g++ Release build.
 */
TEST(ListenerAfterLoopDeathTest, ARunForAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(request_after_the_loop(nullptr, request_run_for, nullptr), "qb: run_for\\(\\) was requested after that thread's event loop");
}

// ============================================================================ the drops and the "nothing" answers

/**
 * @test `async::defer(fn)` from a static destructor: dropped -- the closure is destroyed (its captured state
 *       released) and never run. Base: pushed into the destroyed listener's queue -- heap-use-after-free under
 *       AddressSanitizer (g++ and clang); in a g++ Release build never run and never released.
 */
TEST(ListenerAfterLoopDeathTest, ADeferAfterTheLoopIsDroppedAndReleased) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(nullptr, request_a_defer, judge_defer), ::testing::ExitedWithCode(0),
                "judge: the deferred closure ran 0 \\(0 expected\\), released 1");
}

/**
 * @test `listener::current.clear()` from a static destructor: nothing to do -- the destructor cleared it. Green on
 *       the base, which cleared the destroyed object in silence; red there with the lane guard laid over it alone
 *       (use-after-poison in `listener::clear()`).
 */
TEST(ListenerAfterLoopDeathTest, AClearAfterTheLoopDoesNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(nullptr, request_a_clear, judge_returned), ::testing::ExitedWithCode(0),
                "judge: the request after the loop returned 1");
}

/**
 * @test `async::break_parent()` from a static destructor: no run to break, dropped. Base: a null `ev_loop` under
 *       UBSan (g++ and clang); green in a g++ Release build.
 */
TEST(ListenerAfterLoopDeathTest, ABreakAfterTheLoopIsDropped) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(nullptr, request_a_break, judge_returned), ::testing::ExitedWithCode(0),
                "judge: the request after the loop returned 1");
}

/**
 * @test `listener::current.set_io_poll_interval(1ms)` from a static destructor: dropped. Green on the base (a write
 *       into the destroyed object nothing sees); red there with the lane guard laid over it alone (use-after-poison in
 *       `set_io_poll_interval`).
 */
TEST(ListenerAfterLoopDeathTest, APollIntervalAfterTheLoopIsDropped) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(nullptr, request_a_poll_interval, judge_returned), ::testing::ExitedWithCode(0),
                "judge: the request after the loop returned 1");
}

/**
 * @test `listener::current.has_coro_scheduler()` from a static destructor, the listener having had one: false.
 *       Green on the base, whose destroyed object read false too; red there with the lane guard laid over it alone
 *       (use-after-poison in `has_coro_scheduler`).
 */
TEST(ListenerAfterLoopDeathTest, HasCoroSchedulerAnswersFalseAfterTheLoop) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(make_the_thread_scheduler, ask_for_a_scheduler, judge_no_scheduler), ::testing::ExitedWithCode(0),
                "judge: has_coro_scheduler\\(\\) answered 0");
}

/**
 * @test `listener::current.total_events_processed()` from a static destructor, the loop having processed one: 0.
 *       Base: the destroyed object's count (2), sanitized and Release.
 */
TEST(ListenerAfterLoopDeathTest, TotalEventsProcessedAnswersZeroAfterTheLoop) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(process_one_event, ask_for_the_events_processed, judge_no_events), ::testing::ExitedWithCode(0),
                "after it 0 \\(0 expected\\)");
}

/**
 * @test `listener::current.unregisterEvent(wrapper)` of a registration the listener's `clear()` detached: freed
 *       without reading the destroyed listener. Green on the base, which read the destroyed listener's list first in
 *       silence; red there with the lane guard laid over it alone (use-after-poison in `listener::_unlink`).
 */
TEST(ListenerAfterLoopDeathTest, UnregisteringADetachedRegistrationAfterTheLoopReachesNoListener) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(register_a_timer, unregister_the_detached_timer, judge_returned), ::testing::ExitedWithCode(0),
                "judge: the request after the loop returned 1");
}

/**
 * @test The census of the queries after the loop: `size()`, `has_deferred()`, `is_wake_armed()`,
 *       `io_poll_interval_ticks()`, `backend()` and `dispatch_timing()` answer "nothing" (zero, false, all-zero stats)
 *       once the listener is gone, each having answered non-zero while its loop lived. Base: `io_poll_interval_ticks`
 *       and `dispatch_timing` answered what the destroyed object held (sanitized, Release, MSVC); `backend` read a null
 *       loop (UBSan with g++ and clang; 1 on MSVC Release; 0, green, in a g++ Release build); `size`, `has_deferred`
 *       and `is_wake_armed` answered right from the destroyed object -- green there, red with the lane guard laid
 *       over it alone (use-after-poison in each member).
 */
TEST_P(ListenerAfterLoopQueryDeathTest, AQueryAfterTheLoopAnswersNothing) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(a_query_after_the_loop(GetParam()), ::testing::ExitedWithCode(0), "while the loop lived, 0 after it \\(0 expected\\)");
}

INSTANTIATE_TEST_SUITE_P(EveryQuery, ListenerAfterLoopQueryDeathTest, ::testing::ValuesIn(kQueries), query_name);

/**
 * @test `listener::current.disarm_wake()` from a static destructor, the wake having been armed: dropped -- the
 *       destructor disarmed it. Green on the base (the destroyed object read "not armed"); red there with the lane
 *       guard laid over it alone (use-after-poison in `disarm_wake()`).
 */
TEST(ListenerAfterLoopDeathTest, ADisarmAfterTheLoopIsDropped) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(arm_the_wake, request_a_disarm, judge_returned), ::testing::ExitedWithCode(0),
                "judge: the request after the loop returned 1");
}

/**
 * @test `listener::current.recycle_coro_scheduler()` from a static destructor, the listener having had a scheduler:
 *       dropped -- the destructor destroyed it. Green on the base (it read a null scheduler from the destroyed object);
 *       red there with the lane guard laid over it alone (use-after-poison in `recycle_coro_scheduler()`).
 */
TEST(ListenerAfterLoopDeathTest, ARecycleAfterTheLoopIsDropped) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(make_the_thread_scheduler, request_a_recycle, judge_returned), ::testing::ExitedWithCode(0),
                "judge: the request after the loop returned 1");
}

/**
 * @test `listener::current.reset_coro_scheduler()` from a static destructor, the listener having had a scheduler:
 *       dropped. Green on the base (a null scheduler read from the destroyed object); red there with the lane guard
 *       laid over it alone (use-after-poison in `reset_coro_scheduler()`).
 */
TEST(ListenerAfterLoopDeathTest, AResetAfterTheLoopIsDropped) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(make_the_thread_scheduler, request_a_reset, judge_returned), ::testing::ExitedWithCode(0),
                "judge: the request after the loop returned 1");
}

/**
 * @test `listener::current.set_dispatch_timing(true)` from a static destructor: a setting, dropped. Base: a null
 *       `ev_loop` under UBSan (g++ and clang); green in a g++ Release build; on MSVC Release the request never
 *       returned to its destructor.
 */
TEST(ListenerAfterLoopDeathTest, DispatchTimingSetAfterTheLoopIsDropped) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(request_after_the_loop(nullptr, request_dispatch_timing, judge_returned), ::testing::ExitedWithCode(0),
                "judge: the request after the loop returned 1");
}

/**
 * @test `listener::current.arm_wake()` from a static destructor: the wake needs the loop, and stops by name. Base:
 *       heap-use-after-free in `evpipe_init` on the freed loop (g++ and clang); a crash with no line in a g++ Release
 *       build and on MSVC Release.
 */
TEST(ListenerAfterLoopDeathTest, AWakeArmedAfterTheLoopStopsByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(request_after_the_loop(nullptr, request_an_armed_wake, nullptr),
                 "qb: listener::current\\.arm_wake\\(\\) was requested after that thread's event loop");
}

#ifdef LISTENER_AFTER_LOOP_ASAN
// ===================================================================================== T6: the lane guard, controlled

/**
 * @test T6. In an AddressSanitizer build a read of `listener::current` after its destructor is reported: a static
 *       destructor calls `has_work()` -- a member a core calls on every pass, deliberately not guarded -- and dies of a
 *       use-after-poison. clang and MSVC report it at every optimisation level; g++ only at `-O0`, where the access
 *       is not folded into a direct reference to the thread-local. Base (no guard): heap-use-after-free on the freed
 *       loop's counters (`_loop_has_work()`), never a use-after-poison.
 */
TEST(ListenerAfterLoopDeathTest, TheLaneGuardReportsAReadOfTheListenerAfterItsDestructor) {
#if defined(__GNUC__) && !defined(__clang__) && defined(__OPTIMIZE__)
    GTEST_SKIP() << "g++ folds an optimised access to a thread-local into a direct reference AddressSanitizer does not "
                    "check: the guard is asserted at -O0 with g++, and at every level with clang and MSVC";
#else
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(read_the_listener_after_its_destructor(), "use-after-poison");
#endif
}

/**
 * @test The guard's other side: threads built one after another -- 300 in a row, then 50 rounds of 4 alive together,
 *       each of those with a coroutine parked at its exit -- never see the poison a dead thread's listener left.
 *       Green on the base too: it is the guard's false-positive control, red only if a guard poisoned a block a later
 *       thread reuses without that thread's storage being made addressable again.
 */
TEST(ListenerAfterLoopDeathTest, ThreadsBuiltOneAfterAnotherNeverSeeADeadThreadsPoison) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(threads_one_after_another(), ::testing::ExitedWithCode(0), "sum 800 \\(800 expected\\)");
}
#endif
