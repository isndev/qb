/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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

#ifndef QB_IO_ASYNC_UDP_CLIENT_H
#define QB_IO_ASYNC_UDP_CLIENT_H

#include "../../transport/udp.h"
#include "../io.h"

namespace qb::io::async::udp {

template <typename _Derived>
class client
    : public io<_Derived>
    , public transport::udp {


public:
    constexpr static const bool has_server = false;

    client() noexcept {
        if constexpr (has_member_Protocol<_Derived>::value) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(
                        static_cast<_Derived &>(*this));
            }
        }
    }
};

} // namespace qb::io::async::udp

#endif // QB_IO_ASYNC_UDP_CLIENT_H
