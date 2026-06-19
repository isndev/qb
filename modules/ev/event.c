/*
 * libevent compatibility layer
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

#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

#define EV_ASSERT_MSG(expr, msg) assert ((expr) && msg)
#define EV_CONTAINER_OF(ptr, type, member) ((type *)(void *)((char *)(ptr) - offsetof (type, member)))

#ifdef EV_EVENT_H
# include EV_EVENT_H
#else
# include "event.h"
#endif

#if EV_MULTIPLICITY
# define dLOOPev struct ev_loop *loop = (struct ev_loop *)ev->ev_base
# define dLOOPbase struct ev_loop *loop = (struct ev_loop *)base
#else
# define dLOOPev
# define dLOOPbase
#endif

/* never accessed, will always be cast from/to ev_loop */
struct event_base
{
  int dummy;
};

static struct event_base *ev_x_cur;

static ev_tstamp
ev_tv_get (struct timeval *tv)
{
  if (tv)
    {
      ev_tstamp after = (ev_tstamp)tv->tv_sec + (ev_tstamp)tv->tv_usec * 1e-6;
      return after != 0. ? after : 1e-6;
    }
  else
    return -1.;
}

static void
ev_tv_set (struct timeval *tv, ev_tstamp at)
{
  long sec = (long)at;
  long usec = (long)((at - (ev_tstamp)sec) * 1e6);

  if (usec >= 1000000L)
    {
      ++sec;
      usec -= 1000000L;
    }
  else if (usec < 0)
    {
      --sec;
      usec += 1000000L;
    }

  tv->tv_sec = (time_t)sec;
#if !defined (_WIN32) || defined (__MINGW32__)
  tv->tv_usec = (suseconds_t)usec;
#else
  tv->tv_usec = (long)usec;
#endif
}

#define EVENT_STRINGIFY(s) # s
#define EVENT_VERSION(a,b) EVENT_STRINGIFY (a) "." EVENT_STRINGIFY (b)

const char *
event_get_version (void)
{
  /* returns ABI, not API or library, version */
  return EVENT_VERSION (EV_VERSION_MAJOR, EV_VERSION_MINOR);
}

const char *
event_get_method (void)
{
  return "libev";
}

void *event_init (void)
{
#if EV_MULTIPLICITY
  /* Idempotent: attach to the process-wide default loop once (libevent 1.x semantics). */
  if (!ev_x_cur)
    ev_x_cur = (struct event_base *)ev_default_loop (EVFLAG_AUTO);
#else
  EV_ASSERT_MSG (!ev_x_cur, "libev: multiple event bases not supported when not compiled with EV_MULTIPLICITY");

  ev_x_cur = (struct event_base *)(long)ev_default_loop (EVFLAG_AUTO);
#endif

  return ev_x_cur;
}

const char *
event_base_get_method (const struct event_base *base)
{
  (void) base;
  return "libev";
}

struct event_base *
event_base_new (void)
{
#if EV_MULTIPLICITY
  return (struct event_base *)ev_loop_new (EVFLAG_AUTO);
#else
  EV_ASSERT_MSG (0, "libev: multiple event bases not supported when not compiled with EV_MULTIPLICITY");
  return NULL;
#endif
}

void event_base_free (struct event_base *base)
{
#if EV_MULTIPLICITY
  struct ev_loop *loop;

  if (!base)
    return;

  loop = (struct ev_loop *)base;
  if (!ev_is_default_loop (loop))
    ev_loop_destroy (loop);
#else
  (void) base;
#endif
}

int event_dispatch (void)
{
  if (!ev_x_cur)
    return -1;

  return event_base_dispatch (ev_x_cur);
}

#ifdef EV_STANDALONE
void event_set_log_callback (event_log_cb cb)
{
  /* nop */
}
#endif

int event_loop (int flags)
{
  if (!ev_x_cur)
    return -1;

  return event_base_loop (ev_x_cur, flags);
}

int event_loopexit (struct timeval *tv)
{
  if (!ev_x_cur)
    return -1;

  return event_base_loopexit (ev_x_cur, tv);
}

event_callback_fn event_get_callback
(const struct event *ev)
{
  if (!ev)
    return 0;

  return ev->ev_callback;
}

static void
ev_x_cb (struct event *ev, int revents)
{
  revents &= EV_READ | EV_WRITE | EV_TIMER | EV_SIGNAL;

  ev->ev_res = revents;
  ev->ev_callback (ev->ev_fd, (short)revents, ev->ev_arg);
}

static void
ev_x_cb_sig (EV_P_ struct ev_signal *w, int revents)
{
#if EV_MULTIPLICITY
  (void) loop;
#endif

  struct event *ev = EV_CONTAINER_OF (w, struct event, iosig.sig);

  if (revents & EV_ERROR)
    event_del (ev);

  ev_x_cb (ev, revents);
}

static void
ev_x_cb_io (EV_P_ struct ev_io *w, int revents)
{
#if EV_MULTIPLICITY
  (void) loop;
#endif

  struct event *ev = EV_CONTAINER_OF (w, struct event, iosig.io);

  if ((revents & EV_ERROR) || !(ev->ev_events & EV_PERSIST))
    event_del (ev);

  ev_x_cb (ev, revents);
}

static void
ev_x_cb_to (EV_P_ struct ev_timer *w, int revents)
{
#if EV_MULTIPLICITY
  (void) loop;
#endif

  struct event *ev = EV_CONTAINER_OF (w, struct event, to);

  event_del (ev);

  ev_x_cb (ev, revents);
}

void event_set (struct event *ev, int fd, short events, void (*cb)(int, short, void *), void *arg)
{
  if (!ev)
    return;

  EV_ASSERT_MSG (ev_x_cur, "libev: call event_init before event_set");

  if (events & EV_SIGNAL)
    ev_init (&ev->iosig.sig, ev_x_cb_sig);
  else
    ev_init (&ev->iosig.io, ev_x_cb_io);

  ev_init (&ev->to, ev_x_cb_to);

  ev->ev_base     = ev_x_cur; /* not threadsafe, but it's how libevent works */
  ev->ev_fd       = fd;
  ev->ev_events   = events;
  ev->ev_pri      = 0;
  ev->ev_callback = cb;
  ev->ev_arg      = arg;
  ev->ev_res      = 0;
  ev->ev_flags    = EVLIST_INIT;
}

int event_once (int fd, short events, void (*cb)(int, short, void *), void *arg, struct timeval *tv)
{
  if (!ev_x_cur || !cb)
    return -1;

  return event_base_once (ev_x_cur, fd, events, cb, arg, tv);
}

int event_add (struct event *ev, struct timeval *tv)
{
  if (!ev || !ev->ev_base)
    return -1;

  dLOOPev;

  if (ev->ev_events & EV_SIGNAL)
    {
      if (!ev_is_active (&ev->iosig.sig))
        {
          ev_signal_set (&ev->iosig.sig, ev->ev_fd);
          ev_signal_start (EV_A_ &ev->iosig.sig);

          ev->ev_flags |= EVLIST_SIGNAL;
        }
    }
  else if (ev->ev_events & (EV_READ | EV_WRITE))
    {
      if (!ev_is_active (&ev->iosig.io))
        {
          ev_io_set (&ev->iosig.io, ev->ev_fd, ev->ev_events & (EV_READ | EV_WRITE));
          ev_io_start (EV_A_ &ev->iosig.io);

          ev->ev_flags |= EVLIST_INSERTED;
        }
    }

  if (tv)
    {
      ev->to.repeat = ev_tv_get (tv);
      ev_timer_again (EV_A_ &ev->to);
      ev->ev_flags |= EVLIST_TIMEOUT;
    }
  else
    {
      ev_timer_stop (EV_A_ &ev->to);
      ev->ev_flags &= ~EVLIST_TIMEOUT;
    }

  ev->ev_flags |= EVLIST_ACTIVE;

  return 0;
}

int event_del (struct event *ev)
{
  if (!ev || !ev->ev_base)
    return 0;

  dLOOPev;

  if (ev->ev_events & EV_SIGNAL)
    ev_signal_stop (EV_A_ &ev->iosig.sig);
  else if (ev->ev_events & (EV_READ | EV_WRITE))
    ev_io_stop (EV_A_ &ev->iosig.io);

  if (ev_is_active (&ev->to))
    ev_timer_stop (EV_A_ &ev->to);

  ev->ev_flags = EVLIST_INIT;

  return 0;
}

void event_active (struct event *ev, int res, short ncalls)
{
  (void) ncalls;

  if (!ev || !ev->ev_base)
    return;

  dLOOPev;

  if (res & EV_TIMEOUT)
    ev_feed_event (EV_A_ &ev->to, res & EV_TIMEOUT);

  if (res & EV_SIGNAL)
    ev_feed_event (EV_A_ &ev->iosig.sig, res & EV_SIGNAL);

  if (res & (EV_READ | EV_WRITE))
    ev_feed_event (EV_A_ &ev->iosig.io, res & (EV_READ | EV_WRITE));
}

int event_pending (struct event *ev, short events, struct timeval *tv)
{
  short revents = 0;
  short requested = events & (EV_TIMEOUT | EV_READ | EV_WRITE | EV_SIGNAL);

  if (!ev || !ev->ev_base)
    return 0;

  dLOOPev;

  if (ev->ev_events & EV_SIGNAL)
    {
      /* sig */
      if (ev_is_active (&ev->iosig.sig) || ev_is_pending (&ev->iosig.sig))
        revents |= EV_SIGNAL;
    }
  else if (ev->ev_events & (EV_READ | EV_WRITE))
    {
      /* io */
      if (ev_is_active (&ev->iosig.io) || ev_is_pending (&ev->iosig.io))
        revents |= ev->ev_events & (EV_READ | EV_WRITE);
    }

  if (ev_is_active (&ev->to) || ev_is_pending (&ev->to))
    {
      revents |= EV_TIMEOUT;

      if (tv && (requested & EV_TIMEOUT))
        {
          ev_tstamp at = ev_now (EV_A);

          if (ev_is_active (&ev->to))
            at += ev_timer_remaining (EV_A_ &ev->to);

          ev_tv_set (tv, at);
        }
    }

  return requested & revents;
}

int event_priority_init (int npri)
{
  if (!ev_x_cur)
    return -1;

  return event_base_priority_init (ev_x_cur, npri);
}

int event_priority_set (struct event *ev, int pri)
{
  if (!ev)
    return -1;

  ev->ev_pri = pri;

  return 0;
}

int event_base_set (struct event_base *base, struct event *ev)
{
  if (!ev)
    return -1;

  ev->ev_base = base;

  return 0;
}

int event_base_loop (struct event_base *base, int flags)
{
  if (!base)
    return -1;

#if EV_MULTIPLICITY
  {
    struct ev_loop *loop = (struct ev_loop *)base;

    return !ev_run (EV_A_ flags);
  }
#else
  (void) base;
  return !ev_run (flags);
#endif
}

int event_base_dispatch (struct event_base *base)
{
  return event_base_loop (base, 0);
}

static void
ev_x_loopexit_cb (int revents, void *base)
{
  (void) revents;

#if EV_MULTIPLICITY
  struct ev_loop *loop = (struct ev_loop *)base;

  ev_break (EV_A_ EVBREAK_ONE);
#else
  (void) base;
  (void) revents;
  ev_break (EVBREAK_ONE);
#endif
}

int event_base_loopexit (struct event_base *base, struct timeval *tv)
{
  ev_tstamp after = ev_tv_get (tv);

  if (!base)
    return -1;

#if EV_MULTIPLICITY
  {
    struct ev_loop *loop = (struct ev_loop *)base;

    ev_once (EV_A_ -1, 0, after >= 0. ? after : 0., ev_x_loopexit_cb, (void *)base);
  }
#else
  ev_once (-1, 0, after >= 0. ? after : 0., ev_x_loopexit_cb, (void *)base);
#endif

  return 0;
}

struct ev_x_once
{
  int fd;
  void (*cb)(int, short, void *);
  void *arg;
};

static void
ev_x_once_cb (int revents, void *arg)
{
  struct ev_x_once *once = (struct ev_x_once *)arg;

  if (!once || !once->cb)
    {
      free (once);
      return;
    }

  once->cb (once->fd, (short)revents, once->arg);
  free (once);
}

int event_base_once (struct event_base *base, int fd, short events, void (*cb)(int, short, void *), void *arg, struct timeval *tv)
{
  struct ev_x_once *once;
  short io_events = events & (EV_READ | EV_WRITE);
  ev_tstamp timeout = tv ? ev_tv_get (tv) : ((events & EV_TIMEOUT) && !io_events ? 0. : -1.);

  if (!base || !cb)
    return -1;

  if ((fd < 0 || !io_events) && timeout < 0.)
    return -1;

  once = (struct ev_x_once *)malloc (sizeof (struct ev_x_once));
  if (!once)
    return -1;

  once->fd  = fd;
  once->cb  = cb;
  once->arg = arg;

#if EV_MULTIPLICITY
  {
    struct ev_loop *loop = (struct ev_loop *)base;

    ev_once (EV_A_ fd, io_events, timeout, ev_x_once_cb, (void *)once);
  }
#else
  (void) base;
  ev_once (fd, io_events, timeout, ev_x_once_cb, (void *)once);
#endif

  return 0;
}

int event_base_priority_init (struct event_base *base, int npri)
{
  (void) base;
  (void) npri;
  return 0;
}
