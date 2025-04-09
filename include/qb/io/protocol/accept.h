/**
 * @file qb/io/protocol/accept.h
 * @brief Protocol for accepting connections in asynchronous I/O
 *
 * This file defines the accept protocol template class which is used
 * by acceptors to handle the process of accepting new connections in
 * an asynchronous manner.
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

#ifndef QB_IO_ASYNC_PROTOCOL_ACCEPT_H
#define QB_IO_ASYNC_PROTOCOL_ACCEPT_H

namespace qb::io::protocol {

/**
 * @class accept
 * @brief Protocol for handling connection acceptance
 *
 * This template class implements a protocol for handling the acceptance
 * of new connections. It extends the AProtocol base class and implements
 * its abstract methods for detecting and processing new connections.
 *
 * @tparam _IO_ The I/O type (typically an acceptor)
 * @tparam _Socket The socket type for accepted connections
 */
template <typename _IO_, typename _Socket>
class accept : public async::AProtocol<_IO_> {
public:
    using message = _Socket; /**< Type alias for the socket type */

    /**
     * @brief Default constructor (deleted)
     */
    accept() = delete;

    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object using this protocol
     */
    accept(_IO_ &io) noexcept
        : async::AProtocol<_IO_>(io) {}

    /**
     * @brief Check if a new connection is available
     *
     * Determines if a new connection has been accepted by checking
     * if the accepted socket is open.
     *
     * @return 1 if a new connection is available, 0 otherwise
     */
    std::size_t
    getMessageSize() noexcept final {
        return static_cast<size_t>(this->_io.getAccepted().is_open());
    }

    /**
     * @brief Process a new connection
     *
     * Calls the I/O object's handler for new connections with the
     * newly accepted socket.
     *
     * @param size Ignored parameter (required by interface)
     */
    void
    onMessage(std::size_t) noexcept final {
        this->_io.on(std::move(this->_io.getAccepted()));
    }

    /**
     * @brief Reset the protocol state
     *
     * This protocol doesn't maintain state, so this is a no-op.
     */
    void
    reset() noexcept final {}
};

} // namespace qb::io::protocol

#endif // QB_IO_ASYNC_PROTOCOL_ACCEPT_H
