/*
 * libev solaris event port backend
 *
 * qb-io: on port_getn failure (EINTR/ETIME), force nget=0 before scanning — nget is
 * undefined on those paths; scanning with the initial nget==1 could read stale events.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * Part of qev, a modernized cross-platform fork of libev.
 * Based on libev by Marc Alexander Lehmann <libev@schmorp.de>.
 *   Upstream: http://software.schmorp.de/pkg/libev.html
 *
 * Released under the MIT License (see LICENSE). Portions derived from libev
 * remain Copyright (c) Marc Alexander Lehmann under the BSD-2-Clause license;
 * see THIRD-PARTY-NOTICES.
 */

/* useful reading:
 *
 * http://bugs.opensolaris.org/view_bug.do?bug_id=6268715 (random results)
 * http://bugs.opensolaris.org/view_bug.do?bug_id=6455223 (just totally broken)
 * http://bugs.opensolaris.org/view_bug.do?bug_id=6873782 (manpage ETIME)
 * http://bugs.opensolaris.org/view_bug.do?bug_id=6874410 (implementation ETIME)
 * http://www.mail-archive.com/networking-discuss@opensolaris.org/msg11898.html ETIME vs. nget
 * http://src.opensolaris.org/source/xref/onnv/onnv-gate/usr/src/lib/libc/port/gen/event_port.c (libc)
 * http://cvs.opensolaris.org/source/xref/onnv/onnv-gate/usr/src/uts/common/fs/portfs/port.c#1325 (kernel)
 */

#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <poll.h>
#include <port.h>
#include <string.h>
#include <errno.h>

#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif

inline_speed void
port_associate_and_check(EV_P_ int fd, int ev) {
    if (0 > port_associate((int) (uintptr_t) backend_fd, PORT_SOURCE_FD, fd,
                           (ev & EV_READ ? POLLIN | POLLRDHUP : 0) | (ev & EV_WRITE ? POLLOUT : 0), 0)) {
        if (errno == EBADFD) {
            EV_ASSERT_MSG(errno != EBADFD, "libev: port_associate found invalid fd");
            fd_kill(EV_A_ fd);
        } else
            ev_syserr("(libev) port_associate");
    }
}

static void
port_modify(EV_P_ int fd, int oev, int nev) {
    /* we need to reassociate no matter what, as closes are
     * once more silently being discarded.
     */
    if (!nev) {
        if (oev)
            port_dissociate((int) (uintptr_t) backend_fd, PORT_SOURCE_FD, fd);
    } else
        port_associate_and_check(EV_A_ fd, nev);
}

static void
port_poll(EV_P_ ev_tstamp timeout) {
    int             res, i;
    struct timespec ts;
    uint_t          nget = 1;

    /* we initialise this to something we will skip in the loop, as */
    /* port_getn can return with nget unchanged, but no indication */
    /* whether it was the original value or has been updated :/ */
    port_events[0].portev_source = 0;

    EV_RELEASE_CB;
    EV_TS_SET(ts, timeout);
    res = port_getn((int) (uintptr_t) backend_fd, port_events, port_eventmax, &nget, &ts);
    EV_ACQUIRE_CB;

    if (ecb_expect_false(res == -1)) {
        if (errno != ETIME && errno != EINTR)
            ev_syserr("(libev) port_getn (see http://bugs.opensolaris.org/view_bug.do?bug_id=6268715, try LIBEV_FLAGS=3 env variable)");
        /* nget is undefined on error; do not scan port_events with stale nget. */
        nget = 0;
    }

    for (i = 0; i < nget; ++i) {
        if (port_events[i].portev_source == PORT_SOURCE_FD) {
            int fd = port_events[i].portev_object;

            fd_event(EV_A_ fd, (port_events[i].portev_events & (POLLOUT | POLLERR | POLLHUP) ? EV_WRITE : 0)
                                   | (port_events[i].portev_events & (POLLIN | POLLERR | POLLHUP | POLLRDHUP) ? EV_READ : 0));

            fd_change(EV_A_ fd, EV__IOFDSET);
        }
    }

    if (ecb_expect_false(nget == port_eventmax)) {
        ev_free(port_events);
        port_eventmax = array_nextsize(sizeof(port_event_t), port_eventmax, port_eventmax + 1);
        port_events   = (port_event_t *) ev_malloc(sizeof(port_event_t) * port_eventmax);
    }
}

inline_size int
port_init(EV_P_ int flags) {
    int portfd;

    /* Initialize the kernel queue */
    if ((portfd = port_create()) < 0)
        return 0;

    backend_fd = (uintptr_t) (unsigned) portfd;

    EV_ASSERT_MSG(PORT_SOURCE_FD, "libev: PORT_SOURCE_FD must not be zero");

    fcntl(portfd, F_SETFD, FD_CLOEXEC); /* not sure if necessary, hopefully doesn't hurt */

    /* if my reading of the opensolaris kernel sources are correct, then
     * opensolaris does something very stupid: it checks if the time has already
     * elapsed and doesn't round up if that is the case, otherwise it DOES round
     * up. Since we can't know what the case is, we need to guess by using a
     * "large enough" timeout. Normally, 1e-9 would be correct.
     */
    backend_mintime = EV_TS_CONST(1e-3); /* needed to compensate for port_getn returning early */
    backend_modify  = port_modify;
    backend_poll    = port_poll;

    port_eventmax = 64; /* initial number of events receivable per poll */
    port_events   = (port_event_t *) ev_malloc(sizeof(port_event_t) * port_eventmax);

    return EVBACKEND_PORT;
}

inline_size void
port_destroy(EV_P) {
    ev_free(port_events);
}

inline_size void
port_fork(EV_P) {
    int portfd;

    close((int) (uintptr_t) backend_fd);

    while ((portfd = port_create()) < 0)
        ev_syserr("(libev) port");

    backend_fd = (uintptr_t) (unsigned) portfd;

    fcntl(portfd, F_SETFD, FD_CLOEXEC);

    /* re-register interest in fds */
    fd_rearm_all(EV_A);
}
