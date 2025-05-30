/**
 * @file qb/io/protocol/handshake.h
 * @brief Protocol for handling the handshake of a new connection.
 *
 * This file defines the handshake protocol template class which is used
 * by sockets (e.g., `qb::io::tcp::ssl::socket`) to handle the process of
 * identifying a newly accepted connection from the underlying listener socket.
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
 * @ingroup IO
 */

#ifndef QB_IO_ASYNC_PROTOCOL_HANDSHAKE_H
#define QB_IO_ASYNC_PROTOCOL_HANDSHAKE_H
#include <functional>
#include "../async/protocol.h"
#include "../async/event/handshake.h"
namespace qb::io::protocol {

/**
 * @class handshake
 * @ingroup Protocol
 * @brief Protocol for handling the handshake of a new connection.
 *
 * This template class implements the `AProtocol` interface specifically for
 * handshake components (like `qb::io::tcp::ssl::socket`).
 */
template <typename _IO_>
class handshake : public async::AProtocol<_IO_> {
    bool _handshake_done = false;
public:
    /** 
     * @typedef message
     * @brief The type of message this protocol produces, which is the handshake event.
     */
    using message = qb::io::async::event::handshake; /**<  Type alias for the handshake event */

    /**
     * @brief Default constructor is deleted as an I/O component reference is required.
     */
    handshake() = delete;

    /**
     * @brief Constructor with I/O reference.
     * @param io Reference to the I/O component (e.g., an acceptor) that will use this protocol.
     */
    handshake(_IO_ &io) noexcept
        : async::AProtocol<_IO_>(io) {
            this->set_should_flush(false);
        }

    /**
     * @brief Checks if the handshake is done.
     *
     * This method determines if the handshake is done by checking
     * if the handshake is done.
     *
     * @return `1` if the handshake is done, `0` otherwise.
     *         The return type `std::size_t` is used to conform to the `AProtocol` interface,
     *         but effectively it's a boolean check.
     */
    std::size_t
    getMessageSize() noexcept final {
        if (_handshake_done)
            return 0;
        return static_cast<size_t>(this->_io.transport().do_handshake());
    }

    /**
     * @brief Triggers the handshake event.
     *
     * This method is called when `getMessageSize()` returns a non-zero value (i.e., an open socket was found).
     * It then calls the I/O component's `on()` handler, passing the handshake event.
     *
     * @param size Ignored parameter (required by `AProtocol` interface, but its value is not used here
     *             as `getMessageSize()` for this protocol effectively returns a boolean status).
     */
    void
    onMessage(std::size_t /*size*/) noexcept final { // size is unused
        _handshake_done = true;
        this->_io.on(message{});
    }

    /**
     * @brief Resets the protocol state.
     * @details This protocol is stateless regarding message parsing (it only checks handshake status),
     *          so this method is a no-op.
     */
    void
    reset() noexcept final {
        _handshake_done = false;
    }
};

} // namespace qb::io::protocol

#endif // QB_IO_ASYNC_PROTOCOL_HANDSHAKE_H
