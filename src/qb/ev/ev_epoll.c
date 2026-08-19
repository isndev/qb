/*
 * libev epoll fd activity backend
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

/*
 * general notes about epoll:
 *
 * a) epoll silently removes fds from the fd set. as nothing tells us
 *    that an fd has been removed otherwise, we have to continually
 *    "rearm" fds that we suspect *might* have changed (same
 *    problem with kqueue, but much less costly there).
 * b) the fact that ADD != MOD creates a lot of extra syscalls due to a)
 *    and seems not to have any advantage.
 * c) the inability to handle fork or file descriptors (think dup)
 *    limits the applicability over poll, so this is not a generic
 *    poll replacement.
 * d) epoll doesn't work the same as select with many file descriptors
 *    (such as files). while not critical, no other advanced interface
 *    seems to share this (rather non-unixy) limitation.
 * e) epoll claims to be embeddable, but in practise you never get
 *    a ready event for the epoll fd (broken: <=2.6.26, working: >=2.6.32).
 * f) epoll_ctl returning EPERM means the fd is always ready.
 *
 * lots of "weird code" and complication handling in this file is due
 * to these design problems with epoll, as we try very hard to avoid
 * epoll_ctl syscalls for common usage patterns and handle the breakage
 * ensuing from receiving events for closed and otherwise long gone
 * file descriptors.
 */

#ifdef _WIN32
#include "wepoll.c"
#else
#include <sys/epoll.h>
#endif /* _WIN32 */

#ifndef EPOLLRDHUP
#define EPOLLRDHUP 0
#endif

/*
 * qb-io / VirtualCore execution model (this tree vendors libev for qb):
 * - One struct ev_loop per OS thread; all qev_* calls and watcher callbacks for
 *   that loop run on that thread only (see qb::io::async::listener::current).
 * - On Windows, wepoll is therefore used in the intended single-threaded mode:
 *   no concurrent epoll_wait vs epoll_close on the same handle from different
 *   threads, which sidesteps IOCP lifetime races that multi-threaded misuse could
 *   otherwise create.
 * - ev_embeddable_backends() clears EVBACKEND_EPOLL on _WIN32 so ev_embed is not
 *   advertised for wepoll-backed inner loops (HANDLE is not an embeddable int fd).
 * - Build with EV_VERIFY >= 2 (see ev.c) to enable extra epoll_poll asserts; default
 *   release paths avoid per-event defensive branches for latency.
 * - wepoll waits inside GetQueuedCompletionStatusEx while the port CRITICAL_SECTION is
 *   released; another thread calling epoll_close / CloseHandle on that IOCP is therefore
 *   unsafe. One loop per thread (qb-io) matches wepoll's intended use.
 * - EV_READ registrations use EPOLLRDHUP when available (Linux + wepoll) for TCP half-close.
 */

#define EV_EMASK_EPERM 0x80

/* On Windows wepoll expects a full SOCKET handle (uintptr_t), whereas libev
 * stores file descriptors as plain int.  The ANFD.handle field (populated in
 * ev_io_start for EV_SELECT_IS_WINSOCKET builds) holds the correct 64-bit
 * value; fall back to zero-extending the int when the slot is unset. */
#ifdef _WIN32
#define EV_FD_TO_SOCK(fd) ((anfds[(fd)].handle != 0) ? anfds[(fd)].handle : (SOCKET) (unsigned int) (fd))
#else
#define EV_FD_TO_SOCK(fd) (fd)
#endif

/* backend_fd holds a uintptr_t: Linux epoll fd, or Windows wepoll HANDLE address bits. */
#ifdef _WIN32
#define EV_EPOLL_API_HND ((HANDLE) (void *) (uintptr_t) backend_fd)
#else
#define EV_EPOLL_API_HND ((int) (uintptr_t) backend_fd)
#endif

#define EV_BACKEND_FD_INVALID ((uintptr_t) -1)

static uintptr_t
epoll_epoll_create(void) {
#ifndef _WIN32
    int fd;

#if defined EPOLL_CLOEXEC && !defined __ANDROID__
    fd = epoll_create1(EPOLL_CLOEXEC);

    if (fd < 0 && (errno == EINVAL || errno == ENOSYS))
#endif
    {
        fd = epoll_create(256);
        if (fd >= 0)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    if (fd < 0)
        return EV_BACKEND_FD_INVALID;
    return (uintptr_t) (unsigned) fd;
#else
    {
        HANDLE h = epoll_create(256);

        if (!h)
            return EV_BACKEND_FD_INVALID;
        return (uintptr_t) (void *) h;
    }
#endif
}

static void
epoll_modify(EV_P_ int fd, int oev, int nev) {
    struct epoll_event ev;
    unsigned char      oldmask;

    /*
     * we handle EPOLL_CTL_DEL by ignoring it here
     * on the assumption that the fd is gone anyways
     * if that is wrong, we have to handle the spurious
     * event in epoll_poll.
     * if the fd is added again, we try to ADD it, and, if that
     * fails, we assume it still has the same eventmask.
     */
    if (!nev)
        return;

    oldmask         = anfds[fd].emask;
    anfds[fd].emask = nev;

    /* store the generation counter in the upper 32 bits, the fd in the lower 32 bits */
    ev.data.u64 = (uint64_t) (uint32_t) fd | ((uint64_t) (uint32_t) ++anfds[fd].egen << 32);
    ev.events   = (nev & EV_READ ? EPOLLIN | EPOLLRDHUP : 0) | (nev & EV_WRITE ? EPOLLOUT : 0);

    if (ecb_expect_true(!epoll_ctl(EV_EPOLL_API_HND, oev && oldmask != nev ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, EV_FD_TO_SOCK(fd), &ev)))
        return;

    if (ecb_expect_true(errno == ENOENT)) {
        /* if ENOENT then the fd went away, so try to do the right thing */
        if (!nev)
            goto dec_egen;

        if (!epoll_ctl(EV_EPOLL_API_HND, EPOLL_CTL_ADD, EV_FD_TO_SOCK(fd), &ev))
            return;
    } else if (ecb_expect_true(errno == EEXIST)) {
        /* EEXIST means we ignored a previous DEL, but the fd is still active */
        /* if the kernel mask is the same as the new mask, we assume it hasn't changed */
        if (oldmask == nev)
            goto dec_egen;

        if (!epoll_ctl(EV_EPOLL_API_HND, EPOLL_CTL_MOD, EV_FD_TO_SOCK(fd), &ev))
            return;
    } else if (ecb_expect_true(errno == EPERM)) {
        /* EPERM means the fd is always ready, but epoll is too snobbish */
        /* to handle it, unlike select or poll. */
        anfds[fd].emask = EV_EMASK_EPERM;

        /* add fd to epoll_eperms, if not already inside */
        if (!(oldmask & EV_EMASK_EPERM)) {
            array_needsize(int, epoll_eperms, epoll_epermmax, epoll_epermcnt + 1, array_needsize_noinit);
            epoll_eperms[epoll_epermcnt++] = fd;
        }

        return;
    } else
        EV_ASSERT_MSG(errno != EBADF && errno != ELOOP && errno != EINVAL, "libev: I/O watcher with invalid fd found in epoll_ctl");

    fd_kill(EV_A_ fd);

dec_egen:
    /* we didn't successfully call epoll_ctl, so decrement the generation counter again */
    --anfds[fd].egen;
}

static void
epoll_poll(EV_P_ ev_tstamp timeout) {
    int i;
    int eventcnt;

    if (ecb_expect_false(epoll_epermcnt))
        timeout = EV_TS_CONST(0.);

    /* epoll wait times cannot be larger than (LONG_MAX - 999UL) / HZ msecs, which is below */
    /* the default libev max wait time, however. */
    EV_RELEASE_CB;
    eventcnt = epoll_wait(EV_EPOLL_API_HND, epoll_events, epoll_eventmax, EV_TS_TO_MSEC(timeout));
    EV_ACQUIRE_CB;

    if (ecb_expect_false(eventcnt < 0)) {
        if (errno != EINTR)
            ev_syserr("(libev) epoll_wait");

        return;
    }

    for (i = 0; i < eventcnt; ++i) {
        struct epoll_event *ev = epoll_events + i;

        int fd = (uint32_t) ev->data.u64; /* mask out the lower 32 bits */
#if EV_VERIFY >= 2
        EV_ASSERT_MSG(fd >= 0 && fd < anfdmax, "libev: epoll event fd out of range");
#endif
        /* the kernel only ever returns fds we registered, which stay < anfdmax
         * (anfds is never shrunk), so this never trips in correct operation; the
         * guard hardens the unconditional anfds[fd] indexing below against a
         * corrupted/foreign event, mirroring the kqueue backend. */
        if (ecb_expect_false(fd < 0 || fd >= anfdmax))
            continue;

        int want = anfds[fd].events;
        int got  = (ev->events & (EPOLLOUT | EPOLLERR | EPOLLHUP) ? EV_WRITE : 0)
                   | (ev->events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP) ? EV_READ : 0);

        /*
         * check for spurious notification.
         * this only finds spurious notifications on egen updates
         * other spurious notifications will be found by epoll_ctl, below
         * we assume that fd is always in range, as we never shrink the anfds array
         */
        if (ecb_expect_false((uint32_t) anfds[fd].egen != (uint32_t) (ev->data.u64 >> 32))) {
            /* recreate kernel state */
            postfork |= 2;
            continue;
        }

        if (ecb_expect_false(got & ~want)) {
            anfds[fd].emask = want;

            /*
             * we received an event but are not interested in it, try mod or del
             * this often happens because we optimistically do not unregister fds
             * when we are no longer interested in them, but also when we get spurious
             * notifications for fds from another process. this is partially handled
             * above with the gencounter check (== our fd is not the event fd), and
             * partially here, when epoll_ctl returns an error (== a child has the fd
             * but we closed it).
             * note: for events such as POLLHUP, where we can't know whether it refers
             * to EV_READ or EV_WRITE, we might issue redundant EPOLL_CTL_MOD calls.
             */
            ev->events = (want & EV_READ ? EPOLLIN | EPOLLRDHUP : 0) | (want & EV_WRITE ? EPOLLOUT : 0);

            /* pre-2.6.9 kernels require a non-null pointer with EPOLL_CTL_DEL, */
            /* which is fortunately easy to do for us. */
            if (epoll_ctl(EV_EPOLL_API_HND, want ? EPOLL_CTL_MOD : EPOLL_CTL_DEL, EV_FD_TO_SOCK(fd), ev)) {
                postfork |= 2; /* an error occurred, recreate kernel state */
                continue;
            }
        }

        fd_event(EV_A_ fd, got);
    }

    /* if the receive array was full, increase its size */
    if (ecb_expect_false(eventcnt == epoll_eventmax)) {
        ev_free(epoll_events);
        epoll_eventmax = array_nextsize(sizeof(struct epoll_event), epoll_eventmax, epoll_eventmax + 1);
        epoll_events   = (struct epoll_event *) ev_malloc(sizeof(struct epoll_event) * epoll_eventmax);
    }

    /* now synthesize events for all fds where epoll fails, while select works... */
    for (i = epoll_epermcnt; i--;) {
        int fd = epoll_eperms[i];
#if EV_VERIFY >= 2
        EV_ASSERT_MSG(fd >= 0 && fd < anfdmax, "libev: epoll_eperms fd out of range");
#endif
        unsigned char events = anfds[fd].events & (EV_READ | EV_WRITE);

        if (anfds[fd].emask & EV_EMASK_EPERM && events)
            fd_event(EV_A_ fd, events);
        else {
            epoll_eperms[i] = epoll_eperms[--epoll_epermcnt];
            anfds[fd].emask = 0;
        }
    }
}

inline_size int
epoll_init(EV_P_ int flags) {
    if ((backend_fd = epoll_epoll_create()) == EV_BACKEND_FD_INVALID)
        return 0;

    backend_mintime = EV_TS_CONST(1e-3); /* epoll does sometimes return early, this is just to avoid the worst */
    backend_modify  = epoll_modify;
    backend_poll    = epoll_poll;

    epoll_eventmax = 64; /* initial number of events receivable per poll */
    epoll_events   = (struct epoll_event *) ev_malloc(sizeof(struct epoll_event) * epoll_eventmax);

    return EVBACKEND_EPOLL;
}

inline_size void
epoll_destroy(EV_P) {
    ev_free(epoll_events);
    array_free(epoll_eperm, EMPTY);
}

ecb_cold static void
epoll_fork(EV_P) {
#ifndef _WIN32
    close((int) (uintptr_t) backend_fd);
#else
    epoll_close(EV_EPOLL_API_HND);
#endif

    while ((backend_fd = epoll_epoll_create()) == EV_BACKEND_FD_INVALID)
        ev_syserr("(libev) epoll_create");

    fd_rearm_all(EV_A);
}
