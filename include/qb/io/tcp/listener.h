/**
 * @file qb/io/tcp/listener.h
 * @brief Implementation of a TCP listener for the QB IO library.
 *
 * This file provides the implementation of a TCP listener for accepting
 * incoming connections. It supports IPv4, IPv6, and Unix sockets,
 * building upon the generic `qb::io::socket`.
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
 * @ingroup TCP
 */

#include "socket.h"

#ifndef QB_IO_TCP_LISTENER_H_
#define QB_IO_TCP_LISTENER_H_

namespace qb::io::tcp {

/*!
 * @class listener
 * @ingroup TCP
 * @brief Class implementing a TCP listener for accepting incoming connections.
 *
 * This class provides functionality for listening for incoming TCP connections.
 * It inherits protectedly from the base `qb::io::socket` class and provides methods for
 * binding to a local address, listening for connections, and accepting them.
 * It supports IPv4, IPv6, and Unix Domain Sockets (if enabled).
 */
class QB_API listener : private io::socket {
public:
    /** @brief Indicates that this socket implementation is not secure */
    constexpr static bool is_secure() noexcept { return false; }

    // Methods inherited from the base socket (made public via using-declarations)
    using qb::io::socket::close;
    using qb::io::socket::get_optval;
    using qb::io::socket::is_open;
    using qb::io::socket::local_endpoint;
    using qb::io::socket::native_handle;
    using qb::io::socket::peer_endpoint;
    using qb::io::socket::release_handle;
    using qb::io::socket::set_nonblocking;
    using qb::io::socket::set_optval;
    using qb::io::socket::test_nonblocking;

    /**
     * @brief Start listening on a specific local endpoint.
     * @param ep The `qb::io::endpoint` specifying the local IP address and port to listen on.
     * @return 0 on success, or a non-zero error code on failure (e.g., if binding or listening fails).
     * @details This method first opens a socket with the address family of the endpoint,
     *          then binds to the endpoint and starts listening for incoming connections.
     *          Default backlog is `SOMAXCONN`.
     */
    int listen(io::endpoint const &ep) noexcept;

    /**
     * @brief Start listening on an endpoint specified by a URI.
     * @param uri The `qb::io::uri` specifying the scheme (e.g., "tcp", "unix"), address, and port.
     * @return 0 on success, or a non-zero error code on failure.
     * @details Parses the URI to determine the address and port, then calls the appropriate
     *          `listen_v4`, `listen_v6`, or `listen_un` method.
     */
    int listen(io::uri const &uri) noexcept;

    /**
     * @brief Start listening on a specific IPv4 address and port.
     * @param port The port number to listen on.
     * @param host The IPv4 host address string (e.g., "0.0.0.0" for all interfaces, or a specific IP).
     *             Defaults to "0.0.0.0".
     * @return 0 on success, or a non-zero error code on failure.
     */
    int listen_v4(uint16_t port, std::string const &host = "0.0.0.0") noexcept;

    /**
     * @brief Start listening on a specific IPv6 address and port.
     * @param port The port number to listen on.
     * @param host The IPv6 host address string (e.g., "::" for all interfaces, or a specific IP).
     *             Defaults to "::".
     * @return 0 on success, or a non-zero error code on failure.
     */
    int listen_v6(uint16_t port, std::string const &host = "::") noexcept;

    /**
     * @brief Start listening on a Unix domain socket.
     * @param path The file system path for the Unix domain socket.
     * @return 0 on success, or a non-zero error code on failure.
     * @note This is only effective if `QB_ENABLE_UDS` is active and the system supports AF_UNIX.
     *       The socket file will be created if it doesn't exist, and may need to be unlinked manually
     *       before reuse if the program terminates abnormally.
     */
    int listen_un(std::string const &path) noexcept;

    /**
     * @brief Accept a new incoming TCP connection and return it as a new `tcp::socket`.
     * @return A new `tcp::socket` instance representing the connected client.
     *         If an error occurs during accept, the returned socket will not be open (`is_open()` will be false).
     * @details This is a blocking call by default if the listener socket is blocking.
     *          For non-blocking accept, the listener socket should be set to non-blocking, and this
     *          method would typically be called when `select` or an event loop indicates readability.
     */
    tcp::socket accept() const noexcept;

    /**
     * @brief Accept a new incoming TCP connection into an existing `tcp::socket` object.
     * @param sock A reference to a `tcp::socket` object where the new connection will be placed.
     *             If a connection is accepted, `sock` will wrap the new client socket descriptor.
     * @return 0 on success, indicating `sock` now represents the new connection.
     *         A non-zero error code (typically negative) on failure (e.g., `EWOULDBLOCK` if non-blocking).
     * @details Similar to the other `accept()` method, but uses a pre-existing socket object.
     */
    int accept(tcp::socket &sock) const noexcept;

    /**
     * @brief Disconnect the listener socket, stopping it from accepting new connections.
     * @return 0 on success, or a non-zero error code on failure.
     * @details This typically calls `qb::io::socket::shutdown()` and `close()` on the underlying listener socket.
     */
    int disconnect() const noexcept;
};

} // namespace qb::io::tcp

#endif // QB_IO_TCP_LISTENER_H_
