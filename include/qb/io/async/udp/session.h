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

#ifndef QB_IO_ASYNC_UDP_SESSION_H
#define QB_IO_ASYNC_UDP_SESSION_H

#include "../io.h"

namespace qb {
    namespace io {
        namespace async {
            namespace udp {

                template<typename _Derived, typename _Prot, typename _Server = void>
                class session
                        : public prot::udp::identity
                        , public _Prot {
                    friend _Server;
                protected:
                    _Server &_server;
                public:
                    constexpr static const bool has_server = false;

                    session(_Server &server)
                            :  _server(server) {}

                    inline prot::udp::identity &ident() {
                        return static_cast<prot::udp::identity &>(*this);
                    }

                    inline _Server &server() {
                        return _server;
                    }

                    char *publish(const char *data, std::size_t size) {
                        return _server.publish(ident(), data, size);
                    }

                };

//                template<typename _Derived, typename _Prot>
//                class session<_Derived, _Prot, void>
//                        : public prot::udp::identity
//                        , public io<_Derived, _Prot> {
//                protected:
//                public:
//                    constexpr static const bool has_server = false;

//                    session() {
//                        static_cast<prot::udp::identity &>(*this) = { this->in(); }
//                    };

//                    char *publish(const char *data, std::size_t size) {
//                        return _server.publish(static_cast<prot::udp::identity &>(*this), data, size);
//                    }

//                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_UDP_SESSION_H
