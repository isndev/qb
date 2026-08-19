/**
 * @file qb/io/async/tcp/acceptor.h
 * @brief Asynchronous TCP connection acceptor implementation
 *
 * This file defines the acceptor template class which handles incoming TCP
 * connections for an asynchronous server. It uses the input class for asynchronous
 * operations and a protocol for accepting and processing incoming connections.
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

#ifndef QB_IO_ASYNC_TCP_ACCEPTOR_H
#define QB_IO_ASYNC_TCP_ACCEPTOR_H

#include <filesystem>
#ifdef QB_HAS_SSL
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
    using base_t   = input<acceptor<_Derived, _Prot>>;
    using Protocol = protocol::accept<acceptor, typename _Prot::socket_type>; /**< Protocol type for accepting
                                                                                 connections */

public:
    /**
     * @brief Handler for disconnection events
     *
     * Forwarded to @p _Derived only when it genuinely overrides
     * `on(event::disconnected ...)` (detected via `qb::has_own_on`, which
     * distinguishes a real user override from the one merely inherited from
     * this acceptor through CRTP — the naive `qb::has_on` would always be
     * `true` and cause infinite recursion). Otherwise a `std::runtime_error`
     * is raised so the listener can propagate the fatal condition upward.
     *
     * @param e The disconnection event
     * @throws std::runtime_error If the derived class doesn't handle disconnection
     */
    void
    on(event::disconnected &&e) {
        if constexpr (qb::has_own_on<_Derived, acceptor, event::disconnected>)
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
     * @brief Destructor — stop the accept watcher before the listening fd closes.
     *
     * `_Prot` (which owns the listening socket) is a sibling base listed after
     * `input<acceptor>`, so it is destroyed first and closes the fd. Stopping the
     * watcher in this destructor body (runs before any base) prevents the
     * `input<>` base destructor from calling `ev_io_stop` on a closed fd, which
     * would corrupt libev's per-fd bookkeeping. A no-op if already stopped.
     */
    ~acceptor() {
        this->_async_event.stop();
    }

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
        static_cast<_Derived &>(*this).on(std::forward<typename Protocol::message>(new_socket));
    }

    /**
     * @brief Listen for incoming connections on a given URI.
     * @param uri The URI to listen on.
     * @param cert_file The path to the certificate file (SSL transports only).
     * @param key_file The path to the private key file (SSL transports only).
     * @param alpn_protocols The ALPN protocols to advertise (SSL transports only).
     * @return `true` if the listening socket has been successfully bound and started,
     *         `false` otherwise.
     *
     * @note **Auto-start.** Once the underlying socket is bound, this method also
     *       calls `this->start()` so that the acceptor is wired into the event loop
     *       without the caller having to remember the second step. Callers that
     *       need to defer the watcher registration (for example, to finish some
     *       server-side setup before the first accept fires) can call
     *       `listen_no_start()` instead.
     */
    [[nodiscard]] bool
    listen(qb::io::uri uri, [[maybe_unused]] std::filesystem::path cert_file = {}, [[maybe_unused]] std::filesystem::path key_file = {},
           [[maybe_unused]] std::vector<std::string> alpn_protocols = {}) {
        if (!listen_no_start(std::move(uri), std::move(cert_file), std::move(key_file), std::move(alpn_protocols)))
            return false;
        this->start();
        return true;
    }

    /**
     * @brief Listen on a given URI without registering the accept watcher.
     *
     * Identical to `listen()` but leaves the acceptor idle until the caller explicitly
     * invokes `this->start()`. Useful for servers that need to perform additional
     * initialisation (plug protocol upgraders, register lifecycle hooks, …) before
     * the first accept fires.
     */
    [[nodiscard]] bool
    listen_no_start(qb::io::uri uri, [[maybe_unused]] std::filesystem::path cert_file = {},
                    [[maybe_unused]] std::filesystem::path key_file = {}, [[maybe_unused]] std::vector<std::string> alpn_protocols = {}) {
#ifdef QB_HAS_SSL
        using tpt = std::decay_t<decltype(this->transport())>;
        if constexpr (tpt::is_secure()) {
            this->transport().init(qb::io::ssl::Context::server(std::move(cert_file), std::move(key_file)).alpn(std::move(alpn_protocols)));
            if (!this->transport().context().ok()) {
                QB_LOG_CRIT("Failed to initialize SSL/TLS server context: " << this->transport().context().error());
                return false;
            }
        }
#endif
        return !this->transport().listen(std::move(uri));
    }
};

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_ACCEPTOR_H
