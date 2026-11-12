/**
 * @file qb/io/async/lingering_close.cpp
 * @brief The loop-owned closer of a socket this side ended: `detail::close_after_peer`.
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

#include <cstddef>
#include <new>

#include <qb/io/async/event/all.h>
#include <qb/io/async/lingering_close.h>
#include <qb/io/async/listener.h>
#include <qb/io/system/sys__socket.h>
#include <qb/utility/branch_hints.h>

namespace qb::io::async::detail {

namespace {

/**
 * @brief How many sockets one loop closes after their peer at the same time.
 * @details Each holds a descriptor nobody else owns for up to its bound, so without a cap a thread that ends
 *          connections faster than their peers close them holds close rate x bound of them, and the process fails
 *          with `EMFILE` in an unrelated `socket()`, `accept()` or `open()` (measured: a limit of 256, a peer that
 *          never closes, 600 closes in a row -- the 250th connect fails). One more than the cap closes the OLDEST at
 *          once: it has had the longest time to be read, and it is what curl's shutdown list and nginx's reusable
 *          queue give up first. 64 per loop: a quarter of the smallest descriptor limit a supported system gives a
 *          process by default (256, macOS), and 32 closes a second kept waiting for a peer that never closes, with the
 *          default bound, before the first is given up. The cap bounds what the loop holds; it does not see the
 *          process's limit: `release_closing_sockets()` is what answers `EMFILE`.
 */
constexpr std::size_t sockets_cap = 64;

/**
 * @brief How many octets one closing socket reads and drops before it is closed at once.
 * @details The bound in time alone lets one peer that never stops writing keep a loop thread reading for the whole
 *          bound: gigabytes over loopback. The budget is what a peer may have had on its way when it reads the end of
 *          the stream -- the two socket buffers between the two ends, 10 MiB with a Linux kernel's default limits
 *          (`tcp_rmem` 6 MiB, `tcp_wmem` 4 MiB) -- so that a peer that stops when it reads EOF is always read to its
 *          own end, and only one that goes on producing is reset, as a connection the io's own size guards end is.
 *          A smaller budget resets a peer that had already stopped writing (measured: at 1 MiB a peer that fills the
 *          pipe and then only reads is cut).
 */
constexpr std::size_t byte_budget = std::size_t{16} << 20;

class closing_socket;

/**
 * @brief This thread's closing sockets, oldest first, and how many.
 * @details Constant-initialised and trivially destructible on purpose: the nodes are destroyed by the thread's
 *          listener -- its `clear()`, its own thread-local destructor -- and these three words must be readable then,
 *          whichever thread-local the thread ends first.
 */
struct closing_list {
    closing_socket *oldest = nullptr;
    closing_socket *newest = nullptr;
    std::size_t     count  = 0;
};
thread_local constinit closing_list closing;

/// What a pass over a closing socket found.
enum class drained { open, ended };

/**
 * @brief Read and drop what `socket` holds, at most `reads` chunks.
 * @param dropped Counts the octets dropped, for the byte budget.
 * @return `ended` on the peer's EOF or a socket error, `open` otherwise (nothing more to read now, or `reads` spent).
 */
drained
drop_input(::socket_type socket, int reads, std::size_t &dropped) noexcept {
    char chunk[16 * 1024];
    while (reads-- > 0) {
        const int n = qb::io::socket::recv(socket, chunk, static_cast<int>(sizeof(chunk)));
        if (n > 0) {
            dropped += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0)
            return drained::ended; // the peer's FIN: the connection is over in both directions
        return qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()) ? drained::open : drained::ended;
    }
    return drained::open; // more may be readable: the watcher reports it again (level-triggered)
}

/**
 * @brief A socket this thread's loop closes after its peer: a read watcher, the bound's timer, the descriptor.
 * @details Loop-owned and self-deleting, like `async::callback`'s `Timeout`: it ends on the peer's EOF, a socket
 *          error, the bound or the byte budget; the cap ends the oldest; and `listener::clear()` destroys it through
 *          the owner hook when the loop goes first. Its handlers run no user code, so nothing re-enters it.
 */
class closing_socket {
    event::io      *_io    = nullptr;
    event::timer   *_timer = nullptr;
    closing_socket *_older = nullptr;
    closing_socket *_newer = nullptr;
    ::socket_type   _socket;
    std::size_t     _dropped;
    bool            _read_at_end = true; ///< False once the byte budget ended it: that peer is closed with its input unread.

public:
    /**
     * @param socket  The descriptor, already shut down for sending and non-blocking.
     * @param bound   The longest wait, positive.
     * @param dropped What the caller's own first pass dropped: it counts towards the budget.
     * @throws std::bad_alloc when a watcher's record cannot be allocated: nothing is left registered, and the caller
     *         still owns `socket` (the destructor does not run).
     */
    closing_socket(::socket_type socket, qb::duration bound, std::size_t dropped)
        : _socket(socket)
        , _dropped(dropped) {
        _io = &listener::current.registerEvent<event::io>(*this);
        try {
            _timer = &listener::current.registerEvent<event::timer>(*this);
        } catch (...) {
            listener::current.unregisterEvent(_io->_interface);
            throw;
        }
        // From here nothing throws.
        _io->_interface->set_owner(this, [](void *self) noexcept { delete static_cast<closing_socket *>(self); });
        _older                                     = closing.newest;
        (_older ? _older->_newer : closing.oldest) = this;
        closing.newest                             = this;
        ++closing.count;
        // The native handle, unchanged: on Windows a SOCKET selects the handle overload of ev::io::start; an `int`
        // would select the descriptor one, and the watcher would die at once with EV_ERROR.
        _io->start(_socket, EV_READ);
        _timer->start(qb::detail::to_ev_seconds(bound));
    }

    closing_socket(closing_socket const &)            = delete;
    closing_socket &operator=(closing_socket const &) = delete;

    ~closing_socket() {
        // The watchers first: libev must not watch a descriptor closed under it.
        _io->stop();
        _timer->stop();
        listener::current.unregisterEvent(_io->_interface);
        listener::current.unregisterEvent(_timer->_interface);
        (_older ? _older->_newer : closing.oldest) = _newer;
        (_newer ? _newer->_older : closing.newest) = _older;
        --closing.count;
        // Whatever ended it -- the bound, the cap, a descriptor the process ran out of, the loop cleared or destroyed
        // with the close still pending -- what is readable now is dropped first: a close with input unread is a reset,
        // which drops what is still in OUR send queue and, on Windows, what the peer has received and not read yet.
        // Not after the byte budget: that peer is the one to reset.
        if (_read_at_end)
            static_cast<void>(drop_input(_socket, 8, _dropped));
        ::QB_CLOSESOCKET(_socket);
    }

    /// The oldest closing socket of this thread's loop is closed at once: the cap, or a descriptor the process needs.
    static void
    close_oldest() noexcept {
        delete closing.oldest;
    }

    void
    on(event::io const &event) noexcept {
        if ((event._revents & EV_ERROR) || drop_input(_socket, 4, _dropped) == drained::ended) {
            delete this;
        } else if (_dropped > byte_budget) {
            _read_at_end = false;
            delete this; // a peer that streams into a connection this side ended is cut here, not at the bound
        }
    }

    void
    on(event::timer const &) noexcept {
        delete this; // the bound: a peer that neither closed nor stopped sending is cut here
    }
};

} // namespace

QB_NOINLINE QB_COLD void
close_after_peer(::socket_type socket, qb::duration bound) noexcept {
    if (socket == qb::io::invalid_socket)
        return;
    // Closed at once, nothing registered on the loop: a bound of zero is the caller's "at once".
    if (bound <= qb::duration::zero()) {
        ::QB_CLOSESOCKET(socket);
        return;
    }
    // The FIN leaves after what is queued; a socket that refuses it has no connection left to end gracefully.
    if (::shutdown(socket, QB_SD_SEND) != 0) {
        ::QB_CLOSESOCKET(socket);
        return;
    }
    static_cast<void>(qb::io::socket::set_nonblocking(socket, true)); // a transport never started may still block
    std::size_t dropped = 0;
    if (drop_input(socket, 8, dropped) == drained::ended) {
        ::QB_CLOSESOCKET(socket); // the peer had closed already: nothing to wait for, nothing allocated
        return;
    }
    if (closing.count >= sockets_cap)
        closing_socket::close_oldest();
    try {
        static_cast<void>(new closing_socket(socket, bound, dropped));
    } catch (...) {
        ::QB_CLOSESOCKET(socket); // no node, no linger: closing at once is what is left
    }
}

std::size_t
release_closing_sockets() noexcept {
    std::size_t released = 0;
    while (closing.oldest) {
        closing_socket::close_oldest();
        ++released;
    }
    return released;
}

std::size_t
closing_sockets() noexcept {
    return closing.count;
}

std::size_t
closing_sockets_cap() noexcept {
    return sockets_cap;
}

std::size_t
closing_socket_byte_budget() noexcept {
    return byte_budget;
}

} // namespace qb::io::async::detail
