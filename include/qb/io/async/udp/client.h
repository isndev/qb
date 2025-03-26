/**
 * @file qb/io/async/udp/client.h
 * @brief Asynchronous UDP client implementation
 *
 * This file defines the client template class which implements an asynchronous
 * UDP client. It uses the io class for asynchronous operations and the UDP
 * transport for handling UDP communications.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup UDP
 */

#ifndef QB_IO_ASYNC_UDP_CLIENT_H
#define QB_IO_ASYNC_UDP_CLIENT_H

#include "../../transport/udp.h"
#include "../io.h"

namespace qb::io::async::udp {

/**
 * @class client
 * @brief Asynchronous UDP client implementation
 *
 * This template class implements an asynchronous UDP client.
 * It combines the io class for asynchronous operations with the UDP
 * transport for handling UDP communications. If the derived class
 * defines a Protocol type, it will be used for processing UDP messages.
 *
 * @tparam _Derived The derived class type (CRTP pattern)
 */
template <typename _Derived>
class client
    : public io<_Derived>
    , public transport::udp {

public:
    constexpr static const bool has_server =
        false; /**< Flag indicating server association (false for UDP clients) */

    /**
     * @brief Constructor
     *
     * Creates a new UDP client. If the derived class defines a Protocol type
     * that is not void, an instance of that protocol is created and attached
     * to the client.
     */
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
