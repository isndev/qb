/**
 * @file qb/io/tcp/ssl/socket.h
 * @brief Implementation of SSL/TLS sockets for the QB IO library
 *
 * This file provides the implementation of secure TCP sockets using OpenSSL
 * for encrypted communications, supporting both client and server-side SSL/TLS.
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

#include <openssl/ssl.h>
#include "../socket.h"

#ifndef QB_IO_TCP_SSL_SOCKET_H_
#define QB_IO_TCP_SSL_SOCKET_H_

namespace qb::io::ssl {

/**
 * @struct Certificate
 * @brief Structure to hold SSL certificate information
 */
struct Certificate {
    std::string subject; /**< Subject of the certificate */
    std::string issuer;  /**< Issuer of the certificate */
    int64_t     version; /**< Version of the certificate */
};

/**
 * @brief Extract certificate information from an SSL connection
 * @param ssl Pointer to the SSL connection
 * @return Certificate structure containing the certificate information
 */
Certificate get_certificate(SSL *ssl);

/**
 * @brief Create an SSL context for a client
 * @param method SSL method to use (e.g., TLS_client_method())
 * @return Pointer to the newly created SSL context
 */
SSL_CTX *create_client_context(const SSL_METHOD *method);

/**
 * @brief Create an SSL context for a server
 * @param method SSL method to use (e.g., TLS_server_method())
 * @param cert_path Path to the certificate file
 * @param key_path Path to the private key file
 * @return Pointer to the newly created SSL context
 */
SSL_CTX *create_server_context(const SSL_METHOD *method, std::string const &cert_path,
                               std::string const &key_path);

} // namespace qb::io::ssl
namespace qb::io::tcp::ssl {

// class listener;

/*!
 * @class socket tcp/ssl/socket.h qb/io/tcp/ssl/socket.h
 * @ingroup TCP
 * @brief Class implementing secure SSL/TLS socket functionality
 *
 * This class provides secure socket functionality using OpenSSL for encrypted
 * communications. It inherits from tcp::socket and adds SSL/TLS encryption
 * to the TCP connection.
 */
class QB_API socket : public tcp::socket {
    std::unique_ptr<SSL, void (*)(SSL *)> _ssl_handle; /**< SSL connection handle */
    bool _connected; /**< Flag indicating if the SSL handshake has completed */

    /**
     * @brief Check the SSL handshake status
     * @return 0 on success, error code on failure
     */
    int handCheck() noexcept;

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
    /**
     * @brief Destructor
     */
    ~socket() noexcept;

    /**
     * @brief Default constructor
     */
    socket() noexcept;

    /**
     * @brief Constructor from an SSL context and TCP socket
     * @param ctx SSL context to use
     * @param sock TCP socket to wrap with SSL
     */
    socket(SSL *ctx, tcp::socket &sock) noexcept;

    /**
     * @brief Copy constructor (deleted)
     */
    socket(socket const &rhs) = delete;

    /**
     * @brief Move constructor
     */
    socket(socket &&rhs) = default;

    /**
     * @brief Move assignment operator
     * @return Reference to the moved socket
     */
    socket &operator=(socket &&rhs) = default;

    /**
     * @brief Initialize the socket with an SSL handle
     * @param handle SSL handle to use
     */
    void init(SSL *handle) noexcept;

    /**
     * @brief Connect to an endpoint with optional hostname for SNI
     * @param ep Endpoint to connect to
     * @param hostname Hostname for SNI (Server Name Indication)
     * @return 0 on success, error code on failure
     */
    int connect(endpoint const &ep, std::string const &hostname = "") noexcept;

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
     * @brief Non-blocking connect to an endpoint with optional hostname for SNI
     * @param ep Endpoint to connect to
     * @param hostname Hostname for SNI (Server Name Indication)
     * @return 0 on success, error code on failure
     */
    int n_connect(qb::io::endpoint const &ep, std::string const &hostname = "") noexcept;

    /**
     * @brief Called when a non-blocking connection is established to perform SSL
     * handshake
     * @return 0 on success, error code on failure
     */
    int connected() noexcept;

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
     * @brief Disconnect the socket
     * @return 0 on success, error code on failure
     */
    int disconnect() noexcept;

    /**
     * @brief Read data from the secure socket
     * @param data Destination buffer for the data
     * @param size Maximum number of bytes to read
     * @return Number of bytes read on success, error code on failure
     */
    int read(void *data, std::size_t size) noexcept;

    /**
     * @brief Write data to the secure socket
     * @param data Data to send
     * @param size Number of bytes to send
     * @return Number of bytes sent on success, error code on failure
     */
    int write(const void *data, std::size_t size) noexcept;

    /**
     * @brief Get the SSL handle
     * @return Pointer to the SSL handle
     */
    [[nodiscard]] SSL *ssl_handle() const noexcept;

private:
    //    friend class ssl::listener;
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_SOCKET_H_
