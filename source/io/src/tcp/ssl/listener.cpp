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

} // namespace qb::io::tcp::ssl