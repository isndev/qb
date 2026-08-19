/*
 * wepoll - epoll for Windows
 * https://github.com/piscisaureus/wepoll
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * Part of qev. Vendored from wepoll (epoll for Windows) by Bert Belder,
 * with multiple correctness and portability fixes for qev.
 *   Upstream: https://github.com/piscisaureus/wepoll
 *   Derived from v1.5.8, the last upstream release (2019). Recorded because the
 *   upstream sources carry no version string of their own, so the docs' claim had
 *   nothing in the tree backing it.
 *
 *   THIS IS NOT A REFRESHABLE COPY. Do not re-pull it from upstream: wepoll is
 *   unmaintained at 1.5.8, and this file has diverged from it -- 74% of its
 *   non-comment lines are shared with v1.5.8, the rest being qev's own correctness
 *   and portability fixes plus the qev naming. Replacing it with upstream's would
 *   silently undo all of them.
 *
 * Released under the MIT License (see LICENSE). Portions derived from wepoll
 * remain Copyright (c) 2012-2020 Bert Belder under the BSD-2-Clause license;
 * see THIRD-PARTY-NOTICES.
 */

#ifndef QB_EV_WEPOLL_H_
#define QB_EV_WEPOLL_H_

#ifndef WEPOLL_EXPORT
#define WEPOLL_EXPORT
#endif

#include <stdint.h>

enum EPOLL_EVENTS {
    EPOLLIN      = (int) (1U << 0),
    EPOLLPRI     = (int) (1U << 1),
    EPOLLOUT     = (int) (1U << 2),
    EPOLLERR     = (int) (1U << 3),
    EPOLLHUP     = (int) (1U << 4),
    EPOLLRDNORM  = (int) (1U << 6),
    EPOLLRDBAND  = (int) (1U << 7),
    EPOLLWRNORM  = (int) (1U << 8),
    EPOLLWRBAND  = (int) (1U << 9),
    EPOLLMSG     = (int) (1U << 10), /* Never reported. */
    EPOLLRDHUP   = (int) (1U << 13),
    EPOLLONESHOT = (int) (1U << 31)
};

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2
#define EPOLL_CTL_DEL 3

typedef void     *HANDLE;
typedef uintptr_t SOCKET;

typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
    SOCKET   sock; /* Windows specific */
    HANDLE   hnd;  /* Windows specific */
} epoll_data_t;

struct epoll_event {
    uint32_t     events; /* Epoll events and flags */
    epoll_data_t data;   /* User data variable */
};

#ifdef __cplusplus
extern "C" {
#endif

WEPOLL_EXPORT HANDLE epoll_create(int size);
WEPOLL_EXPORT HANDLE epoll_create1(int flags);

WEPOLL_EXPORT int epoll_close(HANDLE ephnd);

WEPOLL_EXPORT int epoll_ctl(HANDLE ephnd, int op, SOCKET sock, struct epoll_event *event);

WEPOLL_EXPORT int epoll_wait(HANDLE ephnd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QB_EV_WEPOLL_H_ */