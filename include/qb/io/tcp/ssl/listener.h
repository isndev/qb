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

#include "../listener.h"
#include "socket.h"

#ifndef QB_IO_TCP_SSL_LISTENER_H_
#    define QB_IO_TCP_SSL_LISTENER_H_

namespace qb::io::tcp::ssl {

/*!
 * @class listener tcp/listener.h qb/io/tcp/ssl/listener.h
 * @ingroup TCP
 */
class QB_API listener : public tcp::listener {
    std::unique_ptr<SSL_CTX, void(*)(SSL_CTX *)> _ctx;
public:
    ~listener() noexcept;
    listener() noexcept;
    listener(listener const &) = delete;
    listener(listener &&) = default;
    listener &operator=(listener &&) = default;

    void init(SSL_CTX *ctx) noexcept;
    ssl::socket accept() const noexcept;
    int accept(ssl::socket &socket) const noexcept;

    [[nodiscard]] SSL_CTX *ssl_handle() const noexcept;
};

} // namespace qb::io::tcp::ssl

#endif // QB_IO_TCP_SSL_LISTENER_H_
