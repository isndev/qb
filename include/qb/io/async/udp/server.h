/**
 * @file qb/io/async/udp/server.h
 * @brief Asynchronous UDP server implementation
 *
 * This file defines the server template class which implements an asynchronous
 * UDP server. It uses the io class for asynchronous operations and the UDP
 * transport for handling UDP communications.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#ifndef QB_IO_ASYNC_UDP_SERVER_H
#define QB_IO_ASYNC_UDP_SERVER_H

#include "../../transport/udp.h"
#include "../io.h"

namespace qb::io::async::udp {

/**
 * @class server
 * @brief Asynchronous UDP server implementation
 *
 * This template class implements a simple asynchronous UDP server.
 * It combines the io class for asynchronous operations with the UDP
 * transport for handling UDP communications. If the derived class
 * defines a Protocol type, it will be used for processing UDP messages.
 *
 * @tparam _Derived The derived class type (CRTP pattern)
 */
template <typename _Derived>
class server
    : public io<_Derived>
    , public transport::udp {
public:
    constexpr static const bool has_server = false; /**< Flag indicating server association (false for UDP servers) */

    /**
     * @brief Constructor
     *
     * Creates a new UDP server. If the derived class defines a Protocol type
     * that is not void, an instance of that protocol is created and attached
     * to the server.
     */
    server() {
        if constexpr (qb::has_type_Protocol<_Derived>) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(static_cast<_Derived &>(*this));
            }
        }
    }

    /**
     * @brief Destructor — stop the watcher before the transport closes its fd.
     *
     * `transport::udp` is a sibling base listed after `io<_Derived>` and is
     * destroyed first (closing the socket). Stopping the event watcher in this
     * destructor body (which runs before any base) keeps `qev_io_stop` off a
     * closed fd, avoiding libev per-fd bookkeeping corruption.
     */
    ~server() {
        this->_async_event.stop();
    }
};

// NOTE: A per-peer session-tracking UDP server (keyed by transport::udp::identity)
// was prototyped historically but never finished. It has been removed to keep the
// public surface lean. Users needing per-peer state can keep a
// qb::unordered_map<transport::udp::identity, ...> inside their own _Derived
// subclass and look it up in on(..., std::size_t size). See
// qb/QB_IO_PLAN.md finding 2.7 for background.

} // namespace qb::io::async::udp

#endif // QB_IO_ASYNC_UDP_SERVER_H
