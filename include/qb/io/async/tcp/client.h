/**
 * @file qb/io/async/tcp/client.h
 * @brief Asynchronous TCP client implementation for the QB IO library
 *
 * This file defines the client template class which provides functionality
 * for asynchronous TCP client connections. It supports both server-associated
 * clients (typically created when a connection is accepted by a server) and
 * standalone clients (for outgoing connections).
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

#ifndef QB_IO_ASYNC_TCP_SESSION_H
#define QB_IO_ASYNC_TCP_SESSION_H

#include "../../../uuid.h"
#include "../io.h"

namespace qb::io::async::tcp {

/**
 * @class client
 * @brief Server-associated asynchronous TCP client
 *
 * This specialization of the client template is used for clients that are
 * associated with a server, typically created when a connection is accepted.
 * It maintains a reference to the server and has a unique identifier.
 *
 * @tparam _Derived The derived class type (CRTP pattern)
 * @tparam _Transport The transport class type
 * @tparam _Server The server class type
 */
template <typename _Derived, typename _Transport, typename _Server = void>
class client
    : public io<_Derived>
    , _Transport {
    using base_t = io<_Derived>;
    friend base_t;

public:
    using base_io_t = io<_Derived>; /**< Base I/O type */
    using transport_io_type =
        typename _Transport::transport_io_type; /**< Transport I/O type */
    using _Transport::in;        /**< Import the in method from the transport */
    using _Transport::out;       /**< Import the out method from the transport */
    using _Transport::transport; /**< Import the transport method from the transport */
    using base_t::publish;       /**< Import the publish method from the base class */

protected:
    const uuid _uuid;   /**< Unique identifier for this client */
    _Server   &_server; /**< Reference to the associated server */

public:
    using IOServer = _Server; /**< Type alias for the server type */
    constexpr static const bool has_server =
        true; /**< Flag indicating server association */

    /**
     * @brief Default constructor (deleted)
     */
    client() = delete;

    /**
     * @brief Constructor with server reference
     *
     * Creates a new client associated with the given server.
     * If the derived class defines a Protocol type that is not void,
     * an instance of that protocol is created and attached to the client.
     *
     * @param server Reference to the associated server
     */
    explicit client(_Server &server)
        : _uuid(generate_random_uuid())
        , _server(server) {
        if constexpr (has_member_Protocol<_Derived>::value) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(
                    static_cast<_Derived &>(*this));
            }
        }
    }

    /**
     * @brief Destructor
     */
    ~client() = default;

    /**
     * @brief Get the associated server
     * @return Reference to the associated server
     */
    inline _Server &
    server() {
        return _server;
    }

    /**
     * @brief Get the associated server (const version)
     * @return Const reference to the associated server
     */
    inline _Server &
    server() const {
        return _server;
    }

    /**
     * @brief Get the client's unique identifier
     * @return Const reference to the UUID
     */
    inline uuid const &
    id() const noexcept {
        return _uuid;
    }
};

/**
 * @class client
 * @brief Standalone asynchronous TCP client
 *
 * This specialization of the client template is used for standalone clients
 * that are not associated with a server, typically for outgoing connections.
 *
 * @tparam _Derived The derived class type (CRTP pattern)
 * @tparam _Transport The transport class type
 */
template <typename _Derived, typename _Transport>
class client<_Derived, _Transport, void>
    : public io<_Derived>
    , _Transport {
    using base_t = io<_Derived>;
    friend base_t;

public:
    using transport_io_type =
        typename _Transport::transport_io_type; /**< Transport I/O type */
    using _Transport::in;        /**< Import the in method from the transport */
    using _Transport::out;       /**< Import the out method from the transport */
    using _Transport::transport; /**< Import the transport method from the transport */
    using base_t::publish;       /**< Import the publish method from the base class */

public:
    /**
     * @brief Default constructor
     *
     * Creates a new standalone client.
     * If the derived class defines a Protocol type that is not void,
     * an instance of that protocol is created and attached to the client.
     */
    client() noexcept {
        if constexpr (has_member_Protocol<_Derived>::value) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(
                    static_cast<_Derived &>(*this));
            }
        }
    }

    /**
     * @brief Destructor
     *
     * Ensures proper cleanup of resources.
     * If the derived class does not have a dispose event handler
     * and the transport is still open, it calls dispose.
     */
    ~client() {
        if constexpr (!has_method_on<_Derived, void, event::dispose>::value) {
            if (transport().is_open()) {
                this->dispose();
            }
        }
    }
};

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_SESSION_H
