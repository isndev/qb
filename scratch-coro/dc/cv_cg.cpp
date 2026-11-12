// cv_cg.cpp -- SCRATCH: instruction-count cells of the confirmation, for callgrind. Never committed.
//
//   cv_cg <cell> <n>      (two values of <n>; the difference of the totals over the difference of <n> is one iteration)
//
//   loop_empty     <n> times: ev_loop_new + ev_loop_destroy, nothing on the loop          -- E5's fixed cost
//   loop_16w       <n> times: a loop with 8 timers and 8 io watchers started, destroyed    -- E5's walk (never stopped:
//                  libev's "ignore the watchers" contract, legal on the base)
//   listener       <n> times: a temporary qb listener constructed and destroyed            -- what a thread pays at its end
//   sched_spawn    <n> times: a scheduler owned directly spawns one coroutine, runs it to its end, is destroyed
//   sched_parked   <n> times: a scheduler owned directly is destroyed with one coroutine parked on sleep(1h)
//                  (the base abandons it -- and leaks it; the prototype destroys it: not the same work, by design)
//   coro_sched     <n> calls of coro_scheduler() (the second door's test)
//   sleep_factory  <n> sleep(1h) awaiters built and dropped unawaited (listener::current.loop(): the third door's test)
//   finish         <n> spawned coroutines that end at once, one run_ready() each (defer_frame_destruction per frame)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace cv_cg {
long g_sink = 0;
void
tcb(struct ev_loop *, ev_timer *, int) {}
void
icb(struct ev_loop *, ev_io *, int) {}
task<void>
ready() {
    ++g_sink;
    co_return;
}
task<void>
long_sleep() {
    co_await sleep(1h);
    ++g_sink;
}
} // namespace cv_cg

int
main(int argc, char **argv) {
    using namespace cv_cg;
    if (argc < 3)
        return 2;
    const std::string c = argv[1];
    const int         n = std::atoi(argv[2]);
    listener::current.clear();
    auto &sched = coro_scheduler();

    if (c == "loop_empty") {
        for (int i = 0; i < n; ++i) {
            struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
            g_sink += static_cast<long>(ev_backend(l));
            ev_loop_destroy(l);
        }
    } else if (c == "loop_16w") {
#if !defined(_WIN32)
        int fds[2];
        if (::pipe(fds) != 0)
            return 3;
        static ev_timer t[8];
        static ev_io    w[8];
        for (int i = 0; i < n; ++i) {
            struct ev_loop *l = ev_loop_new(EVFLAG_AUTO);
            for (int k = 0; k < 8; ++k) {
                ev_timer_init(&t[k], tcb, 3600. + k, 0.);
                ev_timer_start(l, &t[k]);
                ev_io_init(&w[k], icb, fds[0], EV_READ);
                ev_io_start(l, &w[k]);
            }
            ev_loop_destroy(l);
            for (int k = 0; k < 8; ++k) { // the base leaves them "active": make them startable again, as E5 leaves them
                ((ev_watcher *) &t[k])->active = 0;
                ((ev_watcher *) &w[k])->active = 0;
            }
        }
#else
        return 77;
#endif
    } else if (c == "listener") {
        for (int i = 0; i < n; ++i) {
            listener tmp;
            g_sink += static_cast<long>(tmp.backend());
        }
    } else if (c == "sched_spawn") {
        for (int i = 0; i < n; ++i) {
            CoroutineScheduler s;
            CoroutineScheduler::set_current(&s);
            s.spawn(ready());
            s.run_ready();
        }
        CoroutineScheduler::set_current(&sched);
    } else if (c == "sched_parked") {
        for (int i = 0; i < n; ++i) {
            CoroutineScheduler s;
            CoroutineScheduler::set_current(&s);
            s.spawn(long_sleep());
            s.run_ready();
        }
        CoroutineScheduler::set_current(&sched);
    } else if (c == "coro_sched") {
        for (int i = 0; i < n; ++i)
            g_sink += static_cast<long>(coro_scheduler().pending_count());
    } else if (c == "sleep_factory") {
        for (int i = 0; i < n; ++i) {
            auto a = sleep(1h);
            g_sink += a.await_ready() ? 1 : 0;
        }
    } else if (c == "finish") {
        for (int i = 0; i < n; ++i) {
            sched.spawn(ready());
            sched.run_ready();
        }
    } else {
        return 2;
    }
    std::printf("%s n=%d sink=%ld\n", c.c_str(), n, g_sink);
    return 0;
}
