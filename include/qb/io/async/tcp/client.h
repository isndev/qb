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

namespace qb {
    namespace io {
        namespace async {
            namespace tcp {

                template<typename _Derived,
                        template<typename _BaseProt> typename _Prot,
                        typename _BaseProt,
                        typename _Server = void>
                class client
                        : public io<_Derived, _Prot<_BaseProt>> {
                    using base_t = io<_Derived, _Prot<_BaseProt>>;
                protected:
                    _Server &_server;
                public:
                    constexpr static const bool has_server = true;

                    client(_Server &server)
                            : _server(server) {}

                    inline _Server &server() {
                        return _server;
                    }

                };

                template<typename _Derived,
                        template<typename _BaseProt> typename _Prot,
                        typename _BaseProt>
                class client<_Derived, _Prot, _BaseProt, void>
                        : public io<_Derived, _Prot<_BaseProt>> {
                    using base_t = io<_Derived, _Prot<_BaseProt>>;
                public:

                    client() = default;

                };

            }
        }
    }
}

#endif //QB_IO_ASYNC_TCP_SESSION_H
