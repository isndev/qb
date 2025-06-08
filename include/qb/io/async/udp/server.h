/**
 * @file qb/io/async/udp/server.h
 * @brief Asynchronous UDP server implementation
 *
 * This file defines the server template class which implements an asynchronous
 * UDP server. It uses the io class for asynchronous operations and the UDP
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
    constexpr static const bool has_server =
        false; /**< Flag indicating server association (false for UDP servers) */

    /**
     * @brief Constructor
     *
     * Creates a new UDP server. If the derived class defines a Protocol type
     * that is not void, an instance of that protocol is created and attached
     * to the server.
     */
    server() noexcept {
        if constexpr (has_member_Protocol<_Derived>::value) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(
                    static_cast<_Derived &>(*this));
            }
        }
    }
};

// Note: The following code is commented out in the original file but preserved
// for reference. It represents an alternative implementation of a UDP server
// that tracks client sessions.

///**
// * @class server
// * @brief Alternative UDP server implementation with session tracking
// *
// * This template class implements a more complex UDP server that tracks
// * client sessions based on their endpoint identity.
// *
// * @tparam _Derived The derived class type (CRTP pattern)
// * @tparam _Session The session class type for handling client communications
// */
// template <typename _Derived, typename _Session>
// class server : public io<server<_Derived, _Session>> {
// public:
//    using base_t = io<server<_Derived, _Session>>;
//    using session_map_t = qb::unordered_map<transport::udp::identity, _Session,
//                                            transport::udp::identity::hasher>;
//
// private:
//    session_map_t _sessions{};
//
// public:
//    using IOSession = _Session;
//    server() = default;
//
//    session_map_t &
//    sessions() {
//        return _sessions;
//    }
//
//    void
//    on(transport::udp::message message, std::size_t size) {
//        auto it = _sessions.find(message.ident);
//        if (it == _sessions.end()) {
//            it = _sessions.emplace(message.ident, static_cast<_Derived
//            &>(*this)).first; it->second.ident() = message.ident;
//        }
//        //                            return; // drop the message
//
//        memcpy(it->second.buffer().allocate_back(size), message.data, size);
//        auto ret = 0;
//        while ((ret = it->second.getMessageSize()) > 0) {
//            it->second.on(it->second.getMessage(ret), ret);
//            it->second.flush(ret);
//        }
//    }
//
//    void
//    stream(char const *message, std::size_t size) {
//        for (auto &session : sessions())
//            session.second.publish(message, size);
//    }
//
//    bool
//    disconnected() const {
//        throw std::runtime_error("Server had been disconnected");
//        return true;
//    }
//
//    void
//    disconnected(transport::udp::identity ident) {
//        _sessions.erase(ident);
//    }
//};

} // namespace qb::io::async::udp

#endif // QB_IO_ASYNC_UDP_SERVER_H
