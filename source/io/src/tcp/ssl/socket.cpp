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

#include <qb/io/tcp/ssl/socket.h>

namespace qb {
    namespace io {
        namespace tcp {
            namespace ssl {

                SSL_CTX *create_client_context(SSL_METHOD *method) {
                    return method ? SSL_CTX_new(method) : nullptr;
                }

                SSL_CTX *create_server_context(const SSL_METHOD *method, std::string const &cert_path,
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

                void socket::init(SSL *handle) {
                    _ssl_handle = handle;
                    SSL_set_fd(_ssl_handle, ident());
                    _connected = false;
                    SSL_set_accept_state(handle);
                }

                socket::socket()
                    : tcp::socket()
                    , _ssl_handle(nullptr)
                    , _connected(false) {
                }

                SSL *socket::ssl() const { return _ssl_handle; }

                int socket::handCheck() {
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
                        };
                    }
                    _connected = true;
                    return 1;
                }

                SocketStatus socket::connect(const ip &remoteAddress, unsigned short remotePort, int timeout) {
                    auto ret = tcp::socket::connect(remoteAddress, remotePort, timeout);
                    if (ret != SocketStatus::Done)
                        return ret;
                    _ssl_handle = SSL_new(SSL_CTX_new(SSLv23_client_method()));
                    if (!_ssl_handle) {
                        tcp::socket::disconnect();
                        return SocketStatus::Error;
                    }
                    SSL_set_fd(_ssl_handle, ident());
                    SSL_set_connect_state(_ssl_handle);
                    return handCheck() == 1 ? SocketStatus::Done : SocketStatus::Error;
                }

                void socket::disconnect() {
                    tcp::socket::disconnect();
                    if (_ssl_handle) {
                        SSL_shutdown(_ssl_handle);
                        SSL_free(_ssl_handle);
                        _ssl_handle = nullptr;
                        _connected = false;
                    }
                }

                int socket::read(void *data, std::size_t size) {
                    auto ret = handCheck();
                    if (ret == 1) {
                        ret = SSL_read(_ssl_handle, data, size);
                        return ret >= 0 ? ret : (SSL_get_error(_ssl_handle, ret) <= SSL_ERROR_WANT_WRITE ? 0 : -1);
                    }
                    return ret;
                }

                int socket::write(const void *data, std::size_t size) {
                    auto ret = handCheck();
                    if (ret == 1) {
                        ret = SSL_write(_ssl_handle, data, size);
                        return ret >= 0 ? ret : (SSL_get_error(_ssl_handle, ret) <= SSL_ERROR_WANT_WRITE ? 0 : -1);
                    }
                    return ret;
                }

            } // namsepace ssl
        } // namespace tcp
    } // namespace io
} // namespace qb