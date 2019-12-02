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

#include            <openssl/ssl.h>
#include            "../socket.h"

#ifndef             QB_IO_TCP_SSL_SOCKET_H_
# define            QB_IO_TCP_SSL_SOCKET_H_

namespace           qb {
    namespace       io {
        namespace   tcp {
            namespace ssl {

                SSL_CTX *create_client_context(SSL_METHOD *method);
                SSL_CTX *create_server_context(const SSL_METHOD * method, std::string const &cert_path, std::string const &key_path);

                class listener;

                /*!
                 * @class socket tcp/ssl/socket.h qb/io/tcp/ssl/socket.h
                 * @ingroup TCP
                 */
                class QB_API socket
                    : public tcp::socket {
                    SSL *_ssl_handle;
                    bool _connected;

                    void init(SSL *handle);

                public:
                    socket();
                    socket(socket const &rhs) = default;

                    SSL *ssl() const;
                    int handCheck();
                    SocketStatus connect(const ip &remoteAddress, unsigned short remotePort, int timeout = 0);
                    void disconnect();
                    int read(void *data, std::size_t size);
                    int write(const void *data, std::size_t size);

                private:
                    friend class ssl::listener;
                };

            } // namsepace ssl
        } // namespace tcp
    } // namespace io
} // namespace qb

#endif // QB_IO_TCP_SSL_SOCKET_H_
