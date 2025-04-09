/**
 * @file qb/io/tcp/socket.h
 * @brief Implementation of TCP sockets for the QB IO library
 *
 * This file provides the implementation of TCP sockets supporting
 * synchronous and asynchronous connections to IPv4, IPv6, and Unix sockets.
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

#include "../system/sys__socket.h"
#include "../uri.h"

#ifndef QB_IO_TCP_SOCKET_H_
#define QB_IO_TCP_SOCKET_H_

namespace qb::io::tcp {

/*!
 * @class socket tcp/socket.h qb/io/tcp/socket.h
 * @ingroup TCP
 * @brief Class implementing TCP socket functionality
 *
 * This class provides TCP socket functionality for stream-based communication.
 * It inherits from the base qb::io::socket class and provides methods for
 * connecting, sending, and receiving TCP streams.
 */
class QB_API socket : protected qb::io::socket {
    /**
     * @brief Internal method to connect to an address with a specific address family
     * @param af Address family (AF_INET for IPv4, AF_INET6 for IPv6)
     * @param host Host address to connect to
     * @param port Port number to connect to
     * @return 0 on success, error code on failure
     */
    int connect_in(int af, std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Internal method to perform a non-blocking connect
     * @param af Address family (AF_INET for IPv4, AF_INET6 for IPv6)
     * @param host Host address to connect to
     * @param port Port number to connect to
     * @return 0 on success, error code on failure
     */
    int n_connect_in(int af, std::string const &host, uint16_t port) noexcept;

public:
    // Methods inherited from the base socket
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
     * @brief Default constructor
     */
    socket() = default;

    /**
     * @brief Copy constructor (deleted)
     */
    socket(socket const &) = delete;

    /**
     * @brief Move constructor
     */
    socket(socket &&) = default;

    /**
     * @brief Move assignment operator
     * @return Reference to the moved socket
     */
    socket &operator=(socket &&) = default;

    /**
     * @brief Constructor from an IO socket
     * @param sock Socket to move from
     */
    socket(io::socket &&sock) noexcept;

    /**
     * @brief Move assignment operator from an IO socket
     * @param sock Socket to move from
     * @return Reference to the moved socket
     */
    socket &operator=(io::socket &&sock) noexcept;

    /**
     * @brief Initialize the TCP socket
     * @param af Address family (default: AF_INET for IPv4)
     * @return 0 on success, error code on failure
     */
    int init(int af = AF_INET) noexcept;

    /**
     * @brief Bind the socket to an endpoint
     * @param ep Endpoint to bind to
     * @return 0 on success, error code on failure
     */
    int bind(qb::io::endpoint const &ep) noexcept;

    /**
     * @brief Bind the socket to a URI
     * @param u URI to bind to
     * @return 0 on success, error code on failure
     */
    int bind(qb::io::uri const &u) noexcept;

    /**
     * @brief Connect to an endpoint
     * @param ep Endpoint to connect to
     * @return 0 on success, error code on failure
     */
    int connect(qb::io::endpoint const &ep) noexcept;

    /**
     * @brief Connect to a URI
     * @param u URI to connect to
     * @return 0 on success, error code on failure
     */
    int connect(uri const &u) noexcept;

    /**
     * @brief Connect to an IPv4 address
     * @param host Host address to connect to
     * @param port Port number to connect to
     * @return 0 on success, error code on failure
     */
    int connect_v4(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Connect to an IPv6 address
     * @param host Host address to connect to
     * @param port Port number to connect to
     * @return 0 on success, error code on failure
     */
    int connect_v6(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Connect to a Unix socket
     * @param path Path to the Unix socket
     * @return 0 on success, error code on failure
     */
    int connect_un(std::string const &path) noexcept;

    /**
     * @brief Non-blocking connect to an endpoint
     * @param ep Endpoint to connect to
     * @return 0 on success, error code on failure
     */
    int n_connect(qb::io::endpoint const &ep) noexcept;

    /**
     * @brief Called when a non-blocking connection is established
     */
    inline void
    connected() noexcept {}

    /**
     * @brief Non-blocking connect to a URI
     * @param u URI to connect to
     * @return 0 on success, error code on failure
     */
    int n_connect(uri const &u) noexcept;

    /**
     * @brief Non-blocking connect to an IPv4 address
     * @param host Host address to connect to
     * @param port Port number to connect to
     * @return 0 on success, error code on failure
     */
    int n_connect_v4(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Non-blocking connect to an IPv6 address
     * @param host Host address to connect to
     * @param port Port number to connect to
     * @return 0 on success, error code on failure
     */
    int n_connect_v6(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Non-blocking connect to a Unix socket
     * @param path Path to the Unix socket
     * @return 0 on success, error code on failure
     */
    int n_connect_un(std::string const &path) noexcept;

    /**
     * @brief Read data from the socket
     * @param dest Destination buffer for the data
     * @param len Maximum number of bytes to read
     * @return Number of bytes read on success, error code on failure
     */
    int read(void *dest, std::size_t len) const noexcept;

    /**
     * @brief Write data to the socket
     * @param data Data to send
     * @param size Number of bytes to send
     * @return Number of bytes sent on success, error code on failure
     */
    int write(const void *data, std::size_t size) const noexcept;

    /**
     * @brief Disconnect the socket
     * @return 0 on success, error code on failure
     */
    int disconnect() const noexcept;
};

} // namespace qb::io::tcp

#endif // QB_IO_TCP_SOCKET_H_
