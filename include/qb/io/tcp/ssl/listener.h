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

#include            "../listener.h"
#include            "socket.h"

#ifndef             QB_IO_TCP_SSL_LISTENER_H_
# define            QB_IO_TCP_SSL_LISTENER_H_

namespace qb {
    namespace io {
        namespace tcp {
            namespace ssl {

                /*!
                 * @class listener tcp/listener.h qb/io/tcp/listener.h
                 * @ingroup TCP
                 */
                class QB_API listener
                    : public tcp::listener {
                    SSL_CTX *_ctx;
                public:
                    listener();
                    listener(listener const &) = delete;

                    ~listener();

                    void init(SSL_CTX *ctx);
                    SocketStatus accept(ssl::socket &socket);
                };

            } // namespace ssl
        } // namespace tcp
    } // namespace io
} // namespace qb

#endif // QB_IO_TCP_SSL_LISTENER_H_
