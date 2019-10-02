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

#include            "socket.h"

#ifndef             QB_IO_TCP_LISTENER_H_
# define            QB_IO_TCP_LISTENER_H_

namespace qb {
    namespace io {
        namespace tcp {

            /*!
             * @class listener tcp/listener.h qb/io/tcp/listener.h
             * @ingroup TCP
             */
            class QB_API listener
                    : public socket {
            public:
                listener();

                listener(listener const &) = delete;

                ~listener();

                unsigned short getLocalPort() const;

                SocketStatus listen(unsigned short port, const ip &address = ip::Any);

                SocketStatus accept(socket &socket);
            };

        } // namespace tcp
    } // namespace io
} // namespace qb

#endif // QB_IO_TCP_LISTENER_H_
