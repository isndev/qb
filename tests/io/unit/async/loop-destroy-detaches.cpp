/**
 * @file unit/async/loop-destroy-detaches.cpp
 * @brief `ev_loop_destroy` detaches every watcher still started on the loop or pending in it, on every backend this
 *        build has: each reads inactive and not pending, a stop that comes after the loop returns before it reads
 *        the freed loop, a signal the loop watched is back to its default action, and the watcher starts again on
 *        another loop (Huly QB-1246, E5).
 *
 * Through qev 5.2 a destroyed loop left its watchers "active" -- libev's documented contract ("None of the active
 * event watchers will be stopped"; "signal and child watchers would need to be stopped manually") -- and the stop
 * a watcher's owner made afterwards read and wrote the freed loop: a coroutine frame parked on the timer of a
 * temporary listener's loop and destroyed after that listener, an io object with static storage destroyed after
 * the main thread's loop. Under AddressSanitizer that stop was a heap-use-after-free in `ev_timer_stop` or
 * `ev_io_stop`, which is why the state checks below come first and return before any stop when they fail: on the
 * unpatched loop they fail on every watcher, with no sanitizer needed.
 *
 * What a detached watcher keeps is what a stop leaves: a timer's deadline relative again, so one restarted on another
 * loop as it was left is due in the delay it had left; on Windows an io watcher's reference on the process-wide
 * descriptor registry given back (its `fd` reads -1, its socket kept), so the same socket is watched again by
 * another loop and detached again when that one goes. And the loop's OWN watchers -- the wake pipe an `ev_async`
 * makes it open -- are not detached: `ev_loop_destroy` still sees them started and closes their descriptors, which
 * the process's descriptor count around a hundred loops' lives shows.
 *
 * The loop is qb's EMBEDDED copy (`<qb/ev/ev.h>`), its watcher profile included: the families this build compiles
 * are the ones checked (`EV_PERIODIC_ENABLE`, `EV_ASYNC_ENABLE`, `EV_SIGNAL_ENABLE`, `EV_STAT_ENABLE`, `EV_IDLE_ENABLE`,
 * `EV_PREPARE_ENABLE`, `EV_CHECK_ENABLE`). The C twin of this case is qev's `test_destroy_detaches`
 * (qev `tests/test-loops.c`), over qev's full profile; the two copies of `ev.c` are byte-identical.
 *
 * One more case pins the fix of `ev_select.c`: a select loop that never watched an fd ran its first pass with the
 * four fd vectors still null and handed them to `memcpy` with a size of 0 -- undefined behaviour, which the
 * sanitize lane's UndefinedBehaviorSanitizer stopped at the loop's first pass (`QB_EV_BACKEND=select`, a timer and
 * nothing else).
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

#ifdef _WIN32
#include <winsock2.h> // before anything that may bring <windows.h> in
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <csignal>
#include <cstdio>
#include <ostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/ev/ev.h>

namespace loop_destroy_detaches_test {

int g_timer_fired = 0;
int g_io_fired    = 0;
/// A backend's destroyed loop left watchers started: the later backends are not run on top of them (a signal is
/// the process's, and the leftover still names the freed loop).
bool g_left_started = false;

void
on_timer(struct ev_loop *loop, ev_timer *, int) {
    ++g_timer_fired;
    ev_break(loop, EVBREAK_ALL);
}
void
on_io(struct ev_loop *loop, ev_io *, int) {
    ++g_io_fired;
    ev_break(loop, EVBREAK_ALL);
}
void
on_watchdog(struct ev_loop *loop, ev_timer *, int) {
    ev_break(loop, EVBREAK_ALL);
}
#if EV_PERIODIC_ENABLE
void
on_periodic(struct ev_loop *, ev_periodic *, int) {}
#endif
#if EV_ASYNC_ENABLE
void
on_async(struct ev_loop *, ev_async *, int) {}
#endif
#if EV_SIGNAL_ENABLE && !defined(_WIN32)
void
on_signal(struct ev_loop *, ev_signal *, int) {}
#endif
#if EV_STAT_ENABLE
void
on_stat(struct ev_loop *, ev_stat *, int) {}
#endif
#if EV_IDLE_ENABLE
void
on_idle(struct ev_loop *, ev_idle *, int) {}
#endif
#if EV_PREPARE_ENABLE
void
on_prepare(struct ev_loop *, ev_prepare *, int) {}
#endif
#if EV_CHECK_ENABLE
void
on_check(struct ev_loop *, ev_check *, int) {}
#endif

struct backend {
    unsigned    flag;
    char const *name;
};

/// Every backend the loop knows; a case runs on the ones `ev_supported_backends()` has, after the build's own pick.
constexpr backend kBackends[] = {
    {EVBACKEND_SELECT, "select"}, {EVBACKEND_POLL, "poll"},         {EVBACKEND_EPOLL, "epoll"},     {EVBACKEND_KQUEUE, "kqueue"},
    {EVBACKEND_PORT, "port"},     {EVBACKEND_LINUXAIO, "linuxaio"}, {EVBACKEND_IOURING, "iouring"},
};

/// How a failure names the backend it ran on.
void
PrintTo(backend const &b, std::ostream *const os) {
    *os << b.name;
}

[[nodiscard]] std::vector<backend>
backends_of_this_build() {
    std::vector<backend> all{{EVFLAG_AUTO, "auto"}}; // every build has one
    unsigned const       have = ev_supported_backends();
    for (backend const &b : kBackends)
        if (have & b.flag)
            all.push_back(b);
    return all;
}

/// Three io endpoints, closed however the case ends: a pipe on POSIX, two UDP sockets on Windows (a watcher there
/// is on a socket, `ev_io_init_sock`). `read` is never readable, `write` always writable.
class io_endpoints {
public:
    io_endpoints() {
#ifdef _WIN32
        WSADATA wsa;
        _wsa   = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        _read  = ::socket(AF_INET, SOCK_DGRAM, 0);
        _write = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        _ok                  = _wsa && _read != INVALID_SOCKET && _write != INVALID_SOCKET;
        _ok                  = _ok && ::bind(_read, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
#else
        int fds[2];
        _ok = ::pipe(fds) == 0;
        if (_ok) {
            _read  = fds[0];
            _write = fds[1];
        }
#endif
    }
    io_endpoints(io_endpoints const &)            = delete;
    io_endpoints &operator=(io_endpoints const &) = delete;
    ~io_endpoints() {
#ifdef _WIN32
        if (_read != INVALID_SOCKET)
            ::closesocket(_read);
        if (_write != INVALID_SOCKET)
            ::closesocket(_write);
        if (_wsa)
            WSACleanup();
#else
        if (_read >= 0)
            ::close(_read);
        if (_write >= 0)
            ::close(_write);
#endif
    }
    [[nodiscard]] bool
    ok() const noexcept {
        return _ok;
    }
    void
    init(ev_io &w, int const events, bool const on_write_end) const noexcept {
#ifdef _WIN32
        ev_io_init_sock(&w, on_io, on_write_end ? _write : _read, events);
#else
        ev_io_init(&w, on_io, on_write_end ? _write : _read, events);
#endif
    }

private:
    bool _ok = false;
#ifdef _WIN32
    bool   _wsa   = false;
    SOCKET _read  = INVALID_SOCKET;
    SOCKET _write = INVALID_SOCKET;
#else
    int _read  = -1;
    int _write = -1;
#endif
};

[[nodiscard]] bool
left_on_loop(ev_watcher const *w) noexcept {
    return ev_is_active(w) || ev_is_pending(w);
}

class LoopDestroyDetaches : public ::testing::TestWithParam<backend> {};

#if EV_SIGNAL_ENABLE && !defined(_WIN32)
/// Whether `signum` is blocked on the calling thread -- where a loop that watches a signal through a signalfd
/// blocks it (it installs no handler then).
[[nodiscard]] bool
blocked_here(int const signum) {
    sigset_t mask;
    sigemptyset(&mask);
    pthread_sigmask(SIG_BLOCK, nullptr, &mask);
    return sigismember(&mask, signum) == 1;
}

/// Whether `signum` has its default action: no loop watches it through a handler.
[[nodiscard]] bool
default_action(int const signum) {
    struct sigaction now{};
    sigaction(signum, nullptr, &now);
    return now.sa_handler == SIG_DFL;
}
#endif

#if EV_ASYNC_ENABLE
/// One life of a loop whose wake pipe is open: an `ev_async` started (the loop opens its wake descriptors for it),
/// sent and delivered, the loop destroyed with the async still started, the async stopped late. False when the host
/// refuses a loop on `flag`.
[[nodiscard]] bool
a_loop_with_its_wake_pipe(unsigned const flag) {
    struct ev_loop *const loop = ev_loop_new(flag);
    if (!loop)
        return false;
    ev_async wake;
    ev_async_init(&wake, on_async);
    ev_async_start(loop, &wake);
    ev_async_send(loop, &wake);
    ev_run(loop, EVRUN_NOWAIT); // the wake travels through the pipe and is delivered
    ev_loop_destroy(loop);      // the async detached; the loop's own pipe watcher closed with the loop
    ev_async_stop(loop, &wake);
    return true;
}
#endif

#ifdef _WIN32
/// The process's open handles: what a loop's wake sockets, its port and its backend's helpers count in.
[[nodiscard]] long
open_descriptors() {
    DWORD count = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &count) ? static_cast<long>(count) : -1;
}
#else
/// The process's open descriptors, counted below 4096 -- where a test process's own are: the kernel hands out the
/// lowest free one. Counted, not the lowest free one read: a loop whose backend descriptor (epoll, io_uring) is
/// closed and whose wake descriptor is not leaves the lowest free one where it was, life after life.
[[nodiscard]] long
open_descriptors() {
    long open = 0;
    for (int fd = 0; fd < 4096; ++fd)
        if (::fcntl(fd, F_GETFD) != -1)
            ++open;
    return open;
}
#endif

} // namespace loop_destroy_detaches_test

using namespace loop_destroy_detaches_test;

/**
 * @test On the build's own backend and then on every other one it has: a loop holding started, pending, and started
 *       AND pending timers, two io watchers on one fd and a write watcher on another (all three known to the backend
 *       by a first pass, in which the write watcher fired), a periodic, an async, a signal and a stat watcher -- each
 *       where the build has the family -- is destroyed. Every watcher then reads inactive and not pending; the
 *       stops that follow (with the freed loop, as a late owner hands it) all return; the signal is back to its
 *       default action; on Windows each io watcher has given its registry reference back (`fd` -1). On a new loop,
 *       the repeating timer restarted as it was left is due in its own delay (5 s, `ev_timer_remaining`), a timer
 *       given a short delay fires, and the write watcher fires -- each judged on its own -- and the write watcher,
 *       left started, is detached again when that loop is destroyed. The backend the loop came up on is asserted.
 *       On the base every watcher still read active -- the case returns there, before the stops, which were a
 *       heap-use-after-free.
 */
TEST_P(LoopDestroyDetaches, ADestroyedLoopKeepsNoneOfItsWatchers) {
    backend const be = GetParam();
    if (g_left_started)
        GTEST_SKIP() << "an earlier backend's destroyed loop left watchers started: nothing more is run on top of them";
    io_endpoints io;
    ASSERT_TRUE(io.ok()) << "the io endpoints";
    struct ev_loop *loop = ev_loop_new(be.flag);
    if (!loop)
        GTEST_SKIP() << be.name << " is compiled in, but this host refuses a loop on it";
    char trace[96];
    std::snprintf(trace, sizeof trace, "backend %s: the loop came up on 0x%x", be.name, ev_backend(loop));
    SCOPED_TRACE(trace);
    if (be.flag == EVFLAG_AUTO)
        EXPECT_NE(ev_backend(loop), 0u);
    else
        EXPECT_EQ(ev_backend(loop), be.flag) << "the loop is not on the backend asked for";

    ev_timer t_active, t_repeat, t_pending, t_both;
    ev_timer_init(&t_active, on_timer, 5., 0.);
    ev_timer_start(loop, &t_active);
    ev_timer_init(&t_repeat, on_timer, 5., 7.);
    ev_timer_start(loop, &t_repeat);
    ev_timer_init(&t_pending, on_timer, 5., 0.); // never started: pending only, the state of an expired one-shot
    ev_timer_init(&t_both, on_timer, 6., 0.);    // started AND pending
    ev_timer_start(loop, &t_both);
    ev_io io_read, io_read2, io_write;
    io.init(io_read, EV_READ, false);
    io.init(io_read2, EV_READ, false); // two watchers on one fd: a list of two
    io.init(io_write, EV_WRITE, true);
    ev_io_start(loop, &io_read);
    ev_io_start(loop, &io_read2);
    ev_io_start(loop, &io_write);
    g_io_fired = 0;
    ev_run(loop, EVRUN_NOWAIT); // the backend learns the fds; the write watcher fires and breaks
    EXPECT_GE(g_io_fired, 1) << "the write watcher fired on the loop before it is destroyed";
    ev_feed_event(loop, &t_pending, EV_TIMER); // fed after the pass: still pending when the loop goes
    ev_feed_event(loop, &t_both, EV_TIMER);
    EXPECT_TRUE(ev_is_pending(&t_pending) && ev_is_pending(&t_both)) << "two fed timers are pending before the loop goes";
#if EV_PERIODIC_ENABLE
    ev_periodic per;
    ev_periodic_init(&per, on_periodic, 0., 3600., 0);
    ev_periodic_start(loop, &per);
#endif
#if EV_ASYNC_ENABLE
    ev_async as;
    ev_async_init(&as, on_async);
    ev_async_start(loop, &as);
#endif
#if EV_SIGNAL_ENABLE && !defined(_WIN32)
    bool const blocked_before = blocked_here(SIGUSR2);
    ev_signal  sg;
    ev_signal_init(&sg, on_signal, SIGUSR2);
    ev_signal_start(loop, &sg);
#endif
#if EV_STAT_ENABLE
    ev_stat st;
    ev_stat_init(&st, on_stat, ".", 0.);
    ev_stat_start(loop, &st);
#endif
#if EV_IDLE_ENABLE
    ev_idle idl;
    ev_idle_init(&idl, on_idle);
    ev_idle_start(loop, &idl);
#endif
#if EV_PREPARE_ENABLE
    ev_prepare prep;
    ev_prepare_init(&prep, on_prepare);
    ev_prepare_start(loop, &prep);
#endif
#if EV_CHECK_ENABLE
    ev_check chk;
    ev_check_init(&chk, on_check);
    ev_check_start(loop, &chk);
#endif

    ev_loop_destroy(loop); // `loop` dangles from here: it is handed to the stops below, as a late owner hands it

    struct named {
        ev_watcher const *w;
        char const       *what;
    };
    std::vector<named> const all{
        {reinterpret_cast<ev_watcher const *>(&t_active), "a started timer"},
        {reinterpret_cast<ev_watcher const *>(&t_repeat), "a repeating timer"},
        {reinterpret_cast<ev_watcher const *>(&t_pending), "a pending timer"},
        {reinterpret_cast<ev_watcher const *>(&t_both), "a started and pending timer"},
        {reinterpret_cast<ev_watcher const *>(&io_read), "an io watcher"},
        {reinterpret_cast<ev_watcher const *>(&io_read2), "the second io watcher of one fd"},
        {reinterpret_cast<ev_watcher const *>(&io_write), "a write watcher"},
#if EV_PERIODIC_ENABLE
        {reinterpret_cast<ev_watcher const *>(&per), "a periodic"},
#endif
#if EV_ASYNC_ENABLE
        {reinterpret_cast<ev_watcher const *>(&as), "an async"},
#endif
#if EV_SIGNAL_ENABLE && !defined(_WIN32)
        {reinterpret_cast<ev_watcher const *>(&sg), "a signal watcher"},
#endif
#if EV_STAT_ENABLE
        {reinterpret_cast<ev_watcher const *>(&st), "a stat watcher"},
#endif
#if EV_IDLE_ENABLE
        {reinterpret_cast<ev_watcher const *>(&idl), "an idle"},
#endif
#if EV_PREPARE_ENABLE
        {reinterpret_cast<ev_watcher const *>(&prep), "a prepare"},
#endif
#if EV_CHECK_ENABLE
        {reinterpret_cast<ev_watcher const *>(&chk), "a check"},
#endif
    };
    std::size_t left = 0;
    for (named const &n : all) {
        EXPECT_FALSE(left_on_loop(n.w)) << n.what << " still reads active or pending on a destroyed loop";
        left += left_on_loop(n.w) ? 1 : 0;
    }
    if (left != 0) { // a stop now would read and write the freed loop, and the signal is still that loop's
        g_left_started = true;
        FAIL() << left << " of " << all.size() << " watchers are still on a loop that is gone";
    }
#ifdef _WIN32
    // what ev_io_stop does last: the watcher's reference on the process-wide descriptor registry, given back
    EXPECT_EQ(io_read.fd, -1) << "an io watcher still holds its registry reference";
    EXPECT_EQ(io_read2.fd, -1) << "the second io watcher of one socket still holds its registry reference";
    EXPECT_EQ(io_write.fd, -1) << "a write watcher still holds its registry reference";
#endif

    // the late stops: each read the freed loop until ev_loop_destroy detached its watchers
    ev_timer_stop(loop, &t_active);
    ev_timer_stop(loop, &t_repeat);
    ev_timer_stop(loop, &t_pending);
    ev_timer_stop(loop, &t_both);
    ev_io_stop(loop, &io_read);
    ev_io_stop(loop, &io_read2);
    ev_io_stop(loop, &io_write);
#if EV_PERIODIC_ENABLE
    ev_periodic_stop(loop, &per);
#endif
#if EV_ASYNC_ENABLE
    ev_async_stop(loop, &as);
#endif
#if EV_SIGNAL_ENABLE && !defined(_WIN32)
    ev_signal_stop(loop, &sg);
    EXPECT_TRUE(default_action(SIGUSR2)) << "the signal the destroyed loop watched still has its handler";
    if (!blocked_before) {
        EXPECT_FALSE(blocked_here(SIGUSR2)) << "the signal the destroyed loop watched is still blocked";
    }
#endif
#if EV_STAT_ENABLE
    ev_stat_stop(loop, &st);
#endif
#if EV_IDLE_ENABLE
    ev_idle_stop(loop, &idl);
#endif
#if EV_PREPARE_ENABLE
    ev_prepare_stop(loop, &prep);
#endif
#if EV_CHECK_ENABLE
    ev_check_stop(loop, &chk);
#endif

    // a detached watcher is a stopped one: it starts again on another loop, as it was left, and fires there
    loop = ev_loop_new(be.flag);
    ASSERT_NE(loop, nullptr);
    ev_timer watchdog;
    ev_timer_init(&watchdog, on_watchdog, 2., 0.); // ends a run in which nothing fires: the checks below say so
    ev_timer_start(loop, &watchdog);

    // restarted WITHOUT ev_timer_set: its deadline was made relative again, so it is due in the delay it had left --
    // not at the destroyed loop's absolute deadline read against this loop's clock
    ev_timer_start(loop, &t_repeat);
    EXPECT_NEAR(ev_timer_remaining(loop, &t_repeat), 5., 1.) << "a detached timer kept its destroyed loop's deadline";
    ev_timer_stop(loop, &t_repeat);

    ev_timer_set(&t_active, 0.001, 0.);
    ev_timer_start(loop, &t_active);
    g_timer_fired = 0;
    ev_run(loop, 0); // the timer fires and breaks
    ev_timer_stop(loop, &t_active);
    EXPECT_EQ(g_timer_fired, 1) << "a detached timer, started on a new loop, never fired";

    ev_io_start(loop, &io_write); // writable at once: fires and breaks
    g_io_fired = 0;
    ev_run(loop, 0);
    EXPECT_GE(g_io_fired, 1) << "a detached io watcher, started on a new loop, never fired";

    // a second destroy on the same descriptor: the write watcher, still started, is detached again
    ev_timer_stop(loop, &watchdog);
    ev_loop_destroy(loop);
    EXPECT_FALSE(left_on_loop(reinterpret_cast<ev_watcher const *>(&io_write)))
        << "an io watcher started on a second loop is still on it once that loop is destroyed";
#ifdef _WIN32
    EXPECT_EQ(io_write.fd, -1) << "the second loop's destroy left the registry reference";
#endif
    ev_io_stop(loop, &io_write);
}

#if EV_ASYNC_ENABLE
/**
 * @test The loop's OWN watchers are not detached: a loop whose wake pipe is open -- an `ev_async` started, sent and
 *       delivered, still started when the loop is destroyed -- closes the pipe's descriptors with it. After a first
 *       life (the process-wide state of a socket stack or a backend is created once), a hundred more leave the
 *       process's descriptors where they were: as many open on POSIX, fewer handles than lives on Windows (a handle
 *       the system opens on its own is not the loop's; a wake pipe left open is at least one per life): the detach
 *       leaves the loop's own alone. On the base, which detached nothing, the async's late stop read the freed loop
 *       (a heap-use-after-free in `ev_async_stop` under AddressSanitizer) before any count was made.
 */
TEST_P(LoopDestroyDetaches, ADestroyedLoopClosesItsOwnDescriptors) {
    constexpr long kLives = 100;
    backend const  be     = GetParam();
    if (!a_loop_with_its_wake_pipe(be.flag))
        GTEST_SKIP() << be.name << " is compiled in, but this host refuses a loop on it";
    long const before = open_descriptors();
    ASSERT_GE(before, 0);
    for (long life = 0; life < kLives; ++life)
        ASSERT_TRUE(a_loop_with_its_wake_pipe(be.flag));
    long const after = open_descriptors();
    RecordProperty("descriptors_left", static_cast<int>(after - before)); // what a run left, in the XML report
#ifdef _WIN32
    EXPECT_LT(after - before, kLives) << kLives << " loops left " << after - before << " handles open";
#else
    EXPECT_EQ(after, before) << kLives << " loops left " << after - before << " descriptors open";
#endif
}
#endif

#if EV_SIGNAL_ENABLE && !defined(_WIN32)
/// One signal watched by a loop built with `flags`, the loop destroyed with the watcher started, the watcher stopped
/// late. `through_signalfd` is what the loop did: blocked the signal (a signalfd) rather than install a handler.
struct signal_given_back {
    bool through_signalfd = false;
    bool handler_after    = true; ///< a handler other than SIG_DFL is still installed
    bool blocked_after    = true; ///< the signal is still blocked
    bool left_on_loop     = true;
};

[[nodiscard]] signal_given_back
watch_a_signal_and_destroy_the_loop(unsigned const flags) {
    signal_given_back r;
    struct ev_loop   *loop = ev_loop_new(flags);
    if (!loop)
        return r;
    ev_signal sg;
    ev_signal_init(&sg, on_signal, SIGUSR2);
    ev_signal_start(loop, &sg);
    r.through_signalfd = blocked_here(SIGUSR2) && default_action(SIGUSR2);
    ev_loop_destroy(loop);
    r.left_on_loop = left_on_loop(reinterpret_cast<ev_watcher const *>(&sg));
    if (r.left_on_loop)
        return r; // a stop now would read the freed loop
    ev_signal_stop(loop, &sg);
    r.handler_after = !default_action(SIGUSR2);
    r.blocked_after = blocked_here(SIGUSR2);
    return r;
}

/**
 * @test The two ways a loop watches a signal, each given back when the loop goes: through a HANDLER (the default:
 *       qb's listeners pass no `EVFLAG_SIGNALFD`, so this is the way qb's loops take) the signal's disposition is
 *       SIG_DFL again; through a SIGNALFD (`EVFLAG_SIGNALFD`, Linux) the loop blocked the signal and installed no
 *       handler, so a disposition says nothing there -- the signal is no longer BLOCKED. Each run asserts the way
 *       its loop took. On the base the watcher was still on the destroyed loop.
 */
TEST(LoopDestroyDetachesSignals, ADestroyedLoopGivesItsSignalBackWhicheverWayItWatchedIt) {
    ASSERT_FALSE(blocked_here(SIGUSR2)) << "the case needs SIGUSR2 unblocked on its thread";
    signal_given_back const handler = watch_a_signal_and_destroy_the_loop(EVFLAG_AUTO);
    EXPECT_FALSE(handler.through_signalfd) << "a loop without EVFLAG_SIGNALFD watched the signal through a signalfd";
    EXPECT_FALSE(handler.left_on_loop) << "the signal watcher is still on its destroyed loop";
    EXPECT_FALSE(handler.handler_after) << "the destroyed loop's handler is still installed";
    EXPECT_FALSE(handler.blocked_after);
#if defined(EV_USE_SIGNALFD) && EV_USE_SIGNALFD
    signal_given_back const signalfd = watch_a_signal_and_destroy_the_loop(EVFLAG_AUTO | EVFLAG_SIGNALFD);
    EXPECT_TRUE(signalfd.through_signalfd) << "a loop asked for a signalfd installed a handler instead";
    EXPECT_FALSE(signalfd.left_on_loop) << "the signal watcher is still on its destroyed loop";
    EXPECT_FALSE(signalfd.blocked_after) << "the signal the destroyed loop watched through a signalfd is still blocked";
    EXPECT_FALSE(signalfd.handler_after);
#endif
}
#endif

INSTANTIATE_TEST_SUITE_P(Backends, LoopDestroyDetaches, ::testing::ValuesIn(backends_of_this_build()),
                         [](::testing::TestParamInfo<backend> const &info) { return std::string(info.param.name); });

/**
 * @test A select loop that never watched an fd runs a timer through `ev_run` to its end. Its first pass copies the
 *       four fd vectors, still null with a size of 0: on the base that was `memcpy(nullptr, nullptr, 0)`, undefined
 *       behaviour that the sanitize lane's UndefinedBehaviorSanitizer reports (and its flags make fatal) at
 *       `ev_select.c`, line 134 there.
 */
TEST(LoopDestroyDetachesSelect, ASelectLoopThatNeverWatchedAnFdRunsATimer) {
    if (!(ev_supported_backends() & EVBACKEND_SELECT))
        GTEST_SKIP() << "this build has no select backend";
    struct ev_loop *const loop = ev_loop_new(EVBACKEND_SELECT);
    ASSERT_NE(loop, nullptr);
    EXPECT_EQ(ev_backend(loop), static_cast<unsigned>(EVBACKEND_SELECT));
    ev_timer timer;
    ev_timer_init(&timer, on_timer, 0.001, 0.);
    ev_timer_start(loop, &timer);
    g_timer_fired = 0;
    ev_run(loop, 0); // returns once the timer, the loop's only watcher, has fired
    EXPECT_EQ(g_timer_fired, 1);
    ev_loop_destroy(loop);
}
