/**
 * @file qb/io/tcp/ssl/listener.h
 * @brief Implementation of a secure SSL/TLS listener for the QB IO library.
 *
 * This file provides the implementation of a secure TCP listener using OpenSSL
 * for accepting encrypted connections. It supports SSL/TLS server-side functionality.
 * Requires OpenSSL and `QB_IO_WITH_SSL`.
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
 * @ingroup SSL
 */

#include "../listener.h"
#include "socket.h"

#ifndef QB_IO_TCP_SSL_LISTENER_H_
#define QB_IO_TCP_SSL_LISTENER_H_

namespace qb::io::tcp::ssl {

/*!
 * @class listener
 * @ingroup SSL
 * @brief Class implementing a secure SSL/TLS TCP listener for accepting encrypted connections.
 *
 * This class provides functionality for listening for incoming SSL/TLS connections.
 * It inherits from the base `qb::io::tcp::listener` class and adds SSL/TLS encryption
 * capabilities by managing an `SSL_CTX` (SSL Context). When a connection is accepted,
 * it creates and returns a `qb::io::tcp::ssl::socket` ready for secure communication after handshake.
 */
class QB_API listener : public tcp::listener {
    std::unique_ptr<SSL_CTX, void (*)(SSL_CTX *)>
        _ctx; /**< Unique pointer managing the OpenSSL SSL_CTX (context) object. */

public:
    /**
     * @brief Destructor.
     * @details Ensures the `SSL_CTX` is freed if managed by this listener.
     *          The base class destructor handles closing the listening socket.
     */
    ~listener() noexcept;

    /**
     * @brief Default constructor.
     * @details Initializes the listener with a null SSL context deleter. The SSL context
     *          must be set via `init(SSL_CTX*)` before the listener can be used to accept secure connections.
     */
    listener() noexcept;

    /**
     * @brief Deleted copy constructor. Listeners are not copyable.
     */
    listener(listener const &) = delete;

    /**
     * @brief Default move constructor.
     */
    listener(listener &&) = default;

    /**
     * @brief Default move assignment operator.
     * @return Reference to this listener.
     */
    listener &operator=(listener &&) = default;

    /**
     * @brief Initialize the listener with a pre-configured SSL context.
     * @param ctx A pointer to an `SSL_CTX` object, typically created and configured using
     *            `qb::io::ssl::create_server_context()` or directly with OpenSSL functions.
     *            This listener takes ownership of the context via `std::unique_ptr`.
     * @note This must be called before `listen()` if secure connections are to be accepted.
     */
    void init(SSL_CTX *ctx) noexcept;

    /**
     * @brief Accept a new secure connection and return it as a new `ssl::socket`.
     * @return A new `qb::io::tcp::ssl::socket` instance representing the client connection.
     *         The returned socket will have an associated `SSL` object created from this listener's context.
     *         The SSL handshake is typically initiated by the `ssl::socket::connected()` method or
     *         implicitly during the first read/write on the `ssl::socket` if it's blocking.
     *         If an error occurs during TCP accept, the returned socket will not be open.
     * @details This method first calls the base `tcp::listener::accept()` to get a plain TCP socket,
     *          then creates an `SSL` object from its `_ctx`, associates it with the new socket descriptor,
     *          and wraps it in an `ssl::socket`.
     */
    ssl::socket accept() const noexcept;

    /**
     * @brief Accept a new secure connection into an existing `ssl::socket` object.
     * @param socket A reference to an `ssl::socket` object. If a TCP connection is accepted,
     *               an `SSL` object is created from this listener's context, associated with the new
     *               socket descriptor, and then `socket.init(SSL*)` and `socket = std::move(new_tcp_socket)`
     *               are used to configure the provided `ssl::socket` instance.
     * @return 0 on successful TCP accept (SSL handshake will follow on the `socket` object).
     *         A non-zero error code on TCP accept failure.
     */
    int accept(ssl::socket &socket) const noexcept;

    /**
     * @brief Get the raw OpenSSL `SSL_CTX` handle.
     * @return Pointer to the `SSL_CTX` object, or `nullptr` if not initialized.
     * @note Allows direct access to the OpenSSL API for advanced context configuration if needed.
     */
    [[nodiscard]] SSL_CTX *ssl_handle() const noexcept;
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_LISTENER_H_
