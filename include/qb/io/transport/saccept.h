/**
 * @file qb/io/transport/saccept.h
 * @brief Secure (SSL/TLS) TCP connection acceptance transport for the QB IO library.
 *
 * This file provides a transport implementation for accepting incoming
 * secure (SSL/TLS) TCP connections, wrapping the `qb::io::tcp::ssl::listener` functionality.
 * It is used by asynchronous secure acceptor components.
 * Requires OpenSSL.
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
 * @ingroup SSL
 */

#ifndef QB_IO_TRANSPORT_SACCEPT_H_
#define QB_IO_TRANSPORT_SACCEPT_H_
#include "../tcp/ssl/listener.h"

namespace qb::io::transport {

/**
 * @class saccept
 * @ingroup Transport
 * @brief Secure connection acceptance transport for SSL/TLS TCP connections.
 *
 * This class implements a transport layer for accepting incoming secure (SSL/TLS)
 * TCP connections. It wraps a `qb::io::tcp::ssl::listener` and is used similarly
 * to `qb::io::transport::accept`, but for encrypted connections.
 *
 * Its `read()` method attempts to accept a new secure connection, performing the SSL handshake,
 * and `getAccepted()` provides access to the newly established `qb::io::tcp::ssl::socket`.
 */
class saccept {
    io::tcp::ssl::listener _io; /**< SSL TCP listener for accepting secure connections. */
    io::tcp::ssl::socket   _accepted_io; /**< SSL socket for the most recently accepted secure connection. */

public:
    /** @brief Indicates that this transport implementation is secure */
    static constexpr bool is_secure() noexcept { return true; }
    /** @brief Type of the underlying transport I/O */
    using transport_io_type = io::tcp::ssl::listener;

    /** @brief Type of the accepted socket */
    using socket_type = io::tcp::ssl::socket;

    /**
     * @brief Get the underlying SSL TCP listener
     * @return Reference to the SSL TCP listener
     */
    io::tcp::ssl::listener &
    transport() noexcept {
        return _io;
    }

    /**
     * @brief Accept a new secure connection
     * @return Native handle of the accepted socket, or -1 on failure
     *
     * Attempts to accept a new secure connection. If successful, returns the
     * native handle of the accepted socket which can be used for
     * further secure communication.
     *
     * @note **Transient accept errors.** Mirrors `transport::accept::read()`:
     *       a peer-aborted pending connection (`ECONNABORTED` / `EPROTO`) is
     *       remapped to `EWOULDBLOCK` so the acceptor retries instead of
     *       disposing the whole listening server.
     */
    std::size_t
    read() noexcept {
        const auto ret = _io.accept(_accepted_io);
        if (ret == io::SocketStatus::Done)
            return static_cast<std::size_t>(_accepted_io.native_handle());
        if (ret == ECONNABORTED
#ifdef EPROTO
            || ret == EPROTO
#endif
        )
            qb::io::socket::set_last_errno(EWOULDBLOCK);
        return static_cast<std::size_t>(-1);
    }

    /**
     * @brief Release the native handle of the accepted SSL socket.
     * @param Unused parameter (required by the stream/protocol contract).
     *
     * This mirrors `transport::accept::flush()`: at this point
     * `protocol::accept::onMessage()` has already `std::move()`d the
     * accepted `ssl::socket` into the user session, so `_accepted_io` is in a
     * moved-from (empty) state. We simply release the native handle to make
     * that fact explicit rather than reassigning a fresh `ssl::socket()` — the
     * previous approach destroyed a default-constructed temporary on the hot
     * accept path and obscured the moved-from semantics.
     *
     * The `SSL*` owned by the previously accepted socket has already been
     * transferred by the move; no additional work is required here.
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
     * Closes the underlying SSL TCP listener, stopping the acceptance
     * of new secure connections.
     */
    void
    close() noexcept {
        _io.close();
    }

    /**
     * @brief Get the accepted secure socket
     * @return Reference to the last accepted secure socket
     *
     * Returns a reference to the SSL socket object representing
     * the last accepted secure connection.
     */
    io::tcp::ssl::socket &
    getAccepted() noexcept {
        return _accepted_io;
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_SACCEPT_H_
