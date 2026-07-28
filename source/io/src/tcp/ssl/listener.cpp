/**
 * @file qb/io/src/tcp/ssl/listener.cpp
 * @brief Implementation of SSL/TLS listener functionality
 *
 * This file contains the implementation of secure listener operations using SSL/TLS
 * in the QB framework. It handles accepting incoming secure connections and establishing
 * SSL/TLS sessions with proper certificate handling and encryption setup.
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
 * @ingroup IO
 */

#include <utility>
#include <qb/io/tcp/ssl/listener.h>

namespace qb::io::tcp::ssl {

listener::~listener() noexcept {}

listener::listener() noexcept {}

listener::listener(qb::io::ssl::Context ctx) noexcept
    : _ctx(std::move(ctx)) {}

void
listener::init(SSL_CTX *ctx) noexcept {
    // TRANSFER the caller's single reference into the value-semantic context (matches the historical
    // `reset(ctx)` ownership: the listener frees it on teardown; the caller must not).
    _ctx = qb::io::ssl::Context::adopt(ctx);
}

void
listener::init(qb::io::ssl::Context ctx) noexcept {
    _ctx = std::move(ctx);
}

ssl::socket
listener::accept() const noexcept {
    auto sock = tcp::listener::accept();
    if (!sock.is_open())
        return {};
    auto *ctx = SSL_new(ssl_handle());
    if (!ctx) {
        sock.close();
        return {};
    }
    if (!qb::io::ssl::attach_socket(ctx, sock.native_handle())) {
        SSL_free(ctx); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
        sock.close();
        return {};
    }
    SSL_set_accept_state(ctx);
    return {ctx, sock};
}

int
listener::accept(ssl::socket &ssock) const noexcept {
    tcp::socket sock;
    auto        ret = tcp::listener::accept(sock);
    if (!ret) {
        auto *ctx = SSL_new(ssl_handle());
        if (!ctx) {
            sock.close();
            return -1;
        }
        SSL_set_accept_state(ctx);
        if (!qb::io::ssl::attach_socket(ctx, sock.native_handle())) {
            SSL_free(ctx); // LCOV_EXCL_LINE GCOVR_EXCL_LINE
            sock.close();
            return -1;
        }
        ssock = ssl::socket{ctx, sock};
    }
    return ret;
}

[[nodiscard]] SSL_CTX *
listener::ssl_handle() const noexcept {
    return _ctx.native();
}

[[nodiscard]] const qb::io::ssl::Context &
listener::context() const noexcept {
    return _ctx;
}

bool
listener::load_ca_certificates_for_client_auth(const std::filesystem::path &ca_file_path) {
    if (!_ctx)
        return false;
    return qb::io::ssl::load_ca_certificates(_ctx.native(), ca_file_path);
}

bool
listener::load_ca_directory_for_client_auth(const std::filesystem::path &ca_dir_path) {
    if (!_ctx)
        return false;
    return qb::io::ssl::load_ca_directory(_ctx.native(), ca_dir_path);
}

bool
listener::set_cipher_list(const std::string &ciphers) {
    if (!_ctx)
        return false;
    return qb::io::ssl::set_cipher_list(_ctx.native(), ciphers);
}

bool
listener::set_ciphersuites_tls13(const std::string &ciphersuites) {
    if (!_ctx)
        return false;
    return qb::io::ssl::set_ciphersuites_tls13(_ctx.native(), ciphersuites);
}

bool
listener::set_tls_protocol_versions(int min_version, int max_version) {
    if (!_ctx)
        return false;
    return qb::io::ssl::set_tls_protocol_versions(_ctx.native(), min_version, max_version);
}

bool
listener::configure_mtls(const std::filesystem::path &client_ca_file_path, int verification_mode) {
    if (!_ctx)
        return false;
    return qb::io::ssl::configure_mtls_server_context(_ctx.native(), client_ca_file_path, verification_mode);
}

bool
listener::set_alpn_selection_callback(SSL_CTX_alpn_select_cb_func callback, void *arg) {
    if (!_ctx)
        return false;
    return qb::io::ssl::set_alpn_selection_callback_server(_ctx.native(), callback, arg);
}

bool
listener::enable_session_caching(long cache_size) {
    if (!_ctx)
        return false;
    return qb::io::ssl::enable_server_session_caching(_ctx.native(), cache_size);
}

bool
listener::set_custom_client_verify_callback(int (*callback)(int, X509_STORE_CTX *), int verification_mode) {
    if (!_ctx)
        return false;
    return qb::io::ssl::set_custom_verify_callback(_ctx.native(), callback, verification_mode);
}

bool
listener::set_ocsp_stapling_responder_callback(int (*callback)(SSL *s, void *arg), void *arg) {
    if (!_ctx)
        return false;
    return qb::io::ssl::set_ocsp_stapling_responder_server(_ctx.native(), callback, arg);
}

bool
listener::set_sni_selection_callback(int (*callback)(SSL *s, int *al, void *arg), void *arg) {
    if (!_ctx)
        return false;
    return qb::io::ssl::set_sni_hostname_selection_callback_server(_ctx.native(), callback, arg);
}

bool
listener::set_keylog_callback(SSL_CTX_keylog_cb_func callback) {
    if (!_ctx)
        return false;
    return qb::io::ssl::set_keylog_callback(_ctx.native(), callback);
}

bool
listener::configure_dh_parameters(const std::filesystem::path &dh_param_file_path) {
    if (!_ctx)
        return false;
    return qb::io::ssl::configure_dh_parameters_server(_ctx.native(), dh_param_file_path);
}

bool
listener::configure_ecdh_curves(const std::string &curve_names_list) {
    if (!_ctx)
        return false;
    return qb::io::ssl::configure_ecdh_curves_server(_ctx.native(), curve_names_list);
}

bool
listener::enable_post_handshake_auth() {
    if (!_ctx)
        return false;
    return qb::io::ssl::enable_post_handshake_auth_server(_ctx.native());
}

bool
listener::set_supported_alpn_protocols(const std::vector<std::string> &protocols) {
    if (!_ctx || protocols.empty())
        return false;

    // Serialize protocols into length-prefixed wire format. The buffer lives on
    // the heap (unique_ptr) so its address survives a listener move; that address
    // is what the alpn_select_cb below receives as `arg`.
    if (!_alpn_wire)
        _alpn_wire = std::make_unique<std::vector<unsigned char>>();
    auto &wire = *_alpn_wire;
    wire.clear();
    for (const auto &proto : protocols) {
        if (proto.length() > 255)
            continue;
        wire.push_back(static_cast<unsigned char>(proto.length()));
        wire.insert(wire.end(), proto.begin(), proto.end());
    }

    if (wire.empty())
        return false;

    // Static callback wrapper
    SSL_CTX_set_alpn_select_cb(
        _ctx.native(),
        [](SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen, void *arg) -> int {
            auto *protos = static_cast<std::vector<unsigned char> *>(arg);
            int r = SSL_select_next_proto(const_cast<unsigned char **>(out), outlen, protos->data(), static_cast<unsigned int>(protos->size()),
                                          in, inlen);
            return (r == OPENSSL_NPN_NEGOTIATED) ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_NOACK;
        },
        _alpn_wire.get());

    return true;
}

int
listener::get_min_protocol_version() const {
    if (!_ctx)
        return 0;
    return static_cast<int>(SSL_CTX_get_min_proto_version(_ctx.native()));
}

int
listener::get_max_protocol_version() const {
    if (!_ctx)
        return 0;
    return static_cast<int>(SSL_CTX_get_max_proto_version(_ctx.native()));
}

int
listener::get_verify_mode() const {
    if (!_ctx)
        return -1;
    return SSL_CTX_get_verify_mode(_ctx.native());
}

int
listener::get_verify_depth() const {
    if (!_ctx)
        return -1;
    return SSL_CTX_get_verify_depth(_ctx.native());
}

long
listener::get_session_cache_mode() const {
    if (!_ctx)
        return -1;
    return SSL_CTX_get_session_cache_mode(_ctx.native());
}

long
listener::get_session_cache_size() const {
    if (!_ctx)
        return -1;
    return SSL_CTX_sess_get_cache_size(_ctx.native());
}

// uint64_t, not long -- see the note on listener::set_options in the header: `long` is 32-bit on
// Windows (LLP64), which truncates every SSL_OP_* flag above bit 31 to 0 and sign-extends bit 31
// into all of bits 31..63.
uint64_t
listener::set_options(uint64_t options_to_set) {
    if (!_ctx)
        return 0;
    return SSL_CTX_set_options(_ctx.native(), options_to_set);
}

uint64_t
listener::clear_options(uint64_t options_to_clear) {
    if (!_ctx)
        return 0;
    return SSL_CTX_clear_options(_ctx.native(), options_to_clear);
}

long
listener::set_session_timeout(long seconds) {
    if (!_ctx)
        return 0;
    return SSL_CTX_set_timeout(_ctx.native(), seconds);
}

bool
listener::set_info_callback(void (*callback)(const SSL *ssl, int type, int val)) {
    if (!_ctx)
        return false;
    SSL_CTX_set_info_callback(_ctx.native(), callback);
    return true;
}

bool
listener::set_msg_callback(void (*callback)(int write_p, int version, int content_type, const void *buf, size_t len, SSL *ssl, void *arg),
                           void *arg) {
    if (!_ctx)
        return false;
    SSL_CTX_set_msg_callback(_ctx.native(), callback);
    SSL_CTX_set_msg_callback_arg(_ctx.native(), arg);
    return true;
}

} // namespace qb::io::tcp::ssl
