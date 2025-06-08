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
    mutable std::vector<unsigned char> _alpn_wire;
public:
    /** @brief Indicates that this socket implementation is secure */
    constexpr static bool is_secure() noexcept { return true; }
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

    /**
     * @brief Load CA certificates from a file for client peer verification (mTLS).
     * @param ca_file_path Path to the PEM-encoded CA certificate file.
     * @return true on success, false on failure or if context is not initialized.
     */
    bool load_ca_certificates_for_client_auth(const std::string &ca_file_path);

    /**
     * @brief Load CA certificates from a directory for client peer verification (mTLS).
     * @param ca_dir_path Path to the directory containing PEM-encoded CA certificates.
     * @return true on success, false on failure or if context is not initialized.
     */
    bool load_ca_directory_for_client_auth(const std::string &ca_dir_path);

    /**
     * @brief Set the preferred cipher suites for TLS 1.2 and earlier for this listener's context.
     * @param ciphers A string in OpenSSL cipher list format.
     * @return true on success, false on failure or if context is not initialized.
     */
    bool set_cipher_list(const std::string &ciphers);

    /**
     * @brief Set the preferred cipher suites for TLS 1.3 for this listener's context.
     * @param ciphersuites A string in OpenSSL TLS 1.3 ciphersuite format.
     * @return true on success, false on failure or if context is not initialized.
     */
    bool set_ciphersuites_tls13(const std::string &ciphersuites);

    /**
     * @brief Set the minimum and maximum TLS protocol versions for this listener's context.
     * @param min_version The minimum protocol version (e.g., TLS1_2_VERSION). 0 for default.
     * @param max_version The maximum protocol version (e.g., TLS1_3_VERSION). 0 for default.
     * @return true on success, false on failure or if context is not initialized.
     */
    bool set_tls_protocol_versions(int min_version, int max_version);

    /**
     * @brief Configure client certificate authentication (mTLS) for this listener.
     * @param client_ca_file_path Path to the CA cert file for verifying client certs. Can be empty.
     * @param verification_mode Verification mode (e.g., SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT).
     * @return true on success, false on failure or if context is not initialized.
     */
    bool configure_mtls(const std::string &client_ca_file_path, int verification_mode = SSL_VERIFY_PEER);

    /**
     * @brief Set the ALPN selection callback for this listener's context.
     * @param callback The OpenSSL ALPN selection callback function.
     * @param arg User-defined argument for the callback.
     * @return true on success, false on failure or if context is not initialized.
     */
    bool set_alpn_selection_callback(SSL_CTX_alpn_select_cb_func callback, void *arg);

    /**
     * @brief Enable and configure server-side SSL session caching for this listener.
     * @param cache_size Max number of sessions in cache. (OpenSSL default: SSL_SESSION_CACHE_MAX_SIZE_DEFAULT).
     * @return true on success, false on failure or if context is not initialized.
     */
    bool enable_session_caching(long cache_size = SSL_SESSION_CACHE_MAX_SIZE_DEFAULT);

    /**
     * @brief Set a custom callback for X.509 client certificate verification.
     * @param callback User-defined callback: `int callback(int preverify_ok, X509_STORE_CTX *x509_ctx)`.
     * @param verification_mode Verification mode (e.g., SSL_VERIFY_PEER).
     * @return true on success, false if context is not initialized.
     */
    bool set_custom_client_verify_callback(int (*callback)(int, X509_STORE_CTX *), int verification_mode);

    /**
     * @brief Set a callback for this listener's server to provide an OCSP response to be stapled.
     * @param callback Callback: `int (*cb)(SSL *, void *)`. Responsible for SSL_set_tlsext_status_ocsp_resp().
     * @param arg User-defined argument for the callback.
     * @return true on success, false if context is not initialized.
     */
    bool set_ocsp_stapling_responder_callback(int (*callback)(SSL *s, void *arg), void *arg);

    /**
     * @brief Set a callback for server-side SNI (Server Name Indication) handling.
     * @param callback Callback: `int (*cb)(SSL *s, int *al, void *arg)`. Can switch SSL_CTX.
     * @param arg User-defined argument for the callback.
     * @return true on success, false if context is not initialized.
     */
    bool set_sni_selection_callback(int (*callback)(SSL *s, int *al, void *arg), void *arg);

    /**
     * @brief Set the SSL/TLS key log callback function for debugging.
     * @param callback Keylog callback: `void (*cb)(const SSL *ssl, const char *line)`.
     * @return true on success, false if context is not initialized.
     */
    bool set_keylog_callback(SSL_CTX_keylog_cb_func callback);

    /**
     * @brief Configure Diffie-Hellman parameters for this listener's context.
     * @param dh_param_file_path Path to a PEM-encoded DH parameters file.
     * @return true on success, false on failure or if context is not initialized.
     */
    bool configure_dh_parameters(const std::string& dh_param_file_path);

    /**
     * @brief Configure preferred ECDH curves for this listener's context.
     * @param curve_names_list Colon-separated list of curve NIDs or names (e.g., "P-256:X25519").
     * @return true on success, false on failure or if context is not initialized.
     */
    bool configure_ecdh_curves(const std::string& curve_names_list);

    /**
     * @brief Enable server-side support for TLS 1.3 Post-Handshake Authentication.
     * @return true if PHA enabled or already enabled, false on error or if context not initialized.
     */
    bool enable_post_handshake_auth();

    /**
     * @brief Set the list of ALPN protocols supported by the server listener.
     * @details This list is used if no ALPN selection callback is registered via `set_alpn_selection_callback`.
     *          The server will automatically select a protocol from this list if it's also offered by the client.
     * @param protocols A vector of protocol strings (e.g., {"h2", "http/1.1"}).
     * @return true on success, false if context is not initialized or protocols are invalid.
     */
    bool set_supported_alpn_protocols(const std::vector<std::string>& protocols);

    // --- Getters for SSL_CTX properties ---

    /** @brief Gets the minimum configured TLS protocol version. @return Minimum version or 0 if not set/error. */
    int get_min_protocol_version() const;
    /** @brief Gets the maximum configured TLS protocol version. @return Maximum version or 0 if not set/error. */
    int get_max_protocol_version() const;
    /** @brief Gets the current peer verification mode. @return Verification flags or -1 if not set/error. */
    int get_verify_mode() const;
    /** @brief Gets the current peer certificate verification depth. @return Depth or -1 if not set/error. */
    int get_verify_depth() const;
    /** @brief Gets the SSL session cache mode. @return Cache mode flags or -1 if not set/error. */
    long get_session_cache_mode() const;
    /** @brief Gets the SSL session cache size. @return Cache size or -1 if not set/error. */
    long get_session_cache_size() const;

    // --- Other useful SSL_CTX configurations ---

    /**
     * @brief Set specific SSL options on the listener's context.
     * @param options_to_set A bitmask of SSL_OP_* flags to set.
     * @return The new options mask after setting, or current options if context is null.
     */
    long set_options(long options_to_set);

    /**
     * @brief Clear specific SSL options on the listener's context.
     * @param options_to_clear A bitmask of SSL_OP_* flags to clear.
     * @return The new options mask after clearing, or current options if context is null.
     */
    long clear_options(long options_to_clear);

    /**
     * @brief Set the session timeout for the listener's SSL_CTX.
     * @param seconds The timeout duration in seconds.
     * @return The previously set timeout value, or 0 if context is null.
     */
    long set_session_timeout(long seconds);

    /**
     * @brief Set an informational callback for the listener's SSL_CTX.
     * @details Used for debugging SSL/TLS operations.
     * @param callback The callback function: `void (*cb)(const SSL *ssl, int type, int val)`.
     * @return true if context exists, false otherwise.
     */
    bool set_info_callback(void (*callback)(const SSL *ssl, int type, int val));

    /**
     * @brief Set a message callback for the listener's SSL_CTX for detailed protocol tracing.
     * @param callback The callback function: `void (*cb)(int write_p, int version, int content_type, const void *buf, size_t len, SSL *ssl, void *arg)`.
     * @param arg User-defined argument for the callback.
     * @return true if context exists, false otherwise.
     */
    bool set_msg_callback(void (*callback)(int write_p, int version, int content_type, const void *buf, size_t len, SSL *ssl, void *arg), void *arg);
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_LISTENER_H_
