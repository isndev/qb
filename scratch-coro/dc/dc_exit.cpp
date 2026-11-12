// dc_exit.cpp -- SCRATCH witness: objects whose destructor runs AFTER the thread's listener::current was destroyed.
// Never committed.
//
//   dc_exit <case>
//
// Each case leaves, at `return 0` from main (or at a worker thread's exit), an object destroyed after the thread-local
// listener::current: static storage (destroyed after the main thread's thread-locals, [basic.start.term]) or a
// thread_local constructed BEFORE listener::current on that thread (destroyed after it, reverse order).
//
//   chan_task     static channel; a hand-driven task, never destroyed, parked on recv()      -> ~channel -> close()
//   chan_abandon  static channel; a frame a directly-owned scheduler abandoned, parked on recv() (the lot report's kind 2)
//   chan_empty    static channel with no waiter at all (control: must reach nothing)
//   task_sleep    static task parked on sleep(5s)                                           -> ~task -> ~timer_awaiter
//   task_io       static task parked on wait_readable(fd)                                   -> ~socket_awaiter
//   task_timeout  static task parked on coro_with_timeout(sleep 5s, 4s)                     -> ~timeout_awaiter
//   task_recvfor  static task parked on recv_for(5s) of a static channel
//   task_recv     static task parked on recv() of a static channel (control: no loop, no listener)
//   sched_sleep   static CoroutineScheduler owning a spawned frame parked on sleep(5s)
//   sched_recv    static CoroutineScheduler owning a spawned frame parked on recv() of a static channel
//   io_obj        static io object (with_timeout<>: a registered ev_timer)                  -> ~base -> unregisterEvent
//   scope         static coroutine_scope with a worker parked on sleep(5s) (control: its frames are the listener's)
//   atexit_wake   an atexit handler that closes a heap channel with a waiter
//   tls_task      worker thread: a thread_local built before listener::current holds a task parked on sleep(5s)
//   tls_chan      worker thread: a thread_local built before listener::current holds a channel with a parked waiter
//   mutex_static  static async_mutex with a parked waiter (control: its destructor wakes nobody)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace dc_exit {

struct announce {
    char const *what;
    ~announce() {
        std::fprintf(stderr, "[dc_exit] static destructor phase reached: %s\n", what);
    }
};

task<void>
recv_on(channel<int> &ch) {
    (void) co_await ch.recv();
}
task<void>
recv_for_on(channel<int> &ch) {
    (void) co_await ch.recv_for(5s);
}
task<void>
sleeper() {
    co_await sleep(5s);
}
task<void>
timeouter() {
    try {
        co_await coro_with_timeout(sleeper(), 4s);
    } catch (...) {
    }
}
task<void>
locker(async_mutex &m) {
    co_await m.lock();
    m.unlock();
}
#if !defined(_WIN32)
int g_pair[2] = {-1, -1};
task<void>
reader() {
    co_await wait_readable(g_pair[0]);
}
#endif

struct idle_timer : with_timeout<idle_timer> {
    idle_timer()
        : with_timeout<idle_timer>(5s) {}
    void
    on(event::timer &) noexcept {}
};

// Static storage, constructed lazily from main (function-local statics are destroyed with the other statics, after
// the main thread's thread-locals).
channel<int> &
static_channel() {
    static channel<int> ch;
    return ch;
}
task<void> &
static_task() {
    static task<void> t;
    return t;
}
CoroutineScheduler &
static_scheduler() {
    static CoroutineScheduler s;
    return s;
}
async_mutex &
static_mutex() {
    static async_mutex m;
    return m;
}

struct tls_task_holder {
    task<void> t;
    ~tls_task_holder() {
        std::fprintf(stderr, "[dc_exit] thread_local holder destroyed after this thread's listener: its task goes now\n");
    }
};
struct tls_chan_holder {
    std::optional<channel<int>> ch;
    task<void>                 *waiter = nullptr; // never destroyed
    ~tls_chan_holder() {
        std::fprintf(stderr, "[dc_exit] thread_local holder destroyed after this thread's listener: its channel closes now\n");
    }
};

channel<int> *g_heap_channel = nullptr;

} // namespace dc_exit

int
main(int argc, char **argv) {
    using namespace dc_exit;
    if (argc < 2) {
        std::fprintf(stderr, "usage: dc_exit <case>\n");
        return 2;
    }
    const std::string c = argv[1];
    listener::current.clear(); // the main thread's listener exists
    (void) coro_scheduler();   // and its scheduler, bound as current

    if (c == "chan_task") {
        auto *t = new task<void>(recv_on(static_channel())); // never deleted
        t->handle().resume();                                // parks on the static channel
    } else if (c == "chan_abandon") {
        auto *const lsched = CoroutineScheduler::current_ptr();
        {
            CoroutineScheduler sched;
            CoroutineScheduler::set_current(&sched);
            sched.spawn(recv_on(static_channel()));
            sched.run_ready();
        }
        CoroutineScheduler::set_current(lsched);
    } else if (c == "chan_empty") {
        (void) static_channel();
    } else if (c == "task_sleep") {
        static_task() = sleeper();
        static_task().handle().resume();
    } else if (c == "task_io") {
#if !defined(_WIN32)
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_pair) != 0)
            return 3;
        static_task() = reader();
        static_task().handle().resume();
#else
        return 77;
#endif
    } else if (c == "task_timeout") {
        static_task() = timeouter();
        static_task().handle().resume();
        run_for(5ms);
    } else if (c == "task_recvfor") {
        (void) static_channel(); // built first: destroyed after the task
        static_task() = recv_for_on(static_channel());
        static_task().handle().resume();
        run_for(5ms);
    } else if (c == "task_recv") {
        (void) static_channel();
        static_task() = recv_on(static_channel());
        static_task().handle().resume();
    } else if (c == "sched_sleep") {
        auto *const lsched = CoroutineScheduler::current_ptr();
        CoroutineScheduler::set_current(&static_scheduler());
        static_scheduler().spawn(sleeper());
        static_scheduler().run_ready();
        CoroutineScheduler::set_current(lsched);
    } else if (c == "sched_recv") {
        auto *const lsched = CoroutineScheduler::current_ptr();
        (void) static_channel();
        CoroutineScheduler::set_current(&static_scheduler());
        static_scheduler().spawn(recv_on(static_channel()));
        static_scheduler().run_ready();
        CoroutineScheduler::set_current(lsched);
    } else if (c == "io_obj") {
        static idle_timer t;
        (void) t;
    } else if (c == "scope") {
        static coroutine_scope sc;
        sc.spawn(sleeper());
        run_for(5ms);
    } else if (c == "atexit_wake") {
        g_heap_channel = new channel<int>();
        auto *t        = new task<void>(recv_on(*g_heap_channel));
        t->handle().resume();
        std::atexit([] {
            std::fprintf(stderr, "[dc_exit] atexit handler: closing a channel with a waiter\n");
            g_heap_channel->close();
        });
    } else if (c == "tls_task") {
        std::thread th([] {
            thread_local tls_task_holder h; // built BEFORE this thread's listener::current
            listener::current.clear();      // the thread's listener, built second, destroyed first
            h.t = sleeper();
            h.t.handle().resume();
        });
        th.join();
    } else if (c == "tls_chan") {
        std::thread th([] {
            thread_local tls_chan_holder h;
            listener::current.clear();
            (void) coro_scheduler();
            h.ch.emplace();
            h.waiter = new task<void>(recv_on(*h.ch));
            h.waiter->handle().resume();
        });
        th.join();
    } else if (c == "idle_thread") {
        // A thread that never touches qb: does it get a listener::current (an event loop) all the same?
        std::thread th([] {
            std::fputs("[dc_exit] idle thread: runs, uses nothing of qb, returns", stderr);
            std::fputc(10, stderr);
        });
        th.join();
        std::fputs("[dc_exit] idle thread joined", stderr);
        std::fputc(10, stderr);
    } else if (c == "run_late") {
        // Somebody RUNS a coroutine after the thread's loop is gone: a static destructor resumes a task never started,
        // which goes to its first `co_await sleep(5s)`.
        struct resumer {
            task<void> t;
            ~resumer() {
                std::fputs("[dc_exit] a static destructor resumes a coroutine, after the main thread's listener", stderr);
                std::fputc(10, stderr);
                if (t && !t.done())
                    t.handle().resume();
                std::fputs("[dc_exit] the coroutine suspended", stderr);
                std::fputc(10, stderr);
                (void) t.detach(); // leave the frame: this case is about the resume, not about its destruction
            }
        };
        static resumer r;
        r.t = sleeper();
    } else if (c == "mutex_static") {
        if (!static_mutex().try_lock())
            return 3;
        auto *t = new task<void>(locker(static_mutex()));
        t->handle().resume();
    } else {
        std::fprintf(stderr, "unknown case %s\n", c.c_str());
        return 2;
    }
    static announce a{"statics are being destroyed (the main thread's thread-locals are gone)"}; // built last: goes first
    std::fprintf(stderr, "[dc_exit] %s: returning from main\n", c.c_str());
    return 0;
}
