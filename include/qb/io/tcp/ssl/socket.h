/**
 * @file qb/io/tcp/ssl/socket.h
 * @brief Implementation of SSL/TLS sockets for secure TCP communication in the QB IO library.
 *
 * This file provides the implementation of secure TCP sockets using OpenSSL
 * for encrypted communications, supporting both client and server-side SSL/TLS.
 * Requires OpenSSL to be linked and `QB_IO_WITH_SSL` to be defined.
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

#include <openssl/ssl.h>
#include "../socket.h"

#ifndef QB_IO_TCP_SSL_SOCKET_H_
#define QB_IO_TCP_SSL_SOCKET_H_

namespace qb::io::ssl {

/**
 * @struct Certificate
 * @ingroup SSL
 * @brief Structure to hold essential SSL certificate information.
 * @details Contains common fields extracted from an X509 certificate.
 */
struct Certificate {
    std::string subject; /**< The subject name of the certificate. */
    std::string issuer;  /**< The issuer name of the certificate. */
    int64_t     version; /**< The version number of the certificate. */
};

/**
 * @brief Extract certificate information from an active SSL connection.
 * @ingroup SSL
 * @param ssl Pointer to the SSL connection structure (`SSL*`) from which to extract certificate details.
 * @return A `qb::io::ssl::Certificate` structure populated with the subject, issuer, and version
 *         of the peer's certificate. Returns an empty/default-initialized struct if no certificate
 *         is available or an error occurs.
 */
Certificate get_certificate(SSL *ssl);

/**
 * @brief Create an SSL context (`SSL_CTX`) configured for client-side SSL/TLS operations.
 * @ingroup SSL
 * @param method The SSL/TLS method to use (e.g., `TLS_client_method()`, `SSLv23_client_method()`).
 * @return Pointer to the newly created `SSL_CTX` on success, `nullptr` on failure.
 * @note The caller is responsible for freeing the returned `SSL_CTX` using `SSL_CTX_free()`.
 */
SSL_CTX *create_client_context(const SSL_METHOD *method);

/**
 * @brief Create an SSL context (`SSL_CTX`) configured for server-side SSL/TLS operations.
 * @ingroup SSL
 * @param method The SSL/TLS method to use (e.g., `TLS_server_method()`, `SSLv23_server_method()`).
 * @param cert_path Path to the server's PEM-encoded certificate file.
 * @param key_path Path to the server's PEM-encoded private key file.
 * @return Pointer to the newly created `SSL_CTX` on success, `nullptr` on failure (e.g., if files cannot be loaded).
 * @note The caller is responsible for freeing the returned `SSL_CTX` using `SSL_CTX_free()`.
 */
SSL_CTX *create_server_context(const SSL_METHOD *method, std::string const &cert_path,
                               std::string const &key_path);

} // namespace qb::io::ssl
namespace qb::io::tcp::ssl {

// class listener;

/*!
 * @class socket
 * @ingroup SSL
 * @brief Class implementing secure SSL/TLS TCP socket functionality.
 *
 * This class provides secure socket functionality using OpenSSL for encrypted
 * communications. It inherits from `qb::io::tcp::socket` and adds an SSL/TLS encryption
 * layer to the TCP connection. It handles the SSL handshake process and transparently
 * encrypts/decrypts data for `read` and `write` operations.
 */
class QB_API socket : public tcp::socket {
    std::unique_ptr<SSL, void (*)(SSL *)> _ssl_handle; /**< Unique pointer managing the OpenSSL `SSL` object. */
    bool _connected; /**< Flag indicating if the SSL handshake has successfully completed. */

    /**
     * @brief Performs the SSL handshake check after a non-blocking connect.
     * @return 0 if handshake is complete or still in progress without error, 
     *         a non-zero SSL error code (e.g., `SSL_ERROR_WANT_READ`, `SSL_ERROR_WANT_WRITE`) if it needs more I/O,
     *         or a negative value for other errors.
     * @private
     */
    int handCheck() noexcept;

    /**
     * @brief Internal method to establish a blocking SSL connection to an address with a specific address family.
     * @param af Address family (AF_INET for IPv4, AF_INET6 for IPv6).
     * @param host Host address string (IP or hostname).
     * @param port Port number to connect to.
     * @return 0 on success (TCP connect and SSL handshake complete), non-zero error code on failure.
     * @private
     */
    int connect_in(int af, std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Internal method to initiate a non-blocking SSL connection to an address with a specific address family.
     * @param af Address family (AF_INET for IPv4, AF_INET6 for IPv6).
     * @param host Host address string (IP or hostname).
     * @param port Port number to connect to.
     * @return 0 if TCP connection is in progress or succeeded immediately (SSL handshake may still be pending).
     *         Returns a non-zero error code on immediate TCP connection failure.
     * @private
     */
    int n_connect_in(int af, std::string const &host, uint16_t port) noexcept;

public:
    /**
     * @brief Destructor.
     * @details Ensures the SSL connection is shut down and the `SSL` object is freed if managed.
     *          The base class destructor handles closing the underlying TCP socket.
     */
    ~socket() noexcept;

    /**
     * @brief Default constructor. Creates an uninitialized SSL socket.
     *        Call `init()` with an `SSL_CTX` and then a `connect` or `accept` related method.
     */
    socket() noexcept;

    /**
     * @brief Constructor from an existing OpenSSL `SSL` structure and an established `tcp::socket`.
     * @param ssl_ptr Pointer to an initialized `SSL` object (e.g., from `SSL_new` with an `SSL_CTX`).
     *                This `ssl::socket` will take ownership if `ssl_ptr` is not null.
     * @param sock A `tcp::socket` that is already connected (for clients) or accepted (for servers).
     *             The file descriptor from `sock` will be associated with the `SSL` object.
     *             The state of `sock` is moved into this `ssl::socket`.
     */
    socket(SSL *ssl_ptr, tcp::socket &sock) noexcept;

    /**
     * @brief Deleted copy constructor. SSL sockets are not copyable.
     */
    socket(socket const &rhs) = delete;

    /**
     * @brief Default move constructor.
     */
    socket(socket &&rhs) = default;

    /**
     * @brief Default move assignment operator.
     * @return Reference to this socket.
     */
    socket &operator=(socket &&rhs) = default;

    /**
     * @brief Initialize the SSL socket with an OpenSSL `SSL` handle.
     * @param handle A pointer to an `SSL` object, typically created using `SSL_new()` from an `SSL_CTX`.
     *               This `ssl::socket` takes ownership of the handle via `std::unique_ptr`.
     *               The underlying TCP socket must be set separately (e.g. via move construction or assignment from `tcp::socket`).
     * @note The `SSL` object should not yet have a file descriptor associated if this socket is to be used for a new connection.
     */
    void init(SSL *handle) noexcept;

    /**
     * @brief Establish a blocking SSL/TLS connection to a remote endpoint.
     * @param ep The `qb::io::endpoint` of the remote server.
     * @param hostname Optional hostname string for Server Name Indication (SNI). If empty, SNI is not used.
     * @return 0 on successful connection and SSL handshake, non-zero error code on failure.
     * @details Requires `init(SSL_CTX*)` to have been called first to set up the SSL context for this socket.
     */
    int connect(endpoint const &ep, std::string const &hostname = "") noexcept;

    /**
     * @brief Establish a blocking SSL/TLS connection to a remote endpoint specified by a URI.
     * @param u The `qb::io::uri` of the remote server. The URI's host is used for SNI if not overridden.
     * @return 0 on success, non-zero error code on failure.
     */
    int connect(uri const &u) noexcept;

    /**
     * @brief Establish a blocking SSL/TLS connection to an IPv4 server.
     * @param host The hostname or IP address string of the server. Used for SNI.
     * @param port The port number of the server.
     * @return 0 on success, non-zero error code on failure.
     */
    int connect_v4(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Establish a blocking SSL/TLS connection to an IPv6 server.
     * @param host The hostname or IP address string of the server. Used for SNI.
     * @param port The port number of the server.
     * @return 0 on success, non-zero error code on failure.
     */
    int connect_v6(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Establish a blocking SSL/TLS connection over a Unix domain socket (conceptual, as SSL is typically over TCP).
     * @param path The file system path of the Unix domain socket.
     * @return 0 on success, non-zero error code on failure.
     * @note SSL over Unix domain sockets is uncommon but technically possible if the peer expects it.
     */
    int connect_un(std::string const &path) noexcept;

    /**
     * @brief Initiate a non-blocking SSL/TLS connection to a remote endpoint.
     * @param ep The `qb::io::endpoint` of the remote server.
     * @param hostname Optional hostname for SNI. If empty, SNI is not used.
     * @return 0 if TCP connection is in progress or succeeded (SSL handshake follows via `connected()`).
     *         Non-zero error code on immediate TCP connection failure.
     * @details Sets up SNI if `hostname` is provided. After this call, use event loop mechanisms
     *          to wait for socket writability, then call `connected()` to perform/complete the SSL handshake.
     */
    int n_connect(qb::io::endpoint const &ep, std::string const &hostname = "") noexcept;

    /**
     * @brief Finalizes a non-blocking SSL connection after the underlying TCP socket is connected.
     * @return 0 if SSL handshake completed successfully or is in progress without error (`SSL_ERROR_WANT_READ/WRITE`).
     *         A non-zero SSL error code or negative value on handshake failure.
     * @details This method performs the SSL handshake (`SSL_connect` or `SSL_accept`).
     *          It should be called when a non-blocking `connect` (or `accept` on server side)
     *          has established the TCP layer, and the socket is ready for the SSL handshake I/O.
     *          Sets the internal `_connected` flag on successful handshake.
     */
    int connected() noexcept;

    /**
     * @brief Initiate a non-blocking SSL/TLS connection to a remote URI.
     * @param u The `qb::io::uri` of the remote server. Host from URI is used for SNI.
     * @return 0 on TCP connection progress/success, non-zero error on immediate failure.
     */
    int n_connect(uri const &u) noexcept;

    /**
     * @brief Initiate a non-blocking SSL/TLS connection to an IPv4 server.
     * @param host Hostname or IP string for connection and SNI.
     * @param port Server port number.
     * @return 0 on TCP connection progress/success, non-zero error on immediate failure.
     */
    int n_connect_v4(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Initiate a non-blocking SSL/TLS connection to an IPv6 server.
     * @param host Hostname or IP string for connection and SNI.
     * @param port Server port number.
     * @return 0 on TCP connection progress/success, non-zero error on immediate failure.
     */
    int n_connect_v6(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Initiate a non-blocking SSL/TLS connection over a Unix domain socket.
     * @param path Path to the Unix domain socket.
     * @return 0 on TCP connection progress/success, non-zero error on immediate failure.
     */
    int n_connect_un(std::string const &path) noexcept;

    /**
     * @brief Gracefully shut down the SSL/TLS connection and close the underlying socket.
     * @return 0 on success, non-zero error code on failure during SSL shutdown.
     * @details Performs `SSL_shutdown()` and then calls the base class `disconnect()`.
     */
    int disconnect() noexcept;

    /**
     * @brief Read decrypted data from the secure SSL/TLS socket.
     * @param data Pointer to the buffer where decrypted data will be stored.
     * @param size Maximum number of bytes to read into the buffer.
     * @return Number of bytes actually read and decrypted.
     *         Returns 0 if the peer performed an orderly SSL shutdown.
     *         Returns a negative value on error (e.g., `SSL_ERROR_WANT_READ`, `SSL_ERROR_SYSCALL`).
     * @details Internally calls `SSL_read()`.
     */
    int read(void *data, std::size_t size) noexcept;

    /**
     * @brief Write data to be encrypted and sent over the SSL/TLS socket.
     * @param data Pointer to the plaintext data to be encrypted and sent.
     * @param size Number of bytes to send from the `data` buffer.
     * @return Number of bytes successfully encrypted and written to the SSL/TLS layer.
     *         This can be less than `size` if the SSL/TLS layer cannot accept all data immediately.
     *         Returns a negative value on error.
     * @details Internally calls `SSL_write()`.
     */
    int write(const void *data, std::size_t size) noexcept;

    /**
     * @brief Get the underlying OpenSSL `SSL` handle.
     * @return Pointer to the `SSL` object, or `nullptr` if not initialized.
     * @note Allows direct access to the OpenSSL API for advanced configuration or inspection if needed.
     */
    [[nodiscard]] SSL *ssl_handle() const noexcept;

private:
    //    friend class ssl::listener; // If listener needs to call private methods for accept
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_SOCKET_H_
