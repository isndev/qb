// ch_w.cpp -- SCRATCH witnesses of the challenger (design-coro-ch). Never committed.
//
//   ch_w <case>
//
//   tmp_sched     a scheduler owned directly owns a frame parked on a TEMPORARY listener's loop (an awaiter built by hand
//                 on that loop); the temporary listener is destroyed first, then the scheduler -- the thread's own
//                 listener::current is alive throughout
//   tmp_task      the same frame, owned by a task driven by hand, destroyed after the temporary listener
//   ready_late    a scheduler with static storage (destroyed after the main thread's listener) owns one frame sitting in
//                 its READY queue (woken, not resumed) and one parked on recv(): which destructors of coroutine locals run
//                 after the loop, and what is counted
//   reent_spawn   ~CoroutineScheduler, loop alive: a frame local's destructor spawns on the dying scheduler
//   reent_pump    ...: a frame local's destructor spawns on the dying scheduler and pumps it once (the new frame parks)
//   reent_wake    ...: a frame's destruction releases a mutex guard, which wakes a sibling the same scheduler owns
//   reent_cancel  ...: a frame local's destructor calls cancel_spawned() on a sibling of the same scheduler
//   exit_in_coro  a coroutine spawned on the listener's scheduler calls std::exit(0) after a co_await that suspended
//   late_finish   a static destructor finishes a spawned coroutine by hand after the main thread's listener (D3's door:
//                 final_suspend -> defer_frame_destruction -> CoroutineScheduler::current())
//   late_yield    a static destructor resumes a coroutine that only yields (sleep(0ms)): no loop is needed for that

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace ch_w {

int g_dtors      = 0;
int g_completed  = 0;
int g_late_dtors = 0; // destructors of coroutine locals that ran while the thread's loop was already gone

bool
loop_gone() {
#ifdef CH_HAS_RECORD
    return detail::thread_loop_gone();
#else
    return false;
#endif
}

struct local {
    char const *name;
    explicit local(char const *n)
        : name(n) {}
    ~local() {
        ++g_dtors;
        if (loop_gone())
            ++g_late_dtors;
        std::fprintf(stderr, "[ch_w] coroutine local '%s' destroyed (thread loop gone: %d)\n", name, (int) loop_gone());
    }
};

long
live() {
    return detail::CoroutineFrameAllocator::live_frames;
}

void
checkpoint(char const *what, std::size_t tally0, long live0) {
    std::fprintf(stderr, "[ch_w] %s: frame_dtors=%d late_dtors=%d completed=%d live_frames%+ld tally+%zu current=%p\n", what, g_dtors,
                 g_late_dtors, g_completed, live() - live0, abandoned_coroutine_frames_total() - tally0,
                 (void *) CoroutineScheduler::current_ptr());
}

// ---- tmp_* -------------------------------------------------------------------------------------------------------
task<void>
sleep_on(ev::loop_ref loop) {
    local l{"sleep_on"};
    co_await timer_awaiter{5s, loop}; // built by hand on the loop it is given, as awaiter.h documents
    ++g_completed;
}

// ---- ready_late --------------------------------------------------------------------------------------------------
task<void>
yield_then_end() {
    local l{"yield_then_end"};
    co_await sleep(0ms); // back of the current scheduler's ready queue
    ++g_completed;
}
task<void>
recv_on(channel<int> &ch) {
    local l{"recv_on"};
    (void) co_await ch.recv();
    ++g_completed;
}
channel<int> &
static_channel() {
    static channel<int> ch;
    return ch;
}
CoroutineScheduler &
static_scheduler() {
    static CoroutineScheduler s;
    return s;
}
struct judge {
    std::size_t tally0;
    long        live0;
    ~judge() {
        checkpoint("judge (last static destroyed)", tally0, live0);
    }
};

// ---- reent_* -----------------------------------------------------------------------------------------------------
CoroutineScheduler *g_dying = nullptr;
channel<int>       *g_ch    = nullptr;

struct spawn_in_dtor {
    bool pump;
    ~spawn_in_dtor() {
        std::fprintf(stderr, "[ch_w] a frame local's destructor spawns on the dying scheduler%s\n", pump ? " and pumps it" : "");
        g_dying->spawn(recv_on(*g_ch));
        if (pump)
            g_dying->run_ready();
    }
};
task<void>
parked_with(spawn_in_dtor guard) {
    co_await sleep(5s);
    (void) guard;
}
task<void>
hold_lock_and_sleep(async_mutex &m) {
    local l{"holder"};
    auto  g = co_await m.scoped_lock();
    co_await sleep(5s);
    ++g_completed;
}
task<void>
wait_lock(async_mutex &m) {
    local l{"waiter"};
    auto  g = co_await m.scoped_lock();
    ++g_completed;
}
std::coroutine_handle<> g_sibling{};
struct cancel_in_dtor {
    ~cancel_in_dtor() {
        std::fprintf(stderr, "[ch_w] a frame local's destructor cancels a sibling of the dying scheduler\n");
        g_dying->cancel_spawned(g_sibling);
    }
};
task<void>
parked_cancelling() {
    cancel_in_dtor c;
    local          l{"canceller"};
    co_await sleep(5s);
}
task<void>
sleeper(char const *name) {
    local l{name};
    co_await sleep(5s);
    ++g_completed;
}

#if !defined(_WIN32)
task<void>
read_ready(int fd) {
    local l{"read_ready"};
    co_await wait_readable(fd);
    ++g_completed;
}
#endif

// ---- exit_in_coro ------------------------------------------------------------------------------------------------
task<void>
recv_then_exit(channel<int> &ch) {
    local l{"recv_then_exit"};
    auto  v = co_await ch.recv(); // suspends: the sender comes later
    std::fprintf(stderr, "[ch_w] the coroutine got %d and calls std::exit(0) from its body\n", v ? *v : -1);
    std::exit(0);
}
task<void>
send_one(channel<int> &ch) {
    co_await sleep(1ms);
    co_await ch.send(7);
}

// ---- late_* ------------------------------------------------------------------------------------------------------
task<void>
ends_at_once() {
    local l{"ends_at_once"};
    ++g_completed;
    co_return;
}

} // namespace ch_w

int
main(int argc, char **argv) {
    using namespace ch_w;
    if (argc < 2) {
        std::fprintf(stderr, "usage: ch_w <case>\n");
        return 2;
    }
    const std::string c = argv[1];
    listener::current.clear();
    auto *const       lsched = &coro_scheduler();
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    const long        live0  = live();

    if (c == "tmp_sched") {
        {
            CoroutineScheduler sched;
            CoroutineScheduler::set_current(&sched);
            {
                listener tmp; // a second listener on this thread; it creates no scheduler
                sched.spawn(sleep_on(tmp.loop()));
                sched.run_ready();
                checkpoint("parked on the temporary listener's loop", tally0, live0);
            } // ~listener tmp: its loop is freed; the frame's timer is still "active" in it
            std::fprintf(stderr, "[ch_w] the temporary listener is gone; the scheduler goes now\n");
        }
        CoroutineScheduler::set_current(lsched);
        checkpoint("after the scheduler", tally0, live0);
    } else if (c == "tmp_task") {
        {
            task<void> t;
            {
                listener tmp;
                t = sleep_on(tmp.loop());
                t.handle().resume();
            }
            std::fprintf(stderr, "[ch_w] the temporary listener is gone; the task goes now\n");
        }
        checkpoint("after the task", tally0, live0);
    } else if (c == "ready_late") {
        static judge j{tally0, live0}; // built first: destroyed last
        (void) static_channel();
        CoroutineScheduler::set_current(&static_scheduler());
        static_scheduler().spawn(yield_then_end());
        static_scheduler().spawn(recv_on(static_channel()));
        static_scheduler().run_ready(2); // each runs once: one is back in the READY queue, one is parked on recv()
        std::fprintf(stderr, "[ch_w] static scheduler: pending=%zu active=%zu\n", static_scheduler().pending_count(),
                     static_scheduler().active_count());
        CoroutineScheduler::set_current(lsched);
    } else if (c == "reent_spawn" || c == "reent_pump") {
        channel<int> ch;
        g_ch = &ch;
        {
            CoroutineScheduler sched;
            g_dying = &sched;
            CoroutineScheduler::set_current(&sched);
            sched.spawn(parked_with(spawn_in_dtor{c == "reent_pump"}));
            sched.run_ready();
            checkpoint("one frame parked", tally0, live0);
        }
        CoroutineScheduler::set_current(lsched);
        checkpoint("after the scheduler (loop alive)", tally0, live0);
        ch.close();
        run_for(5ms);
        checkpoint("after the channel closed and the loop ran", tally0, live0);
    } else if (c == "reent_wake") {
        async_mutex m;
        {
            CoroutineScheduler sched;
            CoroutineScheduler::set_current(&sched);
            sched.spawn(hold_lock_and_sleep(m));
            sched.spawn(wait_lock(m));
            sched.run_ready();
            checkpoint("holder parked on sleep, waiter parked on the mutex", tally0, live0);
        }
        CoroutineScheduler::set_current(lsched);
        checkpoint("after the scheduler (loop alive)", tally0, live0);
        std::fprintf(stderr, "[ch_w] mutex locked after both frames went: %d\n", (int) m.is_locked());
    } else if (c == "reent_cancel") {
        {
            CoroutineScheduler sched;
            g_dying = &sched;
            CoroutineScheduler::set_current(&sched);
            sched.spawn(parked_cancelling());
            g_sibling = sched.spawn_tracked(sleeper("sibling"));
            sched.run_ready();
            checkpoint("two frames parked", tally0, live0);
        }
        CoroutineScheduler::set_current(lsched);
        checkpoint("after the scheduler (loop alive)", tally0, live0);
    } else if (c == "exit_in_coro") {
        static channel<int> ch; // outlives the main thread's listener
        coro_scheduler().spawn(recv_then_exit(ch));
        coro_scheduler().spawn(send_one(ch));
        run_for(200ms);
        std::fprintf(stderr, "[ch_w] NOT REACHED: the coroutine did not exit\n");
        return 4;
    } else if (c == "late_finish" || c == "late_yield") {
        struct resumer {
            std::coroutine_handle<> h{};
            ~resumer() {
                std::fprintf(stderr, "[ch_w] a static destructor resumes a spawned coroutine after the main thread's listener\n");
                h.resume();
                std::fprintf(stderr, "[ch_w] it returned\n");
            }
        };
        static judge j{tally0, live0};
        (void) static_scheduler(); // built before the resumer: destroyed after it
        static resumer r;
        // Spawned on a scheduler with static storage, never run: the static destructor above runs it by hand.
        r.h = static_scheduler().spawn_tracked(c == "late_finish" ? ends_at_once() : yield_then_end());
    } else if (c == "ready_io_late") {
#if !defined(_WIN32)
        // A scheduler with static storage owns a frame whose io watcher FIRED: the frame sits in its ready queue, the
        // watcher still active in the loop. Destroyed after the main thread's listener, by the destructor's drain.
        static judge j{tally0, live0};
        int          sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || ::write(sv[1], "x", 1) != 1)
            return 3;
        CoroutineScheduler::set_current(&static_scheduler());
        static_scheduler().spawn(read_ready(sv[0]));
        static_scheduler().run_ready();      // parks: its ev_io is started on the main thread's loop
        listener::current.run(EVRUN_NOWAIT); // the watcher fires: the frame is queued in the CURRENT scheduler
        std::fprintf(stderr, "[ch_w] static scheduler: pending=%zu active=%zu\n", static_scheduler().pending_count(),
                     static_scheduler().active_count());
        CoroutineScheduler::set_current(lsched);
#else
        return 77;
#endif
    } else if (c == "late_coro_scheduler") {
        // The documented accessor, asked after the thread's loop is gone: not the door decision D3 guards.
        struct asker {
            ~asker() {
                std::fprintf(stderr, "[ch_w] a static destructor calls coro_scheduler().spawn() after the main thread's listener\n");
                coro_scheduler().spawn(ends_at_once());
                std::fprintf(stderr, "[ch_w] it returned: current=%p pending=%zu\n", (void *) CoroutineScheduler::current_ptr(),
                             coro_scheduler().pending_count());
            }
        };
        static judge j{tally0, live0};
        static asker a;
    } else {
        std::fprintf(stderr, "unknown case %s\n", c.c_str());
        return 2;
    }
    std::fprintf(stderr, "[ch_w] %s: returning from main\n", c.c_str());
    return 0;
}
