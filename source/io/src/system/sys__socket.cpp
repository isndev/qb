/**
 * @file qb/io/src/system/sys__socket.cpp
 * @brief Implementation of the socket abstraction layer with cross-platform support
 *
 * @details This file provides a comprehensive socket implementation that abstracts
 * platform-specific socket APIs (Windows Winsock and POSIX sockets) into a unified
 * interface. It includes functions for connection establishment, data transmission,
 * socket configuration, and network address operations. The implementation supports both
 * blocking and non-blocking operations, as well as IPv4 and IPv6.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup IO
 */

#ifndef QB__SOCKET_CPP
#define QB__SOCKET_CPP
#include <assert.h>
#ifdef _DEBUG
#include <stdio.h>
#endif

#if !defined(QB_HEADER_ONLY)
#include <qb/io/system/sys__socket.h>
#endif

#include <qb/io/system/sys__utils.h>

#if !defined(_WIN32)
#include <qb/io/system/sys__ifaddrs.h>
#endif

// For apple bsd socket implemention
#if !defined(TCP_KEEPIDLE)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

#if defined(_WIN32) && !defined(_WINSTORE)
static LPFN_ACCEPTEX             __accept_ex               = nullptr;
static LPFN_GETACCEPTEXSOCKADDRS __get_accept_ex_sockaddrs = nullptr;
static LPFN_CONNECTEX            __connect_ex              = nullptr;
#endif

#if !QB__HAS_NTOP
namespace qb {
QB__NS_INLINE
namespace inet {
QB__NS_INLINE
namespace ip {
namespace compat {
#include "sys__inet_compat.inl"
} // namespace compat
} // namespace ip
} // namespace inet
} // namespace qb
#endif

namespace qb::io {
QB__NS_INLINE
namespace inet {

int
socket::xpconnect(const char *hostname, u_short port, u_short local_port) {
    auto flags = getipsv();

    int error = -1;

    socket::resolve_i(
        [&](const endpoint &ep) {
            switch (ep.af()) {
                case AF_INET:
                    if (flags & ipsv_ipv4) {
                        error = pconnect(ep, local_port);
                    } else if (flags & ipsv_ipv6) {
                        socket::resolve_i(
                            [&](const endpoint &ep6) {
                                return 0 == (error = pconnect(ep6, local_port));
                            },
                            hostname, port, AF_INET6, AI_V4MAPPED);
                    }
                    break;
                case AF_INET6:
                    if (flags & ipsv_ipv6)
                        error = pconnect(ep, local_port);
                    break;
            }

            return error == 0;
        },
        hostname, port, AF_UNSPEC, AI_ALL);

    return error;
}

int
socket::xpconnect_n(const char *hostname, u_short port,
                    const std::chrono::microseconds &wtimeout, u_short local_port) {
    auto flags = getipsv();
    int  error = -1;
    socket::resolve_i(
        [&](const endpoint &ep) {
            switch (ep.af()) {
                case AF_INET:
                    if (flags & ipsv_ipv4)
                        error = pconnect_n(ep, wtimeout, local_port);
                    else if (flags & ipsv_ipv6) {
                        socket::resolve_i(
                            [&](const endpoint &ep6) {
                                return 0 ==
                                       (error = pconnect_n(ep6, wtimeout, local_port));
                            },
                            hostname, port, AF_INET6, AI_V4MAPPED);
                    }
                    break;
                case AF_INET6:
                    if (flags & ipsv_ipv6)
                        error = pconnect_n(ep, wtimeout, local_port);
                    break;
            }

            return error == 0;
        },
        hostname, port, AF_UNSPEC, AI_ALL);

    return error;
}

int
socket::pconnect(const char *hostname, u_short port, u_short local_port) {
    int error = -1;
    socket::resolve_i(
        [&](const endpoint &ep) { return 0 == (error = pconnect(ep, local_port)); },
        hostname, port);
    return error;
}

int
socket::pconnect_n(const char *hostname, u_short port,
                   const std::chrono::microseconds &wtimeout, u_short local_port) {
    int error = -1;
    socket::resolve_i(
        [&](const endpoint &ep) {
            return 0 == (error = pconnect_n(ep, wtimeout, local_port));
        },
        hostname, port);
    return error;
}

int
socket::pconnect_n(const char *hostname, u_short port, u_short local_port) {
    int error = -1;
    socket::resolve_i(
        [&](const endpoint &ep) {
            (error = pconnect_n(ep, local_port));
            return true;
        },
        hostname, port);
    return error;
}

int
socket::pconnect(const endpoint &ep, u_short local_port) {
    if (this->reopen(ep.af())) {
        if (local_port != 0)
            this->bind(QB_ADDR_ANY(ep.af()), local_port);
        return this->connect(ep);
    }
    return -1;
}

int
socket::pconnect_n(const endpoint &ep, const std::chrono::microseconds &wtimeout,
                   u_short local_port) {
    if (this->reopen(ep.af())) {
        if (local_port != 0)
            this->bind(QB_ADDR_ANY(ep.af()), local_port);
        return this->connect_n(ep, wtimeout);
    }
    return -1;
}

int
socket::pconnect_n(const endpoint &ep, u_short local_port) {
    if (this->reopen(ep.af())) {
        if (local_port != 0)
            this->bind(QB_ADDR_ANY(ep.af()), local_port);
        return socket::connect_n(this->fd, ep);
    }
    return -1;
}

int
socket::pserve(const char *addr, u_short port) {
    return this->pserve(endpoint{addr, port});
}
int
socket::pserve(const endpoint &ep) {
    if (!this->reopen(ep.af()))
        return -1;

    set_optval(SOL_SOCKET, SO_REUSEADDR, 1);

    int n = this->bind(ep);
    if (n != 0)
        return n;

    return this->listen();
}

int
socket::resolve(std::vector<endpoint> &endpoints, const char *hostname,
                unsigned short port, int socktype) {
    return resolve_i(
        [&](const endpoint &ep) {
            endpoints.push_back(ep);
            return false;
        },
        hostname, port, AF_UNSPEC, AI_ALL, socktype);
}
int
socket::resolve_v4(std::vector<endpoint> &endpoints, const char *hostname,
                   unsigned short port, int socktype) {
    return resolve_i(
        [&](const endpoint &ep) {
            endpoints.push_back(ep);
            return false;
        },
        hostname, port, AF_INET, 0, socktype);
}
int
socket::resolve_v6(std::vector<endpoint> &endpoints, const char *hostname,
                   unsigned short port, int socktype) {
    return resolve_i(
        [&](const endpoint &ep) {
            endpoints.push_back(ep);
            return false;
        },
        hostname, port, AF_INET6, 0, socktype);
}
int
socket::resolve_v4to6(std::vector<endpoint> &endpoints, const char *hostname,
                      unsigned short port, int socktype) {
    return socket::resolve_i(
        [&](const endpoint &ep) {
            endpoints.push_back(ep);
            return false;
        },
        hostname, port, AF_INET6, AI_V4MAPPED, socktype);
}
int
socket::resolve_tov6(std::vector<endpoint> &endpoints, const char *hostname,
                     unsigned short port, int socktype) {
    return resolve_i(
        [&](const endpoint &ep) {
            endpoints.push_back(ep);
            return false;
        },
        hostname, port, AF_INET6, AI_ALL | AI_V4MAPPED, socktype);
}

int
socket::getipsv(void) {
    int flags = 0;
    socket::traverse_local_address([&](const ip::endpoint &ep) -> bool {
        switch (ep.af()) {
            case AF_INET:
                flags |= ipsv_ipv4;
                break;
            case AF_INET6:
                flags |= ipsv_ipv6;
                break;
        }
        return (flags == ipsv_dual_stack);
    });
    //    // QB_LOG("socket::getipsv: flags=%d", flags);
    return flags;
}

void
socket::traverse_local_address(std::function<bool(const ip::endpoint &)> handler) {
    int  family = AF_UNSPEC;
    bool done   = false;
    /* Only windows support use getaddrinfo to get local ip address(not loopback or
      linklocal), Because nullptr same as "localhost": always return loopback address and
      at unix/linux the gethostname always return "localhost"
      */
#if defined(_WIN32)
    char hostname[256] = {0};
    ::gethostname(hostname, sizeof(hostname));

    // ipv4 & ipv6
    addrinfo hint, *ailist = nullptr;
    ::memset(&hint, 0x0, sizeof(hint));

    endpoint ep;
#if defined(_DEBUG)
    // QB_LOG("socket::traverse_local_address: localhost=%s", hostname);
#endif
    int iret = getaddrinfo(hostname, nullptr, &hint, &ailist);

    const char *errmsg = nullptr;
    if (ailist != nullptr) {
        for (auto aip = ailist; aip != NULL; aip = aip->ai_next) {
            family = aip->ai_family;
            if (family == AF_INET || family == AF_INET6) {
                ep.as_is(aip);
                // QB_LOGV("socket::traverse_local_address: ip=%s",
                // ep.ip().c_str());
                switch (ep.af()) {
                    case AF_INET:
                        if (!IN4_IS_ADDR_LOOPBACK(&ep.in4_.sin_addr) &&
                            !IN4_IS_ADDR_LINKLOCAL(&ep.in4_.sin_addr))
                            done = handler(ep);
                        break;
                    case AF_INET6:
                        if (IN6_IS_ADDR_GLOBAL(&ep.in6_.sin6_addr))
                            done = handler(ep);
                        break;
                }
                if (done)
                    break;
            }
        }
        freeaddrinfo(ailist);
    } else {
        errmsg = socket::gai_strerror(iret);
    }
#else // __APPLE__ or linux with <ifaddrs.h>
    struct ifaddrs *ifaddr, *ifa;
    /*
    The value of ifa->ifa_name:
     Android:
      wifi: "w"
      cellular: "r"
     iOS:
      wifi: "en0"
      cellular: "pdp_ip0"
    */

    if (qb::io::getifaddrs(&ifaddr) == -1) {
        // QB_LOG("socket::traverse_local_address: getifaddrs fail!");
        return;
    }

    endpoint ep;
    /* Walk through linked list*/
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;
        family = ifa->ifa_addr->sa_family;
        if (family == AF_INET || family == AF_INET6) {
            ep.as_is(ifa->ifa_addr);
            // QB_LOGV("socket::traverse_local_address: ip=%s", ep.ip().c_str());
            switch (ep.af()) {
                case AF_INET:
                    if (!IN4_IS_ADDR_LOOPBACK(&ep.in4_.sin_addr) &&
                        !IN4_IS_ADDR_LINKLOCAL(&ep.in4_.sin_addr))
                        done = handler(ep);
                    break;
                case AF_INET6:
                    if (IN6_IS_ADDR_GLOBAL(&ep.in6_.sin6_addr))
                        done = handler(ep);
                    break;
            }
            if (done)
                break;
        }
    }

    qb::io::freeifaddrs(ifaddr);
#endif
}

socket::socket(void)
    : fd(invalid_socket) {}
socket::socket(socket_type h)
    : fd(h) {}
socket::socket(socket &&right)
    : fd(invalid_socket) {
    swap(right);
}
socket::socket(int af, int type, int protocol)
    : fd(invalid_socket) {
    open(af, type, protocol);
}
socket::~socket(void) {
    close();
}

socket &
socket::operator=(socket_type handle) {
    if (!this->is_open())
        this->fd = handle;
    return *this;
}
socket &
socket::operator=(socket &&right) {
    return swap(right);
}

socket &
socket::swap(socket &rhs) {
    std::swap(this->fd, rhs.fd);
    return *this;
}

bool
socket::open(int af, int type, int protocol) {
    if (invalid_socket == this->fd)
#if defined(_WIN32)
        this->fd = OPEN_FD_FROM_SOCKET(::socket(af, type, protocol));
#else
        this->fd = ::socket(af, type, protocol);
#endif
    return is_open();
}

bool
socket::reopen(int af, int type, int protocol) {
    this->close();
    return this->open(af, type, protocol);
}

#if defined(_WIN32) && !defined(_WINSTORE)
bool
socket::open_ex(int af, int type, int protocol) {
#if !defined(WP8)
    if (invalid_socket == this->fd) {
        SOCKET sock = ::WSASocket(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);

        DWORD dwBytes = 0;
        if (nullptr == __accept_ex) {
            GUID guidAcceptEx = WSAID_ACCEPTEX;
            (void) WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidAcceptEx,
                            sizeof(guidAcceptEx), &__accept_ex, sizeof(__accept_ex),
                            &dwBytes, nullptr, nullptr);
        }

        if (nullptr == __connect_ex) {
            GUID guidConnectEx = WSAID_CONNECTEX;
            (void) WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidConnectEx,
                            sizeof(guidConnectEx), &__connect_ex, sizeof(__connect_ex),
                            &dwBytes, nullptr, nullptr);
        }

        if (nullptr == __get_accept_ex_sockaddrs) {
            GUID guidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
            (void) WSAIoctl(
                sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidGetAcceptExSockaddrs,
                sizeof(guidGetAcceptExSockaddrs), &__get_accept_ex_sockaddrs,
                sizeof(__get_accept_ex_sockaddrs), &dwBytes, nullptr, nullptr);
        }

        this->fd = OPEN_FD_FROM_SOCKET(sock);
    }
    return is_open();
#else
    return false;
#endif
}

#if !defined(WP8)
bool
socket::accept_ex(SOCKET sockfd_listened, SOCKET sockfd_prepared, PVOID lpOutputBuffer,
                  DWORD dwReceiveDataLength, DWORD dwLocalAddressLength,
                  DWORD dwRemoteAddressLength, LPDWORD lpdwBytesReceived,
                  LPOVERLAPPED lpOverlapped) {
    return __accept_ex(sockfd_listened, sockfd_prepared, lpOutputBuffer,
                       dwReceiveDataLength, dwLocalAddressLength, dwRemoteAddressLength,
                       lpdwBytesReceived, lpOverlapped) != FALSE;
}

bool
socket::connect_ex(SOCKET s, const struct sockaddr *name, int namelen,
                   PVOID lpSendBuffer, DWORD dwSendDataLength, LPDWORD lpdwBytesSent,
                   LPOVERLAPPED lpOverlapped) {
    return __connect_ex(s, name, namelen, lpSendBuffer, dwSendDataLength, lpdwBytesSent,
                        lpOverlapped);
}

void
socket::translate_sockaddrs(PVOID lpOutputBuffer, DWORD dwReceiveDataLength,
                            DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength,
                            sockaddr **LocalSockaddr, LPINT LocalSockaddrLength,
                            sockaddr **RemoteSockaddr, LPINT RemoteSockaddrLength) {
    __get_accept_ex_sockaddrs(lpOutputBuffer, dwReceiveDataLength, dwLocalAddressLength,
                              dwRemoteAddressLength, LocalSockaddr, LocalSockaddrLength,
                              RemoteSockaddr, RemoteSockaddrLength);
}
#endif

#endif

bool
socket::is_open(void) const {
    return this->fd != invalid_socket;
}

socket_type
socket::native_handle(void) const {
    return this->fd;
}
socket_type
socket::release_handle(void) {
    socket_type result = this->fd;
    this->fd           = invalid_socket;
    return result;
}

int
socket::set_nonblocking(bool nonblocking) const {
    return set_nonblocking(this->fd, nonblocking);
}
int
socket::set_nonblocking(socket_type s, bool nonblocking) {
#if defined(_WIN32)
    u_long argp = nonblocking;
    return ::ioctlsocket(FD_TO_SOCKET(s), FIONBIO, &argp);
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    return ::fcntl(s, F_SETFL,
                   nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

int
socket::test_nonblocking() const {
    return socket::test_nonblocking(this->fd);
}
int
socket::test_nonblocking(socket_type s) {
#if defined(_WIN32)
    int           r = 0;
    unsigned char b[1];
    r = socket::recv(s, b, 0, 0);
    if (r == 0)
        return 1;
    else if (r == -1 && GetLastError() == WSAEWOULDBLOCK)
        return 0;
    return -1; /* In  case it is a connection socket (TCP) and it is not in connected
                  state you will get here 10060 */
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    return flags & O_NONBLOCK;
#endif
}

int
socket::bind(const char *addr, unsigned short port) const {
    return this->bind(endpoint(addr, port));
}
int
socket::bind(const endpoint &ep) const {
    return ::bind(FD_TO_SOCKET(this->fd), &ep.sa_, ep.len());
}
int
socket::bind_any(bool ipv6) const {
    return this->bind(endpoint(!ipv6 ? "0.0.0.0" : "::", 0));
}

int
socket::listen(int backlog) const {
    return ::listen(FD_TO_SOCKET(this->fd), backlog);
}

socket
socket::accept() const {
    return OPEN_FD_FROM_SOCKET(::accept(FD_TO_SOCKET(this->fd), nullptr, nullptr));
}
int
socket::accept_n(socket_type &new_sock) const {
    for (;;) {
        // Accept the waiting connection.
        new_sock =
            OPEN_FD_FROM_SOCKET(::accept(FD_TO_SOCKET(this->fd), nullptr, nullptr));

        // Check if operation succeeded.
        if (new_sock != invalid_socket) {
            socket::set_nonblocking(new_sock, true);
            return 0;
        }

        auto error = get_last_errno();
        // Retry operation if interrupted by signal.
        if (error == EINTR)
            continue;

        /* Operation failed.
         ** The error maybe EWOULDBLOCK, EAGAIN, ECONNABORTED, EPROTO,
         ** Simply Fall through to retry operation.
         */
        return error;
    }
}

int
socket::connect(const char *addr, u_short port) {
    return connect(endpoint(addr, port));
}
int
socket::connect(const endpoint &ep) {
    return socket::connect(fd, ep);
}
int
socket::connect(socket_type s, const char *addr, u_short port) {
    endpoint peer(addr, port);

    return socket::connect(s, peer);
}
int
socket::connect(socket_type s, const endpoint &ep) {
    return ::connect(FD_TO_SOCKET(s), &ep.sa_, ep.len());
}

int
socket::connect_n(const char *addr, u_short port,
                  const std::chrono::microseconds &wtimeout) {
    return connect_n(ip::endpoint(addr, port), wtimeout);
}
int
socket::connect_n(const endpoint &ep, const std::chrono::microseconds &wtimeout) {
    return this->connect_n(this->fd, ep, wtimeout);
}
int
socket::connect_n(socket_type s, const endpoint &ep, const std::chrono::microseconds &) {
    //    fd_set rset, wset;
    int n, error = 0;

    set_nonblocking(s, true);

    if ((n = socket::connect(s, ep)) < 0) {
        error = socket::get_last_errno();
        if (error != EINPROGRESS && error != EWOULDBLOCK)
            return -1;
    }

    return n;
    /* Do whatever we want while the connect is taking place. */
    //    if (n == 0)
    //        goto done; /* connect completed immediately */
    //
    //    if ((n = socket::select(s, &rset, &wset, NULL, wtimeout)) <= 0)
    //        error = socket::get_last_errno();
    //    else if ((FD_ISSET(s, &rset) || FD_ISSET(s, &wset))) { /* Everythings are ok */
    //        socklen_t len = sizeof(error);
    //        if (::getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &len) < 0)
    //            return (-1); /* Solaris pending error */
    //    }
    //
    // done:
    //    if (error != 0) {
    //        ::closesocket(s); /* just in case */
    //        return (-1);
    //    }
    //
    //    /* Since v3.31.2, we don't restore file status flags for unify behavior for all
    //     * platforms */
    //    // pitfall: because on win32, there is no way to test whether the s is
    //    non-blocking
    //    // so, can't restore properly
    //    return (0);
}

int
socket::connect_n(const endpoint &ep) {
    return socket::connect_n(this->fd, ep);
}
int
socket::connect_n(socket_type s, const endpoint &ep) {
    set_nonblocking(s, true);

    return socket::connect(s, ep);
}

int
socket::disconnect() const {
    return socket::disconnect(this->fd);
}
int
socket::disconnect(socket_type s) {
    sockaddr addr_unspec;
    memset(&addr_unspec, 0, sizeof(addr_unspec));
    addr_unspec.sa_family = AF_UNSPEC;
    return ::connect(FD_TO_SOCKET(s), &addr_unspec, sizeof(addr_unspec));
}

int
socket::send_n(const void *buf, int len, const std::chrono::microseconds &wtimeout,
               int flags) {
    return this->send_n(this->fd, buf, len, wtimeout, flags);
}
int
socket::send_n(socket_type s, const void *buf, int len,
               std::chrono::microseconds wtimeout, int flags) {
    int bytes_transferred = 0;
    int n;
    int error = 0;

    socket::set_nonblocking(s, true);

    for (; bytes_transferred < len;) {
        // Try to transfer as much of the remaining data as possible.
        // Since the socket is in non-blocking mode, this call will not
        // block.
        n = socket::send(s, (const char *) buf + bytes_transferred,
                         len - bytes_transferred, flags);
        if (n > 0) {
            bytes_transferred += n;
            continue;
        }

        // Check for possible blocking.
        error = socket::get_last_errno();
        if (n == -1 && socket::not_send_error(error)) {
            // Wait upto <timeout> for the blocking to subside.
            auto      start = qb::highp_clock();
            int const rtn   = handle_write_ready(s, wtimeout);
            wtimeout -= std::chrono::microseconds(qb::highp_clock() - start);

            // Did select() succeed?
            if (rtn != -1 && wtimeout.count() > 0) {
                // Blocking subsided in <timeout> period.  Continue
                // data transfer.
                continue;
            }
        }

        // Wait in select() timed out or other data transfer or
        // select() failures.
        break;
    }

    return bytes_transferred;
}

int
socket::recv_n(void *buf, int len, const std::chrono::microseconds &wtimeout,
               int flags) const {
    return this->recv_n(this->fd, buf, len, wtimeout, flags);
}
int
socket::recv_n(socket_type s, void *buf, int len, std::chrono::microseconds wtimeout,
               int flags) {
    int bytes_transferred = 0;
    int n;
    int error = 0;

    socket::set_nonblocking(s, true);

    for (; bytes_transferred < len;) {
        // Try to transfer as much of the remaining data as possible.
        // Since the socket is in non-blocking mode, this call will not
        // block.
        n = socket::recv(s, static_cast<char *>(buf) + bytes_transferred,
                         len - bytes_transferred, flags);
        if (n > 0) {
            bytes_transferred += n;
            continue;
        }

        // Check for possible blocking.
        error = socket::get_last_errno();
        if (n == -1 && socket::not_recv_error(error)) {
            // Wait upto <timeout> for the blocking to subside.
            auto      start = qb::highp_clock();
            int const rtn   = handle_read_ready(s, wtimeout);
            wtimeout -= std::chrono::microseconds(qb::highp_clock() - start);

            // Did select() succeed?
            if (rtn != -1 && wtimeout.count() > 0) {
                // Blocking subsided in <timeout> period.  Continue
                // data transfer.
                continue;
            }
        }

        // Wait in select() timed out or other data transfer or
        // select() failures.
        break;
    }

    return bytes_transferred;
}

int
socket::send(const void *buf, int len, int flags) const {
    return static_cast<int>(
        ::send(FD_TO_SOCKET(this->fd), (const char *) buf, len, flags));
}
int
socket::send(socket_type s, const void *buf, int len, int flags) {
    return static_cast<int>(::send(FD_TO_SOCKET(s), (const char *) buf, len, flags));
}

int
socket::recv(void *buf, int len, int flags) const {
    return static_cast<int>(this->recv(this->fd, buf, len, flags));
}
int
socket::recv(socket_type s, void *buf, int len, int flags) {
    return static_cast<int>(::recv(FD_TO_SOCKET(s), (char *) buf, len, flags));
}

int
socket::sendto(const void *buf, int len, const endpoint &to, int flags) const {
    return static_cast<int>(::sendto(FD_TO_SOCKET(this->fd), (const char *) buf, len,
                                     flags, &to.sa_, to.len()));
}

int
socket::recvfrom(void *buf, int len, endpoint &from, int flags) const {
    socklen_t addrlen{sizeof(from)};
    int n = static_cast<int>(::recvfrom(FD_TO_SOCKET(this->fd), (char *) buf, len, flags,
                                        &from.sa_, &addrlen));
    from.len(addrlen);
    return n;
}

int
socket::handle_write_ready(const std::chrono::microseconds &wtimeout) const {
    return handle_write_ready(this->fd, wtimeout);
}
int
socket::handle_write_ready(socket_type s, const std::chrono::microseconds &wtimeout) {
    fd_set writefds;
    return socket::select(s, nullptr, &writefds, nullptr, wtimeout);
}

int
socket::handle_read_ready(const std::chrono::microseconds &wtimeout) const {
    return handle_read_ready(this->fd, wtimeout);
}
int
socket::handle_read_ready(socket_type s, const std::chrono::microseconds &wtimeout) {
    fd_set readfds;
    return socket::select(s, &readfds, nullptr, nullptr, wtimeout);
}

int
socket::select(socket_type s, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
               std::chrono::microseconds wtimeout) {
    int n = 0;

    for (;;) {
        reregister_descriptor(s, readfds);
        reregister_descriptor(s, writefds);
        reregister_descriptor(s, exceptfds);

        timeval waitd_tv = {
            static_cast<decltype(timeval::tv_sec)>(wtimeout.count() / std::micro::den),
            static_cast<decltype(timeval::tv_usec)>(wtimeout.count() % std::micro::den)};
        long long start = highp_clock();
        n               = ::select(s + 1, readfds, writefds, exceptfds, &waitd_tv);
        wtimeout -= std::chrono::microseconds(highp_clock() - start);

        if (n < 0 && socket::get_last_errno() == EINTR) {
            if (wtimeout.count() > 0)
                continue;
            n = 0;
        }

        if (n == 0)
            socket::set_last_errno(ETIMEDOUT);
        break;
    }

    return n;
}

void
socket::reregister_descriptor(socket_type s, fd_set *fds) {
    if (fds) {
        FD_ZERO(fds);
        FD_SET(s, fds);
    }
}

endpoint
socket::local_endpoint(void) const {
    return local_endpoint(this->fd);
}
endpoint
socket::local_endpoint(socket_type fd) {
    endpoint  ep;
    socklen_t socklen = sizeof(ep);
    getsockname(FD_TO_SOCKET(fd), &ep.sa_, &socklen);
    ep.len(socklen);
    return ep;
}

endpoint
socket::peer_endpoint(void) const {
    return peer_endpoint(this->fd);
}
endpoint
socket::peer_endpoint(socket_type fd) {
    endpoint  ep;
    socklen_t socklen = sizeof(ep);
    getpeername(FD_TO_SOCKET(fd), &ep.sa_, &socklen);
    ep.len(socklen);
    return ep;
}

int
socket::set_keepalive(int flag, int idle, int interval, int probes) {
    return set_keepalive(this->fd, flag, idle, interval, probes);
}
int
socket::set_keepalive(socket_type s, int flag, int idle, int interval, int probes) {
#if defined(_WIN32) && !defined(WP8) && !defined(_WINSTORE)
    tcp_keepalive buffer_in;
    buffer_in.onoff             = flag;
    buffer_in.keepalivetime     = idle * 1000;
    buffer_in.keepaliveinterval = interval * 1000;

    return WSAIoctl(FD_TO_SOCKET(s), SIO_KEEPALIVE_VALS, &buffer_in, sizeof(buffer_in),
                    nullptr, 0, (DWORD *) &probes, nullptr, nullptr);
#else
    int n = set_optval(s, SOL_SOCKET, SO_KEEPALIVE, flag);
    n += set_optval(s, IPPROTO_TCP, TCP_KEEPIDLE, idle);
    n += set_optval(s, IPPROTO_TCP, TCP_KEEPINTVL, interval);
    n += set_optval(s, IPPROTO_TCP, TCP_KEEPCNT, probes);
    return n;
#endif
}

void
socket::reuse_address(bool reuse) {
    int optval = reuse ? 1 : 0;

    // All operating systems have 'SO_REUSEADDR'
    this->set_optval(SOL_SOCKET, SO_REUSEADDR, optval);
#if defined(SO_REUSEPORT) // macos,ios,linux,android
    this->set_optval(SOL_SOCKET, SO_REUSEPORT, optval);
#endif
}

void
socket::exclusive_address(bool exclusive) {
#if defined(SO_EXCLUSIVEADDRUSE)
    this->set_optval(SOL_SOCKET, SO_EXCLUSIVEADDRUSE, exclusive ? 1 : 0);
#elif defined(SO_EXCLBIND)
    this->set_optval(SOL_SOCKET, SO_EXCLBIND, exclusive ? 1 : 0);
#else
    (void) exclusive;
#endif
}

socket::operator socket_type(void) const {
    return this->fd;
}

int
socket::shutdown(int how) const {
    if (!is_open())
        return -1;
    return ::shutdown(FD_TO_SOCKET(this->fd), how);
}

void
socket::close(int shut_how) {
    if (is_open()) {
        if (shut_how >= 0)
            ::shutdown(FD_TO_SOCKET(this->fd), shut_how);
#if defined(_WIN32)
        ::close(fd);
#else
        ::closesocket(this->fd);
#endif

        this->fd = invalid_socket;
    }
}

unsigned int
socket::tcp_rtt() const {
    return socket::tcp_rtt(this->fd);
}
unsigned int
socket::tcp_rtt(socket_type s) {
#if defined(_WIN32)
#if defined(NTDDI_WIN10_RS2) && NTDDI_VERSION >= NTDDI_WIN10_RS2
    TCP_INFO_v0 info;
    DWORD       tcpi_ver = 0, bytes_transferred = 0;
    int         status = WSAIoctl(
        FD_TO_SOCKET(s), SIO_TCP_INFO,
        (LPVOID) &tcpi_ver, // lpvInBuffer pointer to a DWORD, version of tcp info
        (DWORD) sizeof(tcpi_ver),     // size, in bytes, of the input buffer
        (LPVOID) &info,               // pointer to a TCP_INFO_v0 structure
        (DWORD) sizeof(info),         // size of the output buffer
        (LPDWORD) &bytes_transferred, // number of bytes returned
        (LPWSAOVERLAPPED) nullptr,    // OVERLAPPED structure
        (LPWSAOVERLAPPED_COMPLETION_ROUTINE) nullptr);
    /*
    info.RttUs: The current estimated round-trip time for the connection, in
    microseconds. info.MinRttUs: The minimum sampled round trip time, in microseconds.
    */
    if (status == 0)
        return info.RttUs;
#endif
#elif defined(__linux__)
    struct tcp_info info;
    // int length = sizeof(struct tcp_info);
    if (0 == socket::get_optval(s, IPPROTO_TCP, TCP_INFO, info))
        return info.tcpi_rtt;
#elif defined(__APPLE__)
    struct tcp_connection_info info;
    int                        length = sizeof(struct tcp_connection_info);
    /*
    info.tcpi_srtt: average RTT in ms
    info.tcpi_rttcur: most recent RTT in ms
    */
    if (0 == socket::get_optval(s, IPPROTO_TCP, TCP_CONNECTION_INFO, info))
        return info.tcpi_srtt * std::milli::den;
#endif
    return 0;
}

void
socket::init_ws32_lib(void) {}

int
socket::get_last_errno(void) {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}
void
socket::set_last_errno(int error) {
#if defined(_WIN32)
    ::WSASetLastError(error);
#else
    errno = error;
#endif
}

bool
socket::not_send_error(int error) {
    return (error == EWOULDBLOCK || error == EAGAIN || error == EINTR ||
            error == ENOBUFS);
}
bool
socket::not_recv_error(int error) {
    return (error == EWOULDBLOCK || error == EAGAIN || error == EINTR);
}

const char *
socket::strerror(int error) {
#if defined(_MSC_VER) && !defined(_WINSTORE)
    static char error_msg[256];
    ZeroMemory(error_msg, sizeof(error_msg));
    ::FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
            FORMAT_MESSAGE_MAX_WIDTH_MASK /* remove line-end charactors \r\n */,
        NULL, error, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), // english language
        error_msg, sizeof(error_msg), NULL);

    return error_msg;
#else
    return ::strerror(error);
#endif
}

const char *
socket::gai_strerror(int error) {
#if defined(_WIN32)
    return socket::strerror(error);
#else
    return ::gai_strerror(error);
#endif
}
} // namespace inet
} // namespace qb::io

// initialize win32 socket library
#ifdef _WIN32
namespace {
struct ws2_32_gc {
    WSADATA dat = {0};

    ws2_32_gc(void) {
        WSAStartup(0x0202, &dat);
    }
    ~ws2_32_gc(void) {
#ifndef QB_IO_WITH_SSL
        WSACleanup();
#endif // QB_IO_WITH_SSL
    }
};

ws2_32_gc __ws32_lib_gc;
} // namespace
#endif

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif