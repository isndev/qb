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

#ifndef             QB_NETWORK_UDP_H_
# define            QB_NETWORK_UDP_H_

namespace           qb {
    namespace       network {
        namespace   udp {

            /*!
             * @class Socket udp.h qb/network/udp.h
             * @ingroup UDP
             */
            class QB_API Socket
                : public sys::Socket<SocketType::UDP> {
            public:
                constexpr static const std::size_t MaxDatagramSize = 65507;

                Socket();

                unsigned short getLocalPort() const;

                SocketStatus bind(unsigned short port, const ip &address = ip::Any);

                void unbind();

                SocketStatus
                send(const void *data, std::size_t size, const ip &remoteAddress, unsigned short remotePort) const;

                SocketStatus
                receive(void *data, std::size_t size, std::size_t &received, ip &remoteAddress, unsigned short &remotePort) const;

            };

        } // namespace udp
    } // namespace network
} // namespace qb

#endif // QB_NETWORK_UDP_H_
