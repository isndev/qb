/**
 * @file qb/io/tcp/listener.cpp
 * @brief Implementation of TCP listener functionality
 *
 * This file contains the implementation of TCP listener operations in the QB framework,
 * responsible for accepting incoming TCP connection requests and creating new socket
 * connections. It handles binding to network interfaces and listening on specific ports.
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

#include <utility>
#include <qb/io/async/lingering_close.h> /* async::detail::out_of_descriptors, release_closing_sockets */
#include <qb/io/tcp/listener.h>

namespace qb::io::tcp {

int
listener::listen(io::endpoint const &ep, listen_options const &opts) noexcept {
    const auto ret = pserve(ep, opts.reuse_port);
    if (!ret && ep.af() != AF_UNIX)
        set_optval<int>(IPPROTO_TCP, TCP_NODELAY, 1);
    return ret;
}

int
listener::listen(io::uri const &u, listen_options const &opts) noexcept {
    switch (u.af()) {
        case AF_INET:
        case AF_INET6:
            return listen(io::endpoint().as_in(std::string(u.host()).c_str(), u.u_port()), opts);
        case AF_UNIX:
            const auto path = std::string(u.path()) + std::string(u.host());
            return listen_un(path.c_str());
    }
    return -1;
}

int
listener::listen_v4(uint16_t port, std::string const &host, listen_options const &opts) noexcept {
    return listen(io::endpoint().as_in(host.c_str(), port), opts);
}

int
listener::listen_v6(uint16_t port, std::string const &host, listen_options const &opts) noexcept {
    return listen(io::endpoint().as_in(host.c_str(), port), opts);
}

int
listener::listen_un(std::filesystem::path const &path) noexcept {
    return listen(io::endpoint().as_un(path.string().c_str()));
}

namespace {
/// Whether the connection an accept() refused for lack of descriptors is still in the backlog, to be accepted again.
/// Linux keeps it there, the listener still readable. XNU has taken it off the queue BEFORE it allocates the
/// descriptor, and closes it when the allocation fails (bsd/kern/uipc_syscalls.c, accept_nocancel(): « Don't put this
/// back on the socket like we used to, that just causes the client to spin. Drop the socket. »): nothing is left to
/// accept again, and an accept made again would wait, on a blocking listener, for ANOTHER connection -- reporting
/// nothing of the one the system dropped. Measured on macOS: that second accept() never returned (Huly QB-1270).
constexpr bool refused_connection_stays_queued =
#if defined(__APPLE__)
    false;
#else
    true;
#endif
} // namespace

socket
listener::accept() const noexcept {
    io::socket accepted = io::socket::accept();
    // As accept(tcp::socket &) below: the sockets this thread's loop was closing after their peer give their
    // descriptors back, and where the refused connection is still in the backlog it is accepted again. Where the
    // system dropped it, the failure stays the caller's to read, as it came: the next accept finds descriptors.
    if (!accepted.is_open()) {
        const int error = io::socket::get_last_errno();
        if (async::detail::out_of_descriptors(error) && async::detail::release_closing_sockets() != 0) {
            if (refused_connection_stays_queued)
                accepted = io::socket::accept();
            else
                io::socket::set_last_errno(error); // the closes above ran system calls of their own
        }
    }
    return {std::move(accepted)};
}

int
listener::accept(tcp::socket &sock) const noexcept {
    socket_type nt_sock = qb::io::inet::invalid_socket;

    auto ret = io::socket::accept_n(nt_sock);
    // No descriptor left for the connection waiting in the backlog. The sockets this thread's loop is closing after
    // their peer hold some, as a courtesy to those peers: they are closed at once, oldest first, and the accept is
    // made again -- an acceptor that only retried would spin on a readable listener until their bound (Huly QB-1270).
    // Not where the system dropped the refused connection (refused_connection_stays_queued): the error is returned
    // as it came, and the next connection finds the descriptors.
    if (async::detail::out_of_descriptors(ret) && async::detail::release_closing_sockets() != 0 && refused_connection_stays_queued)
        ret = io::socket::accept_n(nt_sock);
    if (!ret) {
        sock = nt_sock;
        sock.set_optval<int>(IPPROTO_TCP, TCP_NODELAY, 1);
    }
    return ret;
}

int
listener::disconnect() const noexcept {
    return shutdown();
}

} // namespace qb::io::tcp
