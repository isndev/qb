/*
 * libev win32 compatibility cruft (_not_ a backend)
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

#ifdef _WIN32

/* note: the comment below could not be substantiated, but what would I care */
/* MSDN says this is required to handle SIGFPE */
/* my wild guess would be that using something floating-pointy is required */
/* for the crt to do something about it */
volatile double SIGFPE_REQ = 0.0f;

static SOCKET
ev_tcp_socket (void)
{
#if EV_USE_WSASOCKET
  return WSASocket (AF_INET, SOCK_STREAM, 0, 0, 0, 0);
#else
  return socket (AF_INET, SOCK_STREAM, 0);
#endif
}

/* oh, the humanity! */
static int
ev_pipe (int filedes [2])
{
  struct sockaddr_in addr = { 0 };
  int addr_size = sizeof (addr);
  struct sockaddr_in adr2;
  int adr2_size = sizeof (adr2);
  SOCKET listener;
  SOCKET sock [2] = { INVALID_SOCKET, INVALID_SOCKET };

  if ((listener = ev_tcp_socket ()) == INVALID_SOCKET)
    return -1;

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind (listener, (struct sockaddr *)&addr, addr_size))
    goto fail;

  if (getsockname (listener, (struct sockaddr *)&addr, &addr_size))
    goto fail;

  if (listen (listener, 1))
    goto fail;

  if ((sock [0] = ev_tcp_socket ()) == INVALID_SOCKET)
    goto fail;

  if (connect (sock [0], (struct sockaddr *)&addr, addr_size))
    goto fail;

  sock [1] = accept (listener, 0, 0);
  if (sock [1] == INVALID_SOCKET)
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
  if (getpeername (sock [0], (struct sockaddr *)&addr, &addr_size))
    goto fail;

  if (getsockname (sock [1], (struct sockaddr *)&adr2, &adr2_size))
    goto fail;

  errno = WSAEINVAL;
  if (addr_size != adr2_size
      || addr.sin_addr.s_addr != adr2.sin_addr.s_addr /* just to be sure, I mean, it's windows */
      || addr.sin_port        != adr2.sin_port)
    goto fail;

  closesocket (listener);

#if EV_SELECT_IS_WINSOCKET
  filedes [0] = EV_WIN32_HANDLE_TO_FD (sock [0]);
  filedes [1] = EV_WIN32_HANDLE_TO_FD (sock [1]);
#else
  /* when select isn't winsocket, we also expect socket, connect, accept etc.
   * to work on fds */
  filedes [0] = sock [0];
  filedes [1] = sock [1];
#endif

  return 0;

fail:
  closesocket (listener);

  if (sock [0] != INVALID_SOCKET) closesocket (sock [0]);
  if (sock [1] != INVALID_SOCKET) closesocket (sock [1]);

  return -1;
}

#undef pipe
#define pipe(filedes) ev_pipe (filedes)

#define EV_HAVE_EV_TIME 1
ev_tstamp
ev_time (void)
{
  FILETIME ft;
  ULARGE_INTEGER ui;

  GetSystemTimeAsFileTime (&ft);
  ui.u.LowPart  = ft.dwLowDateTime;
  ui.u.HighPart = ft.dwHighDateTime;

  /* also, msvc cannot convert ulonglong to double... yes, it is that sucky */
  return EV_TS_FROM_USEC (((LONGLONG)(ui.QuadPart - 116444736000000000) * 1e-1));
}

#endif

