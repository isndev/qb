/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include <qb/io/tcp/ssl/socket.h>

namespace qb::io::ssl {

Certificate
get_certificate(SSL *ssl) {
    X509 *cert;
    char *line;
    Certificate ret{};

    cert = SSL_get_peer_certificate(ssl); /* Get certificates (if available) */
    if (cert != nullptr) {
        line = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
        ret.subject = line;
        free(line);
        line = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
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

void
socket::init(SSL *handle) noexcept {
    _ssl_handle = handle;
    _connected = false;
}

socket::socket()
    : tcp::socket()
    , _ssl_handle(nullptr)
    , _connected(false) {}

SSL *
socket::ssl() const noexcept {
    return _ssl_handle;
}

int
socket::handCheck() noexcept {
    if (_connected)
        return 1;
    auto ret = SSL_do_handshake(_ssl_handle);
    if (ret != 1) {
        auto err = SSL_get_error(_ssl_handle, ret);
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

SocketStatus
socket::connect(const ip &remoteAddress, unsigned short remotePort, int timeout) {
    auto ret = tcp::socket::connect(remoteAddress, remotePort, timeout);
    if (ret >= SocketStatus::Partial)
        return ret;
    if (!_ssl_handle) {
        const auto ctx = SSL_CTX_new(SSLv23_client_method());
        _ssl_handle = SSL_new(ctx);
        if (!_ssl_handle) {
            SSL_CTX_free(ctx);
            tcp::socket::disconnect();
            return SocketStatus::Error;
        }
    }
    SSL_set_quiet_shutdown(_ssl_handle, 1);
    SSL_set_fd(_ssl_handle, ident());
    SSL_set_connect_state(_ssl_handle);
    if (ret == SocketStatus::NotReady)
        return ret;
    return handCheck() < 0 ? SocketStatus::Error : SocketStatus::Done;
}

SocketStatus
socket::connect(const uri &uri, int timeout) {
    auto ret = tcp::socket::connect(uri, timeout);
    if (ret >= SocketStatus::Partial)
        return ret;
    if (!_ssl_handle) {
        const auto ctx = SSL_CTX_new(SSLv23_client_method());
        _ssl_handle = SSL_new(ctx);
        if (!_ssl_handle) {
            SSL_CTX_free(ctx);
            tcp::socket::disconnect();
            return SocketStatus::Error;
        }
    }
    SSL_set_quiet_shutdown(_ssl_handle, 1);
    SSL_set_tlsext_host_name(_ssl_handle, std::string(uri.host()).c_str());
    SSL_set_fd(_ssl_handle, ident());
    SSL_set_connect_state(_ssl_handle);
    if (ret == SocketStatus::NotReady)
        return ret;
    return handCheck() < 0 ? SocketStatus::Error : SocketStatus::Done;
}

void
socket::disconnect() noexcept {
    tcp::socket::disconnect();
    if (_ssl_handle) {
        const auto ctx = SSL_get_SSL_CTX(_ssl_handle);
        const auto is_client = !SSL_is_server(_ssl_handle);
        SSL_shutdown(_ssl_handle);
        SSL_free(_ssl_handle);
        if (is_client)
            SSL_CTX_free(ctx);
        _ssl_handle = nullptr;
        _connected = false;
    }
}

int
socket::read(void *data, std::size_t size) noexcept {
    auto ret = handCheck();
    if (ret == 1) {
        ret = SSL_read(_ssl_handle, data, static_cast<int>(size));
        return ret >= 0
                   ? ret
                   : (SSL_get_error(_ssl_handle, ret) <= SSL_ERROR_WANT_WRITE ? 0 : -1);
    }
    return ret;
}

int
socket::write(const void *data, std::size_t size) noexcept {
    auto ret = handCheck();
    if (ret == 1) {
        ret = SSL_write(_ssl_handle, data, static_cast<int>(size));
        return ret >= 0
                   ? ret
                   : (SSL_get_error(_ssl_handle, ret) <= SSL_ERROR_WANT_WRITE ? 0 : -1);
    }
    return ret;
}

} // namespace qb::io::tcp::ssl