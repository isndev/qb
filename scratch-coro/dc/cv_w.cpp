// cv_w.cpp -- SCRATCH witnesses of the confirmation (design-coro-cv). Never committed.
//
//   cv_w <case>
//
//   reent_forever    ~CoroutineScheduler, loop alive: every frame it destroys spawns one more on it and pumps it (the
//                    new one parks). An unbounded "repeat until it owns nothing" never returns.
//   exit_in_nested   a spawned root awaits a child; the CHILD suspends on recv(), is resumed (the handle the drain
//                    resumes is the child's, not the root's) and calls std::exit(0) from its body
//   exit_in_parent   the same child returns to its parent, and the PARENT calls std::exit(0): the handle the drain
//                    resumed is a frame that is finished by then
//   exit_static_any  std::exit(0) from a coroutine body while a task with static storage, driven by hand, is parked on
//                    when_any(sleep, sleep): its branches are the listener's; its destructor reclaims them after the loop
//   run_late_bound   a scheduler with static storage left bound as the CURRENT one; a static destructor resumes a
//                    coroutine that goes to co_await sleep(5s): the scheduler doors are not asked
//   late_cancel      a static destructor cancels a token a static task, driven by hand, waits on (cancellable_sleep):
//                    the cancel hook is a cleanup and a wake, after the loop
//   xthread_exit     a scheduler with static storage is built and used on a WORKER thread, which stays alive; the
//                    main thread, which never built a listener, returns from main: exit() destroys the scheduler on
//                    the main thread, while the loop its coroutine waits on belongs to the worker
//   task_sendfor     a task with static storage, driven by hand, parked on send_for(5s) of a full static channel:
//                    destroyed after the main thread's listener (the timed SEND awaiter's watchdog; the dossier's
//                    task_recvfor covers the timed receive)
//   poison_control   the lane guard's own control (T6): a static destructor reads listener::current.has_coro_scheduler()
//                    after the main thread's listener -- must be a use-after-poison in an AddressSanitizer build
//   poison_threads   300 threads one after the other, each with its own listener::current, then 50 rounds of 4 alive
//                    together: the poisoning of a dead thread's listener must never be seen by a later thread

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace cv_w {

int g_dtors      = 0;
int g_completed  = 0;
int g_late_dtors = 0;

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
        std::fprintf(stderr, "[cv_w] coroutine local '%s' destroyed (thread loop gone: %d)\n", name, (int) loop_gone());
    }
};

long
live() {
    return detail::CoroutineFrameAllocator::live_frames;
}

void
checkpoint(char const *what, std::size_t tally0, long live0) {
    std::fprintf(stderr, "[cv_w] %s: frame_dtors=%d late_dtors=%d completed=%d live_frames%+ld tally+%zu current=%s\n", what, g_dtors,
                 g_late_dtors, g_completed, live() - live0, abandoned_coroutine_frames_total() - tally0,
                 CoroutineScheduler::current_ptr() ? "bound" : "none");
}

struct judge {
    std::size_t tally0;
    long        live0;
    ~judge() {
        checkpoint("judge (last static destroyed)", tally0, live0);
    }
};

CoroutineScheduler &
static_scheduler() {
    static CoroutineScheduler s;
    return s;
}

// ---- reent_forever -----------------------------------------------------------------------------------------------
CoroutineScheduler *g_dying   = nullptr;
int                 g_respawn = 0;
task<void>          respawner();
struct respawn_in_dtor {
    ~respawn_in_dtor() {
        if (++g_respawn > 100000) { // a run-away guard for the witness itself: 100 000 generations is "for ever"
            std::fprintf(stderr, "[cv_w] 100000 generations respawned: the teardown does not end\n");
            std::fflush(stderr);
            std::_Exit(9);
        }
        g_dying->spawn(respawner());
        g_dying->run_ready(); // the new one parks
    }
};
task<void>
respawner() {
    respawn_in_dtor r;
    co_await sleep(5s);
    ++g_completed;
}

// ---- exit_in_* ---------------------------------------------------------------------------------------------------
task<void>
child_recv(channel<int> &ch, bool exit_here) {
    local l{"child"};
    auto  v = co_await ch.recv(); // suspends: the handle a waker queues is THIS frame's
    if (exit_here) {
        std::fprintf(stderr, "[cv_w] the child got %d and calls std::exit(0) from its body\n", v ? *v : -1);
        std::exit(0);
    }
}
task<void>
parent_of(channel<int> &ch, bool exit_in_child) {
    local l{"parent"};
    co_await child_recv(ch, exit_in_child);
    std::fprintf(stderr, "[cv_w] the parent, resumed by its child's end, calls std::exit(0) from its body\n");
    std::exit(0);
}
task<void>
send_one(channel<int> &ch) {
    co_await sleep(1ms);
    co_await ch.send(7);
}
task<void>
bystander() {
    local l{"bystander"};
    co_await sleep(5s);
    ++g_completed;
}
task<int>
sleeper_int(qb::duration d, int v) {
    local l{"branch"};
    co_await sleep(d);
    co_return v;
}
task<void>
any_of_two() {
    local l{"any_of_two"};
    (void) co_await when_any(sleeper_int(5s, 1), sleeper_int(6s, 2));
    ++g_completed;
}
task<void>
exit_soon() {
    co_await sleep(5ms);
    std::fprintf(stderr, "[cv_w] a coroutine calls std::exit(0) from its body\n");
    std::exit(0);
}
task<void> &
static_task() {
    static task<void> t;
    return t;
}

// ---- run_late_bound / late_cancel ----------------------------------------------------------------------------------
task<void>
sleeper() {
    local l{"sleeper"};
    co_await sleep(5s);
    ++g_completed;
}
task<void>
csleeper(cancellation_token tok) {
    local l{"csleeper"};
    (void) co_await cancellable_sleep(5s, tok);
    ++g_completed;
}
cancellation_token &
static_source() {
    static cancellation_token s;
    return s;
}

// ---- task_sendfor ---------------------------------------------------------------------------------------------------
task<void>
send_for_on(channel<int> &ch) {
    local l{"send_for_on"};
    (void) co_await ch.send_for(1, 5s);
    ++g_completed;
}
channel<int> &
static_full_channel() {
    static channel<int> ch(0); // capacity 0, no receiver: a send parks
    return ch;
}

// ---- xthread_exit --------------------------------------------------------------------------------------------------
std::thread::id g_worker{};
struct thread_local_witness {
    ~thread_local_witness() {
        ++g_dtors;
        std::fprintf(stderr, "[cv_w] coroutine local 'xthread' destroyed on %s thread\n",
                     std::this_thread::get_id() == g_worker ? "its OWN" : "ANOTHER");
    }
};
task<void>
xsleeper() {
    thread_local_witness w;
    co_await sleep(5s);
    ++g_completed;
}

} // namespace cv_w

int
main(int argc, char **argv) {
    using namespace cv_w;
    if (argc < 2) {
        std::fprintf(stderr, "usage: cv_w <case>\n");
        return 2;
    }
    const std::string c = argv[1];
    if (c == "xthread_exit") { // before this thread touches its listener: its record stays where the platform left it
        struct last {
            std::size_t tally0;
            ~last() {
                std::fprintf(stderr, "[cv_w] judge (last static destroyed): frame_dtors=%d completed=%d tally+%zu\n", g_dtors, g_completed,
                             abandoned_coroutine_frames_total() - tally0);
            }
        };
        static last               l{abandoned_coroutine_frames_total()}; // built first: destroyed after the scheduler
        static std::promise<void> parked;
        std::thread               worker([] {
            g_worker = std::this_thread::get_id();
            listener::current.clear();
            CoroutineScheduler::set_current(&static_scheduler()); // BUILT here, on the worker
            static_scheduler().spawn(xsleeper());
            static_scheduler().run_ready(); // parks on a timer of the worker's loop
            parked.set_value();
            std::this_thread::sleep_for(30s); // alive, with its loop, while the main thread exits
        });
        parked.get_future().wait();
        worker.detach();
        std::fprintf(stderr, "[cv_w] xthread_exit: the main thread returns from main while the worker and its loop live\n");
        return 0;
    }
    listener::current.clear();
    auto *const       lsched = &coro_scheduler();
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    const long        live0  = live();

    if (c == "reent_forever") {
        {
            CoroutineScheduler sched;
            g_dying = &sched;
            CoroutineScheduler::set_current(&sched);
            sched.spawn(respawner());
            sched.run_ready();
            checkpoint("one frame parked", tally0, live0);
        }
        CoroutineScheduler::set_current(lsched);
        std::fprintf(stderr, "[cv_w] the scheduler's destructor returned after %d generation(s)\n", g_respawn);
        checkpoint("after the scheduler (loop alive)", tally0, live0);
    } else if (c == "exit_in_nested" || c == "exit_in_parent") {
        static judge        j{tally0, live0};
        static channel<int> ch; // outlives the main thread's listener
        coro_scheduler().spawn(parent_of(ch, c == "exit_in_nested"));
        coro_scheduler().spawn(bystander());
        coro_scheduler().spawn(send_one(ch));
        run_for(200ms);
        std::fprintf(stderr, "[cv_w] NOT REACHED: no coroutine exited\n");
        return 4;
    } else if (c == "exit_static_any") {
        static judge j{tally0, live0};
        static_task() = any_of_two();
        static_task().handle().resume(); // parks on when_any: two branch frames spawned on the listener's scheduler
        coro_scheduler().spawn(exit_soon());
        run_for(200ms);
        std::fprintf(stderr, "[cv_w] NOT REACHED: no coroutine exited\n");
        return 4;
    } else if (c == "run_late_bound") {
        struct resumer {
            task<void> t;
            ~resumer() {
                std::fprintf(stderr, "[cv_w] a static destructor resumes a coroutine after the main thread's listener; current=%s\n",
                             CoroutineScheduler::current_ptr() ? "bound" : "none");
                if (t && !t.done())
                    t.handle().resume();
                std::fprintf(stderr, "[cv_w] the coroutine suspended\n");
                (void) t.detach();
            }
        };
        static judge j{tally0, live0};
        (void) static_scheduler(); // built before the resumer: destroyed after it
        static resumer r;
        r.t = sleeper();
        CoroutineScheduler::set_current(&static_scheduler()); // and LEFT bound: ~listener unbinds only its own
    } else if (c == "late_cancel") {
        struct canceller {
            ~canceller() {
                std::fprintf(stderr, "[cv_w] a static destructor cancels the token after the main thread's listener\n");
                static_source().cancel();
                std::fprintf(stderr, "[cv_w] it returned: current=%s\n", CoroutineScheduler::current_ptr() ? "bound" : "none");
            }
        };
        static judge j{tally0, live0};
        (void) static_source();
        (void) static_task(); // destroyed after the canceller ran
        static canceller k;
        static_task() = csleeper(static_source());
        static_task().handle().resume();
        run_for(5ms);
    } else if (c == "task_sendfor") {
        static judge j{tally0, live0};
        (void) static_full_channel(); // built first: destroyed after the task
        static_task() = send_for_on(static_full_channel());
        static_task().handle().resume();
        run_for(5ms);
    } else if (c == "poison_control") {
        struct toucher {
            ~toucher() {
                std::fprintf(stderr, "[cv_w] a static destructor reads listener::current.has_coro_scheduler() after its destructor\n");
                std::fflush(stderr);
                const bool has = listener::current.has_coro_scheduler();
                std::fprintf(stderr, "[cv_w] it read %d and nothing reported it\n", (int) has);
            }
        };
        static toucher t;
    } else if (c == "poison_threads") {
        long sum = 0;
        for (int i = 0; i < 300; ++i) {
            std::thread th([&sum] {
                listener::current.clear();
                sum += listener::current.has_coro_scheduler() ? 1 : 2;
            });
            th.join();
        }
        for (int i = 0; i < 50; ++i) {
            std::vector<std::thread> ts;
            long                     part[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k)
                ts.emplace_back([&part, k] {
                    coro_scheduler().spawn(sleeper());
                    run_for(1ms);
                    part[k] = listener::current.has_coro_scheduler() ? 1 : 2;
                });
            for (auto &t : ts)
                t.join();
            sum += part[0] + part[1] + part[2] + part[3];
        }
        std::fprintf(stderr, "[cv_w] 300 threads one after the other and 50 rounds of 4: sum %ld, no report\n", sum);
    } else {
        std::fprintf(stderr, "unknown case %s\n", c.c_str());
        return 2;
    }
    std::fprintf(stderr, "[cv_w] %s: returning from main\n", c.c_str());
    return 0;
}
