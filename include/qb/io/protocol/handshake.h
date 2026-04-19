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
    /**
     * @brief Cached result of the most recent `do_handshake()` probe in
     *        `getMessageSize()` so that `onMessage()` can consume it without
     *        re-invoking the SSL state machine.
     *
     * Decoupling the probe from the consumption preserves the
     * "`getMessageSize()` is a pure query" intuition expected by the
     * surrounding protocol dispatch loops.
     */
    std::size_t _pending_handshake_size = 0;
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
     * @brief Probe the transport to see whether the handshake has advanced.
     *
     * Historically this method drove the SSL handshake synchronously, coupling a
     * side-effecting operation to what protocol dispatch loops expect to be a pure
     * "how many bytes are available?" query. The call to `do_handshake()` is still
     * necessary here (the underlying transport needs this hook to progress its
     * state machine when fresh bytes arrive), but the result is now cached on the
     * protocol and consumed atomically by `onMessage()` so that a single logical
     * handshake step is never executed twice per buffer cycle.
     *
     * @return `> 0` if the handshake produced a usable event, `0` otherwise.
     */
    std::size_t
    getMessageSize() noexcept final {
        if (_handshake_done)
            return 0;
        const auto result = this->_io.transport().do_handshake();
        if (result <= 0) {
            _pending_handshake_size = 0;
            return 0;
        }
        _pending_handshake_size = static_cast<std::size_t>(result);
        return _pending_handshake_size;
    }

    /**
     * @brief Consume the pending handshake step and notify the I/O component.
     *
     * @param size The size reported by the most recent `getMessageSize()` call
     *             (ignored — we rely on `_pending_handshake_size` which was set
     *             by that same call, guaranteeing we never re-drive the SSL
     *             state machine from `onMessage()`).
     */
    void
    onMessage(std::size_t /*size*/) noexcept final {
        _handshake_done = true;
        _pending_handshake_size = 0;
        this->_io.on(message{});
    }

    /**
     * @brief Resets the protocol state, allowing a new handshake cycle.
     */
    void
    reset() noexcept final {
        _handshake_done = false;
        _pending_handshake_size = 0;
    }
};

} // namespace qb::io::protocol

#endif // QB_IO_ASYNC_PROTOCOL_HANDSHAKE_H
