/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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

#include "../socket.h"
#include <openssl/ssl.h>

#ifndef QB_IO_TCP_SSL_SOCKET_H_
#    define QB_IO_TCP_SSL_SOCKET_H_

namespace qb::io::ssl {

struct Certificate {
    std::string subject;
    std::string issuer;
    int64_t version;
};

Certificate get_certificate(SSL *ssl);
SSL_CTX *create_client_context(const SSL_METHOD *method);
SSL_CTX *create_server_context(const SSL_METHOD *method, std::string const &cert_path,
                               std::string const &key_path);

} // namespace qb::io::ssl
namespace qb::io::tcp::ssl {

// class listener;

/*!
 * @class socket tcp/ssl/socket.h qb/io/tcp/ssl/socket.h
 * @ingroup TCP
 */
class QB_API socket : public tcp::socket {
    std::unique_ptr<SSL, void (*)(SSL *)> _ssl_handle;
    bool _connected;

    int handCheck() noexcept;
    int connect_in(int af, std::string const &host, uint16_t port) noexcept;
    int n_connect_in(int af, std::string const &host, uint16_t port) noexcept;

public:
    ~socket() noexcept;
    socket() noexcept;
    socket(SSL *ctx, tcp::socket &sock) noexcept;
    socket(socket const &rhs) = delete;
    socket(socket &&rhs) = default;
    socket &operator=(socket &&rhs) = default;

    void init(SSL *handle) noexcept;

    int connect(endpoint const &ep, std::string const &hostname = "") noexcept;
    int connect(uri const &u) noexcept;
    int connect_v4(std::string const &host, uint16_t port) noexcept;
    int connect_v6(std::string const &host, uint16_t port) noexcept;
    int connect_un(std::string const &path) noexcept;

    int n_connect(qb::io::endpoint const &ep, std::string const &hostname = "") noexcept;
    int connected() noexcept;
    int n_connect(uri const &u) noexcept;
    int n_connect_v4(std::string const &host, uint16_t port) noexcept;
    int n_connect_v6(std::string const &host, uint16_t port) noexcept;
    int n_connect_un(std::string const &path) noexcept;

    int disconnect() noexcept;

    int read(void *data, std::size_t size) noexcept;
    int write(const void *data, std::size_t size) noexcept;

    [[nodiscard]] SSL *ssl_handle() const noexcept;

private:
    //    friend class ssl::listener;
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_SOCKET_H_
