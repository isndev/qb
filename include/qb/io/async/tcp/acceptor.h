/**
 * @file qb/io/async/tcp/acceptor.h
 * @brief Asynchronous TCP connection acceptor implementation
 *
 * This file defines the acceptor template class which handles incoming TCP
 * connections for an asynchronous server. It uses the input class for asynchronous
 * operations and a protocol for accepting and processing incoming connections.
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

#ifndef QB_IO_ASYNC_TCP_ACCEPTOR_H
#define QB_IO_ASYNC_TCP_ACCEPTOR_H

#include <filesystem>
#ifdef QB_IO_WITH_SSL
#include "../../tcp/ssl/listener.h"
#endif
#include "../../protocol/accept.h"
#include "../io.h"

namespace qb::io::async::tcp {

/**
 * @class acceptor
 * @brief Handles accepting incoming TCP connections asynchronously
 *
 * This template class provides functionality for accepting incoming TCP connections
 * in an asynchronous manner. It uses the input class for asynchronous operations
 * and a protocol for accepting and processing incoming connections. When a new
 * connection is accepted, it passes the new socket to the derived class for handling.
 *
 * @tparam _Derived The derived class type (CRTP pattern)
 * @tparam _Prot The protocol class type to use with the acceptor
 */
template <typename _Derived, typename _Prot>
class acceptor
    : public input<acceptor<_Derived, _Prot>>
    , public _Prot {
    friend class input<acceptor<_Derived, _Prot>>;
    using base_t = input<acceptor<_Derived, _Prot>>;
    using Protocol =
        protocol::accept<acceptor,
                         typename _Prot::socket_type>; /**< Protocol type for accepting
                                                          connections */

public:
    /**
     * @brief Handler for disconnection events
     *
     * This method is called when the acceptor is disconnected.
     * If the derived class has a handler for disconnection events,
     * it forwards the event to that handler. Otherwise, it throws
     * a runtime error.
     *
     * @param e The disconnection event
     * @throws std::runtime_error If the derived class doesn't handle disconnection
     */
    void
    on(event::disconnected &&e) {
        if constexpr (has_method_on<_Derived, void, event::disconnected>::value)
            static_cast<_Derived &>(*this).on(std::forward<event::disconnected>(e));
        else
            throw std::runtime_error("Acceptor has been disconnected");
    }

public:
    /**
     * @brief Type of socket created for accepted connections
     */
    using accepted_socket_type = typename _Prot::socket_type;

public:
    /**
     * @brief Constructor
     *
     * Creates a new acceptor with the accept protocol.
     */
    acceptor() noexcept
        : base_t(new Protocol(*this)) {}

    /**
     * @brief Destructor
     */
    ~acceptor() = default;

    /**
     * @brief Handler for new connections
     *
     * This method is called when a new connection is accepted.
     * It forwards the new socket to the derived class for handling.
     *
     * @param new_socket The new socket for the accepted connection
     */
    void
    on(typename Protocol::message &&new_socket) {
        static_cast<_Derived &>(*this).on(
            std::forward<typename Protocol::message>(new_socket));
    }

    /**
     * @brief Listen for incoming connections on a given URI.
     * @param uri The URI to listen on.
     * @param cert_file The path to the certificate file.
     * @param key_file The path to the key file.
     * @param alpn_protocols The ALPN protocols to support.
     * @return True if the server is listening, false otherwise.
     */
    bool listen(qb::io::uri uri,
                std::filesystem::path cert_file = {},
                std::filesystem::path key_file = {},
                std::vector<std::string> alpn_protocols = {}) {
#ifdef QB_IO_WITH_SSL
        using tpt = std::decay_t<decltype(this->transport())>;
        if constexpr (tpt::is_secure()) {
            this->transport().init(qb::io::ssl::create_server_context(TLS_server_method(), cert_file, key_file));
            if (!this->transport().ssl_handle()) {
                LOG_CRIT("Failed to initialize SSL/TLS server context.");
                return false;
            }
            this->transport().set_supported_alpn_protocols(std::move(alpn_protocols));
        }
#endif
        return !this->transport().listen(std::move(uri));
    }
};

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_ACCEPTOR_H
