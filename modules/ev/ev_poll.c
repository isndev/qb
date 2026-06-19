/*
 * libev poll fd activity backend
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2025 qb - isndev (cpp.actor).
 *
 * Part of qb-ev, a modernized cross-platform fork of libev.
 * Based on libev by Marc Alexander Lehmann <libev@schmorp.de>.
 *   Upstream: http://software.schmorp.de/pkg/libev.html
 *
 * Released under the MIT License (see LICENSE). Portions derived from libev
 * remain Copyright (c) Marc Alexander Lehmann under the BSD-2-Clause license;
 * see THIRD-PARTY-NOTICES.
 */

#include <poll.h>

/* Linux: half-close detection (parity with epoll EPOLLRDHUP path in libev). */
#ifndef POLLRDHUP
# define POLLRDHUP 0
#endif

inline_size
void
array_needsize_pollidx (int *base, int offset, int count)
{
  /* using memset (.., -1, ...) is tempting, we we try
   * to be ultraportable
   */
  base += offset;
  while (count--)
    *base++ = -1;
}

static void
poll_modify (EV_P_ int fd, int oev, int nev)
{
  int idx;

  if (oev == nev)
    return;

  array_needsize (int, pollidxs, pollidxmax, fd + 1, array_needsize_pollidx);

  idx = pollidxs [fd];

  if (idx < 0) /* need to allocate a new pollfd */
    {
      pollidxs [fd] = idx = pollcnt++;
      array_needsize (struct pollfd, polls, pollmax, pollcnt, array_needsize_noinit);
      polls [idx].fd = fd;
    }

  assert (polls [idx].fd == fd);

  if (nev)
    polls [idx].events =
        (short)((nev & EV_READ ? POLLIN | POLLRDHUP : 0)
        | (nev & EV_WRITE ? POLLOUT : 0));
  else /* remove pollfd */
    {
      pollidxs [fd] = -1;

      if (ecb_expect_true (idx < --pollcnt))
        {
          polls [idx] = polls [pollcnt];
          pollidxs [polls [idx].fd] = idx;
        }
    }
}

static void
poll_poll (EV_P_ ev_tstamp timeout)
{
  struct pollfd *p;
  int res;

  EV_RELEASE_CB;
  res = poll (polls, (nfds_t)pollcnt, (int)EV_TS_TO_MSEC (timeout));
  EV_ACQUIRE_CB;

  if (ecb_expect_false (res < 0))
    {
      if (errno == EBADF)
        fd_ebadf (EV_A);
      else if (errno == ENOMEM && !syserr_cb)
        fd_enomem (EV_A);
      else if (errno != EINTR)
        ev_syserr ("(libev) poll");
    }
  else
    /* Bound the scan: if the kernel's `res` disagrees with revents bits, avoid walking past the array. */
    for (p = polls; res && p < polls + pollcnt; ++p)
      {
        if (ecb_expect_false (p->revents)) /* this expect is debatable */
          {
            --res;

            if (ecb_expect_false (p->revents & POLLNVAL))
              {
                EV_ASSERT_MSG (0, "libev: poll found invalid fd in poll set");
                fd_kill (EV_A_ p->fd);
              }
            else
              fd_event (
                EV_A_
                p->fd,
                (p->revents & (POLLOUT | POLLERR | POLLHUP) ? EV_WRITE : 0)
                | (p->revents & (POLLIN | POLLERR | POLLHUP | POLLRDHUP) ? EV_READ : 0)
              );
          }
      }
}

inline_size
int
poll_init (EV_P_ int flags)
{
  (void) flags;

  backend_mintime = EV_TS_CONST (1e-3);
  backend_modify  = poll_modify;
  backend_poll    = poll_poll;

  pollidxs = 0; pollidxmax = 0;
  polls    = 0; pollmax    = 0; pollcnt = 0;

  return EVBACKEND_POLL;
}

inline_size
void
poll_destroy (EV_P)
{
  ev_free (pollidxs);
  ev_free (polls);
}
