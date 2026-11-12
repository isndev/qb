// ch_cg.cpp -- SCRATCH: instruction-count cells for callgrind, base versus prototype C. Never committed.
//
//   ch_cg <cell> <n>
//
// Each cell runs <n> iterations of one path; run it under `valgrind --tool=callgrind` with two values of <n> and
// divide the difference of the totals by the difference of <n>: the instructions of ONE iteration, the fixed
// start-up and teardown cancelled out.
//
//   resume          spawn a coroutine that yields <n> times (resume path: run_ready, schedule, no destruction)
//   sleep_fire      <n> coroutines each await a 0-delay one-shot... not used: needs the loop's clock
//   park_destroy    <n> times: spawn a coroutine parked on sleep(1h), then cancel_spawned() it: ~timer_awaiter of a PARKED
//                   frame -- the path prototype C adds its guard to
//   when_any_loser  <n> times: when_any(sleep(1h), ready()): the winner is immediate, the loser is destroyed parked
//   timeout_win     <n> times: coro_with_timeout(ready(), 1h): the timer is armed, the task wins, finish() stops it
//   recvfor_destroy <n> times: spawn a coroutine parked on recv_for(1h), then cancel_spawned() it
//   io_churn        <n> times: construct and destroy an io object (with_timeout<>): ~base
//   sched_dtor      <n> times: a scheduler owned directly runs one coroutine to its end and is destroyed
//   spawn           spawn <n> coroutines that end at once, run them (the spawn-1000 shape)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace ch_cg {

long g_sink = 0;

task<void>
ready() {
    ++g_sink;
    co_return;
}
task<int>
ready_int() {
    ++g_sink;
    co_return 1;
}
task<void>
long_sleep() {
    co_await sleep(1h);
    ++g_sink;
}
task<int>
long_sleep_int() {
    co_await sleep(1h);
    co_return 2;
}
task<void>
yielder(int n) {
    for (int i = 0; i < n; ++i)
        co_await sleep(0ms);
    ++g_sink;
}
task<void>
any_loop(int n) {
    for (int i = 0; i < n; ++i) {
        auto r = co_await when_any(long_sleep_int(), ready_int());
        g_sink += static_cast<long>(r.index);
    }
}
task<void>
timeout_loop(int n) {
    for (int i = 0; i < n; ++i)
        co_await coro_with_timeout(ready(), 1h);
}
task<void>
recv_for_one(channel<int> &ch) {
    (void) co_await ch.recv_for(1h);
    ++g_sink;
}
struct idle_timer : with_timeout<idle_timer> {
    idle_timer()
        : with_timeout<idle_timer>(3600s) {}
    void
    on(event::timer &) noexcept {}
};

} // namespace ch_cg

int
main(int argc, char **argv) {
    using namespace ch_cg;
    if (argc < 3) {
        std::fprintf(stderr, "usage: ch_cg <cell> <n>\n");
        return 2;
    }
    const std::string c = argv[1];
    const int         n = std::atoi(argv[2]);
    listener::current.clear();
    auto &sched = coro_scheduler();

    if (c == "resume") {
        sched.spawn(yielder(n));
        while (sched.has_ready())
            sched.run_ready();
    } else if (c == "park_destroy") {
        for (int i = 0; i < n; ++i) {
            auto h = sched.spawn_tracked(long_sleep());
            sched.run_ready();
            sched.cancel_spawned(h);
        }
    } else if (c == "when_any_loser") {
        sched.spawn(any_loop(n));
        for (int guard = 0; guard < 16 * n + 64 && (sched.has_work() || sched.active_count() != 0); ++guard)
            listener::current.run(EVRUN_NOWAIT);
    } else if (c == "timeout_win") {
        sched.spawn(timeout_loop(n));
        for (int guard = 0; guard < 16 * n + 64 && (sched.has_work() || sched.active_count() != 0); ++guard)
            listener::current.run(EVRUN_NOWAIT);
    } else if (c == "recvfor_destroy") {
        channel<int> ch;
        for (int i = 0; i < n; ++i) {
            auto h = sched.spawn_tracked(recv_for_one(ch));
            sched.run_ready();
            sched.cancel_spawned(h);
        }
    } else if (c == "io_churn") {
        for (int i = 0; i < n; ++i) {
            idle_timer t;
            g_sink += static_cast<long>(sizeof t);
        }
    } else if (c == "sched_dtor") {
        auto *const lsched = CoroutineScheduler::current_ptr();
        for (int i = 0; i < n; ++i) {
            CoroutineScheduler s;
            CoroutineScheduler::set_current(&s);
            s.spawn(ready());
            s.run_ready();
        }
        CoroutineScheduler::set_current(lsched);
    } else if (c == "spawn") {
        for (int i = 0; i < n; ++i)
            sched.spawn(ready());
        sched.run_ready();
    } else {
        std::fprintf(stderr, "unknown cell %s\n", c.c_str());
        return 2;
    }
    std::printf("%s n=%d sink=%ld active=%zu\n", c.c_str(), n, g_sink, sched.active_count());
    return 0;
}
