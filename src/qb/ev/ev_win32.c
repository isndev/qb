/*
 * libev win32 compatibility cruft (_not_ a backend)
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

#ifdef _WIN32

/* note: the comment below could not be substantiated, but what would I care */
/* MSDN says this is required to handle SIGFPE */
/* my wild guess would be that using something floating-pointy is required */
/* for the crt to do something about it */
volatile double SIGFPE_REQ = 0.0f;

static SOCKET
ev_tcp_socket(void) {
#if EV_USE_WSASOCKET
    return WSASocket(AF_INET, SOCK_STREAM, 0, 0, 0, 0);
#else
    return socket(AF_INET, SOCK_STREAM, 0);
#endif
}

/* oh, the humanity! */
static int
ev_pipe(int filedes[2]) {
    struct sockaddr_in addr      = {0};
    int                addr_size = sizeof(addr);
    struct sockaddr_in adr2;
    int                adr2_size = sizeof(adr2);
    SOCKET             listener;
    SOCKET             sock[2] = {INVALID_SOCKET, INVALID_SOCKET};

    if ((listener = ev_tcp_socket()) == INVALID_SOCKET)
        return -1;

    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(listener, (struct sockaddr *) &addr, addr_size))
        goto fail;

    if (getsockname(listener, (struct sockaddr *) &addr, &addr_size))
        goto fail;

    if (listen(listener, 1))
        goto fail;

    if ((sock[0] = ev_tcp_socket()) == INVALID_SOCKET)
        goto fail;

    if (connect(sock[0], (struct sockaddr *) &addr, addr_size))
        goto fail;

    sock[1] = accept(listener, 0, 0);
    if (sock[1] == INVALID_SOCKET)
        goto fail;

    /* windows vista returns fantasy port numbers for sockets:
     * example for two interconnected tcp sockets:
     *
     * (Socket::unpack_sockaddr_in getsockname $sock0)[0] == 53364
     * (Socket::unpack_sockaddr_in getpeername $sock0)[0] == 53363
     * (Socket::unpack_sockaddr_in getsockname $sock1)[0] == 53363
     * (Socket::unpack_sockaddr_in getpeername $sock1)[0] == 53365
     *
     * wow! tridirectional sockets!
     *
     * this way of checking ports seems to work:
     */
    if (getpeername(sock[0], (struct sockaddr *) &addr, &addr_size))
        goto fail;

    if (getsockname(sock[1], (struct sockaddr *) &adr2, &adr2_size))
        goto fail;

    errno = WSAEINVAL;
    if (addr_size != adr2_size || addr.sin_addr.s_addr != adr2.sin_addr.s_addr /* just to be sure, I mean, it's windows */
        || addr.sin_port != adr2.sin_port)
        goto fail;

    closesocket(listener);

#if EV_SELECT_IS_WINSOCKET
    filedes[0] = EV_WIN32_HANDLE_TO_FD(sock[0]);
    filedes[1] = EV_WIN32_HANDLE_TO_FD(sock[1]);
#else
    /* when select isn't winsocket, we also expect socket, connect, accept etc.
     * to work on fds */
    filedes[0] = sock[0];
    filedes[1] = sock[1];
#endif

    return 0;

fail:
    closesocket(listener);

    if (sock[0] != INVALID_SOCKET)
        closesocket(sock[0]);
    if (sock[1] != INVALID_SOCKET)
        closesocket(sock[1]);

    return -1;
}

#undef pipe
#define pipe(filedes) ev_pipe(filedes)

/* qev: the two clocks of a loop on Windows (Huly QB-193). libev read both from
 * GetSystemTimeAsFileTime -- the system TICK, a memory read of KUSER_SHARED_DATA (3 ns) that
 * steps every 1 to 15.6 ms (measured 2.2 ms here) -- and, MSVC having no clock_gettime, used it
 * as the MONOTONIC clock too: every timer was judged at the tick's granularity, a 0-second
 * timer started and run in the same tick did not fire, and a wall-clock jump moved the loop's
 * idea of "now". The wall clock is GetSystemTimePreciseAsFileTime now (Windows 8+, resolved at
 * run time so the binary still loads where only the coarse call exists), read as rarely as on
 * every other host -- once per MIN_TIMEJUMP/2 -- and the monotonic clock is
 * QueryPerformanceCounter: sub-microsecond, never steps back, no syscall on any Windows this
 * decade.
 *
 * Both are process-wide caches -- the precise call's address, the seconds per QPC tick -- set on
 * first use (Huly QB-337). Every cold thread used to compute and store them, plain stores racing
 * plain loads on the other threads: identical values, a data race all the same. They are set ONCE
 * now, by ev_win32_clock_init_once under InitOnceExecuteOnce (a concurrent first caller waits for
 * it), and published by a release store each. A hot path tests its cache with the matching acquire
 * load -- on x86-64 the plain mov it made before (MSVC's volatile read, __atomic on clang/gcc), on
 * arm64 an ldar -- calls the out-of-line initialiser while it reads 0, then uses the value with an
 * ordinary read, ordered after that load. Each cache is a union of the value and its integer image:
 * the atomics move the image, the call and the arithmetic use the value, with the instructions the
 * hot path made before. */
#if defined(__GNUC__) || defined(__clang__)
#define EV_WIN32_LOAD_ACQ_PTR(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define EV_WIN32_STORE_REL_PTR(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define EV_WIN32_LOAD_ACQ64(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define EV_WIN32_STORE_REL64(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#define EV_WIN32_LOAD_ACQ_PTR(p) ReadPointerAcquire((PVOID const volatile *) (p))
#define EV_WIN32_STORE_REL_PTR(p, v) WritePointerRelease((PVOID volatile *) (p), (v))
#define EV_WIN32_LOAD_ACQ64(p) ReadAcquire64((LONG64 const volatile *) (p))
#define EV_WIN32_STORE_REL64(p, v) WriteRelease64((LONG64 volatile *) (p), (v))
#endif

typedef VOID(WINAPI *ev_win32_get_time_fn)(LPFILETIME);
typedef union {
    PVOID                p; /* the published image: 0 until ev_win32_clock_init_once */
    ev_win32_get_time_fn fn;
} ev_win32_time_cache;
typedef union {
    LONG64 bits;  /* the published image: 0 until ev_win32_clock_init_once */
    double scale; /* seconds per QueryPerformanceCounter tick */
} ev_win32_scale_cache;
static ev_win32_time_cache  ev_win32_get_time;
static ev_win32_scale_cache ev_win32_qpc;
static INIT_ONCE            ev_win32_clock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK
ev_win32_clock_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    ev_win32_time_cache  get_time;
    ev_win32_scale_cache qpc;
    LARGE_INTEGER        f;
    HMODULE              k32 = GetModuleHandleW(L"kernel32.dll");
    (void) once;
    (void) param;
    (void) ctx;

    get_time.fn = 0;
    if (k32)
        get_time.fn = (ev_win32_get_time_fn) (void (*)(void)) GetProcAddress(k32, "GetSystemTimePreciseAsFileTime");
    if (!get_time.fn)
        get_time.fn = GetSystemTimeAsFileTime;

    QueryPerformanceFrequency(&f); /* fixed at boot; cannot fail on XP and later */
    qpc.scale = 1. / (double) f.QuadPart;

    EV_WIN32_STORE_REL64(&ev_win32_qpc.bits, qpc.bits);
    EV_WIN32_STORE_REL_PTR(&ev_win32_get_time.p, get_time.p);
    return TRUE;
}

ecb_noinline ecb_cold static void
ev_win32_clock_init(void) {
    InitOnceExecuteOnce(&ev_win32_clock_once, ev_win32_clock_init_once, 0, 0);
}

#define EV_HAVE_EV_TIME 1
ev_tstamp
ev_time(void) {
    FILETIME       ft;
    ULARGE_INTEGER ui;

    if (ecb_expect_false(!EV_WIN32_LOAD_ACQ_PTR(&ev_win32_get_time.p)))
        ev_win32_clock_init();

    ev_win32_get_time.fn(&ft);
    ui.u.LowPart  = ft.dwLowDateTime;
    ui.u.HighPart = ft.dwHighDateTime;

    /* also, msvc cannot convert ulonglong to double... yes, it is that sucky */
    return EV_TS_FROM_USEC(((LONGLONG) (ui.QuadPart - 116444736000000000) * 1e-1));
}

#define EV_HAVE_EV_GET_CLOCK 1

static ev_tstamp
ev_win32_get_clock(void) {
    LARGE_INTEGER c;

    if (ecb_expect_false(!EV_WIN32_LOAD_ACQ64(&ev_win32_qpc.bits)))
        ev_win32_clock_init();

    QueryPerformanceCounter(&c);
    return (ev_tstamp) ((double) c.QuadPart * ev_win32_qpc.scale);
}

#endif
