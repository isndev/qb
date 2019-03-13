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

#include            "ip.h"
#include            "sys.h"

#ifndef             QB_NETWORK_TCP_H_
# define            QB_NETWORK_TCP_H_

namespace           qb {
    namespace       network {
        namespace   tcp {

            /*!
             * @class Socket tcp.h qb/network/tcp.h
             * @ingroup TCP
             */
            class QB_API Socket
                : public sys::Socket<SocketType::TCP> {
            public:
                Socket();
                Socket(Socket const &rhs) = default;
                Socket(SocketHandler fd);

                ip getRemoteAddress() const;
                unsigned short getLocalPort() const;
                unsigned short getRemotePort() const;

                SocketStatus connect(const ip &remoteAddress, unsigned short remotePort, int timeout = 0);
                void disconnect();

                SocketStatus send(const void *data, std::size_t size) const;
                SocketStatus send(const void *data, std::size_t size, std::size_t &sent) const;
                SocketStatus sendall(const void *data, std::size_t size, std::size_t &sent) const;
                SocketStatus receive(void *data, std::size_t size, std::size_t &received) const;

            private:
                friend class Listener;
            };

            /*!
             * @class Listener tcp.h qb/network/tcp.h
             * @ingroup TCP
             */
            class QB_API Listener
                    : public Socket {
            public:
                Listener();
                Listener(Listener const &) = delete;
                ~Listener();

                unsigned short getLocalPort() const;

                SocketStatus listen(unsigned short port, const ip &address = ip::Any);
                SocketStatus accept(Socket &socket);
            };

        } // namespace tcp
    } // namespace network
} // namespace qb

#endif // QB_NETWORK_TCP_H_
