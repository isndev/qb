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
 * @ingroup TCP
 */

#ifndef QB_IO_ASYNC_TCP_CLIENT_H
#define QB_IO_ASYNC_TCP_CLIENT_H

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
    using base_io_t         = io<_Derived>;                           /**< Base I/O type */
    using transport_io_type = typename _Transport::transport_io_type; /**< Transport I/O type */
    using _Transport::in;                                             /**< Import the in method from the transport */
    using _Transport::out;                                            /**< Import the out method from the transport */
    using _Transport::transport;                                      /**< Import the transport method from the transport */
    using base_t::publish;                                            /**< Import the publish method from the base class */

protected:
    const uuid _uuid;   /**< Unique identifier for this client */
    _Server   &_server; /**< Reference to the associated server */

public:
    using IOServer                         = _Server; /**< Type alias for the server type */
    constexpr static const bool has_server = true;    /**< Flag indicating server association */

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
        if constexpr (qb::has_type_Protocol<_Derived>) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(static_cast<_Derived &>(*this));
            }
        }
    }

    /**
     * @brief Destructor
     *
     * Stops the I/O event watcher while the transport socket is still open.
     * See the standalone `client<_Derived, _Transport, void>` destructor for the
     * full rationale: `_Transport` is a sibling base listed after `io<_Derived>`,
     * so it is destroyed first (closing the fd) — the watcher must therefore be
     * stopped here, in the destructor body, before any base is torn down.
     * Server-associated sessions are especially exposed because `dispose()` does
     * not stop their watcher (the server owns the lifecycle), so without this the
     * watcher would routinely outlive its fd. Stopping an already-stopped (or
     * never-started) watcher is a no-op.
     */
    ~client() {
        this->_async_event.stop();
    }

    /**
     * @brief Get the associated server
     * @return Reference to the associated server
     */
    [[nodiscard]] inline _Server &
    server() noexcept {
        return _server;
    }

    /**
     * @brief Get the associated server (const version)
     * @return Const reference to the associated server
     */
    [[nodiscard]] inline _Server const &
    server() const noexcept {
        return _server;
    }

    /**
     * @brief Get the client's unique identifier
     * @return Const reference to the UUID
     */
    [[nodiscard]] inline uuid const &
    id() const noexcept {
        return _uuid;
    }

    /**
     * @brief Get the client as a shared pointer
     * @return Shared pointer to the client
     */
    [[nodiscard]] inline auto
    shared() {
        return server().session(id());
    }

    /**
     * @brief Get the client's IP address
     * @return IP address
     */
    [[nodiscard]] inline auto
    ip() const {
        return transport().peer_endpoint().ip();
    }

    /**
     * @brief Get the client's port
     * @return Port
     */
    [[nodiscard]] inline auto
    port() const {
        return transport().peer_endpoint().port();
    }
};

/**
 * @class client
 * @brief Standalone asynchronous TCP client specialization without server association
 * @details
 * This specialization of the client template is used for standalone clients that are not
 * associated with any server, typically for outgoing connections. It includes all the
 * basic functionality of the generic client but without server-specific features.
 *
 * @tparam _Derived The derived class type (CRTP pattern)
 * @tparam _Transport The transport layer implementation type
 *
 * @ingroup TCP
 */
template <typename _Derived, typename _Transport>
class client<_Derived, _Transport, void>
    : public io<_Derived>
    , _Transport {
    using base_t = io<_Derived>;
    friend base_t;

protected:
    const uuid _uuid; /**< Unique identifier for this client */

public:
    using base_io_t         = io<_Derived>;                           /**< Base I/O type */
    using transport_io_type = typename _Transport::transport_io_type; /**< Transport I/O type */
    using _Transport::in;                                             /**< Import the in method from the transport */
    using _Transport::out;                                            /**< Import the out method from the transport */
    using _Transport::transport;                                      /**< Import the transport method from the transport */
    using base_t::publish;                                            /**< Import the publish method from the base class */

public:
    /**
     * @brief Default constructor
     *
     * Creates a new standalone client.
     * If the derived class defines a Protocol type that is not void,
     * an instance of that protocol is created and attached to the client.
     */
    client() noexcept {
        if constexpr (qb::has_type_Protocol<_Derived>) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(static_cast<_Derived &>(*this));
            }
        }
    }

    /**
     * @brief Destructor
     *
     * Stops the I/O event watcher while the transport socket is still open.
     *
     * @warning Base destruction order is the reverse of the base list above:
     *          `_Transport` (the socket fd owner) is destroyed BEFORE the
     *          `io<_Derived>` base (which owns the libev watcher). If the watcher
     *          were left active until the `io<_Derived>` base destructor, libev's
     *          `qev_io_stop` would run against an already-closed fd and corrupt its
     *          per-fd bookkeeping (`anfds[fd]`), an intermittent
     *          use-after-close that surfaces as a later crash in `clear_pending`/
     *          `fd_change`. This destructor body runs *before* any base
     *          destructor, i.e. while `_Transport` is still alive and the fd is
     *          still valid, so stopping the watcher here is the only safe point.
     *          Stopping an already-stopped (or never-started) watcher is a no-op.
     */
    ~client() {
        this->_async_event.stop();
    }
};

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_CLIENT_H
