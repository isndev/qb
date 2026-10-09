/**
 * @file qb/io/tcp/socket.cpp
 * @brief Implementation of TCP socket functionality
 *
 * This file contains the implementation of TCP socket operations in the QB framework,
 * including connection establishment, reading, writing, and socket state management.
 * It provides reliable stream-based communication with support for both IPv4 and IPv6.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#include <algorithm>
#include <chrono>
#include <limits>
#include <qb/io/tcp/socket.h>

namespace qb::io::tcp {

namespace {
// An open socket takes `ep` unless its family is KNOWN and different. getsockname() of a socket that is open but
// not yet bound fails on Windows (WSAEINVAL), so its family reads AF_UNSPEC: that is no mismatch, and taking it
// for one made the synchronous bind() / connect() refuse an initialised socket (Huly QB-310) -- n_connect()'s rule,
// now the one rule of the four. A genuine incompatibility is still reported, by the call itself.
bool
tcp_open_family_refuses(socket const &self, qb::io::endpoint const &ep) noexcept {
    const int local_af = self.local_endpoint().af();
    return local_af != AF_UNSPEC && local_af != ep.af();
}
} // namespace

socket::socket(io::socket &&sock) noexcept
    : io::socket(sock.release_handle()) {}

socket &
socket::operator=(io::socket &&sock) noexcept {
    static_cast<io::socket &>(*this) = sock.release_handle();
    return *this;
}

int
socket::init(int af) noexcept {
    if (io::socket::open(af, SOCK_STREAM, 0)) {
        set_optval<int>(IPPROTO_TCP, TCP_NODELAY, 1);
        return 0;
    }
    return -1;
}

int
socket::bind(qb::io::endpoint const &ep) noexcept {
    if (is_open()) {
        if (tcp_open_family_refuses(*this, ep))
            return -1;
    } else if (init(ep.af()))
        return -1;

    return qb::io::socket::bind(ep);
}

int
socket::bind(io::uri const &u) noexcept {
    switch (u.af()) {
        case AF_INET:
        case AF_INET6:
            return bind(io::endpoint().as_in(std::string(u.host()).c_str(), u.u_port()));
        case AF_UNIX:
            const auto path = std::string(u.path()) + std::string(u.host());
            return bind(io::endpoint().as_un(path.c_str()));
    }
    return -1;
}

std::vector<qb::io::endpoint>
resolve_endpoints(int const af, std::string const &host, uint16_t const port) {
    std::vector<qb::io::endpoint> endpoints;
    qb::io::socket::resolve_i(
        [&](const auto &ep) {
            if (ep.af() == af)
                endpoints.push_back(ep);
            return false; // every address of the family, in the resolver's order
        },
        host.c_str(), port, af, SOCK_STREAM);
    return endpoints;
}

qb::duration
connect_attempt_budget(qb::duration const remaining, std::size_t const left) noexcept {
    if (remaining <= qb::duration::zero())
        return qb::duration::zero();
    if (left <= 1)
        return remaining;
    constexpr qb::duration floor = std::chrono::seconds{2};
    return std::min(remaining, std::max(remaining / static_cast<qb::duration::rep>(left), floor));
}

// The three connects by name fall back through every address of the family (Huly QB-164): one that
// refuses, does not route or does not answer in its share of the time is closed and the next is tried.
// Only a descriptor this socket opens itself is replaced -- one that was already open when the connect
// began may carry options (a bind, a socket option) a fresh descriptor would not, so it gets the one
// attempt it always had. A name that resolves to nothing leaves -1, as before.

int
socket::connect(std::vector<qb::io::endpoint> const &endpoints) noexcept {
    const bool own_fd = !is_open();
    auto       ret    = -1;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        if (i > 0) {
            if (!own_fd)
                break;
            close(); // a failed connect leaves the descriptor unusable: the next attempt opens its own
        }
        ret = connect(endpoints[i]);
        if (ret == 0)
            break;
    }
    return ret;
}

int
socket::connect(std::vector<qb::io::endpoint> const &endpoints, qb::duration wtimeout) noexcept {
    const bool own_fd   = !is_open();
    auto const deadline = qb::mono_now() + wtimeout;
    auto       ret      = -1;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        if (i > 0) {
            if (!own_fd || qb::mono_now() >= deadline)
                break;
            close();
        }
        ret = connect(endpoints[i], connect_attempt_budget(deadline - qb::mono_now(), endpoints.size() - i));
        if (ret == 0)
            break;
    }
    return ret;
}

int
socket::connect_in(int af, std::string const &host, uint16_t port) noexcept {
    try {
        return connect(resolve_endpoints(af, host, port));
    } catch (...) {
        return -1; // the address list could not be allocated
    }
}

int
socket::connect_in(int af, std::string const &host, uint16_t port, qb::duration wtimeout) noexcept {
    try {
        return connect(resolve_endpoints(af, host, port), wtimeout);
    } catch (...) {
        return -1;
    }
}

int
socket::connect(qb::io::endpoint const &ep) noexcept {
    if (is_open()) {
        if (tcp_open_family_refuses(*this, ep))
            return -1;
    } else if (init(ep.af()))
        return -1;

    return qb::io::socket::connect(ep);
}

int
socket::connect(qb::io::endpoint const &ep, qb::duration wtimeout) noexcept {
    if (is_open()) {
        if (tcp_open_family_refuses(*this, ep))
            return -1;
    } else if (init(ep.af()))
        return -1;

    return qb::io::socket::connect_n(ep, wtimeout);
}

int
socket::connect(uri const &u) noexcept {
    switch (u.af()) {
        case AF_INET:
        case AF_INET6:
            return connect_in(u.af(), std::string(u.host()), u.u_port());
        case AF_UNIX:
            const auto path = std::string(u.path()) + std::string(u.host());
            return connect_un(path);
    }
    return -1;
}

int
socket::connect(uri const &u, qb::duration wtimeout) noexcept {
    switch (u.af()) {
        case AF_INET:
        case AF_INET6:
            return connect_in(u.af(), std::string(u.host()), u.u_port(), wtimeout);
        case AF_UNIX:
            const auto path = std::string(u.path()) + std::string(u.host());
            return connect(qb::io::endpoint().as_un(path.c_str()), wtimeout);
    }
    return -1;
}

int
socket::connect_v4(std::string const &host, uint16_t port) noexcept {
    return connect_in(AF_INET, host, port);
}

int
socket::connect_v6(std::string const &host, uint16_t port) noexcept {
    return connect_in(AF_INET6, host, port);
}

int
socket::connect_un(std::filesystem::path const &path) noexcept {
    return connect(qb::io::endpoint().as_un(path.string().c_str()));
}

// non blocking version

// Non-blocking: an address is left for the next only when its connect fails AT ONCE (a refusal Windows
// reports synchronously on loopback, a family or route the host cannot reach); one in progress is the
// caller's to complete -- `async::tcp::connector` falls back on a later failure itself.
int
socket::n_connect_in(int af, std::string const &host, uint16_t port) noexcept {
    try {
        const bool own_fd    = !is_open();
        auto const endpoints = resolve_endpoints(af, host, port);
        auto       ret       = -1;
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            if (i > 0) {
                if (!own_fd)
                    break;
                close(); // a failed connect leaves the descriptor unusable: the next attempt opens its own
            }
            ret = n_connect(endpoints[i]);
            if (ret == 0 || socket_no_error(qb::io::socket::get_last_errno()))
                break; // connected, or in progress: the errno is the caller's to read
        }
        return ret;
    } catch (...) {
        return -1;
    }
}

int
socket::n_connect(qb::io::endpoint const &ep) noexcept {
    if (is_open()) {
        // Only a KNOWN family mismatch is refused (tcp_open_family_refuses): without that, connecting through an
        // existing unbound socket (connect_with_socket) always failed on Windows.
        if (tcp_open_family_refuses(*this, ep))
            return -1;
    } else if (init(ep.af()))
        return -1;

    return qb::io::socket::connect_n(ep);
}

int
socket::n_connect(uri const &u) noexcept {
    switch (u.af()) {
        case AF_INET:
        case AF_INET6:
            return n_connect_in(u.af(), std::string(u.host()), u.u_port());
        case AF_UNIX:
            const auto path = std::string(u.path()) + std::string(u.host());
            return n_connect_un(path);
    }
    return -1;
}

int
socket::n_connect_v4(std::string const &host, uint16_t port) noexcept {
    return n_connect_in(AF_INET, host, port);
}

int
socket::n_connect_v6(std::string const &host, uint16_t port) noexcept {
    return n_connect_in(AF_INET6, host, port);
}

int
socket::n_connect_un(std::filesystem::path const &path) noexcept {
    return n_connect(qb::io::endpoint().as_un(path.string().c_str()));
}

int
socket::read(void *dest, std::size_t len) const noexcept {
    if (len > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        len = static_cast<std::size_t>(std::numeric_limits<int>::max());
    int ret = recv(dest, static_cast<int>(len));
    if (ret > 0)
        return ret;
    if (ret == 0) {
        // Peer closed the connection gracefully.
        // recv() == 0 does not set errno / WSAGetLastError(), so clear it explicitly;
        // otherwise io::on() will capture a stale error code from a previous syscall
        // and misreport a graceful close as an error in on(event::disconnected).
#ifdef _WIN32
        WSASetLastError(0);
#else
        errno = 0;
#endif
    }
    return -1;
}

int
socket::write(const void *data, std::size_t size) const noexcept {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        size = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return send(data, static_cast<int>(size));
}

int
socket::disconnect() const noexcept {
    return shutdown();
}

} // namespace qb::io::tcp