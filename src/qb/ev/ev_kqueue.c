/*
 * libev kqueue backend
 *
 * qb-io: kevent() EINTR retries without clearing kqueue_changecnt first, so a signal
 * during kevent does not drop pending changelist entries before the kernel applies them.
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

#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <sys/event.h>
#include <string.h>
#include <errno.h>

inline_speed void
kqueue_change(EV_P_ int fd, int filter, int flags, int fflags) {
    ++kqueue_changecnt;
    array_needsize(struct kevent, kqueue_changes, kqueue_changemax, kqueue_changecnt, array_needsize_noinit);

    EV_SET(&kqueue_changes[kqueue_changecnt - 1], fd, filter, flags, fflags, 0, 0);
}

/* OS X at least needs this */
#ifndef EV_ENABLE
#define EV_ENABLE 0
#endif
#ifndef NOTE_EOF
#define NOTE_EOF 0
#endif

static void
kqueue_modify(EV_P_ int fd, int oev, int nev) {
    if (oev != nev) {
        if (oev & EV_READ)
            kqueue_change(EV_A_ fd, EVFILT_READ, EV_DELETE, 0);

        if (oev & EV_WRITE)
            kqueue_change(EV_A_ fd, EVFILT_WRITE, EV_DELETE, 0);
    }

    /* to detect close/reopen reliably, we have to re-add */
    /* event requests even when oev == nev */

    if (nev & EV_READ)
        kqueue_change(EV_A_ fd, EVFILT_READ, EV_ADD | EV_ENABLE, NOTE_EOF);

    if (nev & EV_WRITE)
        kqueue_change(EV_A_ fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, NOTE_EOF);
}

static void
kqueue_poll(EV_P_ ev_tstamp timeout) {
    int             res, i;
    struct timespec ts;

    /* need to resize so there is enough space for errors */
    if (kqueue_changecnt > kqueue_eventmax) {
        ev_free(kqueue_events);
        kqueue_eventmax = array_nextsize(sizeof(struct kevent), kqueue_eventmax, kqueue_changecnt);
        kqueue_events   = (struct kevent *) ev_malloc((long) ((size_t) sizeof(struct kevent) * (size_t) kqueue_eventmax));
    }

    EV_RELEASE_CB;
    {
        int eintr;
        for (eintr = 0;; ++eintr) {
            EV_TS_SET(ts, timeout);
            res = kevent((int) (uintptr_t) backend_fd, kqueue_changes, kqueue_changecnt, kqueue_events, kqueue_eventmax, &ts);
            if (ecb_expect_true(res >= 0) || errno != EINTR)
                break;
            /* Do not clear kqueue_changecnt until kevent consumes the changelist; retry bounded EINTR storms. */
            if (ecb_expect_false(eintr >= 255))
                break;
        }
    }
    EV_ACQUIRE_CB;

    if (ecb_expect_false(res < 0)) {
        if (errno == EINTR)
            return;

        ev_syserr("(libev) kqueue kevent");
    }

    kqueue_changecnt = 0;

    for (i = 0; i < res; ++i) {
        int fd = (int) kqueue_events[i].ident;

#if EV_VERIFY >= 2
        EV_ASSERT_MSG(fd >= 0 && fd < anfdmax, "libev: kqueue event fd out of range");
#endif
        if (ecb_expect_false(fd < 0 || fd >= anfdmax))
            continue;

        if (ecb_expect_false(kqueue_events[i].flags & EV_ERROR)) {
            int err = (int) kqueue_events[i].data;

            /* we are only interested in errors for fds that we are interested in :) */
            if (anfds[fd].events) {
                if (err == ENOENT) /* resubmit changes on ENOENT */
                    kqueue_modify(EV_A_ fd, 0, anfds[fd].events);
                else if (err == EBADF) /* on EBADF, we re-check the fd */
                {
                    if (fd_valid(fd))
                        kqueue_modify(EV_A_ fd, 0, anfds[fd].events);
                    else {
                        EV_ASSERT_MSG(0, "libev: kqueue found invalid fd");
                        fd_kill(EV_A_ fd);
                    }
                } else /* on all other errors, we error out on the fd */
                {
                    EV_ASSERT_MSG(0, "libev: kqueue found invalid fd");
                    fd_kill(EV_A_ fd);
                }
            }
        } else
            fd_event(EV_A_ fd, kqueue_events[i].filter == EVFILT_READ ? EV_READ : kqueue_events[i].filter == EVFILT_WRITE ? EV_WRITE : 0);
    }

    if (ecb_expect_false(res == kqueue_eventmax)) {
        ev_free(kqueue_events);
        kqueue_eventmax = array_nextsize(sizeof(struct kevent), kqueue_eventmax, kqueue_eventmax + 1);
        kqueue_events   = (struct kevent *) ev_malloc((long) ((size_t) sizeof(struct kevent) * (size_t) kqueue_eventmax));
    }
}

inline_size int
kqueue_init(EV_P_ int flags) {
    int kq;
    (void) flags;

    /* initialize the kernel queue */
    kqueue_fd_pid = getpid();
    if ((kq = kqueue()) < 0)
        return 0;

    backend_fd = (uintptr_t) (unsigned) kq;

    fcntl(kq, F_SETFD, FD_CLOEXEC); /* not sure if necessary, hopefully doesn't hurt */

    backend_mintime = EV_TS_CONST(1e-9); /* apparently, they did the right thing in freebsd */
    backend_modify  = kqueue_modify;
    backend_poll    = kqueue_poll;

    kqueue_eventmax = 64; /* initial number of events receivable per poll */
    kqueue_events   = (struct kevent *) ev_malloc((long) ((size_t) sizeof(struct kevent) * (size_t) kqueue_eventmax));

    kqueue_changes   = 0;
    kqueue_changemax = 0;
    kqueue_changecnt = 0;

    return EVBACKEND_KQUEUE;
}

inline_size void
kqueue_destroy(EV_P) {
    ev_free(kqueue_events);
    ev_free(kqueue_changes);
}

inline_size void
kqueue_fork(EV_P) {
    /* some BSD kernels don't just destroy the kqueue itself,
     * but also close the fd, which isn't documented, and
     * impossible to support properly.
     * we remember the pid of the kqueue call and only close
     * the fd if the pid is still the same.
     * this leaks fds on sane kernels, but BSD interfaces are
     * notoriously buggy and rarely get fixed.
     */
    pid_t newpid = getpid();

    if (newpid == kqueue_fd_pid)
        close((int) (uintptr_t) backend_fd);

    kqueue_fd_pid = newpid;
    {
        int kq2;

        while ((kq2 = kqueue()) < 0)
            ev_syserr("(libev) kqueue");

        backend_fd = (uintptr_t) (unsigned) kq2;
    }

    fcntl((int) (uintptr_t) backend_fd, F_SETFD, FD_CLOEXEC);

    /* re-register interest in fds */
    fd_rearm_all(EV_A);
}

/* sys/event.h defines EV_ERROR */
#undef EV_ERROR
