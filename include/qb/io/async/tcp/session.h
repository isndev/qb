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

#ifndef QB_IO_ASYNC_TCP_SESSION_H
#define QB_IO_ASYNC_TCP_SESSION_H

#include "../io.h"
#include "../../protocol/tcp.h"

namespace qb {
    namespace io {
        namespace async {
            namespace tcp {

                template<typename _Derived,
                         template<typename _BaseProt> typename _Prot,
                         typename _Server = void>
                class session
                        : public io<_Derived, _Prot<protocol::tcp>> {
                    using base_t = io<_Derived, _Prot<protocol::tcp>>;
                protected:
                    _Server &_server;
                public:
                    constexpr static const bool has_server = true;

                    session(_Server &server)
                            : base_t(listener::current), _server(server) {}

                    inline _Server &server() {
                        return _server;
                    }


                    bool disconnected() const {
                        std::cout << "session disconnected" << std::endl;
                        return true;
                    }
                };

                template<typename _Derived,
                         template<typename _BaseProt> typename _Prot>
                class session<_Derived, _Prot, void>
                        : public io<_Derived, _Prot<protocol::tcp>> {
                    using base_t = io<_Derived, _Prot<protocol::tcp>>;
                public:

                    session() = default;

                    session(listener &handler)
                            : base_t(handler) {}

                    bool disconnected() const {
                        std::cout << "session disconnected" << std::endl;
                        return true;
                    }
                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_TCP_SESSION_H
