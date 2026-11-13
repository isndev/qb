/**
 * @file qb/io/transport/stcp.h
 * @brief Secure TCP (SSL/TLS) stream transport for the QB IO library.
 *
 * This file provides a transport implementation for secure (SSL/TLS) TCP sockets,
 * extending the `qb::io::stream` class with `qb::io::tcp::ssl::socket` specific functionality.
 * It handles encrypted stream-based communication.
 * Requires OpenSSL.
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
 * @ingroup SSL
 */

#ifndef QB_IO_TRANSPORT_STCP_H
#define QB_IO_TRANSPORT_STCP_H
#include "../config.h"
#include "../stream.h"
#include "../tcp/ssl/socket.h"
namespace qb::io::transport {

/**
 * @class stcp
 * @ingroup Transport
 * @brief Secure TCP (SSL/TLS) transport providing encrypted stream communication.
 *
 * This class implements a transport layer for secure (SSL/TLS) TCP sockets by extending
 * the generic `qb::io::stream` class, specializing it with `qb::io::tcp::ssl::socket`
 * as the underlying I/O mechanism. A TLS record is decrypted whole: what a read leaves of one
 * stays decrypted in the SSL object, and the transport says how much (`held_input()`), so that the
 * io layer reads it although the socket announces nothing.
 */
class stcp : public stream<io::tcp::ssl::socket> {
public:
    /** @brief Indicates that this transport implementation is secure */
    static constexpr bool
    is_secure() noexcept {
        return true;
    }
    /**
     * @brief Read data from the secure TCP socket
     * @return Number of bytes read on success, error code on failure (-2, `ErrBufferLimitExceeded`: no room left)
     *
     * One read of the TLS socket, of at most the room left under `max_read_buffer_size()`: nothing is read past the
     * cap. A record is decrypted whole, so a read that takes less than the record leaves the rest decrypted in the
     * SSL object, where no readiness of the socket announces it: `held_input()` counts it, and the io layer
     * dispatches the component again for it. Nothing is lost.
     */
    [[nodiscard]] int
    read() noexcept {
        static constexpr const std::size_t bucket_read = QB_DEFAULT_READ_BUFFER_SIZE;

        if (_in_buffer.size() >= this->_max_read_buffer_size)
            return ErrBufferLimitExceeded; // no room left under the cap: what the session holds waits for it
        const std::size_t room      = this->_max_read_buffer_size - _in_buffer.size();
        const std::size_t read_size = room < bucket_read ? room : bucket_read; // never past the cap (Huly QB-86)

        const auto ret = _in.read(_in_buffer.allocate_back(read_size), read_size);
        if (ret >= 0)
            _in_buffer.free_back(read_size - static_cast<std::size_t>(ret));
        else
            _in_buffer.free_back(read_size);
        _last_read_filled = ret >= 0 && static_cast<std::size_t>(ret) == read_size; // only then may its record hold more
        return ret;
    }

    /**
     * @brief Decrypted octets the TLS session holds that `read()` has not moved to the input buffer (`SSL_pending()`).
     * @return Their count; 0 without a session.
     * @details A read with less room than its record leaves them there, unannounced by the descriptor: `async::io` and
     *          `async::input` dispatch a started component again for them (Huly QB-86). A transport of one's own may
     *          declare it too, counting only what its next `read()` with room takes; its async base must be able to name
     *          it -- public, or the base a friend as in `async::tcp::client` -- or it is taken for none, silently.
     */
    [[nodiscard]] std::size_t
    held_input() const noexcept {
        const SSL *const session = transport().ssl_handle();
        return session ? static_cast<std::size_t>(SSL_pending(session)) : 0u;
    }

    /**
     * @brief Whether the last `read()` of the socket filled the place it asked for.
     * @details Only such a read can leave octets of its record inside the session: one that took less took the end of
     *          its record. `async::io` and `async::input` test it after a read, before they ask `held_input()`, so a
     *          dispatch whose read emptied its record calls nothing of the TLS library for it. A refused read (-2)
     *          reads nothing and leaves it as it was. A transport of one's own that declares `held_input()` may
     *          declare it too; without it the bases ask `held_input()` after every read that took octets.
     */
    [[nodiscard]] bool
    last_read_filled() const noexcept {
        return _last_read_filled;
    }

private:
    bool _last_read_filled = false; ///< `read()` records it: its last read of the socket filled the place it asked for.
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_STCP_H
