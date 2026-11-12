// e5_ev.cpp -- SCRATCH witness of E5 at the LOOP: what ev_loop_destroy leaves of the watchers still on it, and what a
// stop does afterwards. Valid C and C++; uses the ev API only. Never committed.
//
//   e5_ev <backend> [stops-first]
//     backend      select | poll | epoll | iouring | linuxaio | kqueue | auto
//     stops-first  skip the state checks and go to the late stops: under AddressSanitizer an unpatched ev.c reports
//                  its heap-use-after-free there (without it, the state check fails first, with no sanitizer needed)
//
// exit 0  every watcher is inactive and not pending after ev_loop_destroy, every late stop returned, the signal went
//         back to its default action, and the watchers started again on a new loop fired
// exit 1  a check failed (the line says which)      exit 77  the backend is not available here
#ifdef E5_QEV_STANDALONE
#include <qev/ev.h>
#else
#include <qb/ev/ev.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

static int g_fired  = 0;
static int g_failed = 0;

#define CHECK(cond, what)                               \
    do {                                                \
        if (!(cond)) {                                  \
            printf("[e5] FAIL %s (%s)\n", what, #cond); \
            ++g_failed;                                 \
        }                                               \
    } while (0)
#define DETACHED(w, what) CHECK(!ev_is_active(w) && !ev_is_pending(w), what " is inactive and not pending after ev_loop_destroy")

static void
timer_cb(EV_P_ ev_timer *w, int revents) {
    (void) loop;
    (void) w;
    (void) revents;
    ++g_fired;
}
static void
io_cb(EV_P_ ev_io *w, int revents) {
    (void) w;
    (void) revents;
    ++g_fired;
    ev_break(EV_A_ EVBREAK_ALL);
}
#if EV_PERIODIC_ENABLE
static void
periodic_cb(EV_P_ ev_periodic *w, int revents) {
    (void) loop;
    (void) w;
    (void) revents;
}
#endif
#if EV_ASYNC_ENABLE
static void
async_cb(EV_P_ ev_async *w, int revents) {
    (void) loop;
    (void) w;
    (void) revents;
}
#endif
#if EV_SIGNAL_ENABLE && !defined(_WIN32)
static void
signal_cb(EV_P_ ev_signal *w, int revents) {
    (void) loop;
    (void) w;
    (void) revents;
}
#endif
#if EV_STAT_ENABLE
static void
stat_cb(EV_P_ ev_stat *w, int revents) {
    (void) loop;
    (void) w;
    (void) revents;
}
#endif
#if EV_IDLE_ENABLE
static void
idle_cb(EV_P_ ev_idle *w, int revents) {
    (void) loop;
    (void) w;
    (void) revents;
}
#endif
#if EV_PREPARE_ENABLE
static void
prepare_cb(EV_P_ ev_prepare *w, int revents) {
    (void) loop;
    (void) w;
    (void) revents;
}
#endif
#if EV_CHECK_ENABLE
static void
check_cb(EV_P_ ev_check *w, int revents) {
    (void) loop;
    (void) w;
    (void) revents;
}
#endif

static unsigned
backend_flag(const char *name) {
    if (!strcmp(name, "select"))
        return EVBACKEND_SELECT;
    if (!strcmp(name, "poll"))
        return EVBACKEND_POLL;
    if (!strcmp(name, "epoll"))
        return EVBACKEND_EPOLL;
    if (!strcmp(name, "iouring"))
        return EVBACKEND_IOURING;
    if (!strcmp(name, "linuxaio"))
        return EVBACKEND_LINUXAIO;
    if (!strcmp(name, "kqueue"))
        return EVBACKEND_KQUEUE;
    return EVFLAG_AUTO;
}

int
main(int argc, char **argv) {
    const char     *name        = argc > 1 ? argv[1] : "auto";
    const int       stops_first = argc > 2 && !strcmp(argv[2], "stops-first");
    struct ev_loop *loop;
    ev_timer        t_active, t_repeat, t_pending, t_both;
    ev_io           io_read, io_read2, io_write;
#ifdef _WIN32
    WSADATA            wsa;
    SOCKET             a, b;
    struct sockaddr_in addr;
    int                len = (int) sizeof addr;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    a = socket(AF_INET, SOCK_DGRAM, 0);
    b = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(a, (struct sockaddr *) &addr, sizeof addr) != 0 || getsockname(a, (struct sockaddr *) &addr, &len) != 0)
        return 3;
#else
    int fds[2];
    if (pipe(fds) != 0)
        return 3;
#endif

    loop = ev_loop_new(backend_flag(name));
    if (!loop) {
        printf("[e5] backend %s is not available here\n", name);
        return 77;
    }
    printf("[e5] asked for %s, the loop came up on backend 0x%x\n", name, ev_backend(loop));
    if (strcmp(name, "auto") && ev_backend(loop) != backend_flag(name)) {
        printf("[e5] FAIL the loop is not on the backend asked for\n");
        return 1;
    }

    ev_timer_init(&t_active, timer_cb, 5., 0.);
    ev_timer_start(loop, &t_active);
    ev_timer_init(&t_repeat, timer_cb, 5., 7.);
    ev_timer_start(loop, &t_repeat);
    ev_timer_init(&t_pending, timer_cb, 5., 0.); /* never started: pending only, the state of an expired one-shot */
    ev_timer_init(&t_both, timer_cb, 6., 0.);    /* started AND pending */
    ev_timer_start(loop, &t_both);
#ifdef _WIN32
    ev_io_init_sock(&io_read, io_cb, a, EV_READ);
    ev_io_init_sock(&io_read2, io_cb, a, EV_READ);
    ev_io_init_sock(&io_write, io_cb, b, EV_WRITE);
#else
    ev_io_init(&io_read, io_cb, fds[0], EV_READ);
    ev_io_init(&io_read2, io_cb, fds[0], EV_READ); /* two watchers on one fd: a list of two */
    ev_io_init(&io_write, io_cb, fds[1], EV_WRITE);
#endif
    ev_io_start(loop, &io_read);
    ev_io_start(loop, &io_read2);
    ev_io_start(loop, &io_write);
    ev_run(loop, EVRUN_NOWAIT); /* the backend learns the fds; io_write fires and breaks */
    CHECK(g_fired >= 1, "the write watcher fired once on the first loop");
    g_fired = 0;
    ev_feed_event(loop, &t_pending, EV_TIMER); /* fed after the pass: still pending when the loop goes */
    ev_feed_event(loop, &t_both, EV_TIMER);
    CHECK(ev_is_pending(&t_pending) && ev_is_pending(&t_both), "the two fed timers are pending before the loop is destroyed");
#if EV_PERIODIC_ENABLE
    ev_periodic per;
    ev_periodic_init(&per, periodic_cb, 0., 3600., 0);
    ev_periodic_start(loop, &per);
#endif
#if EV_ASYNC_ENABLE
    ev_async as;
    ev_async_init(&as, async_cb);
    ev_async_start(loop, &as);
#endif
#if EV_SIGNAL_ENABLE && !defined(_WIN32)
    ev_signal sg;
    ev_signal_init(&sg, signal_cb, SIGUSR1);
    ev_signal_start(loop, &sg);
#endif
#if EV_STAT_ENABLE
    ev_stat st;
    ev_stat_init(&st, stat_cb, ".", 0.);
    ev_stat_start(loop, &st);
#endif
#if EV_IDLE_ENABLE
    ev_idle idl;
    ev_idle_init(&idl, idle_cb);
    ev_idle_start(loop, &idl);
#endif
#if EV_PREPARE_ENABLE
    ev_prepare prep;
    ev_prepare_init(&prep, prepare_cb);
    ev_prepare_start(loop, &prep);
#endif
#if EV_CHECK_ENABLE
    ev_check chk;
    ev_check_init(&chk, check_cb);
    ev_check_start(loop, &chk);
#endif

    ev_loop_destroy(loop); /* `loop` dangles from here: it is passed to the stops below, as a late destructor passes it */
    printf("[e5] the loop is destroyed with its watchers still started\n");

    if (!stops_first) {
        DETACHED(&t_active, "a started timer");
        DETACHED(&t_repeat, "a repeating timer");
        DETACHED(&t_pending, "a pending timer");
        DETACHED(&t_both, "a started and pending timer");
        DETACHED(&io_read, "an io watcher");
        DETACHED(&io_read2, "the second io watcher of one fd");
        DETACHED(&io_write, "a write watcher");
#if EV_PERIODIC_ENABLE
        DETACHED(&per, "a periodic");
#endif
#if EV_ASYNC_ENABLE
        DETACHED(&as, "an async");
#endif
#if EV_SIGNAL_ENABLE && !defined(_WIN32)
        DETACHED(&sg, "a signal watcher");
#endif
#if EV_STAT_ENABLE
        DETACHED(&st, "a stat watcher");
#endif
#if EV_IDLE_ENABLE
        DETACHED(&idl, "an idle");
#endif
#if EV_PREPARE_ENABLE
        DETACHED(&prep, "a prepare");
#endif
#if EV_CHECK_ENABLE
        DETACHED(&chk, "a check");
#endif
        if (g_failed) {
            printf("[e5] FAIL %d watcher(s) still on a loop that is gone: a stop now would read and write freed memory\n", g_failed);
            return 1;
        }
    }

    /* the late stops: each read the freed loop until ev_loop_destroy detached its watchers */
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
    CHECK(signal(SIGUSR1, SIG_DFL) == SIG_DFL, "SIGUSR1 is back to its default action: the loop left the signal table");
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
    printf("[e5] every late stop returned\n");

    /* a detached watcher is a stopped one: it starts again on another loop */
    loop = ev_loop_new(backend_flag(name));
    ev_timer_set(&t_active, 0.001, 0.);
    ev_timer_start(loop, &t_active);
    ev_io_start(loop, &io_write);
    ev_run(loop, 0); /* io_write fires and breaks */
    ev_timer_stop(loop, &t_active);
    ev_io_stop(loop, &io_write);
    ev_loop_destroy(loop);
    CHECK(g_fired >= 1, "a detached watcher, started on a new loop, fired");

    if (g_failed) {
        printf("[e5] FAIL %d check(s)\n", g_failed);
        return 1;
    }
    printf("[e5] PASS backend %s: detached, stopped late, started again\n", name);
    return 0;
}
