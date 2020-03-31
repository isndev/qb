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

#include            "../ip.h"
#include            "../system/socket.h"

#ifndef             QB_IO_TCP_SOCKET_H_
# define            QB_IO_TCP_SOCKET_H_

namespace           qb {
    namespace       io {
        namespace   tcp {

            /*!
             * @class socket tcp/socket.h qb/io/tcp/socket.h
             * @ingroup TCP
             */
            class QB_API socket
                : public sys::socket<SocketType::TCP> {
            public:
                socket();
                socket(socket const &rhs) = default;
//                socket(SocketHandler fd);

                ip getRemoteAddress() const;
                unsigned short getLocalPort() const;
                unsigned short getRemotePort() const;

                SocketStatus connect(const ip &remoteAddress, unsigned short remotePort, int timeout = 0);
                void disconnect();

                int read(void *data, std::size_t size) const;
                int write(const void *data, std::size_t size) const;

            private:
                friend class listener;
            };

        } // namespace tcp
    } // namespace io
} // namespace qb

#endif // QB_IO_TCP_SOCKET_H_
