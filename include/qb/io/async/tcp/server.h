/**
 * @file qb/io/async/tcp/server.h
 * @brief Asynchronous TCP server implementation for the QB IO library
 * 
 * This file defines the server template class which provides a complete
 * implementation of an asynchronous TCP server. It combines the TCP acceptor
 * for handling incoming connections with the IO handler for managing client
 * sessions.
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

#ifndef QB_IO_ASYNC_TCP_SERVER_H
#define QB_IO_ASYNC_TCP_SERVER_H

#include "../io_handler.h"
#include "acceptor.h"

namespace qb::io::async::tcp {

/**
 * @class server
 * @brief Asynchronous TCP server implementation
 * 
 * This template class implements a complete asynchronous TCP server.
 * It combines the TCP acceptor for handling incoming connections with
 * the IO handler for managing client sessions. New connections are
 * automatically accepted and registered as sessions.
 * 
 * @tparam _Derived The derived class type (CRTP pattern)
 * @tparam _Session The session class type for handling client connections
 * @tparam _Prot The protocol class for parsing messages
 */
template <typename _Derived, typename _Session, typename _Prot>
class server
    : public acceptor<server<_Derived, _Session, _Prot>, _Prot>
    , public io_handler<_Derived, _Session> {
    using acceptor_type = acceptor<server<_Derived, _Session, _Prot>, _Prot>; /**< Type alias for the acceptor base */

public:
    /**
     * @brief Default constructor
     */
    server() = default;

    /**
     * @brief Handler for new accepted connections
     * 
     * This method is called when a new connection is accepted by the
     * acceptor. It registers the new connection as a session.
     * 
     * @param new_io The new socket for the accepted connection
     */
    void
    on(typename acceptor_type::accepted_socket_type &&new_io) {
        this->registerSession(
            std::forward<typename acceptor_type::accepted_socket_type>(new_io));
    }

    /**
     * @brief Handler for server disconnection events
     * 
     * This method is called when the server is disconnected.
     * The default implementation does nothing, but it can be overridden
     * in derived classes to handle server disconnection.
     * 
     * @param Disconnected event
     */
    void
    on(event::disconnected &&) {
        // throw std::runtime_error("Server had been disconnected");
    }
};

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_SERVER_H
