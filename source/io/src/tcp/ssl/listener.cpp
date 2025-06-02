/**
 * @file qb/io/src/tcp/ssl/listener.cpp
 * @brief Implementation of SSL/TLS listener functionality
 *
 * This file contains the implementation of secure listener operations using SSL/TLS
 * in the QB framework. It handles accepting incoming secure connections and establishing
 * SSL/TLS sessions with proper certificate handling and encryption setup.
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
 * @ingroup IO
 */

#include <qb/io/tcp/ssl/listener.h>

namespace qb::io::tcp::ssl {

void
bristou(SSL_CTX *ctx) {
    SSL_CTX_free(ctx);
}

listener::~listener() noexcept {}

listener::listener() noexcept
    : _ctx(nullptr, bristou) {}

void
listener::init(SSL_CTX *ctx) noexcept {
    _ctx.reset(ctx);
}

ssl::socket
listener::accept() const noexcept {
    auto sock = tcp::listener::accept();
    if (!sock.is_open())
        return {};
    const auto ctx = SSL_new(ssl_handle());
    SSL_set_fd(ctx, sock.native_handle());
    SSL_set_accept_state(ctx);
    return {ctx, sock};
}

int
listener::accept(ssl::socket &ssock) const noexcept {
    tcp::socket sock;
    auto        ret = tcp::listener::accept(sock);
    if (!ret) {
        const auto ctx = SSL_new(ssl_handle());
        SSL_set_accept_state(ctx);
        SSL_set_fd(ctx, static_cast<int>(sock.native_handle()));
        ssock = ssl::socket{ctx, sock};
    }
    return ret;
}

[[nodiscard]] SSL_CTX *
listener::ssl_handle() const noexcept {
    return _ctx.get();
}

bool listener::load_ca_certificates_for_client_auth(const std::string &ca_file_path) {
    if (!_ctx) return false;
    return qb::io::ssl::load_ca_certificates(_ctx.get(), ca_file_path);
}

bool listener::load_ca_directory_for_client_auth(const std::string &ca_dir_path) {
    if (!_ctx) return false;
    return qb::io::ssl::load_ca_directory(_ctx.get(), ca_dir_path);
}

bool listener::set_cipher_list(const std::string &ciphers) {
    if (!_ctx) return false;
    return qb::io::ssl::set_cipher_list(_ctx.get(), ciphers);
}

bool listener::set_ciphersuites_tls13(const std::string &ciphersuites) {
    if (!_ctx) return false;
    return qb::io::ssl::set_ciphersuites_tls13(_ctx.get(), ciphersuites);
}

bool listener::set_tls_protocol_versions(int min_version, int max_version) {
    if (!_ctx) return false;
    return qb::io::ssl::set_tls_protocol_versions(_ctx.get(), min_version, max_version);
}

bool listener::configure_mtls(const std::string &client_ca_file_path, int verification_mode) {
    if (!_ctx) return false;
    return qb::io::ssl::configure_mtls_server_context(_ctx.get(), client_ca_file_path, verification_mode);
}

bool listener::set_alpn_selection_callback(SSL_CTX_alpn_select_cb_func callback, void *arg) {
    if (!_ctx) return false;
    return qb::io::ssl::set_alpn_selection_callback_server(_ctx.get(), callback, arg);
}

bool listener::enable_session_caching(long cache_size) {
    if (!_ctx) return false;
    return qb::io::ssl::enable_server_session_caching(_ctx.get(), cache_size);
}

bool listener::set_custom_client_verify_callback(int (*callback)(int, X509_STORE_CTX *), int verification_mode) {
    if (!_ctx) return false;
    return qb::io::ssl::set_custom_verify_callback(_ctx.get(), callback, verification_mode);
}

bool listener::set_ocsp_stapling_responder_callback(int (*callback)(SSL *s, void *arg), void *arg) {
    if (!_ctx) return false;
    return qb::io::ssl::set_ocsp_stapling_responder_server(_ctx.get(), callback, arg);
}

bool listener::set_sni_selection_callback(int (*callback)(SSL *s, int *al, void *arg), void *arg) {
    if (!_ctx) return false;
    return qb::io::ssl::set_sni_hostname_selection_callback_server(_ctx.get(), callback, arg);
}

bool listener::set_keylog_callback(SSL_CTX_keylog_cb_func callback) {
    if (!_ctx) return false;
    return qb::io::ssl::set_keylog_callback(_ctx.get(), callback);
}

bool listener::configure_dh_parameters(const std::string& dh_param_file_path) {
    if (!_ctx) return false;
    return qb::io::ssl::configure_dh_parameters_server(_ctx.get(), dh_param_file_path);
}

bool listener::configure_ecdh_curves(const std::string& curve_names_list) {
    if (!_ctx) return false;
    return qb::io::ssl::configure_ecdh_curves_server(_ctx.get(), curve_names_list);
}

bool listener::enable_post_handshake_auth() {
    if (!_ctx) return false;
    return qb::io::ssl::enable_post_handshake_auth_server(_ctx.get());
}

    bool listener::set_supported_alpn_protocols(const std::vector<std::string>& protocols) {
    if (!_ctx || protocols.empty())
        return false;

    // Serialize protocols into length-prefixed wire format
    _alpn_wire.clear();
    for (const auto& proto : protocols) {
        if (proto.length() > 255) continue;
        _alpn_wire.push_back(static_cast<unsigned char>(proto.length()));
        _alpn_wire.insert(_alpn_wire.end(), proto.begin(), proto.end());
    }

    if (_alpn_wire.empty())
        return false;

    // Static callback wrapper
    SSL_CTX_set_alpn_select_cb(_ctx.get(),
        [](SSL*,
           const unsigned char** out,
           unsigned char* outlen,
           const unsigned char* in,
           unsigned int inlen,
           void* arg) -> int
        {
            auto* protos = static_cast<std::vector<unsigned char>*>(arg);
            int r = SSL_select_next_proto(
                const_cast<unsigned char**>(out), outlen,
                protos->data(), static_cast<unsigned int>(protos->size()),
                in, inlen);
            return (r == OPENSSL_NPN_NEGOTIATED) ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_NOACK;
        },
        &_alpn_wire
    );

    return true;
}

int listener::get_min_protocol_version() const {
    if (!_ctx) return 0;
    return static_cast<int>(SSL_CTX_get_min_proto_version(_ctx.get()));
}

int listener::get_max_protocol_version() const {
    if (!_ctx) return 0;
    return static_cast<int>(SSL_CTX_get_max_proto_version(_ctx.get()));
}

int listener::get_verify_mode() const {
    if (!_ctx) return -1; 
    return SSL_CTX_get_verify_mode(_ctx.get());
}

int listener::get_verify_depth() const {
    if (!_ctx) return -1;
    return SSL_CTX_get_verify_depth(_ctx.get());
}

long listener::get_session_cache_mode() const {
    if (!_ctx) return -1;
    return SSL_CTX_get_session_cache_mode(_ctx.get());
}

long listener::get_session_cache_size() const {
    if (!_ctx) return -1;
    return SSL_CTX_sess_get_cache_size(_ctx.get());
}

long listener::set_options(long options_to_set) {
    if (!_ctx) return SSL_CTX_get_options(_ctx.get());
    return SSL_CTX_set_options(_ctx.get(), options_to_set);
}

long listener::clear_options(long options_to_clear) {
    if (!_ctx) return SSL_CTX_get_options(_ctx.get());
    return SSL_CTX_clear_options(_ctx.get(), options_to_clear);
}

long listener::set_session_timeout(long seconds) {
    if (!_ctx) return 0;
    return SSL_CTX_set_timeout(_ctx.get(), seconds);
}

bool listener::set_info_callback(void (*callback)(const SSL *ssl, int type, int val)) {
    if (!_ctx) return false;
    SSL_CTX_set_info_callback(_ctx.get(), callback);
    return true;
}

bool listener::set_msg_callback(void (*callback)(int write_p, int version, int content_type, const void *buf, size_t len, SSL *ssl, void *arg), void *arg) {
    if (!_ctx) return false;
    SSL_CTX_set_msg_callback(_ctx.get(), callback);
    SSL_CTX_set_msg_callback_arg(_ctx.get(), arg);
    return true;
}

} // namespace qb::io::tcp::ssl
