/**
 * @file qb/io/async/lingering_close.h
 * @brief Close a socket after its peer, owned by the thread's loop (the close of a connection THIS side ended).
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
 * @ingroup Async
 */

#ifndef QB_IO_ASYNC_LINGERING_CLOSE_H
#define QB_IO_ASYNC_LINGERING_CLOSE_H

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <qb/io/config.h> /* ::socket_type, the socket error codes */
#include <qb/system/time.h>

namespace qb::io::async::detail {

/// The bound of a close after the peer when the object carries none of its own (`input<>`, `output<>`): `io<>`'s default.
inline constexpr qb::duration default_linger = std::chrono::seconds(2);

/**
 * @brief Take `socket` over and close it after its peer, on this thread's loop.
 * @details For a connection THIS side ended. The sending side is shut down (the peer reads everything queued, then
 *          EOF), what the peer still sends is read and dropped, and the descriptor is closed on the first of: the
 *          peer's EOF, a socket error, `bound`, or `closing_socket_byte_budget()` octets dropped. The caller keeps
 *          nothing: its owner is free, or connects again, at once. What is readable is dropped in the call: a peer that
 *          has already closed costs no allocation and no watcher. Otherwise one loop-owned node (a read watcher and the
 *          bound's timer) holds the descriptor; at most `closing_sockets_cap()` of them exist on a loop -- one more
 *          closes the oldest at once -- and `listener::clear()` and the listener's destruction close what is left. The
 *          bound runs on the loop: a thread that stops running its loop keeps the descriptor until the loop runs again,
 *          is cleared or dies. Never throws: when the node cannot be made, or once the thread's loop is gone (a static
 *          object ending its connection after its thread's listener), the socket is closed in the call.
 *
 *          This is the close of `disconnect()`, the abort: the peer is read so that what this side had already
 *          sent is not reset away, and no further than the byte budget -- a peer that goes on sending is reset. To
 *          let a peer that is STILL SENDING read your last bytes (an upload answered early, a request refused), a
 *          server session calls `close_after_deliver()` instead: it waits for the delivery, then reads the peer
 *          for `linger()`, bounded in time alone.
 * @param socket A connected stream socket this call now owns; `invalid_socket` is a no-op.
 * @param bound  The longest wait; zero or less closes at once, with no shutdown first.
 */
void close_after_peer(::socket_type socket, qb::duration bound) noexcept;

/**
 * @brief Close every socket this thread's loop is closing after its peer, at once, oldest first.
 * @details What a thread calls when the system refuses it a descriptor (`out_of_descriptors`): the sockets the loop
 *          holds are a courtesy to their peers, and the process needs the descriptors more. Each is read once more
 *          and closed, as when its bound elapses. A thread whose loop holds none does nothing, and touches no loop.
 * @return How many were closed.
 */
std::size_t release_closing_sockets() noexcept;

/// Whether `error` (a socket call's last error) says the process, or the system, has no descriptor left to give.
[[nodiscard]] constexpr bool
out_of_descriptors(int error) noexcept {
#ifdef _WIN32
    return error == WSAEMFILE;
#else
    return error == EMFILE || error == ENFILE;
#endif
}

/// How many sockets this thread's loop is closing after their peer: diagnostics and tests.
[[nodiscard]] std::size_t closing_sockets() noexcept;

/// The most sockets one loop closes after their peer at the same time (the constant, for tests and diagnostics).
[[nodiscard]] std::size_t closing_sockets_cap() noexcept;

/// The most octets one closing socket reads and drops before it is closed at once (the constant, likewise).
[[nodiscard]] std::size_t closing_socket_byte_budget() noexcept;

} // namespace qb::io::async::detail

#endif // QB_IO_ASYNC_LINGERING_CLOSE_H
