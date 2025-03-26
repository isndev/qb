/**
 * @file qb/io/tcp/listener.h
 * @brief Implementation of a TCP listener for the QB IO library
 * 
 * This file provides the implementation of a TCP listener for accepting
 * incoming connections. It supports IPv4, IPv6, and Unix sockets.
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
#    define QB_IO_TCP_LISTENER_H_

namespace qb::io::tcp {

/*!
 * @class listener tcp/listener.h qb/io/tcp/listener.h
 * @ingroup TCP
 * @brief Class implementing a TCP listener
 * 
 * This class provides functionality for listening for incoming TCP connections.
 * It inherits from the base qb::io::socket class and provides methods for
 * listening and accepting connections on different types of addresses.
 */
class QB_API listener : private io::socket {
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
     * @brief Start listening on an endpoint
     * @param ep Endpoint to listen on
     * @return 0 on success, error code on failure
     */
    int listen(io::endpoint const &ep) noexcept;
    
    /**
     * @brief Start listening on a URI
     * @param uri URI to listen on
     * @return 0 on success, error code on failure
     */
    int listen(io::uri const &uri) noexcept;
    
    /**
     * @brief Start listening on an IPv4 address
     * @param port Port number
     * @param host Host address (default: "0.0.0.0" for all interfaces)
     * @return 0 on success, error code on failure
     */
    int listen_v4(uint16_t port, std::string const &host = "0.0.0.0") noexcept;
    
    /**
     * @brief Start listening on an IPv6 address
     * @param port Port number
     * @param host Host address (default: "::" for all interfaces)
     * @return 0 on success, error code on failure
     */
    int listen_v6(uint16_t port, std::string const &host = "::") noexcept;
    
    /**
     * @brief Start listening on a Unix socket
     * @param path Path to the Unix socket
     * @return 0 on success, error code on failure
     */
    int listen_un(std::string const &path) noexcept;

    /**
     * @brief Accept a new connection and create a TCP socket
     * @return Newly created TCP socket for the accepted connection
     */
    tcp::socket accept() const noexcept;
    
    /**
     * @brief Accept a new connection into an existing socket
     * @param sock TCP socket to use for the connection
     * @return 0 on success, error code on failure
     */
    int accept(tcp::socket &sock) const noexcept;

    /**
     * @brief Disconnect the listener
     * @return 0 on success, error code on failure
     */
    int disconnect() const noexcept;
};

} // namespace qb::io::tcp

#endif // QB_IO_TCP_LISTENER_H_
