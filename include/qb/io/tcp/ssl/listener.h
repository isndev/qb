/**
 * @file qb/io/tcp/ssl/listener.h
 * @brief Implementation of a secure SSL/TLS listener for the QB IO library
 *
 * This file provides the implementation of a secure TCP listener using OpenSSL
 * for accepting encrypted connections. It supports SSL/TLS server-side functionality.
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

#include "../listener.h"
#include "socket.h"

#ifndef QB_IO_TCP_SSL_LISTENER_H_
#define QB_IO_TCP_SSL_LISTENER_H_

namespace qb::io::tcp::ssl {

/*!
 * @class listener tcp/listener.h qb/io/tcp/ssl/listener.h
 * @ingroup TCP
 * @brief Class implementing a secure SSL/TLS listener
 *
 * This class provides functionality for listening for incoming SSL/TLS connections.
 * It inherits from the base tcp::listener class and adds SSL/TLS encryption
 * capabilities for secure server applications.
 */
class QB_API listener : public tcp::listener {
    std::unique_ptr<SSL_CTX, void (*)(SSL_CTX *)>
        _ctx; /**< SSL context for the listener */

public:
    /**
     * @brief Destructor
     */
    ~listener() noexcept;

    /**
     * @brief Default constructor
     */
    listener() noexcept;

    /**
     * @brief Copy constructor (deleted)
     */
    listener(listener const &) = delete;

    /**
     * @brief Move constructor
     */
    listener(listener &&) = default;

    /**
     * @brief Move assignment operator
     * @return Reference to the moved listener
     */
    listener &operator=(listener &&) = default;

    /**
     * @brief Initialize the listener with an SSL context
     * @param ctx SSL context to use
     */
    void init(SSL_CTX *ctx) noexcept;

    /**
     * @brief Accept a new secure connection and create an SSL socket
     * @return Newly created SSL socket for the accepted connection
     */
    ssl::socket accept() const noexcept;

    /**
     * @brief Accept a new secure connection into an existing SSL socket
     * @param socket SSL socket to use for the connection
     * @return 0 on success, error code on failure
     */
    int accept(ssl::socket &socket) const noexcept;

    /**
     * @brief Get the SSL context handle
     * @return Pointer to the SSL context
     */
    [[nodiscard]] SSL_CTX *ssl_handle() const noexcept;
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_LISTENER_H_
