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

#include < filesystem>
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
    std::string serial_number; /**< The serial number of the certificate as a hex string. */
    int64_t     not_before;    /**< Certificate validity start date (Unix timestamp). */
    int64_t     not_after;     /**< Certificate validity end date (Unix timestamp). */
    std::string signature_algorithm; /**< The signature algorithm used in the certificate. */
    std::vector<std::string> subject_alternative_names; /**< List of Subject Alternative Names (DNS, IP, etc.). */
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
SSL_CTX *create_server_context(const SSL_METHOD *method, std::filesystem::path cert_path,
                               std::filesystem::path key_path);

/**
 * @brief Load CA certificates from a file for peer verification.
 * @ingroup SSL
 * @param ctx The SSL_CTX to configure.
 * @param ca_file_path Path to the PEM-encoded CA certificate file.
 * @return true on success, false on failure.
 */
bool load_ca_certificates(SSL_CTX *ctx, const std::string &ca_file_path);

/**
 * @brief Load CA certificates from a directory for peer verification.
 * @ingroup SSL
 * @param ctx The SSL_CTX to configure.
 * @param ca_dir_path Path to the directory containing PEM-encoded CA certificates.
 *                   The directory must be prepared with `c_rehash` or equivalent.
 * @return true on success, false on failure.
 */
bool load_ca_directory(SSL_CTX *ctx, const std::string &ca_dir_path);

/**
 * @brief Set the preferred cipher suites for TLS 1.2 and earlier.
 * @ingroup SSL
 * @param ctx The SSL_CTX to configure.
 * @param ciphers A string in OpenSSL cipher list format (e.g., "HIGH:!aNULL:!MD5").
 * @return true on success, false on failure.
 */
bool set_cipher_list(SSL_CTX *ctx, const std::string &ciphers);

/**
 * @brief Set the preferred cipher suites for TLS 1.3.
 * @ingroup SSL
 * @param ctx The SSL_CTX to configure.
 * @param ciphersuites A string in OpenSSL TLS 1.3 ciphersuite format (e.g., "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256").
 * @return true on success, false on failure.
 */
bool set_ciphersuites_tls13(SSL_CTX *ctx, const std::string &ciphersuites);

/**
 * @brief Set the minimum and maximum TLS protocol versions.
 * @ingroup SSL
 * @param ctx The SSL_CTX to configure.
 * @param min_version The minimum protocol version (e.g., TLS1_2_VERSION). Use 0 for default.
 * @param max_version The maximum protocol version (e.g., TLS1_3_VERSION). Use 0 for default.
 * @return true on success, false on failure to set either version if specified.
 */
bool set_tls_protocol_versions(SSL_CTX *ctx, int min_version, int max_version);

/**
 * @brief Configure client certificate authentication (mTLS) for a server SSL_CTX.
 * @ingroup SSL
 * @param ctx The server SSL_CTX to configure.
 * @param client_ca_file_path Path to the PEM-encoded CA certificate file for verifying client certificates.
 *                              If empty, system default CAs might be used, or no specific client CA is set.
 * @param verification_mode The verification mode (e.g., SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT).
 *                          Defaults to SSL_VERIFY_PEER.
 * @return true on success, false on failure.
 */
bool configure_mtls_server_context(SSL_CTX *ctx, const std::string &client_ca_file_path, int verification_mode = SSL_VERIFY_PEER);

/**
 * @brief Configure a client SSL_CTX to use a specific client certificate and private key.
 * @ingroup SSL
 * @param ctx The client SSL_CTX to configure.
 * @param client_cert_path Path to the PEM-encoded client certificate file.
 * @param client_key_path Path to the PEM-encoded client private key file.
 * @return true on success, false on failure.
 */
bool configure_client_certificate(SSL_CTX *ctx, const std::string &client_cert_path, const std::string &client_key_path);

/**
 * @brief Set the ALPN protocols for a client SSL_CTX to offer during handshake.
 * @ingroup SSL
 * @param ctx The client SSL_CTX to configure.
 * @param protocols A vector of protocol strings (e.g., {"h2", "http/1.1"}).
 * @return true on success, false on failure.
 */
bool set_alpn_protos_client(SSL_CTX *ctx, const std::vector<std::string>& protocols);

/**
 * @brief Set the ALPN selection callback for a server SSL_CTX.
 * @ingroup SSL
 * @param ctx The server SSL_CTX to configure.
 * @param callback The OpenSSL ALPN selection callback function.
 * @param arg User-defined argument to be passed to the callback.
 * @return true on success (callback was set), false otherwise.
 * @note See OpenSSL documentation for SSL_CTX_set_alpn_select_cb for callback signature and behavior.
 */
bool set_alpn_selection_callback_server(SSL_CTX *ctx, SSL_CTX_alpn_select_cb_func callback, void *arg);

/**
 * @brief Enable and configure server-side SSL session caching.
 * @ingroup SSL
 * @param ctx The server SSL_CTX to configure.
 * @param cache_size The maximum number of sessions to store in the cache.
 *                   OpenSSL's default is SSL_SESSION_CACHE_MAX_SIZE_DEFAULT.
 *                   A size of 0 means unlimited (not recommended).
 * @return true if session caching was successfully configured, false otherwise.
 * @note This function enables the internal OpenSSL session cache (SSL_SESS_CACHE_SERVER).
 */
bool enable_server_session_caching(SSL_CTX *ctx, long cache_size);

/**
 * @brief Disable client-side SSL session caching for an SSL_CTX.
 * @ingroup SSL
 * @param ctx The client SSL_CTX to configure.
 * @return true if session caching was successfully disabled, false otherwise.
 * @note This prevents SSL objects created from this context from reusing sessions.
 */
bool disable_client_session_cache(SSL_CTX *ctx);

/**
 * @brief Set a custom callback for X.509 certificate verification.
 * @ingroup SSL
 * @param ctx The SSL_CTX to configure.
 * @param callback A user-defined callback function.
 *                 The callback signature is `int callback(int preverify_ok, X509_STORE_CTX *x509_ctx)`.
 *                 It should return 1 for success, 0 for failure.
 * @param verification_mode The verification mode to set (e.g., SSL_VERIFY_PEER).
 *                          This is passed to SSL_CTX_set_verify along with the callback.
 * @return true if the callback and mode were set, false on error (e.g., null context).
 */
bool set_custom_verify_callback(SSL_CTX *ctx, int (*callback)(int, X509_STORE_CTX *), int verification_mode);

/**
 * @brief Set a callback for the client to handle stapled OCSP responses from the server.
 * @ingroup SSL
 * @param ctx The client SSL_CTX to configure.
 * @param callback The callback function of type `int (*cb)(SSL *, void *)`.
 *                 Inside this callback, the user can retrieve the OCSP response using
 *                 `SSL_get_tlsext_status_ocsp_resp()`.
 * @param arg User-defined argument to be passed to the callback.
 * @return true if the callback was set, false otherwise.
 */
bool set_ocsp_stapling_client_callback(SSL_CTX *ctx, int (*callback)(SSL *s, void *arg), void *arg);

/**
 * @brief Set a callback for the server to provide an OCSP response to be stapled.
 * @ingroup SSL
 * @param ctx The server SSL_CTX to configure.
 * @param callback The callback function of type `int (*cb)(SSL *, void *)`.
 *                 This callback is responsible for setting the OCSP response using
 *                 `SSL_set_tlsext_status_ocsp_resp()`.
 * @param arg User-defined argument to be passed to the callback.
 * @return true if the callback was set, false otherwise.
 */
bool set_ocsp_stapling_responder_server(SSL_CTX *ctx, int (*callback)(SSL *s, void *arg), void *arg);

/**
 * @brief Set a callback for server-side SNI (Server Name Indication) handling.
 * @ingroup SSL
 * @param ctx The server SSL_CTX on which to set the callback. This context is used if the callback doesn't switch to another one.
 * @param callback The callback function `int (*cb)(SSL *s, int *al, void *arg)`.
 *                 This callback can inspect the server name and potentially switch to a different SSL_CTX.
 *                 It should return `SSL_TLSEXT_ERR_OK` on success.
 * @param arg User-defined argument to be passed to the callback.
 * @return true if the callback was set, false otherwise.
 */
bool set_sni_hostname_selection_callback_server(SSL_CTX *ctx, int (*callback)(SSL *s, int *al, void *arg), void *arg);

/**
 * @brief Set the SSL/TLS key log callback function.
 * @ingroup SSL
 * @param ctx The SSL_CTX to configure.
 * @param callback The keylog callback function `void (*cb)(const SSL *ssl, const char *line)`.
 *                 This function will be called with lines of text representing key material.
 * @return true if the callback was set, false on error (e.g., null context).
 */
bool set_keylog_callback(SSL_CTX *ctx, SSL_CTX_keylog_cb_func callback);

/**
 * @brief Configure Diffie-Hellman parameters for a server SSL_CTX.
 * @ingroup SSL
 * @param ctx The server SSL_CTX to configure.
 * @param dh_param_file_path Path to a PEM-encoded DH parameters file.
 * @return true on success, false on failure (e.g., file not found, invalid format).
 * @note Important for PFS with DHE cipher suites (TLS 1.2 and earlier).
 */
bool configure_dh_parameters_server(SSL_CTX* ctx, const std::string& dh_param_file_path);

/**
 * @brief Configure preferred ECDH curves for a server SSL_CTX.
 * @ingroup SSL
 * @param ctx The server SSL_CTX to configure.
 * @param curve_names_list A colon-separated list of curve NIDs or names (e.g., "P-256:X25519:P-384").
 *                         If empty, OpenSSL's default list may be used or auto-selection enabled if supported.
 * @return true on success, false on failure to set the curves.
 * @note Important for PFS with ECDHE cipher suites. Using `SSL_CTX_set_ecdh_auto(ctx, 1)` is also an option for some OpenSSL versions.
 */
bool configure_ecdh_curves_server(SSL_CTX* ctx, const std::string& curve_names_list);

/**
 * @struct Session
 * @ingroup SSL
 * @brief Opaque wrapper for an OpenSSL SSL_SESSION object.
 * @details Used for client-side session caching and resumption.
 *          Obtain via socket::get_session() and free via qb::io::ssl::free_session().
 */
struct Session {
    SSL_SESSION *_session_handle = nullptr;
    // Add any other metadata if needed, e.g., creation time, peer identifier

    /** @brief Checks if the session handle is valid (not null). */
    [[nodiscard]] bool is_valid() const { return _session_handle != nullptr; }
};

/**
 * @brief Frees an SSL_SESSION object held by qb::io::ssl::Session.
 * @ingroup SSL
 * @param session The qb::io::ssl::Session object to free. The internal handle will be nullified.
 */
void free_session(Session& session);

/**
 * @brief Enable server-side support for TLS 1.3 Post-Handshake Authentication (PHA).
 * @ingroup SSL
 * @param ctx The server SSL_CTX to configure.
 * @return true if PHA was successfully enabled (or was already enabled), false on error.
 * @note Requires OpenSSL 1.1.1 or later. The server application will also need to handle
 *       the actual authentication request, typically via an info callback or by checking
 *       SSL_get_post_handshake_auth().
 */
bool enable_post_handshake_auth_server(SSL_CTX* ctx);

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
    /** @brief Indicates that this socket implementation is secure */
    constexpr static bool is_secure() noexcept { return true; }
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
    void init(SSL *handle = nullptr) noexcept;

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

    /**
     * @brief Get details of the peer's certificate, if available.
     * @return A qb::io::ssl::Certificate structure. Empty if no certificate or handshake not complete.
     */
    qb::io::ssl::Certificate get_peer_certificate_details() const noexcept;

    /**
     * @brief Get the negotiated cipher suite string.
     * @return A string describing the cipher suite, or empty if not connected/negotiated.
     */
    std::string get_negotiated_cipher_suite() const noexcept;

    /**
     * @brief Get the negotiated TLS protocol version string.
     * @return A string like "TLSv1.2", "TLSv1.3", or empty if not connected/negotiated.
     */
    std::string get_negotiated_tls_version() const noexcept;

    /**
     * @brief Get the ALPN protocol selected by the peer (typically for clients) or by this endpoint (for servers).
     * @return The selected protocol string (e.g., "h2", "http/1.1"), or empty if ALPN was not used or no protocol was selected.
     */
    std::string get_alpn_selected_protocol() const noexcept;
    
    /**
     * @brief Get the last OpenSSL error string for the current SSL handle.
     * @return A string describing the last error on the OpenSSL error queue for this SSL connection.
     *         Returns an empty string if there is no error or the SSL handle is invalid.
     */
    std::string get_last_ssl_error_string() const noexcept;

    /**
     * @brief Disable SSL/TLS session resumption for this specific connection (client-side).
     * @details Must be called before the SSL handshake (e.g., before `connect` or `connected`).
     *          This function sets the SSL_OP_NO_TICKET and SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION options
     *          and attempts to clear any previously set session using SSL_set_session(ssl, NULL).
     * @return true if options were set successfully and an SSL handle exists, false otherwise.
     */
    bool disable_session_resumption() noexcept;

    /**
     * @brief Request OCSP stapling from the server for this connection (client-side).
     * @details Must be called before the SSL handshake.
     * @param enable Set to true to request OCSP stapling, false to not request (or clear previous request).
     * @return true if the request preference was set and an SSL handle exists, false otherwise.
     * @note The actual handling of the OCSP response needs to be done via a callback
     *       set on the SSL_CTX using `qb::io::ssl::set_ocsp_stapling_client_callback`.
     */
    bool request_ocsp_stapling(bool enable = true) noexcept;

    /**
     * @brief Get the peer's full certificate chain.
     * @return A vector of qb::io::ssl::Certificate structures, representing the chain.
     *         The first certificate in the vector is the peer's end-entity certificate,
     *         followed by intermediate CAs. The vector is empty if not connected,
     *         no chain is available, or an error occurs.
     */
    std::vector<qb::io::ssl::Certificate> get_peer_certificate_chain() const noexcept;

    /**
     * @brief Retrieves the current SSL session from this connection.
     * @details This session can be cached by the client and later used with `set_session()`
     *          on a new connection to the same server to attempt session resumption.
     *          The caller is responsible for freeing the returned session using `qb::io::ssl::free_session()`
     *          when it is no longer needed.
     * @return A qb::io::ssl::Session object. If no session is available or an error occurs,
     *         the returned Session object will be !is_valid().
     * @note The session should typically be retrieved after a successful handshake and before disconnect.
     */
    qb::io::ssl::Session get_session() const noexcept;

    /**
     * @brief Sets an SSL session to be used for resumption on this connection (client-side).
     * @details Must be called before the SSL handshake (e.g., before `connect()` or `connected()`).
     *          The provided session should be one previously obtained from `get_session()` from a connection
     *          to the same server and subsequently stored by the application.
     * @param session The qb::io::ssl::Session object to attempt to resume.
     * @return true if the session was successfully set on the SSL handle, false otherwise (e.g., no SSL handle, invalid session).
     * @note Setting a session does not guarantee resumption; the server must also agree.
     */
    bool set_session(qb::io::ssl::Session& session) noexcept;

    /**
     * @brief Request Post-Handshake Authentication from the server (client-side, TLS 1.3+).
     * @details This function initiates a post-handshake authentication request if the connection
     *          is TLS 1.3 or newer. The server must be configured to support PHA.
     *          The call is non-blocking; the application should monitor the connection for the server's response
     *          (e.g. CertificateRequest) through standard read/write mechanisms or SSL info callbacks.
     * @return true if the PHA request was successfully initiated, false on error (e.g., not TLS 1.3,
     *         SSL handle not valid, OpenSSL version too old, or PHA already in progress).
     */
    bool request_client_post_handshake_auth() noexcept;

    /**
     * @brief Set the Server Name Indication (SNI) hostname for this SSL connection.
     * @details Must be called before the SSL handshake (e.g., before `connect()` or `connected()`).
     *          This overrides any SNI set by connect methods if called after them but before handshake.
     * @param hostname The hostname to use for SNI.
     * @return true if SNI was set successfully and an SSL handle exists, false otherwise.
     */
    bool set_sni_hostname(const std::string& hostname) noexcept;

    /**
     * @brief Set the ALPN protocols to offer for this specific SSL connection (client-side).
     * @details Must be called before the SSL handshake. Overrides ALPN protocols set on the SSL_CTX for this connection.
     * @param protocols A vector of protocol strings (e.g., {"h2", "http/1.1"}).
     * @return true if ALPN protocols were set successfully and an SSL handle exists, false otherwise.
     */
    bool set_alpn_protocols(const std::vector<std::string>& protocols) noexcept;

    /**
     * @brief Set a custom X.509 certificate verification callback and mode for this SSL connection.
     * @details Must be called before the SSL handshake. Overrides the verification settings from the SSL_CTX.
     * @param callback A user-defined callback function. Signature: `int callback(int preverify_ok, X509_STORE_CTX *x509_ctx)`.
     *                 Return 1 for success, 0 for failure.
     * @param verification_mode The verification mode (e.g., SSL_VERIFY_PEER).
     * @return true if the callback and mode were set on the SSL handle, false otherwise.
     */
    bool set_verify_callback(int (*callback)(int, X509_STORE_CTX *), int verification_mode) noexcept;

    /**
     * @brief Set the maximum verification depth for the peer certificate chain for this SSL connection.
     * @details Must be called before the SSL handshake. Overrides the depth set on the SSL_CTX.
     * @param depth The maximum number of intermediate CA certificates that may be traversed.
     * @return true if the depth was set successfully and an SSL handle exists, false otherwise.
     */
    bool set_verify_depth(int depth) noexcept;

    inline int do_handshake() noexcept {
        return handCheck();
    }

private:
    //    friend class ssl::listener; // If listener needs to call private methods for accept
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_SOCKET_H_
