// dc_probe.cpp -- SCRATCH witness for the T7 coroutine-teardown design investigation. Never committed.
//
//   dc_probe <family> <mode>
//
// family: what the spawned root is parked on
//   park      a custom awaiter that registers the frame as suspended, no watcher (the QB-84 test's shape)
//   plain     a custom awaiter that registers nothing at all
//   sleep     sleep(5s): an ev_timer on the thread's loop
//   io_r      wait_readable(fd): an ev_io (read) on the thread's loop
//   io_w      wait_writable(fd): an ev_io (write) on a socket whose send buffer is full
//   recv      channel::recv() on a channel the probe keeps (released at checkpoint 2)
//   recv_own  channel::recv() on a channel only the frame holds
//   recv_for  channel::recv_for(5s): select registration + watchdog ev_timer
//   send      channel::send() on a capacity-0 channel with no receiver
//   select    select(a, b) on two channels the probe keeps
//   when_any  when_any(sleep 5s, sleep 6s): two branch frames spawned on the LISTENER's scheduler
//   when_all  when_all(sleep 5s, sleep 6s)
//   timeout   coro_with_timeout(sleep 5s, 4s): runner frame on the listener's scheduler + raw ev_timer
//   mutex     async_mutex::lock() on a mutex the probe holds
//   nested    root awaits a child task that sleeps 5s (root owned, leaf registered)
//   csleep    cancellable_sleep(5s, token)
//   scope     coroutine_scope: one worker sleeping 5s, root in join_all()
//
// mode: how the owner goes
//   S   a CoroutineScheduler owned directly, destroyed while the thread's loop lives
//   R   spawned on the listener's scheduler; listener::current.reset_coro_scheduler()
//   C   spawned on the listener's scheduler; listener::current.recycle_coro_scheduler()
//   T   spawned on the listener's scheduler of a WORKER thread; the thread returns (~listener at thread exit)
//   E   spawned on the main thread's listener scheduler; return from main (thread-locals, then statics)
//   X   as E, but std::exit(0) from inside main
//   Q   as E, but std::quick_exit(0)
//   U   as E, but _exit(0)
//   D   a CoroutineScheduler destroyed AFTER its thread's listener::current (a thread_local constructed first, in a
//       worker thread): the order the decision log fears
//
// Checkpoints printed on stdout (one line each):
//   cp1  right after the owner went
//   cp2  after the probe released what it kept (channels, mutex) and ran the loop 40 ms
// and the process exit code tells what LeakSanitizer said at exit (23 = leaks).

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define DC_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DC_ASAN 1
#endif
#endif
#if defined(DC_ASAN) && !defined(_WIN32)
#include <sanitizer/lsan_interface.h>
#define DC_LSAN 1
#endif

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace dc_probe {

std::atomic<int> g_frame_dtors{0}; // frames whose locals were destroyed (the frame was destroyed, or it completed)
std::atomic<int> g_completed{0};   // frames that ran to their end

struct frame_local {
    char const *name;
    ~frame_local() {
        g_frame_dtors.fetch_add(1);
    }
};

struct done_mark {
    ~done_mark() {}
};

struct park_registered {
    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }
    void
    await_suspend(std::coroutine_handle<> h) const noexcept {
        CoroutineScheduler::current().register_suspended(h);
    }
    void
    await_resume() const noexcept {}
};

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

#if !defined(_WIN32)
int g_pair[2] = {-1, -1};
int g_full    = -1;

void
make_fds() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_pair) != 0)
        std::abort();
    int full[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, full) != 0)
        std::abort();
    ::fcntl(full[0], F_SETFL, ::fcntl(full[0], F_GETFL) | O_NONBLOCK);
    char buf[4096];
    std::memset(buf, 'x', sizeof buf);
    while (::write(full[0], buf, sizeof buf) > 0) {
    }
    g_full = full[0]; // not writable until its peer reads, which never happens
}
#endif

using chan = std::shared_ptr<channel<int>>;

task<void>
sleeper(qb::duration d) {
    frame_local l{"sleeper"};
    co_await sleep(d);
    g_completed.fetch_add(1);
}

task<void>
f_park() {
    frame_local l{"park"};
    co_await park_registered{};
    g_completed.fetch_add(1);
}
task<void>
f_plain() {
    frame_local l{"plain"};
    co_await park_plain{};
    g_completed.fetch_add(1);
}
task<void>
f_sleep() {
    frame_local l{"sleep"};
    co_await sleep(5s);
    g_completed.fetch_add(1);
}
#if !defined(_WIN32)
task<void>
f_io_r() {
    frame_local l{"io_r"};
    co_await wait_readable(g_pair[0]);
    g_completed.fetch_add(1);
}
task<void>
f_io_w() {
    frame_local l{"io_w"};
    co_await wait_writable(g_full);
    g_completed.fetch_add(1);
}
#endif
task<void>
f_recv(chan ch) {
    frame_local l{"recv"};
    (void) co_await ch->recv();
    g_completed.fetch_add(1);
}
task<void>
f_recv_for(chan ch) {
    frame_local l{"recv_for"};
    (void) co_await ch->recv_for(5s);
    g_completed.fetch_add(1);
}
task<void>
f_send(chan ch) {
    frame_local l{"send"};
    try {
        co_await ch->send(1);
    } catch (channel_closed const &) {
    }
    g_completed.fetch_add(1);
}
task<void>
f_select(chan a, chan b) {
    frame_local l{"select"};
    (void) co_await select(*a, *b);
    g_completed.fetch_add(1);
}
task<void>
f_when_any() {
    frame_local l{"when_any"};
    (void) co_await when_any(sleeper(5s), sleeper(6s));
    g_completed.fetch_add(1);
}
task<void>
f_when_all() {
    frame_local l{"when_all"};
    (void) co_await when_all(sleeper(5s), sleeper(6s));
    g_completed.fetch_add(1);
}
task<void>
f_timeout() {
    frame_local l{"timeout"};
    try {
        co_await coro_with_timeout(sleeper(5s), 4s);
    } catch (...) {
    }
    g_completed.fetch_add(1);
}
task<void>
f_mutex(std::shared_ptr<async_mutex> m) {
    frame_local l{"mutex"};
    co_await m->lock();
    m->unlock();
    g_completed.fetch_add(1);
}
task<void>
f_nested() {
    frame_local l{"nested"};
    co_await sleeper(5s);
    g_completed.fetch_add(1);
}
task<void>
f_csleep(cancellation_token tok) {
    frame_local l{"csleep"};
    (void) co_await cancellable_sleep(5s, tok);
    g_completed.fetch_add(1);
}
task<void>
f_scope() {
    frame_local     l{"scope"};
    coroutine_scope sc;
    sc.spawn(sleeper(5s));
    co_await sc.join_all();
    g_completed.fetch_add(1);
}

task<void>
f_mcancel(cancellation_token tok) {
    frame_local l{"mcancel"};
    try {
        co_await make_cancellable(sleeper(5s), tok);
    } catch (...) {
    }
    g_completed.fetch_add(1);
}
task<void>
f_deadline() {
    frame_local l{"deadline"};
    try {
        co_await with_deadline(sleeper(5s), std::chrono::steady_clock::now() + 4s);
    } catch (...) {
    }
    g_completed.fetch_add(1);
}
task<void>
f_scope_for() {
    frame_local     l{"scope_for"};
    coroutine_scope sc;
    sc.spawn(sleeper(5s));
    (void) co_await sc.join_all_for(4s);
    g_completed.fetch_add(1);
}
task<void>
f_shared() {
    frame_local l{"shared"};
    auto        st = make_shared_task(sleeper(5s));
    co_await st;
    g_completed.fetch_add(1);
}

struct kept {
    chan                         a, b;
    std::shared_ptr<async_mutex> m;
    cancellation_token           tok;
    bool                         locked = false;
};

task<void>
make(std::string const &family, kept &k) {
    if (family == "park")
        return f_park();
    if (family == "plain")
        return f_plain();
    if (family == "sleep")
        return f_sleep();
#if !defined(_WIN32)
    if (family == "io_r")
        return f_io_r();
    if (family == "io_w")
        return f_io_w();
#endif
    if (family == "recv") {
        k.a = std::make_shared<channel<int>>(0);
        return f_recv(k.a);
    }
    if (family == "recv_own")
        return f_recv(std::make_shared<channel<int>>(0));
    if (family == "recv_for") {
        k.a = std::make_shared<channel<int>>(0);
        return f_recv_for(k.a);
    }
    if (family == "send") {
        k.a = std::make_shared<channel<int>>(0);
        return f_send(k.a);
    }
    if (family == "select") {
        k.a = std::make_shared<channel<int>>(0);
        k.b = std::make_shared<channel<int>>(0);
        return f_select(k.a, k.b);
    }
    if (family == "when_any")
        return f_when_any();
    if (family == "when_all")
        return f_when_all();
    if (family == "timeout")
        return f_timeout();
    if (family == "mutex") {
        k.m = std::make_shared<async_mutex>();
        if (!k.m->try_lock())
            std::abort();
        k.locked = true;
        return f_mutex(k.m);
    }
    if (family == "nested")
        return f_nested();
    if (family == "csleep")
        return f_csleep(k.tok);
    if (family == "scope")
        return f_scope();
    if (family == "mcancel")
        return f_mcancel(k.tok);
    if (family == "deadline")
        return f_deadline();
    if (family == "scope_for")
        return f_scope_for();
    if (family == "shared")
        return f_shared();
    std::fprintf(stderr, "unknown family %s\n", family.c_str());
    std::exit(2);
}

void
checkpoint(char const *tag, std::size_t tally0, long live0) {
    int leak = -1;
#if defined(DC_LSAN)
    leak = __lsan_do_recoverable_leak_check();
#endif
    std::printf("%s tally+%zu live_frames%+ld frame_dtors=%d completed=%d lsan_now=%d current=%s\n", tag,
                abandoned_coroutine_frames_total() - tally0, detail::CoroutineFrameAllocator::live_frames - live0, g_frame_dtors.load(),
                g_completed.load(), leak, CoroutineScheduler::current_ptr() ? "bound" : "none");
    std::fflush(stdout);
}

void
release(kept &k) {
    if (k.locked) {
        k.m->unlock();
        k.locked = false;
    }
    k.a.reset();
    k.b.reset();
    k.m.reset();
}

// mode D: a scheduler destroyed after its thread's listener::current
struct late_scheduler {
    std::unique_ptr<CoroutineScheduler> sched;
    ~late_scheduler() {
        std::fprintf(stderr, "[D] ~late_scheduler: destroying the scheduler now (listener::current already destroyed)\n");
        sched.reset();
        std::fprintf(stderr, "[D] scheduler destroyed\n");
    }
};

} // namespace dc_probe

int
main(int argc, char **argv) {
    using namespace dc_probe;
    if (argc < 3) {
        std::fprintf(stderr, "usage: dc_probe <family> <mode>\n");
        return 2;
    }
    const std::string family = argv[1];
    const std::string mode   = argv[2];
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: _IOLBF with size 0 is an invalid parameter on the MSVC CRT
#if !defined(_WIN32)
    make_fds();
#endif
    listener::current.clear(); // the main thread's loop exists from here
    const std::size_t tally0 = abandoned_coroutine_frames_total();
    const long        live0  = detail::CoroutineFrameAllocator::live_frames;
    kept              k;

    if (mode == "S") {
        auto *const lsched = &coro_scheduler(); // the listener's, current
        {
            CoroutineScheduler sched;
            CoroutineScheduler::set_current(&sched);
            sched.spawn(make(family, k));
            sched.run_ready(); // the root runs to its park
            // helpers (when_any branches, a timeout's runner, a scope's workers) are spawned on the LISTENER's
            // scheduler: let them reach their park too, with `sched` still the current one
            lsched->run_ready();
            std::printf("S parked: sched.active=%zu listener.active=%zu\n", sched.active_count(), lsched->active_count());
        } // ~CoroutineScheduler, current: it unbinds itself
        CoroutineScheduler::set_current(lsched);
        checkpoint("cp1", tally0, live0);
        release(k);
        run_for(40ms);
        checkpoint("cp2", tally0, live0);
        return 0;
    }
    if (mode == "R" || mode == "C") {
        coro_scheduler().spawn(make(family, k));
        run_for(5ms);
        std::printf("%s parked: listener.active=%zu\n", mode.c_str(), coro_scheduler().active_count());
        if (mode == "R")
            listener::current.reset_coro_scheduler();
        else
            listener::current.recycle_coro_scheduler();
        checkpoint("cp1", tally0, live0);
        release(k);
        run_for(40ms);
        checkpoint("cp2", tally0, live0);
        return 0;
    }
    if (mode == "T") {
        std::thread th([&] {
            coro_scheduler().spawn(make(family, k));
            run_for(5ms);
        }); // the thread returns: ~listener (clear, reset_coro_scheduler), then its loop
        th.join();
        // live_frames is per thread: the worker's is gone; frame_dtors tells whether the frame was destroyed
        checkpoint("cp1", tally0, live0);
        // what the probe kept is released on THIS thread; a waiter of the dead thread would be a cross-thread wake
        release(k);
        run_for(40ms);
        checkpoint("cp2", tally0, live0);
        return 0;
    }
    if (mode == "E" || mode == "X" || mode == "Q" || mode == "U") {
        static kept static_k; // what the frame is parked on outlives main
        coro_scheduler().spawn(make(family, static_k));
        run_for(5ms);
        std::printf("%s parked: listener.active=%zu\n", mode.c_str(), coro_scheduler().active_count());
        checkpoint("cp1", tally0, live0);
        if (mode == "X")
            std::exit(0);
        if (mode == "Q")
            std::quick_exit(0);
#if !defined(_WIN32)
        if (mode == "U")
            ::_exit(0);
#endif
        return 0;
    }
    if (mode == "G" || mode == "H") {
        // A task driven by hand, with STATIC storage: destroyed after the main thread's listener::current.
        //   G  the task goes before what it is parked on (channels, mutex)
        //   H  what it is parked on goes first: the channel's destructor wakes it, with no loop left
        if (mode == "G") {
            static kept       static_k;
            static task<void> g;
            g = make(family, static_k);
            g.handle().resume();
        } else {
            static task<void> g;
            static kept       static_k;
            g = make(family, static_k);
            g.handle().resume();
        }
        run_for(5ms);
        checkpoint("cp1", tally0, live0);
        return 0;
    }
    if (mode == "D") {
        std::thread th([&] {
            thread_local late_scheduler late; // constructed BEFORE this thread's listener::current: destroyed AFTER it
            late.sched = std::make_unique<CoroutineScheduler>();
            listener::current.clear(); // the thread's loop, constructed second
            CoroutineScheduler::set_current(late.sched.get());
            late.sched->spawn(make(family, k));
            late.sched->run_ready();
            if (listener::current.has_coro_scheduler())
                listener::current.coro_scheduler().run_ready();
            std::printf("D parked: sched.active=%zu\n", late.sched->active_count());
            std::fflush(stdout);
        });
        th.join();
        checkpoint("cp1", tally0, live0);
        release(k);
        run_for(40ms);
        checkpoint("cp2", tally0, live0);
        return 0;
    }
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
