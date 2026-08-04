/*
 * libev native API header
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor).
 *
 * Part of qb-ev, a modernized cross-platform fork of libev.
 * Based on libev by Marc Alexander Lehmann <libev@schmorp.de>.
 *   Upstream: http://software.schmorp.de/pkg/libev.html
 *
 * Released under the MIT License (see LICENSE). Portions derived from libev
 * remain Copyright (c) Marc Alexander Lehmann under the BSD-2-Clause license;
 * see THIRD-PARTY-NOTICES.
 */

/* Coexistence with a system libev / libevent.
 *
 * 1. INCLUDE GUARDS -- resolved. Every header of this fork now carries a guard named
 *    after the fork, not after upstream: QEV_H_ here, QEVPP_H_ in qev++.h,
 *    QEV_EVENT_H_ in event.h, QEV_EVENT_COMPAT_H_ in event_compat.h, QEV_WRAP_H in
 *    qev_wrap.h, QEV_WEPOLL_H_ in wepoll.h, QEV_CONFIG_H_ in the generated
 *    qev_config.h. So ONE translation unit may include both <qb/vendor/qev/qev.h> and
 *    a system <ev.h>, in either order, and both sets of declarations are present.
 *
 *    This was NOT always true. These guards used to keep upstream's spellings (EV_H_,
 *    EVPP_H__, EVENT_H_, ...), so whichever header came second in a translation unit
 *    was silently swallowed by the other's guard and its declarations were simply
 *    absent -- and because <qb/main.h> pulls qev.h in transitively, any consumer that
 *    also used a real libev hit it. The failure was at least loud (a compile error,
 *    e.g. `ev_default_loop` undeclared), never silent, but it had no workaround short
 *    of separating the two APIs into different .cpp files.
 *
 *    Note the C symbols were already distinct: everything libev-native is renamed
 *    ev_* -> qev_*. Only the header-level guards were unfinished.
 *
 * 2. LINK SYMBOLS -- NOT resolved, deliberately. libqev.a and a real libev.a each
 *    export 82 symbols and share exactly 24 of them: the `event_*` libevent-
 *    compatibility layer (event_init, event_add, event_base_loop, ...). Those 24 are
 *    not a fork artefact -- they are libevent's published API, and any libev built
 *    with its compat layer exports the same names. Renaming them would destroy the
 *    one thing the compat layer exists to provide, which is libevent's spelling.
 *
 *    The consequence, measured: linking both archives succeeds silently in either
 *    order and the linker simply takes the first definition (confirmed with -Wl,-y;
 *    there is no diagnostic, and -fvisibility=hidden does not close it). A program
 *    that pulls in both therefore gets ONE `event_*` implementation and cannot tell
 *    which. If you use the `event_*` compat API, link one or the other, not both.
 *    Everything else -- the qev_* native API and the C++ ev:: wrappers -- is
 *    unaffected and safe to mix.
 *
 * This comment is the authoritative copy of both caveats for consumers: it ships with
 * the installed header (<prefix>/include/qb/vendor/qev/qev.h), whereas qb's readme/
 * tree is not installed. Keep it in sync with readme/7_reference/cmake_dependencies.md.
 */
#ifndef QEV_H_
#define QEV_H_
#include <stdint.h>
/* Pull in the build-time config when present. qev.h is self-contained (every
 * config macro it reads has a built-in default), so a missing config header is
 * fine — this just lets a generated config override the defaults. Works under
 * CMake (qev_config.h), autotools (config.h + -DHAVE_CONFIG_H) and when the header
 * is installed and consumed elsewhere (no config available → defaults). */
#if defined EV_CONFIG_H
#include EV_CONFIG_H
#elif defined HAVE_CONFIG_H
#include "config.h"
#elif defined __has_include
#if __has_include("qev_config.h")
#include "qev_config.h"
#endif
#endif
#ifdef __cplusplus
#define EV_CPP(x) x
#if __cplusplus >= 201103L
#define EV_NOEXCEPT noexcept
#else
#define EV_NOEXCEPT
#endif
#else
#define EV_CPP(x)
#define EV_NOEXCEPT
#endif
#define EV_THROW EV_NOEXCEPT /* pre-4.25, do not use in new code */

EV_CPP(extern "C" {)

/*****************************************************************************/

/* pre-4.0 compatibility */
#ifndef EV_COMPAT3
#define EV_COMPAT3 1
#endif

#ifndef EV_FEATURES
#if defined __OPTIMIZE_SIZE__
#define EV_FEATURES 0x7c
#else
#define EV_FEATURES 0x7f
#endif
#endif

#define EV_FEATURE_CODE ((EV_FEATURES) & 1)
#define EV_FEATURE_DATA ((EV_FEATURES) & 2)
#define EV_FEATURE_CONFIG ((EV_FEATURES) & 4)
#define EV_FEATURE_API ((EV_FEATURES) & 8)
#define EV_FEATURE_WATCHERS ((EV_FEATURES) & 16)
#define EV_FEATURE_BACKENDS ((EV_FEATURES) & 32)
#define EV_FEATURE_OS ((EV_FEATURES) & 64)

/* these priorities are inclusive, higher priorities will be invoked earlier */
#ifndef EV_MINPRI
#define EV_MINPRI (EV_FEATURE_CONFIG ? -2 : 0)
#endif
#ifndef EV_MAXPRI
#define EV_MAXPRI (EV_FEATURE_CONFIG ? +2 : 0)
#endif

#ifndef EV_MULTIPLICITY
#define EV_MULTIPLICITY EV_FEATURE_CONFIG
#endif

#ifndef EV_PERIODIC_ENABLE
#define EV_PERIODIC_ENABLE EV_FEATURE_WATCHERS
#endif

#ifndef EV_STAT_ENABLE
#define EV_STAT_ENABLE EV_FEATURE_WATCHERS
#endif

#ifndef EV_PREPARE_ENABLE
#define EV_PREPARE_ENABLE 0 /* EV_FEATURE_WATCHERS */
#endif

#ifndef EV_CHECK_ENABLE
#define EV_CHECK_ENABLE 0 /* EV_FEATURE_WATCHERS */
#endif

#ifndef EV_IDLE_ENABLE
#define EV_IDLE_ENABLE 0 /* EV_FEATURE_WATCHERS */
#endif

#ifndef EV_FORK_ENABLE
#define EV_FORK_ENABLE 0 /* EV_FEATURE_WATCHERS */
#endif

#ifndef EV_CLEANUP_ENABLE
#define EV_CLEANUP_ENABLE EV_FEATURE_WATCHERS
#endif

#ifndef EV_SIGNAL_ENABLE
#define EV_SIGNAL_ENABLE EV_FEATURE_WATCHERS
#endif

#ifndef EV_CHILD_ENABLE
#ifdef _WIN32
#define EV_CHILD_ENABLE 0
#else
#define EV_CHILD_ENABLE 0 /* EV_FEATURE_WATCHERS */
#endif
#endif

#ifndef EV_ASYNC_ENABLE
#define EV_ASYNC_ENABLE 0 /* EV_FEATURE_WATCHERS */
#endif

#ifndef EV_EMBED_ENABLE
#define EV_EMBED_ENABLE 0 /* EV_FEATURE_WATCHERS */
#endif

#ifndef EV_WALK_ENABLE
#define EV_WALK_ENABLE 0 /* not yet */
#endif

    /*****************************************************************************/

#if EV_CHILD_ENABLE && !EV_SIGNAL_ENABLE
#undef EV_SIGNAL_ENABLE
#define EV_SIGNAL_ENABLE 1
#endif

    /*****************************************************************************/

#ifndef EV_TSTAMP_T
#define EV_TSTAMP_T double
#endif
typedef EV_TSTAMP_T qev_tstamp;

#include <stddef.h> /* for offsetof */
#include <string.h> /* for memmove */

#ifndef EV_ATOMIC_T
#include <signal.h>
#define EV_ATOMIC_T sig_atomic_t volatile
#endif

#if EV_STAT_ENABLE
#ifdef _WIN32
#include <time.h>
#include <sys/types.h>
#endif
#include <sys/stat.h>
#endif

/* support multiple event loops? */
#if EV_MULTIPLICITY
    struct qev_loop;
#define EV_P struct qev_loop *loop           /* a loop as sole parameter in a declaration */
#define EV_P_ EV_P,                         /* a loop as first of multiple parameters */
#define EV_A loop                           /* a loop as sole argument to a function call */
#define EV_A_ EV_A,                         /* a loop as first of multiple arguments */
#define EV_DEFAULT_UC qev_default_loop_uc_() /* the default loop, if initialised, as sole arg */
#define EV_DEFAULT_UC_ EV_DEFAULT_UC,       /* the default loop as first of multiple arguments */
#define EV_DEFAULT qev_default_loop(0)       /* the default loop as sole arg */
#define EV_DEFAULT_ EV_DEFAULT,             /* the default loop as first of multiple arguments */
#else
#define EV_P void
#define EV_P_
#define EV_A
#define EV_A_
#define EV_DEFAULT
#define EV_DEFAULT_
#define EV_DEFAULT_UC
#define EV_DEFAULT_UC_
#undef EV_EMBED_ENABLE
#endif

/* EV_INLINE is used for functions in header files */
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) || (defined(__GNUC__) && __GNUC__ >= 3)
#define EV_INLINE static inline
#else
#define EV_INLINE static
#endif

#ifdef EV_API_STATIC
#define EV_API_DECL static
#else
#define EV_API_DECL extern
#endif

/* EV_PROTOTYPES can be used to switch of prototype declarations */
#ifndef EV_PROTOTYPES
#define EV_PROTOTYPES 1
#endif

    /*****************************************************************************/

#define EV_VERSION_MAJOR 5
#define EV_VERSION_MINOR 0

    /* eventmask, revents, events... */
    enum {
        EV_UNDEF    = (int) 0xFFFFFFFF, /* guaranteed to be invalid */
        EV_NONE     = 0x00,             /* no events */
        EV_READ     = 0x01,             /* qev_io detected read will not block */
        EV_WRITE    = 0x02,             /* qev_io detected write will not block */
        EV__IOFDSET = 0x80,             /* internal use only */
        EV_IO       = EV_READ,          /* alias for type-detection */
        EV_TIMER    = 0x00000100,       /* timer timed out */
#if EV_COMPAT3
        EV_TIMEOUT = EV_TIMER, /* pre 4.0 API compatibility */
#endif
        EV_PERIODIC = 0x00000200,      /* periodic timer timed out */
        EV_SIGNAL   = 0x00000400,      /* signal was received */
        EV_CHILD    = 0x00000800,      /* child/pid had status change */
        EV_STAT     = 0x00001000,      /* stat data changed */
        EV_IDLE     = 0x00002000,      /* event loop is idling */
        EV_PREPARE  = 0x00004000,      /* event loop about to poll */
        EV_CHECK    = 0x00008000,      /* event loop finished poll */
        EV_EMBED    = 0x00010000,      /* embedded event loop needs sweep */
        EV_FORK     = 0x00020000,      /* event loop resumed in child */
        EV_CLEANUP  = 0x00040000,      /* event loop resumed in child */
        EV_ASYNC    = 0x00080000,      /* async intra-loop signal */
        EV_CUSTOM   = 0x01000000,      /* for use by user code */
        EV_ERROR    = (int) 0x80000000 /* sent when an error occurs */
    };

/* can be used to add custom fields to all watchers, while losing binary compatibility */
#ifndef EV_COMMON
#define EV_COMMON void *data;
#endif

#ifndef EV_CB_DECLARE
#define EV_CB_DECLARE(type) void (*cb)(EV_P_ struct type * w, int revents);
#endif
#ifndef EV_CB_INVOKE
#define EV_CB_INVOKE(watcher, revents) (watcher)->cb(EV_A_(watcher), (revents))
#endif

/* not official, do not use */
#define EV_CB(type, name) void name(EV_P_ struct qev_##type *w, int revents)

    /*
     * struct member types:
     * private: you may look at them, but not change them,
     *          and they might not mean anything to you.
     * ro: can be read anytime, but only changed when the watcher isn't active.
     * rw: can be read and modified anytime, even when the watcher is active.
     *
     * some internal details that might be helpful for debugging:
     *
     * active is either 0, which means the watcher is not active,
     *           or the array index of the watcher (periodics, timers)
     *           or the array index + 1 (most other watchers)
     *           or simply 1 for watchers that aren't in some array.
     * pending is either 0, in which case the watcher isn't,
     *           or the array index + 1 in the pendings array.
     */

#if EV_MINPRI == EV_MAXPRI
#define EV_DECL_PRIORITY
#elif !defined(EV_DECL_PRIORITY)
#define EV_DECL_PRIORITY int priority;
#endif

/* shared by all watchers */
#define EV_WATCHER(type)                  \
    int active;             /* private */ \
    int pending;            /* private */ \
    EV_DECL_PRIORITY        /* private */ \
        EV_COMMON           /* rw */      \
        EV_CB_DECLARE(type) /* private */

#define EV_WATCHER_LIST(type) \
    EV_WATCHER(type)          \
    struct qev_watcher_list *next; /* private */

#define EV_WATCHER_TIME(type) \
    EV_WATCHER(type)          \
    qev_tstamp at; /* private */

    /* base class, nothing to see here unless you subclass */
    typedef struct qev_watcher {
        EV_WATCHER(qev_watcher)
    } qev_watcher;

    /* base class, nothing to see here unless you subclass */
    typedef struct qev_watcher_list {
        EV_WATCHER_LIST(qev_watcher_list)
    } qev_watcher_list;

    /* base class, nothing to see here unless you subclass */
    typedef struct qev_watcher_time {
        EV_WATCHER_TIME(qev_watcher_time)
    } qev_watcher_time;

    EV_INLINE int *qev_watcher_active_(void *w) EV_NOEXCEPT {
        return (int *) (void *) ((char *) w + offsetof(qev_watcher, active));
    }

    EV_INLINE const int *qev_watcher_active_const_(const void *w) EV_NOEXCEPT {
        return (const int *) (const void *) ((const char *) w + offsetof(qev_watcher, active));
    }

    EV_INLINE int *qev_watcher_pending_(void *w) EV_NOEXCEPT {
        return (int *) (void *) ((char *) w + offsetof(qev_watcher, pending));
    }

    EV_INLINE const int *qev_watcher_pending_const_(const void *w) EV_NOEXCEPT {
        return (const int *) (const void *) ((const char *) w + offsetof(qev_watcher, pending));
    }

#if EV_MINPRI != EV_MAXPRI
    EV_INLINE int *qev_watcher_priority_(void *w) EV_NOEXCEPT {
        return (int *) (void *) ((char *) w + offsetof(qev_watcher, priority));
    }

    EV_INLINE const int *qev_watcher_priority_const_(const void *w) EV_NOEXCEPT {
        return (const int *) (const void *) ((const char *) w + offsetof(qev_watcher, priority));
    }
#endif

    /* invoked when fd is either EV_READable or EV_WRITEable */
    /* revent EV_READ, EV_WRITE */
    typedef struct qev_io {
        EV_WATCHER_LIST(qev_io)

        int fd;     /* ro */
        int events; /* ro */
#if defined _WIN32
        uintptr_t handle; /* ro: native win32 socket/handle for fd mapping */
#endif
    } qev_io;

    /* invoked after a specific time, repeatable (based on monotonic clock) */
    /* revent EV_TIMEOUT */
    typedef struct qev_timer {
        EV_WATCHER_TIME(qev_timer)

        qev_tstamp repeat; /* rw */
    } qev_timer;

    /* invoked at some specific time, possibly repeating at regular intervals (based on UTC) */
    /* revent EV_PERIODIC */
    typedef struct qev_periodic {
        EV_WATCHER_TIME(qev_periodic)

        qev_tstamp offset;                                                             /* rw */
        qev_tstamp interval;                                                           /* rw */
        qev_tstamp (*reschedule_cb)(struct qev_periodic *w, qev_tstamp now) EV_NOEXCEPT; /* rw */
    } qev_periodic;

    /* invoked when the given signal has been received */
    /* revent EV_SIGNAL */
    typedef struct qev_signal {
        EV_WATCHER_LIST(qev_signal)

        int signum; /* ro */
    } qev_signal;

    /* invoked when sigchld is received and waitpid indicates the given pid */
    /* revent EV_CHILD */
    /* does not support priorities */
    typedef struct qev_child {
        EV_WATCHER_LIST(qev_child)

        int flags;   /* private */
        int pid;     /* ro */
        int rpid;    /* rw, holds the received pid */
        int rstatus; /* rw, holds the exit status, use the macros from sys/wait.h */
    } qev_child;

#if EV_STAT_ENABLE
/* st_nlink = 0 means missing file or other error */
#ifdef _WIN32
    typedef struct _stati64 qev_statdata;
#else
typedef struct stat qev_statdata;
#endif

    /* invoked each time the stat data changes for a given path */
    /* revent EV_STAT */
    typedef struct qev_stat {
        EV_WATCHER_LIST(qev_stat)

        qev_timer    timer;    /* private */
        qev_tstamp   interval; /* ro */
        const char *path;     /* ro */
        qev_statdata prev;     /* ro */
        qev_statdata attr;     /* ro */

        int wd; /* wd for inotify, fd for kqueue */
    } qev_stat;
#endif

    /* invoked when the nothing else needs to be done, keeps the process from blocking */
    /* revent EV_IDLE */
    typedef struct qev_idle {
        EV_WATCHER(qev_idle)
    } qev_idle;

    /* invoked for each run of the mainloop, just before the blocking call */
    /* you can still change events in any way you like */
    /* revent EV_PREPARE */
    typedef struct qev_prepare {
        EV_WATCHER(qev_prepare)
    } qev_prepare;

    /* invoked for each run of the mainloop, just after the blocking call */
    /* revent EV_CHECK */
    typedef struct qev_check {
        EV_WATCHER(qev_check)
    } qev_check;

    /* the callback gets invoked before check in the child process when a fork was detected */
    /* revent EV_FORK */
    typedef struct qev_fork {
        EV_WATCHER(qev_fork)
    } qev_fork;

    /* is invoked just before the loop gets destroyed */
    /* revent EV_CLEANUP */
    typedef struct qev_cleanup {
        EV_WATCHER(qev_cleanup)
    } qev_cleanup;

#if EV_EMBED_ENABLE
    /* used to embed an event loop inside another */
    /* the callback gets invoked when the event loop has handled events, and can be 0 */
    typedef struct qev_embed {
        EV_WATCHER(qev_embed)

        struct qev_loop *other; /* ro */
#undef EV_IO_ENABLE
#define EV_IO_ENABLE 1
        qev_io io; /* private */
#undef EV_PREPARE_ENABLE
#define EV_PREPARE_ENABLE 1
        qev_prepare  prepare;  /* private */
        qev_check    check;    /* unused */
        qev_timer    timer;    /* unused */
        qev_periodic periodic; /* unused */
        qev_idle     idle;     /* unused */
        qev_fork     fork;     /* private */
        qev_cleanup  cleanup;  /* unused */
    } qev_embed;
#endif

#if EV_ASYNC_ENABLE
    /* invoked when somebody calls qev_async_send on the watcher */
    /* revent EV_ASYNC */
    typedef struct qev_async {
        EV_WATCHER(qev_async)

        EV_ATOMIC_T sent; /* private */
    } qev_async;

#define qev_async_pending(w) (+(w)->sent)
#endif

    /* the presence of this union forces similar struct layout */
    union qev_any_watcher {
        struct qev_watcher      w;
        struct qev_watcher_list wl;

        struct qev_io       io;
        struct qev_timer    timer;
        struct qev_periodic periodic;
        struct qev_signal   signal;
        struct qev_child    child;
#if EV_STAT_ENABLE
        struct qev_stat stat;
#endif
#if EV_IDLE_ENABLE
        struct qev_idle idle;
#endif
        struct qev_prepare prepare;
        struct qev_check   check;
#if EV_FORK_ENABLE
        struct qev_fork fork;
#endif
#if EV_CLEANUP_ENABLE
        struct qev_cleanup cleanup;
#endif
#if EV_EMBED_ENABLE
        struct qev_embed embed;
#endif
#if EV_ASYNC_ENABLE
        struct qev_async async;
#endif
    };

    /* flag bits for qev_default_loop and qev_loop_new */
    enum {
        /* the default */
        EVFLAG_AUTO = 0x00000000U, /* not quite a mask */
        /* flag bits */
        EVFLAG_NOENV     = 0x01000000U, /* do NOT consult environment */
        EVFLAG_FORKCHECK = 0x02000000U, /* check for a fork in each iteration */
        /* debugging/feature disable */
        EVFLAG_NOINOTIFY = 0x00100000U, /* do not attempt to use inotify */
#if EV_COMPAT3
        EVFLAG_NOSIGFD = 0, /* compatibility to pre-3.9 */
#endif
        EVFLAG_SIGNALFD  = 0x00200000U, /* attempt to use signalfd */
        EVFLAG_NOSIGMASK = 0x00400000U, /* avoid modifying the signal mask */
        EVFLAG_NOTIMERFD = 0x00800000U  /* avoid creating a timerfd */
    };

    /* method bits to be ored together */
    enum {
        EVBACKEND_SELECT  = 0x00000001U, /* available just about anywhere */
        EVBACKEND_POLL    = 0x00000002U, /* !win, !aix, broken on osx */
        EVBACKEND_EPOLL   = 0x00000004U, /* linux */
        EVBACKEND_KQUEUE  = 0x00000008U, /* bsd, broken on osx */
        EVBACKEND_DEVPOLL = 0x00000010U,
        /* solaris 8 */                   /* NYI */
        EVBACKEND_PORT     = 0x00000020U, /* solaris 10 */
        EVBACKEND_LINUXAIO = 0x00000040U, /* linux AIO, 4.19+ */
        EVBACKEND_IOURING  = 0x00000080U, /* linux io_uring, 5.1+ */
        EVBACKEND_ALL      = 0x000000FFU, /* all known backends */
        EVBACKEND_MASK     = 0x0000FFFFU  /* all future backends */
    };

#if EV_PROTOTYPES
    EV_API_DECL int qev_version_major(void) EV_NOEXCEPT;
    EV_API_DECL int qev_version_minor(void) EV_NOEXCEPT;

    EV_API_DECL unsigned int qev_supported_backends(void) EV_NOEXCEPT;
    EV_API_DECL unsigned int qev_recommended_backends(void) EV_NOEXCEPT;
    EV_API_DECL unsigned int qev_embeddable_backends(void) EV_NOEXCEPT;

    EV_API_DECL qev_tstamp qev_time(void) EV_NOEXCEPT;
    EV_API_DECL void      qev_sleep(qev_tstamp delay) EV_NOEXCEPT; /* sleep for a while */

    /* Sets the allocation function to use, works like realloc.
     * It is used to allocate and free memory.
     * If it returns zero when memory needs to be allocated, the library might abort
     * or take some potentially destructive action.
     * The default is your system realloc function.
     */
    EV_API_DECL void qev_set_allocator(void *(*cb)(void *ptr, long size) EV_NOEXCEPT) EV_NOEXCEPT;

    /* set the callback function to call on a
     * retryable syscall error
     * (such as failed select, poll, epoll_wait)
     */
    EV_API_DECL void qev_set_syserr_cb(void (*cb)(const char *msg) EV_NOEXCEPT) EV_NOEXCEPT;

#if EV_MULTIPLICITY

    /* the default loop is the only one that handles signals and child watchers */
    /* you can call this as often as you like */
    EV_API_DECL struct qev_loop *qev_default_loop(unsigned int flags EV_CPP(= 0)) EV_NOEXCEPT;

#ifdef EV_API_STATIC
    EV_API_DECL struct qev_loop *qev_default_loop_ptr;
#endif

    EV_INLINE struct qev_loop *qev_default_loop_uc_(void) EV_NOEXCEPT {
        extern struct qev_loop *qev_default_loop_ptr;

        return qev_default_loop_ptr;
    }

    EV_INLINE int qev_is_default_loop(EV_P) EV_NOEXCEPT {
        return EV_A == EV_DEFAULT_UC;
    }

    /* create and destroy alternative loops that don't handle signals */
    EV_API_DECL struct qev_loop *qev_loop_new(unsigned int flags EV_CPP(= 0)) EV_NOEXCEPT;

    EV_API_DECL qev_tstamp qev_now(EV_P) EV_NOEXCEPT; /* time w.r.t. timers and the eventloop, updated after each poll */

#else

EV_API_DECL int qev_default_loop (unsigned int flags EV_CPP (= 0)) EV_NOEXCEPT; /* returns true when successful */

EV_API_DECL qev_tstamp qev_rt_now;

EV_INLINE qev_tstamp
qev_now (void) EV_NOEXCEPT
{
  return qev_rt_now;
}

/* looks weird, but qev_is_default_loop (EV_A) still works if this exists */
EV_INLINE int
qev_is_default_loop (void) EV_NOEXCEPT
{
  return 1;
}

#endif /* multiplicity */

    /* destroy event loops, also works for the default loop */
    EV_API_DECL void qev_loop_destroy(EV_P);

    /* this needs to be called after fork, to duplicate the loop */
    /* when you want to re-use it in the child */
    /* you can call it in either the parent or the child */
    /* you can actually call it at any time, anywhere :) */
    EV_API_DECL void qev_loop_fork(EV_P) EV_NOEXCEPT;

    EV_API_DECL unsigned int qev_backend(EV_P) EV_NOEXCEPT; /* backend in use by loop */

    EV_API_DECL void qev_now_update(EV_P) EV_NOEXCEPT; /* update event loop time */

#if EV_WALK_ENABLE
    /* walk (almost) all watchers in the loop of a given type, invoking the */
    /* callback on every such watcher. The callback might stop the watcher, */
    /* but do nothing else with the loop */
    EV_API_DECL void qev_walk(EV_P_ int types, void (*cb)(EV_P_ int type, void *w)) EV_NOEXCEPT;
#endif

#endif /* prototypes */

    /* qev_run flags values */
    enum {
        EVRUN_NOWAIT = 1, /* do not block/wait */
        EVRUN_ONCE   = 2  /* block *once* only */
    };

    /* qev_break how values */
    enum {
        EVBREAK_CANCEL = 0, /* undo unloop */
        EVBREAK_ONE    = 1, /* unloop once */
        EVBREAK_ALL    = 2  /* unloop all loops */
    };

#if EV_PROTOTYPES
    EV_API_DECL int  qev_run(EV_P_ int flags EV_CPP(= 0));
    EV_API_DECL void qev_break(EV_P_ int how EV_CPP(= EVBREAK_ONE)) EV_NOEXCEPT; /* break out of the loop */

    /*
     * ref/unref can be used to add or remove a refcount on the mainloop. every watcher
     * keeps one reference. if you have a long-running watcher you never unregister that
     * should not keep qev_loop from running, unref() after starting, and ref() before stopping.
     */
    EV_API_DECL void qev_ref(EV_P) EV_NOEXCEPT;
    EV_API_DECL void qev_unref(EV_P) EV_NOEXCEPT;

    /*
     * One-shot: register temporary I/O and/or a timer, invoke cb once, then tear down.
     * - fd >= 0 and (events & EV_READ|EV_WRITE): watch that fd; otherwise skip I/O.
     * - timeout >= 0.: one-shot timer after that delay; timeout < 0. skips the timer (no "wait forever" here).
     * - If cb is NULL, or both I/O and timer are skipped, the call does nothing.
     */
    EV_API_DECL void qev_once(EV_P_ int fd, int events, qev_tstamp timeout, void (*cb)(int revents, void *arg), void *arg) EV_NOEXCEPT;

    EV_API_DECL void qev_invoke_pending(EV_P); /* invoke all pending watchers */

#if EV_FEATURE_API
    EV_API_DECL unsigned int qev_iteration(EV_P) EV_NOEXCEPT; /* number of loop iterations */
    EV_API_DECL unsigned int qev_depth(EV_P) EV_NOEXCEPT;     /* #qev_loop enters - #qev_loop leaves */
    EV_API_DECL void         qev_verify(EV_P) EV_NOEXCEPT;    /* abort if loop data corrupted */

    EV_API_DECL void qev_set_io_collect_interval(EV_P_ qev_tstamp interval) EV_NOEXCEPT;      /* sleep at least this time, default 0 */
    EV_API_DECL void qev_set_timeout_collect_interval(EV_P_ qev_tstamp interval) EV_NOEXCEPT; /* sleep at least this time, default 0 */

    /* advanced stuff for threading etc. support, see docs */
    EV_API_DECL void  qev_set_userdata(EV_P_ void *data) EV_NOEXCEPT;
    EV_API_DECL void *qev_userdata(EV_P) EV_NOEXCEPT;
    typedef void (*qev_loop_callback)(EV_P);
    EV_API_DECL void qev_set_invoke_pending_cb(EV_P_ qev_loop_callback invoke_pending_cb) EV_NOEXCEPT;
    /* C++ doesn't allow the use of the qev_loop_callback typedef here, so we need to spell it out */
    EV_API_DECL void qev_set_loop_release_cb(EV_P_ void (*release)(EV_P) EV_NOEXCEPT, void (*acquire)(EV_P) EV_NOEXCEPT) EV_NOEXCEPT;

    EV_API_DECL unsigned int qev_pending_count(EV_P) EV_NOEXCEPT; /* number of pending events, if any */

    /*
     * stop/start the timer handling.
     */
    EV_API_DECL void qev_suspend(EV_P) EV_NOEXCEPT;
    EV_API_DECL void qev_resume(EV_P) EV_NOEXCEPT;
#endif

#endif

/* these may evaluate ev multiple times, and the other arguments at most once */
/* either use qev_init + qev_TYPE_set, or the qev_TYPE_init macro, below, to first initialise a watcher */
#define qev_init(ev, cb_)                                        \
    do {                                                        \
        *qev_watcher_active_(ev) = *qev_watcher_pending_(ev) = 0; \
        qev_set_priority((ev), 0);                               \
        qev_set_cb((ev), cb_);                                   \
    } while (0)

/* qev_io_modify() is a real function (see qev.c): it must notify the backend via
 * fd_change() so the kernel registration is updated in place (EPOLL_CTL_MOD),
 * instead of the old macro that only touched w->events and relied on callers
 * doing a stop/start cycle. Declared with the other qev_io_* prototypes below. */
#if defined _WIN32
    EV_API_DECL int qev_win32_socket_fd(uintptr_t handle) EV_NOEXCEPT;
#define qev_io_set(ev, fd_, events_)             \
    do {                                        \
        (ev)->fd     = (fd_);                   \
        (ev)->events = (events_) | EV__IOFDSET; \
        (ev)->handle = 0;                       \
    } while (0)
#define qev_io_set_sock(ev, sock_, events_)      \
    do {                                        \
        (ev)->fd     = -1;                      \
        (ev)->events = (events_) | EV__IOFDSET; \
        (ev)->handle = (uintptr_t) (sock_);     \
    } while (0)
#else
#define qev_io_set(ev, fd_, events_)             \
    do {                                        \
        (ev)->fd     = (fd_);                   \
        (ev)->events = (events_) | EV__IOFDSET; \
    } while (0)
#endif
#ifdef __cplusplus
#define qev_timer_set(ev, after_, repeat_) \
    do {                                  \
        (ev)->at     = (after_);          \
        (ev)->repeat = (repeat_);         \
    } while (0)
#else
#define qev_timer_set(ev, after_, repeat_) \
    do {                                  \
        (ev)->at     = (after_);          \
        (ev)->repeat = (repeat_);         \
    } while (0)
#endif
#define qev_periodic_set(ev, ofs_, ival_, rcb_) \
    do {                                       \
        (ev)->offset        = (ofs_);          \
        (ev)->interval      = (ival_);         \
        (ev)->reschedule_cb = (rcb_);          \
    } while (0)
#define qev_signal_set(ev, signum_) \
    do {                           \
        (ev)->signum = (signum_);  \
    } while (0)
#define qev_child_set(ev, pid_, trace_) \
    do {                               \
        (ev)->pid   = (pid_);          \
        (ev)->flags = !!(trace_);      \
    } while (0)
#define qev_stat_set(ev, path_, interval_) \
    do {                                  \
        (ev)->path     = (path_);         \
        (ev)->interval = (interval_);     \
        (ev)->wd       = -2;              \
    } while (0)
#define qev_idle_set(ev)    /* nop, yes, this is a serious in-joke */
#define qev_prepare_set(ev) /* nop, yes, this is a serious in-joke */
#define qev_check_set(ev)   /* nop, yes, this is a serious in-joke */
#define qev_embed_set(ev, other_) \
    do {                         \
        (ev)->other = (other_);  \
    } while (0)
#define qev_fork_set(ev)    /* nop, yes, this is a serious in-joke */
#define qev_cleanup_set(ev) /* nop, yes, this is a serious in-joke */
#define qev_async_set(ev)   /* nop, yes, this is a serious in-joke */

#define qev_io_init(ev, cb, fd, events)   \
    do {                                 \
        qev_init((ev), (cb));             \
        qev_io_set((ev), (fd), (events)); \
    } while (0)
#if defined _WIN32
#define qev_io_init_sock(ev, cb, sock, events)   \
    do {                                        \
        qev_init((ev), (cb));                    \
        qev_io_set_sock((ev), (sock), (events)); \
    } while (0)
#endif
#define qev_timer_init(ev, cb, after, repeat)   \
    do {                                       \
        qev_init((ev), (cb));                   \
        qev_timer_set((ev), (after), (repeat)); \
    } while (0)
#define qev_periodic_init(ev, cb, ofs, ival, rcb)     \
    do {                                             \
        qev_init((ev), (cb));                         \
        qev_periodic_set((ev), (ofs), (ival), (rcb)); \
    } while (0)
#define qev_signal_init(ev, cb, signum) \
    do {                               \
        qev_init((ev), (cb));           \
        qev_signal_set((ev), (signum)); \
    } while (0)
#define qev_child_init(ev, cb, pid, trace)   \
    do {                                    \
        qev_init((ev), (cb));                \
        qev_child_set((ev), (pid), (trace)); \
    } while (0)
#define qev_stat_init(ev, cb, path, interval)   \
    do {                                       \
        qev_init((ev), (cb));                   \
        qev_stat_set((ev), (path), (interval)); \
    } while (0)
#define qev_idle_init(ev, cb) \
    do {                     \
        qev_init((ev), (cb)); \
        qev_idle_set((ev));   \
    } while (0)
#define qev_prepare_init(ev, cb) \
    do {                        \
        qev_init((ev), (cb));    \
        qev_prepare_set((ev));   \
    } while (0)
#define qev_check_init(ev, cb) \
    do {                      \
        qev_init((ev), (cb));  \
        qev_check_set((ev));   \
    } while (0)
#define qev_embed_init(ev, cb, other) \
    do {                             \
        qev_init((ev), (cb));         \
        qev_embed_set((ev), (other)); \
    } while (0)
#define qev_fork_init(ev, cb) \
    do {                     \
        qev_init((ev), (cb)); \
        qev_fork_set((ev));   \
    } while (0)
#define qev_cleanup_init(ev, cb) \
    do {                        \
        qev_init((ev), (cb));    \
        qev_cleanup_set((ev));   \
    } while (0)
#define qev_async_init(ev, cb) \
    do {                      \
        qev_init((ev), (cb));  \
        qev_async_set((ev));   \
    } while (0)

#define qev_is_pending(ev) (0 + *qev_watcher_pending_const_(ev)) /* ro, true when watcher is waiting for callback invocation */
#define qev_is_active(ev) (0 + *qev_watcher_active_const_(ev))   /* ro, true when the watcher has been started */

#define qev_cb_(ev) (ev)->cb /* rw */
#define qev_cb(ev) (memmove(&qev_cb_(ev), (const char *) (const void *) (ev) + offsetof(qev_watcher, cb), sizeof(qev_cb_(ev))), (ev)->cb)

#if EV_MINPRI == EV_MAXPRI
#define qev_priority(ev) ((ev), EV_MINPRI)
#define qev_set_priority(ev, pri) ((ev), (pri))
#else
#define qev_priority(ev) (+*qev_watcher_priority_const_(ev))
#define qev_set_priority(ev, pri) (*qev_watcher_priority_(ev) = (pri))
#endif

#define qev_periodic_at(ev) (+(ev)->at)

#ifndef qev_set_cb
/* memmove is used here to avoid strict aliasing violations, and hopefully is optimized out by any reasonable compiler */
#define qev_set_cb(ev, cb_) (qev_cb_(ev) = (cb_), memmove((char *) (void *) (ev) + offsetof(qev_watcher, cb), &qev_cb_(ev), sizeof(qev_cb_(ev))))
#endif

/* stopping (enabling, adding) a watcher does nothing if it is already running */
/* stopping (disabling, deleting) a watcher does nothing unless it's already running */
#if EV_PROTOTYPES

    /* feeds an event into a watcher as if the event actually occurred */
    /* accepts any qev_watcher type */
    EV_API_DECL void qev_feed_event(EV_P_ void *w, int revents) EV_NOEXCEPT;
    EV_API_DECL void qev_feed_fd_event(EV_P_ int fd, int revents) EV_NOEXCEPT;
#if EV_SIGNAL_ENABLE
    EV_API_DECL void qev_feed_signal(int signum) EV_NOEXCEPT;
    EV_API_DECL void qev_feed_signal_event(EV_P_ int signum) EV_NOEXCEPT;
#endif
    EV_API_DECL void qev_invoke(EV_P_ void *w, int revents);
    EV_API_DECL int  qev_clear_pending(EV_P_ void *w) EV_NOEXCEPT;

    EV_API_DECL void qev_io_start(EV_P_ qev_io * w) EV_NOEXCEPT;
    EV_API_DECL void qev_io_stop(EV_P_ qev_io * w) EV_NOEXCEPT;
    EV_API_DECL void qev_io_modify(EV_P_ qev_io * w, int events) EV_NOEXCEPT;

    EV_API_DECL void qev_timer_start(EV_P_ qev_timer * w) EV_NOEXCEPT;
    EV_API_DECL void qev_timer_stop(EV_P_ qev_timer * w) EV_NOEXCEPT;
    /* stops if active and no repeat, restarts if active and repeating, starts if inactive and repeating */
    EV_API_DECL void qev_timer_again(EV_P_ qev_timer * w) EV_NOEXCEPT;
    /* return remaining time */
    EV_API_DECL qev_tstamp qev_timer_remaining(EV_P_ qev_timer * w) EV_NOEXCEPT;

#if EV_PERIODIC_ENABLE
    EV_API_DECL void qev_periodic_start(EV_P_ qev_periodic * w) EV_NOEXCEPT;
    EV_API_DECL void qev_periodic_stop(EV_P_ qev_periodic * w) EV_NOEXCEPT;
    EV_API_DECL void qev_periodic_again(EV_P_ qev_periodic * w) EV_NOEXCEPT;
#endif

/* only supported in the default loop */
#if EV_SIGNAL_ENABLE
    EV_API_DECL void qev_signal_start(EV_P_ qev_signal * w) EV_NOEXCEPT;
    EV_API_DECL void qev_signal_stop(EV_P_ qev_signal * w) EV_NOEXCEPT;
#endif

/* only supported in the default loop */
#if EV_CHILD_ENABLE
    EV_API_DECL void qev_child_start(EV_P_ qev_child * w) EV_NOEXCEPT;
    EV_API_DECL void qev_child_stop(EV_P_ qev_child * w) EV_NOEXCEPT;
#endif

#if EV_STAT_ENABLE
    EV_API_DECL void qev_stat_start(EV_P_ qev_stat * w) EV_NOEXCEPT;
    EV_API_DECL void qev_stat_stop(EV_P_ qev_stat * w) EV_NOEXCEPT;
    EV_API_DECL void qev_stat_stat(EV_P_ qev_stat * w) EV_NOEXCEPT;
#endif

#if EV_IDLE_ENABLE
    EV_API_DECL void qev_idle_start(EV_P_ qev_idle * w) EV_NOEXCEPT;
    EV_API_DECL void qev_idle_stop(EV_P_ qev_idle * w) EV_NOEXCEPT;
#endif

#if EV_PREPARE_ENABLE
    EV_API_DECL void qev_prepare_start(EV_P_ qev_prepare * w) EV_NOEXCEPT;
    EV_API_DECL void qev_prepare_stop(EV_P_ qev_prepare * w) EV_NOEXCEPT;
#endif

#if EV_CHECK_ENABLE
    EV_API_DECL void qev_check_start(EV_P_ qev_check * w) EV_NOEXCEPT;
    EV_API_DECL void qev_check_stop(EV_P_ qev_check * w) EV_NOEXCEPT;
#endif

#if EV_FORK_ENABLE
    EV_API_DECL void qev_fork_start(EV_P_ qev_fork * w) EV_NOEXCEPT;
    EV_API_DECL void qev_fork_stop(EV_P_ qev_fork * w) EV_NOEXCEPT;
#endif

#if EV_CLEANUP_ENABLE
    EV_API_DECL void qev_cleanup_start(EV_P_ qev_cleanup * w) EV_NOEXCEPT;
    EV_API_DECL void qev_cleanup_stop(EV_P_ qev_cleanup * w) EV_NOEXCEPT;
#endif

#if EV_EMBED_ENABLE
    /* only supported when loop to be embedded is in fact embeddable */
    EV_API_DECL void qev_embed_start(EV_P_ qev_embed * w) EV_NOEXCEPT;
    EV_API_DECL void qev_embed_stop(EV_P_ qev_embed * w) EV_NOEXCEPT;
    EV_API_DECL void qev_embed_sweep(EV_P_ qev_embed * w) EV_NOEXCEPT;
#endif

#if EV_ASYNC_ENABLE
    EV_API_DECL void qev_async_start(EV_P_ qev_async * w) EV_NOEXCEPT;
    EV_API_DECL void qev_async_stop(EV_P_ qev_async * w) EV_NOEXCEPT;
    EV_API_DECL void qev_async_send(EV_P_ qev_async * w) EV_NOEXCEPT;
#endif

#if EV_COMPAT3
#define EVLOOP_NONBLOCK EVRUN_NOWAIT
#define EVLOOP_ONESHOT EVRUN_ONCE
#define EVUNLOOP_CANCEL EVBREAK_CANCEL
#define EVUNLOOP_ONE EVBREAK_ONE
#define EVUNLOOP_ALL EVBREAK_ALL
#if EV_PROTOTYPES
    EV_INLINE void qev_loop(EV_P_ int flags) {
        qev_run(EV_A_ flags);
    }
    EV_INLINE void qev_unloop(EV_P_ int how) {
        qev_break(EV_A_ how);
    }
    EV_INLINE void qev_default_destroy(void) {
        qev_loop_destroy(EV_DEFAULT);
    }
    EV_INLINE void qev_default_fork(void) {
        qev_loop_fork(EV_DEFAULT);
    }
#if EV_FEATURE_API
    EV_INLINE unsigned int qev_loop_count(EV_P) {
        return qev_iteration(EV_A);
    }
    EV_INLINE unsigned int qev_loop_depth(EV_P) {
        return qev_depth(EV_A);
    }
    EV_INLINE void qev_loop_verify(EV_P) {
        qev_verify(EV_A);
    }
#endif
#endif
#else
  typedef struct qev_loop qev_loop;
#endif

#endif

EV_CPP(
})

#endif
