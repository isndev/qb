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

class listener;

/*!
 * @class socket tcp/ssl/socket.h qb/io/tcp/ssl/socket.h
 * @ingroup TCP
 */
class QB_API socket : public tcp::socket {
    SSL *_ssl_handle;
    bool _connected;

    int handCheck() noexcept;

public:
    socket();
    socket(socket const &rhs) = default;

    void init(SSL *handle) noexcept;
    SocketStatus connect(const ip &remoteAddress, unsigned short remotePort,
                         int timeout = 0);
    SocketStatus connect(const uri &remoteAddress, int timeout = 0);
    void disconnect() noexcept;

    int read(void *data, std::size_t size) noexcept;
    int write(const void *data, std::size_t size) noexcept;
    //int ssl_pending() noexcept;

    [[nodiscard]] SSL *ssl() const noexcept;

private:
    friend class ssl::listener;
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_SOCKET_H_
