/**
 * @file qb/io/src/tcp/ssl/socket.cpp
 * @brief Implementation of SSL/TLS socket functionality
 *
 * This file contains the implementation of secure socket operations using SSL/TLS
 * in the QB framework. It provides encrypted communication channels over TCP,
 * including certificate validation, secure handshake operations, and encrypted
 * data transmission.
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

#include <qb/io/tcp/ssl/socket.h>

namespace qb::io::ssl {

Certificate
get_certificate(SSL *ssl) {
    X509       *cert;
    char       *line;
    Certificate ret{};

    cert = SSL_get_peer_certificate(ssl); /* Get certificates (if available) */
    if (cert != nullptr) {
        line        = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
        ret.subject = line;
        free(line);
        line       = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
        ret.issuer = line;
        free(line);
        ret.version = X509_get_version(cert);
        X509_free(cert);
    }

    return ret;
}

SSL_CTX *
create_client_context(const SSL_METHOD *method) {
    return method ? SSL_CTX_new(method) : nullptr;
}

SSL_CTX *
create_server_context(const SSL_METHOD *method, std::string const &cert_path,
                      std::string const &key_path) {
    SSL_CTX *ctx = nullptr;
    if (!method)
        goto error;
    ctx = SSL_CTX_new(method);
    if (!ctx ||
        SSL_CTX_use_certificate_file(ctx, cert_path.c_str(), SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_check_private_key(ctx) <= 0)
        goto error;

    return ctx;
error:
    if (ctx)
        SSL_CTX_free(ctx);
    return nullptr;
}

} // namespace qb::io::ssl

namespace qb::io::tcp::ssl {

socket::socket() noexcept
    : tcp::socket()
    , _ssl_handle(nullptr, SSL_free)
    , _connected(false) {}

socket::socket(SSL *ctx, tcp::socket &sock) noexcept
    : tcp::socket(std::move(sock))
    , _ssl_handle(ctx, SSL_free)
    , _connected(false) {}

socket::~socket() noexcept {
    if (_ssl_handle) {
        const auto handle    = ssl_handle();
        const auto ctx       = SSL_get_SSL_CTX(handle);
        const auto is_client = !SSL_is_server(handle);
        // SSL_shutdown(handle);
        // SSL_free(handle);
        if (is_client)
            SSL_CTX_free(ctx);
        _ssl_handle.reset(nullptr);
        _connected = false;
    }
}

void
socket::init(SSL *handle) noexcept {
    _ssl_handle.reset(handle);
    _connected = false;
}

int
socket::handCheck() noexcept {
    if (_connected)
        return 1;
    auto ret = SSL_do_handshake(ssl_handle());
    if (ret != 1) {
        auto err = SSL_get_error(ssl_handle(), ret);
        switch (err) {
            case SSL_ERROR_WANT_WRITE:
            case SSL_ERROR_WANT_READ:
                return 0;
            default:
                disconnect();
                return -1;
        }
    }
    _connected = true;
    return 1;
}

int
socket::connect_in(int af, std::string const &host, uint16_t port) noexcept {
    auto ret = -1;
    qb::io::socket::resolve_i(
        [&, this](const auto &ep) {
            if (ep.af() == af) {
                ret = connect(ep, host);
                return true;
            }
            return false;
        },
        host.c_str(), port, af, SOCK_STREAM);

    return ret;
}

int
socket::connect(endpoint const &ep, std::string const &hostname) noexcept {
    auto ret = tcp::socket::connect(ep);
    if (ret != 0 && !socket_no_error(qb::io::socket::get_last_errno()))
        return ret;
    if (!_ssl_handle) {
        const auto ctx = SSL_CTX_new(SSLv23_client_method());
        _ssl_handle.reset(SSL_new(ctx));
        if (!_ssl_handle) {
            SSL_CTX_free(ctx);
            tcp::socket::disconnect();
            return SocketStatus::Error;
        }
    }
    const auto h_ssl = ssl_handle();
    SSL_set_quiet_shutdown(h_ssl, 1);
    SSL_set_tlsext_host_name(h_ssl, hostname.c_str());
    SSL_set_connect_state(h_ssl);
    SSL_set_fd(h_ssl, static_cast<int>(FD_TO_SOCKET(native_handle())));
    if (ret != 0)
        return ret;
    return handCheck() < 0 ? -1 : 0;
}

int
socket::connect(uri const &u) noexcept {
    switch (u.af()) {
        case AF_INET:
        case AF_INET6:
            return connect_in(u.af(), std::string(u.host()), u.u_port());
        case AF_UNIX:
            const auto path = std::string(u.path()) + std::string(u.host());
            return connect_un(path);
    }
    return -1;
}

int
socket::connect_v4(std::string const &host, uint16_t port) noexcept {
    return connect_in(AF_INET, host, port);
}

int
socket::connect_v6(std::string const &host, uint16_t port) noexcept {
    return connect_in(AF_INET6, host, port);
}

int
socket::connect_un(std::string const &path) noexcept {
    return connect(endpoint().as_un(path.c_str()));
}

// NON BLOCKING
int
socket::n_connect_in(int af, std::string const &host, uint16_t port) noexcept {
    auto ret = -1;
    qb::io::socket::resolve_i(
        [&, this](const auto &ep) {
            if (ep.af() == af) {
                ret = n_connect(ep, host);
                return true;
            }
            return false;
        },
        host.c_str(), port, af, SOCK_STREAM);

    return ret;
}

int
socket::n_connect(endpoint const &ep, std::string const &hostname) noexcept {
    auto ret = tcp::socket::n_connect(ep);
    if (ret != 0 && !socket_no_error(qb::io::socket::get_last_errno()))
        return ret;
    if (!_ssl_handle) {
        const auto ctx = SSL_CTX_new(SSLv23_client_method());
        _ssl_handle.reset(SSL_new(ctx));
        if (!_ssl_handle) {
            SSL_CTX_free(ctx);
            tcp::socket::disconnect();
            return SocketStatus::Error;
        }
    }
    const auto h_ssl = ssl_handle();
    SSL_set_quiet_shutdown(h_ssl, 1);
    SSL_set_tlsext_host_name(h_ssl, hostname.c_str());
    SSL_set_connect_state(h_ssl);

    return ret;
}

// used for async
int
socket::connected() noexcept {
    if (!_ssl_handle)
        return -1;
    const auto h_ssl = ssl_handle();
    SSL_set_fd(h_ssl, static_cast<int>(FD_TO_SOCKET(native_handle())));
    return handCheck() < 0 ? -1 : 0;
}

int
socket::n_connect(uri const &u) noexcept {
    switch (u.af()) {
        case AF_INET:
        case AF_INET6:
            return n_connect_in(u.af(), std::string(u.host()), u.u_port());
        case AF_UNIX:
            const auto path = std::string(u.path()) + std::string(u.host());
            return n_connect_un(path);
    }
    return -1;
}

int
socket::n_connect_v4(std::string const &host, uint16_t port) noexcept {
    return n_connect_in(AF_INET, host, port);
}

int
socket::n_connect_v6(std::string const &host, uint16_t port) noexcept {
    return n_connect_in(AF_INET6, host, port);
}

int
socket::n_connect_un(std::string const &path) noexcept {
    return n_connect(endpoint().as_un(path.c_str()));
}

int
socket::disconnect() noexcept {
    _connected = false;
    //    SSL_shutdown(ssl_handle());
    return tcp::socket::disconnect();
}

int
socket::read(void *data, std::size_t size) noexcept {
    auto ret = handCheck();
    if (ret == 1) {
        ret = SSL_read(ssl_handle(), data, static_cast<int>(size));
        if (ret < 0) {
            auto err = SSL_get_error(ssl_handle(), ret);
            switch (err) {
                case SSL_ERROR_WANT_WRITE:
                case SSL_ERROR_WANT_READ:
                    return 0;
                default:
                    return -1;
            }
        }
    }
    return ret;
}

int
socket::write(const void *data, std::size_t size) noexcept {
    auto ret = handCheck();
    if (ret == 1) {
        ret = SSL_write(ssl_handle(), data, static_cast<int>(size));
        if (ret < 0) {
            auto err = SSL_get_error(ssl_handle(), ret);
            switch (err) {
                case SSL_ERROR_WANT_WRITE:
                case SSL_ERROR_WANT_READ:
                    return 0;
                default:
                    return -1;
            }
        }
    }
    return ret;
}

[[nodiscard]] SSL *
socket::ssl_handle() const noexcept {
    return _ssl_handle.get();
}

} // namespace qb::io::tcp::ssl