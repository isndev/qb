/**
 * @file qb/io/tcp/ssl/socket.h
 * @brief Implementation of SSL/TLS sockets for secure TCP communication in the QB IO library.
 *
 * This file provides the implementation of secure TCP sockets using OpenSSL
 * for encrypted communications, supporting both client and server-side SSL/TLS.
 * Requires OpenSSL to be linked and `QB_HAS_SSL` to be defined.
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
 * @ingroup SSL
 */

#ifndef QB_IO_TCP_SSL_SOCKET_H_
#define QB_IO_TCP_SSL_SOCKET_H_
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <openssl/ssl.h>
#include <qb/system/time.h>
#include "../socket.h"
#include "context.h"

namespace qb::io::ssl {

/**
 * @brief Attach a native socket handle to an OpenSSL `SSL` object.
 * @ingroup SSL
 * @param ssl The SSL connection object.
 * @param native_socket The platform-native socket handle.
 * @return true on success, false on failure.
 * @details Centralizes the Windows `SOCKET` to OpenSSL `int` conversion so the
 *          rest of qb-io does not have to scatter narrowing conversions.
 */
bool attach_socket(SSL *ssl, ::socket_type native_socket);

/**
 * @struct Certificate
 * @ingroup SSL
 * @brief Structure to hold essential SSL certificate information.
 * @details Contains common fields extracted from an X509 certificate.
 */
struct Certificate {
    std::string              subject;                   /**< The subject name of the certificate. */
    std::string              issuer;                    /**< The issuer name of the certificate. */
    int64_t                  version;                   /**< The version number of the certificate. */
    std::string              serial_number;             /**< The serial number of the certificate as a hex string. */
    int64_t                  not_before;                /**< Certificate validity start date (Unix timestamp). */
    int64_t                  not_after;                 /**< Certificate validity end date (Unix timestamp). */
    std::string              signature_algorithm;       /**< The signature algorithm used in the certificate. */
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
SSL_CTX *create_server_context(const SSL_METHOD *method, std::filesystem::path cert_path, std::filesystem::path key_path);

/**
 * @brief Load CA certificates from a file for peer verification.
 * @ingroup SSL
 * @param ctx The SSL_CTX to configure.
 * @param ca_file_path Path to the PEM-encoded CA certificate file.
 * @return true on success, false on failure.
 */
bool load_ca_certificates(SSL_CTX *ctx, const std::filesystem::path &ca_file_path);

/**
 * @brief Load CA certificates from a directory for peer verification.
 * @ingroup SSL
 * @param ctx The SSL_CTX to configure.
 * @param ca_dir_path Path to the directory containing PEM-encoded CA certificates.
 *                   The directory must be prepared with `c_rehash` or equivalent.
 * @return true on success, false on failure.
 */
bool load_ca_directory(SSL_CTX *ctx, const std::filesystem::path &ca_dir_path);

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
bool configure_mtls_server_context(SSL_CTX *ctx, const std::filesystem::path &client_ca_file_path, int verification_mode = SSL_VERIFY_PEER);

/**
 * @brief Configure a client SSL_CTX to use a specific client certificate and private key.
 * @ingroup SSL
 * @param ctx The client SSL_CTX to configure.
 * @param client_cert_path Path to the PEM-encoded client certificate file.
 * @param client_key_path Path to the PEM-encoded client private key file.
 * @return true on success, false on failure.
 */
bool configure_client_certificate(SSL_CTX *ctx, const std::filesystem::path &client_cert_path, const std::filesystem::path &client_key_path);

/**
 * @brief Set the ALPN protocols for a client SSL_CTX to offer during handshake.
 * @ingroup SSL
 * @param ctx The client SSL_CTX to configure.
 * @param protocols A vector of protocol strings (e.g., {"h2", "http/1.1"}).
 * @return true on success, false on failure.
 */
bool set_alpn_protos_client(SSL_CTX *ctx, const std::vector<std::string> &protocols);

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
bool configure_dh_parameters_server(SSL_CTX *ctx, const std::filesystem::path &dh_param_file_path);

/**
 * @brief Configure preferred ECDH curves for a server SSL_CTX.
 * @ingroup SSL
 * @param ctx The server SSL_CTX to configure.
 * @param curve_names_list A colon-separated list of curve NIDs or names (e.g., "P-256:X25519:P-384").
 *                         If empty, OpenSSL's default list may be used or auto-selection enabled if supported.
 * @return true on success, false on failure to set the curves.
 * @note Important for PFS with ECDHE cipher suites. Using `SSL_CTX_set_ecdh_auto(ctx, 1)` is also an option for some OpenSSL versions.
 */
bool configure_ecdh_curves_server(SSL_CTX *ctx, const std::string &curve_names_list);

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
    [[nodiscard]] bool
    is_valid() const {
        return _session_handle != nullptr;
    }
};

/**
 * @brief Frees an SSL_SESSION object held by qb::io::ssl::Session.
 * @ingroup SSL
 * @param session The qb::io::ssl::Session object to free. The internal handle will be nullified.
 */
void free_session(Session &session);

/**
 * @brief Enable server-side support for TLS 1.3 Post-Handshake Authentication (PHA).
 * @ingroup SSL
 * @param ctx The server SSL_CTX to configure.
 * @return true if PHA was successfully enabled (or was already enabled), false on error.
 * @note Requires OpenSSL 1.1.1 or later. The server application will also need to handle
 *       the actual authentication request, typically via an info callback or by checking
 *       SSL_get_post_handshake_auth().
 */
bool enable_post_handshake_auth_server(SSL_CTX *ctx);

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
    std::unique_ptr<SSL, void (*)(SSL *)> _ssl_handle;             /**< Unique pointer managing the OpenSSL `SSL` object. */
    bool                                  _connected;              /**< Flag indicating if the SSL handshake has successfully completed. */
    std::string                           _pending_sni_hostname;   /**< Desired SNI hostname to apply to the next/client SSL handle. */
    std::vector<std::string>              _pending_alpn_protocols; /**< Desired ALPN offers to apply before handshake starts. */
    std::unique_ptr<SSL_SESSION, void (*)(SSL_SESSION *)> _pending_session{
        nullptr, SSL_SESSION_free}; /**< Session to resume, held (own ref) until the SSL is minted at connect. */
    bool _pending_disable_resumption = false; /**< Deferred disable_session_resumption(): applied when the SSL is minted at connect. */
    bool _pending_request_ocsp       = false; /**< Deferred request_ocsp_stapling(true): applied when the SSL is minted at connect. */
    bool _verify_peer = true; /**< Secure-by-default: verify the server certificate chain + hostname on the auto-created client context. Cleared
                                 by set_insecure(). */
    qb::io::ssl::Context _ctx; /**< Optional value-semantic context to mint the client SSL from; empty => auto-create a secure client context. */

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

    int connect_in(int af, std::string const &host, uint16_t port, qb::duration wtimeout) noexcept;

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

    /**
     * @brief Applies any cached pre-handshake client settings to the current SSL handle.
     * @return true when all requested settings are applied successfully or nothing is pending.
     */
    bool apply_pending_client_settings() noexcept;

    /**
     * @brief Ensure a client `SSL` handle exists, minting it from the bound context or auto-creating one.
     * @return true if `_ssl_handle` is usable, false on allocation failure.
     * @details No-op if a handle already exists (e.g. supplied via `init(SSL*)`). If the socket was built
     *          from an `ssl::Context`, mints `SSL_new(_ctx.native())`; otherwise auto-creates a
     *          secure-by-default client context (TLS 1.2+, system trust store when verifying).
     * @private
     */
    bool ensure_client_ssl_() noexcept;

    /**
     * @brief Apply the per-connection client verification policy to the current SSL handle.
     * @param hostname SNI / hostname-verification target (empty for chain-only, e.g. AF_UNIX).
     * @details Auto-context path: this socket owns the policy (`SSL_VERIFY_PEER` + hostname, or none when
     *          insecure). Context path: honor an explicit `set_insecure()`, else RESPECT the context's own
     *          verify mode and install the per-connection hostname target only when the context already
     *          verifies the peer — never force a mode the user's context did not ask for.
     * @private
     */
    void apply_client_verification_(const std::string &hostname) noexcept;

    /**
     * @brief Set up the client-side TLS state on this socket without connecting TCP.
     * @param hostname Optional SNI hostname.
     * @return 0 on success, a non-zero error code on failure (the TCP socket is closed on failure).
     * @details Creates the client `SSL_CTX`/`SSL` (unless one was supplied via init()),
     *          applies SNI/ALPN + peer-verification policy, and calls `SSL_set_connect_state`.
     *          Shared by the blocking `connect()` overloads (via `finish_client_connect_()`),
     *          `n_connect()` (after its TCP connect) and `init_client()` (which runs it on an
     *          already-connected fd for STARTTLS-style opportunistic upgrades). Does NOT touch the fd
     *          until the first `handshake_status()`/`connected()` attaches it to the SSL object.
     * @private
     */
    int setup_client_ssl(std::string const &hostname) noexcept;

    /**
     * @brief Shared completion for the two blocking `connect()` overloads (they differ only in whether the
     *        TCP connect is time-bounded): the connect-error gate, `setup_client_ssl()`, the fd attach and
     *        the inline handshake — identical across both — live here once.
     * @param ret The return value of the underlying `tcp::socket::connect()`.
     * @param err `qb::io::socket::get_last_errno()` captured immediately after that connect.
     * @param hostname SNI / hostname-verification target.
     * @return 0 on success (or connect-in-progress), a non-zero error code / -1 on failure.
     * @private
     */
    int finish_client_connect_(int ret, int err, const std::string &hostname) noexcept;

public:
    /** @brief Indicates that this socket implementation is secure */
    constexpr static bool
    is_secure() noexcept {
        return true;
    }
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
     * @brief Construct a client socket bound to a value-semantic `qb::io::ssl::Context`.
     * @param ctx The TLS context the connection's `SSL` is minted from — e.g.
     *            `qb::io::ssl::Context::client().alpn({"h2"})`. Shared by reference count, so there is no
     *            manual `SSL_CTX` lifetime management. Per-connection settings (`sni()`, `resume()`,
     *            `insecure()`) still apply on top of it.
     */
    explicit socket(qb::io::ssl::Context ctx) noexcept;

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
     * @brief Move assignment operator.
     * @return Reference to this socket.
     * @details Releases this socket's current `SSL` before taking over @p rhs. Under the
     *          reference-counted `SSL_CTX` model (`SSL_free` drops the `SSL`'s own context reference;
     *          the socket never `SSL_CTX_free`s directly), a defaulted memberwise move would already
     *          be correct — the explicit release simply keeps the teardown ordering obvious for an
     *          in-place reconnect. A caller that passes its own context via `init()` owns that
     *          `SSL_CTX` and must free it (see `create_client_context()`).
     */
    socket &operator=(socket &&rhs) noexcept;

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
     * @brief Like `connect(endpoint, hostname)` but bounds the underlying TCP connect phase.
     */
    int connect(endpoint const &ep, std::string const &hostname, qb::duration wtimeout) noexcept;

    /**
     * @brief Establish a blocking SSL/TLS connection to a remote endpoint specified by a URI.
     * @param u The `qb::io::uri` of the remote server. The URI's host is used for SNI if not overridden.
     * @return 0 on success, non-zero error code on failure.
     */
    int connect(uri const &u) noexcept;

    /**
     * @brief URI connect with a bounded TCP connect (TLS handshake is not separately timed here).
     */
    int connect(uri const &u, qb::duration wtimeout) noexcept;

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
    int connect_un(std::filesystem::path const &path) noexcept;

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
     * @brief Prepare a client TLS handshake on an ALREADY-CONNECTED socket (no TCP connect).
     * @param hostname Optional SNI hostname (also used for hostname verification when verifying).
     * @return 0 on success, non-zero on failure.
     * @details This is the entry point for opportunistic / STARTTLS-style TLS upgrades: take a
     *          plaintext `tcp::socket` that has already been connected (and used for a plaintext
     *          negotiation, e.g. PostgreSQL's SSLRequest, SMTP/IMAP `STARTTLS`), move it into an
     *          `ssl::socket`, call `init_client()`, then drive `handshake_status()` from the event
     *          loop until it returns 1. Pair with `qb::io::async::tcp::starttls_connect()`. Honors the
     *          secure-by-default verification policy unless `set_insecure()` was called first.
     */
    int init_client(std::string const &hostname = "") noexcept;

    /**
     * @brief Progress the TLS handshake and report its precise state.
     * @return 1 when the TLS handshake is complete, 0 when OpenSSL needs more
     *         socket readiness (WANT_READ/WANT_WRITE), -1 on fatal error.
     */
    int handshake_status() noexcept;

    /**
     * @brief Whether the TLS handshake has completed successfully.
     */
    [[nodiscard]] bool handshake_complete() const noexcept;

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
    int n_connect_un(std::filesystem::path const &path) noexcept;

    /**
     * @brief Close the SSL/TLS connection and the underlying socket.
     * @return 0 on success, non-zero error code from the base-class `disconnect()`.
     * @details Does NOT send a TLS `close_notify`: the auto-created client context is put into
     *          quiet-shutdown mode at connect time (`SSL_set_quiet_shutdown`), so teardown is
     *          immediate and this call just clears the connected flag and closes the underlying TCP
     *          socket. The `SSL` object — and, through it, its reference to the reference-counted
     *          `SSL_CTX` — is released when the socket is destroyed or re-`init()`ed, not here.
     *          Higher-level framing (HTTP `Content-Length`/chunked, WebSocket close) delimits
     *          messages, so the absence of a `close_notify` is not a truncation ambiguity there.
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
     * @brief Compute the RFC 5929 `tls-server-end-point` channel-binding value.
     * @return The hash of the peer's (server's) certificate, or an empty vector if there
     *         is no peer certificate / no SSL handle.
     * @details The hash algorithm is the one in the certificate's signature algorithm,
     *          except MD5 and SHA-1 (and any unknown) are upgraded to SHA-256, per
     *          RFC 5929 §4.1. This is the channel-binding data a client feeds into a
     *          SCRAM-SHA-256-PLUS exchange (`p=tls-server-end-point`) to bind the
     *          authentication to this specific TLS channel.
     */
    std::vector<unsigned char> tls_server_end_point() const noexcept;

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
     * @details Call before the SSL handshake. Sets SSL_OP_NO_TICKET and
     *          SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION and clears any set session
     *          (`SSL_set_session(ssl, NULL)`). If the `SSL` handle is not minted yet (a socket built from an
     *          `ssl::Context`), the request is DEFERRED and applied when the handle is created at connect;
     *          any pending `resume()` session is dropped, since disabling resumption and resuming are
     *          mutually exclusive (the last of the two calls wins).
     * @return Always true — the request is applied to the existing handle or deferred to connect.
     */
    bool disable_session_resumption() noexcept;

    /**
     * @brief Request OCSP stapling from the server for this connection (client-side).
     * @details Call before the SSL handshake. If the `SSL` handle is not minted yet (a socket built from an
     *          `ssl::Context`), the request is DEFERRED and applied when the handle is created at connect.
     * @param enable Set to true to request OCSP stapling, false to not request (or clear previous request).
     * @return true when the request is applied or deferred; false only if enabling it on an existing handle failed.
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
     * @return true when the session is accepted — applied to the `SSL` handle if one already exists, otherwise
     *         DEFERRED (held with its own reference and applied when the handle is minted at connect, e.g. a
     *         socket built from an `ssl::Context`); false only if the session is invalid.
     * @note Setting a session does not guarantee resumption; the server must also agree.
     */
    bool set_session(qb::io::ssl::Session &session) noexcept;

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
     * @details Must be called before the SSL handshake. The value is cached on the socket and
     *          applied to the underlying `SSL` object as soon as it exists, so it is valid to
     *          call this before `connect()` / `n_connect()` create the `SSL` handle.
     *          This overrides any SNI set by connect methods if called after them but before handshake.
     * @param hostname The hostname to use for SNI.
     * @return true when the hostname is accepted — cached for the pending handshake and, if the `SSL` handle
     *         already exists, applied to it; false if @p hostname is empty (or applying to an existing handle failed).
     */
    bool set_sni_hostname(const std::string &hostname) noexcept;

    /**
     * @brief Set the ALPN protocols to offer for this specific SSL connection (client-side).
     * @details Must be called before the SSL handshake. The offered protocol list is cached on
     *          the socket and applied to the `SSL` object once it exists, so callers may set it
     *          before `connect()` / `n_connect()`. Overrides ALPN protocols set on the SSL_CTX
     *          for this connection.
     * @param protocols A vector of protocol strings (e.g., {"h2", "http/1.1"}).
     * @return true when the protocols are accepted — cached for the pending handshake and, if the `SSL` handle
     *         already exists, applied to it; false if @p protocols is empty (or applying to an existing handle failed).
     */
    bool set_alpn_protocols(const std::vector<std::string> &protocols) noexcept;

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

    /**
     * @brief Disable TLS peer verification for connections made on the
     *        SSL context that qb-io creates automatically.
     * @details qb-io is **secure by default**: when it builds the client
     *          `SSL_CTX` itself (the usual `connect()` / `n_connect()` /
     *          async-connector path), it loads the system trust store, enables
     *          `SSL_VERIFY_PEER`, and checks the server certificate against the
     *          target hostname (or IP). Call this **before** `connect()` /
     *          `n_connect()` to opt out — e.g. for self-signed certificates in
     *          tests, certificate pinning handled elsewhere, or trusted private
     *          channels.
     * @warning Disabling verification removes protection against
     *          man-in-the-middle attacks. Only use it when the channel is
     *          trusted through other means.
     * @note When you supply your own `SSL` handle via `init(SSL*)`, qb-io does
     *       not touch verification — your context's policy is used as-is.
     */
    void set_insecure() noexcept;

    /**
     * @brief Whether peer verification is enabled for the auto-created context.
     * @return `true` (default) unless `set_insecure()` was called.
     */
    [[nodiscard]] bool verify_peer() const noexcept;

    /**
     * @brief Access the value-semantic TLS context bound to this socket (falsy if none / auto-context).
     */
    [[nodiscard]] const qb::io::ssl::Context &context() const noexcept;

    /**
     * @brief Set this connection's SNI + hostname-verification target (chainable). Call before connecting.
     * @return `*this` for fluent chaining.
     */
    socket &sni(std::string hostname) noexcept;

    /**
     * @brief Offer this connection's ALPN protocols (chainable), overriding any set on the context.
     * @return `*this` for fluent chaining.
     */
    socket &alpn(std::vector<std::string> protocols) noexcept;

    /**
     * @brief Resume a previously saved TLS session on this connection (chainable). Call before connecting.
     * @return `*this` for fluent chaining.
     */
    socket &resume(qb::io::ssl::Session session) noexcept;

    /**
     * @brief Opt this connection out of peer verification (chainable). Equivalent to `set_insecure()`.
     * @return `*this` for fluent chaining.
     */
    socket &insecure() noexcept;

    inline int
    do_handshake() noexcept {
        return handCheck();
    }

private:
    //    friend class ssl::listener; // If listener needs to call private methods for accept

    /**
     * @brief Release the owned `SSL` (shared teardown for the destructor and move-assignment).
     * @details `SSL_free` (via the `unique_ptr` deleter) drops this `SSL`'s reference to its
     *          reference-counted `SSL_CTX`; the socket never calls `SSL_CTX_free` directly. The context
     *          is destroyed automatically once its last referencing `SSL` — and, for the `ssl::Context`
     *          path, the last `Context` copy — is gone, so a shared/caller-provided context is never
     *          torn out from under its other users.
     */
    void release_ssl_() noexcept;
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_SOCKET_H_
