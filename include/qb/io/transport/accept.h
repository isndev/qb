/**
 * @file qb/io/transport/accept.h
 * @brief TCP connection acceptance transport implementation for the QB IO library.
 *
 * This file provides a transport implementation for accepting incoming
 * TCP connections, wrapping the `qb::io::tcp::listener` functionality.
 * It is used by asynchronous acceptor components (e.g., `qb::io::async::tcp::acceptor`).
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
 * @ingroup TCP
 */

#ifndef QB_IO_TRANSPORT_ACCEPT_H_
#define QB_IO_TRANSPORT_ACCEPT_H_
#include "../tcp/listener.h"

namespace qb::io::transport {

/**
 * @class accept
 * @ingroup Transport
 * @brief Connection acceptance transport for TCP connections.
 *
 * This class implements a transport layer specific to accepting incoming TCP connections.
 * It wraps a `qb::io::tcp::listener` and is primarily used as the IO type (`_IO_`)
 * for stream classes (like `qb::io::istream`) when building acceptor components
 * (e.g., `qb::io::async::tcp::acceptor`).
 *
 * Its `read()` method attempts to accept a new connection via the listener, and
 * `getAccepted()` provides access to the newly accepted `qb::io::tcp::socket`.
 */
class accept {
    io::tcp::listener _io;          /**< TCP listener for accepting connections */
    io::tcp::socket   _accepted_io; /**< Socket for the accepted connection */

public:
    /** @brief Indicates that this transport implementation is not secure */
    constexpr bool is_secure() const noexcept { return false; }
    /** @brief Type of the underlying transport I/O */
    using transport_io_type = io::tcp::listener;

    /** @brief Type of the accepted socket */
    using socket_type = io::tcp::socket;

    /**
     * @brief Get the underlying TCP listener
     * @return Reference to the TCP listener
     */
    io::tcp::listener &
    transport() noexcept {
        return _io;
    }

    /**
     * @brief Accept a new connection
     * @return Native handle of the accepted socket, or -1 on failure
     *
     * Attempts to accept a new connection. If successful, returns the
     * native handle of the accepted socket which can be used for
     * further communication.
     */
    std::size_t
    read() noexcept {
        if (_io.accept(_accepted_io) == io::SocketStatus::Done)
            return static_cast<std::size_t>(_accepted_io.native_handle());
        return static_cast<std::size_t>(-1);
    }

    /**
     * @brief Release the accepted socket handle
     * @param Unused parameter
     *
     * Releases the handle of the last accepted socket to prevent
     * it from being closed when the socket object is destroyed.
     */
    void
    flush(std::size_t) noexcept {
        _accepted_io.release_handle();
    }

    /**
     * @brief End-of-file handling (no-op)
     *
     * This is a placeholder method with no implementation as
     * connection acceptance doesn't have an EOF concept.
     */
    void
    eof() const noexcept {}

    /**
     * @brief Close the listener
     *
     * Closes the underlying TCP listener, stopping the acceptance
     * of new connections.
     */
    void
    close() noexcept {
        _io.close();
    }

    /**
     * @brief Get the accepted socket
     * @return Reference to the last accepted socket
     *
     * Returns a reference to the socket object representing
     * the last accepted connection.
     */
    io::tcp::socket &
    getAccepted() {
        return _accepted_io;
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_ACCEPT_H_
