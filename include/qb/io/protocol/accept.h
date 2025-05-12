/**
 * @file qb/io/protocol/accept.h
 * @brief Protocol for accepting new connections in asynchronous I/O.
 *
 * This file defines the accept protocol template class which is used
 * by acceptors (e.g., `qb::io::async::tcp::acceptor`) to handle the process of
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

#ifndef QB_IO_ASYNC_PROTOCOL_ACCEPT_H
#define QB_IO_ASYNC_PROTOCOL_ACCEPT_H

namespace qb::io::protocol {

/**
 * @class accept
 * @ingroup Protocol
 * @brief Protocol for handling the acceptance of new network connections.
 *
 * This template class implements the `AProtocol` interface specifically for
 * connection-accepting components (like `qb::io::async::tcp::acceptor`).
 * Its primary role is to detect when a new connection has been successfully
 * accepted by the underlying listener socket and to deliver the new connection's
 * socket (as `_Socket` type) to the I/O component.
 *
 * @tparam _IO_ The I/O component type that uses this protocol (typically an acceptor class).
 *              It must provide a `getAccepted()` method returning a reference to the newly accepted socket.
 * @tparam _Socket The type of socket that represents the newly accepted connection (e.g., `qb::io::tcp::socket`).
 */
template <typename _IO_, typename _Socket>
class accept : public async::AProtocol<_IO_> {
public:
    /** 
     * @typedef message
     * @brief The type of message this protocol produces, which is the accepted socket itself.
     */
    using message = _Socket; /**< Type alias for the socket type */

    /**
     * @brief Default constructor is deleted as an I/O component reference is required.
     */
    accept() = delete;

    /**
     * @brief Constructor with I/O reference.
     * @param io Reference to the I/O component (e.g., an acceptor) that will use this protocol.
     */
    accept(_IO_ &io) noexcept
        : async::AProtocol<_IO_>(io) {}

    /**
     * @brief Checks if a new connection is available to be processed.
     *
     * This method determines if a new connection has been accepted by checking
     * if the socket obtained from `this->_io.getAccepted()` is currently open.
     *
     * @return `1` if a new, open connection is available, `0` otherwise.
     *         The return type `std::size_t` is used to conform to the `AProtocol` interface,
     *         but effectively it's a boolean check.
     */
    std::size_t
    getMessageSize() noexcept final {
        return static_cast<size_t>(this->_io.getAccepted().is_open());
    }

    /**
     * @brief Processes a newly accepted connection.
     *
     * This method is called when `getMessageSize()` returns a non-zero value (i.e., an open socket was found).
     * It then calls the I/O component's `on()` handler, passing the newly accepted socket
     * (obtained via `this->_io.getAccepted()`) by moving it.
     *
     * @param size Ignored parameter (required by `AProtocol` interface, but its value is not used here
     *             as `getMessageSize()` for this protocol effectively returns a boolean status).
     */
    void
    onMessage(std::size_t /*size*/) noexcept final { // size is unused
        this->_io.on(std::move(this->_io.getAccepted()));
    }

    /**
     * @brief Resets the protocol state.
     * @details This protocol is stateless regarding message parsing (it only checks socket status),
     *          so this method is a no-op.
     */
    void
    reset() noexcept final {}
};

} // namespace qb::io::protocol

#endif // QB_IO_ASYNC_PROTOCOL_ACCEPT_H
