/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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

#include <qb/io/tcp/ssl/listener.h>

namespace qb {
    namespace io {
        namespace tcp {
            namespace ssl {

                listener::listener() : _ctx(nullptr) {}

                listener::~listener() {
                    if (_ctx)
                        SSL_CTX_free(_ctx);
                }

                void listener::init(SSL_CTX *ctx) { _ctx = ctx; }

                SocketStatus listener::accept(ssl::socket &socket) {
                    if (tcp::listener::accept(socket) == SocketStatus::Done) {
                        socket.init(SSL_new(_ctx));
                        return SocketStatus::Done;
                    }
                    return SocketStatus::Error;
                }

            } // namespace ssl
        } // namespace tcp
    } // namespace io
} // namespace qb