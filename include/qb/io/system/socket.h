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

#include            "../helper.h"

#ifndef             QB_IO_SYS_SOCKET_H_
# define            QB_IO_SYS_SOCKET_H_

namespace           qb {
    namespace       io {
        namespace   sys {

            template<SocketType _Type>
            class QB_API socket {
            protected:
#ifdef _WIN32
                int           _fd;
#endif
                SocketHandler _handle;

                void init() {
                    if (!good()) {
                        SocketHandler handle;
                        if constexpr (_Type == SocketType::TCP)
                            handle = ::socket(PF_INET, SOCK_STREAM, 0);
                        else
                            handle = ::socket(PF_INET, SOCK_DGRAM, 0);
                        init(handle);
                    }
                }

                void init(SocketHandler handle) {
                    if (!good() && (handle != SOCKET_INVALID)) {
                        if constexpr (_Type == SocketType::TCP) {
                            // Disable the Nagle algorithm (i.e. removes buffering of TCP packets)
                            int yes = 1;
                            if (setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&yes),
                                           sizeof(yes)) == -1) {
                                std::cerr << "Failed to set socket option \"TCP_NODELAY\" ; "
                                          << "all your TCP packets will be buffered" << std::endl;
                            }
                        } else {
                            // Enable broadcast by default for UDP Sockets
                            int yes = 1;
                            if (setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char *>(&yes),
                                           sizeof(yes)) == -1)
                                std::cerr << "Failed to enable broadcast on UDP socket" << std::endl;
                        }
                        _handle = handle;
#ifdef _WIN32
                        _fd = _open_osfhandle(handle, 0);
#endif
                    } else {
                        throw std::runtime_error("Failed to init socket");
                    }
                }

            public:
                constexpr static const SocketType type = _Type;

                socket()
                        : _handle(SOCKET_INVALID)
                {}

                ~socket() {
                }

                SocketHandler ident() const {
                    return _handle;
                }

                int fd() const {
#ifdef _WIN32
                    return _fd;
#else
                    return _handle;
#endif
                }

                void set(SocketHandler new_handle) {
                    _handle = new_handle;
                }

                int setBlocking(bool new_state) const {
                    return helper::block(_handle, new_state);
                }

                bool isBlocking() const {
                    return helper::is_blocking(_handle);
                }

                bool setReceiveBufferSize(int size) const {
                    return setsockopt(_handle, SOL_SOCKET, SO_RCVBUF, &size, sizeof(int)) != -1;
                }

                bool setSendBufferSize(int size) const {
                    return setsockopt(_handle, SOL_SOCKET, SO_SNDBUF, &size, sizeof(int)) != -1;
                }

                bool good() const {
                    return _handle != SOCKET_INVALID;
                }

                void close() {
                    if (good()) {
                        if(helper::close(_handle))
                            _handle = SOCKET_INVALID;
                        else
                            std::cerr << "Failed to close socket" << std::endl;
                    }
                }
            };

        } // namespace sys
    } // namespace io
} // namespace qb

#endif // QB_IO_SYS_SOCKET_H_
