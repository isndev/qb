/**
 * @file qb/io/async/tcp/connector.h
 * @brief Asynchronous TCP connection establishment utilities
 *
 * This file provides utilities for establishing asynchronous TCP connections.
 * It defines the connector class template which handles the async connection
 * process and a connect function for initiating asynchronous connections.
 *
 * C++23 Coroutine Support:
 * ========================
 *
 * This file also provides C++23 coroutine awaiters for async TCP connections,
 * enabling `co_await` style programming:
 *
 * @code
 * #include <qb/io/async/tcp/connector.h>
 *
 * qb::io::async::task<void> my_connection() {
 *     using namespace std::chrono_literals;
 *
 *     auto socket = co_await qb::io::async::tcp::connect(
 *         qb::io::uri{"tcp://localhost:6379"},
 *         5s
 *     );
 *
 *     if (!socket) {
 *         // Connection failed
 *         co_return;
 *     }
 *
 *     // Use connected socket
 *     // ...
 * }
 * @endcode
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

#ifndef QB_IO_ASYNC_TCP_CONNECTOR_H
#define QB_IO_ASYNC_TCP_CONNECTOR_H

#include <atomic>
#include <memory>

#include <qb/io.h>
#include <qb/io/system/sys__socket.h>
#include "../../uri.h"
#include "../../transport/tcp.h"
#include "../event/io.h"
#include "../io.h"
#include "../listener.h"

namespace qb::io::async::tcp {

/**
 * @class connector
 * @brief Handles asynchronous TCP connection establishment
 *
 * Manages non-blocking `n_connect`, completion on `EV_WRITE`, and an optional
 * wall-clock deadline via `async::callback`. The instance is kept alive with
 * `std::shared_ptr` and `self_hold_` until exactly one completion is delivered to
 * the user callback (so the object is not destroyed while libev still references
 * it).
 *
 * @note **Completion ordering:** `on(event::io const &)` always unregisters the I/O
 *       watcher using `event._interface` *before* checking whether another path
 *       (e.g. deadline) already completed. Returning early *without* unregistering
 *       caused invalid-fd regressions on kqueue/epoll.
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type that will be called on connection completion
 */
template <typename Socket_, typename Func_>
class connector : public std::enable_shared_from_this<connector<Socket_, Func_>> {
    Func_        func_;    /**< Callback function to call when connection completes */
    Socket_      socket_;  /**< Socket for the connection */
    uri          remote_;  /**< URI of the remote endpoint */
    /** Absolute libev time `ev_time() + timeout` when `timeout > 0`; else `0` (no deadline). */
    const double deadline_;

    /** Set after exactly one successful delivery to `func_` (immediate or async path). */
    std::atomic<bool>        completed_{false};
    IRegisteredKernelEvent * io_iface_{nullptr};
    /** Strong ref to self while an async path (write watcher or deadline) may still run. */
    std::shared_ptr<connector> self_hold_;

    /**
     * @brief Marks this connect attempt as finished for callback purposes.
     * @return true if this call is the first completion; false if already completed.
     * @private
     */
    [[nodiscard]] bool
    mark_completed_once() noexcept {
        bool expected = false;
        return completed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    /**
     * @brief Invokes the user callback and releases the self-retention ref.
     * @private
     */
    void
    deliver(Socket_ &&s) {
        func_(std::move(s));
        self_hold_.reset();
    }

public:
    /**
     * @brief Constructs a connector and stores parameters (does not connect yet).
     * @param func Callback invoked exactly once with the connected socket or an empty socket
     * @param remote Remote URI
     * @param timeout_sec Connection deadline in seconds from construction (`ev_time()`);
     *                    `0` means no deadline timer (wait indefinitely for writability).
     */
    connector(Func_ &&func, uri remote, double timeout_sec)
        : func_(std::forward<Func_>(func))
        , remote_(std::move(remote))
        , deadline_(timeout_sec > 0. ? ev_time() + timeout_sec : 0.) {}

    /**
     * @brief Constructs a connector with an existing socket (does not connect yet).
     * @param func Callback invoked exactly once on completion
     * @param existing Socket to use (moved from)
     * @param remote Remote URI
     * @param timeout_sec Same semantics as the other constructor
     */
    connector(Func_ &&func, Socket_ &&existing, uri remote, double timeout_sec)
        : func_(std::forward<Func_>(func))
        , socket_(std::move(existing))
        , remote_(std::move(remote))
        , deadline_(timeout_sec > 0. ? ev_time() + timeout_sec : 0.) {}

    /**
     * @brief Runs `n_connect` and either completes immediately or registers `EV_WRITE`
     *        (and optionally a deadline callback).
     */
    void
    run() {
        LOG_DEBUG("Started async connect to " << remote_.source());
        auto ret = socket_.n_connect(remote_);
        if (!ret) {
            LOG_DEBUG("Connected directly to " << remote_.source());
            if (mark_completed_once())
                deliver(std::move(socket_));
            return;
        }
        if (socket_no_error(qb::io::socket::get_last_errno())) {
            self_hold_ = this->shared_from_this();
            auto &io_ev = listener::current.registerEvent<event::io>(
                *this, socket_.native_handle(), EV_WRITE);
            io_iface_ = io_ev._interface;
            io_ev.start();

            if (deadline_ > 0.) {
                const double remain = deadline_ - ev_time();
                std::weak_ptr<connector> w = this->shared_from_this();
                qb::io::async::callback(
                    [w]() {
                        if (auto self = w.lock())
                            self->on_deadline();
                    },
                    remain > 0. ? remain : 0.);
            }
            return;
        }

        socket_.disconnect();
        LOG_DEBUG("Failed to connect to "
                  << remote_.source() << " err=" << qb::io::socket::get_last_errno());
        if (mark_completed_once())
            deliver(Socket_{});
    }

    /**
     * @brief I/O event handler when the socket becomes writable (connect completion).
     * @param event The I/O event (must unregister `event._interface` here, always).
     */
    void
    on(event::io const &event) {
        int err = 0;
        if (!(event._revents & EV_WRITE) ||
            socket_.template get_optval<int>(SOL_SOCKET, SO_ERROR, err)) {
            socket_.disconnect();
            err = 1;
        }
        listener::current.unregisterEvent(event._interface);
        io_iface_ = nullptr;

        if (!mark_completed_once())
            return;

        if (!err || err == EISCONN) {
            LOG_DEBUG("Connected async to " << remote_.source());
            socket_.connected();
            deliver(std::move(socket_));
        } else {
            LOG_DEBUG("Failed to connect to " << remote_.source() << " err="
                                              << qb::io::socket::get_last_errno());
            deliver(Socket_{});
        }
    }

    /**
     * @brief Deadline handler: unregister the write watcher if still registered,
     *        close the socket, and complete with failure if still the first completion.
     */
    void
    on_deadline() {
        if (io_iface_) {
            listener::current.unregisterEvent(io_iface_);
            io_iface_ = nullptr;
        }
        socket_.disconnect();

        if (!mark_completed_once())
            return;

        LOG_DEBUG("Async connect deadline for " << remote_.source());
        deliver(Socket_{});
    }
};

/**
 * @brief Initiates an asynchronous TCP connection
 *
 * Allocates a `std::shared_ptr<connector>` and calls `run()`. The connector stays
 * alive until the user callback runs (including across `n_connect` in progress).
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type that will be called on connection completion
 * @param remote URI of the remote endpoint to connect to
 * @param func Callback function to call when connection completes
 * @param timeout Connection timeout in seconds (`0` = no deadline, same as before)
 */
template <typename Socket_, typename Func_>
void
connect(uri const &remote, Func_ &&func, double timeout = 0.) {
    auto op = std::make_shared<connector<Socket_, Func_>>(
        std::forward<Func_>(func), remote, timeout);
    LOG_DEBUG("Connector: Initializing for " << remote.source());
    op->run();
}

/**
 * @brief Initiates an asynchronous TCP connection using an existing socket
 *
 * Same as `connect(uri, func, timeout)` but moves an existing `Socket_` into the
 * connector before `n_connect`.
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type that will be called on connection completion
 * @param existing_socket Existing socket (moved from)
 * @param remote URI of the remote endpoint to connect to
 * @param func Callback function to call when connection completes
 * @param timeout Connection timeout in seconds (`0` = no deadline)
 */
template <typename Socket_, typename Func_>
void
connect(Socket_&& existing_socket, uri const &remote, Func_ &&func, double timeout = 0.) {
    auto op = std::make_shared<connector<Socket_, Func_>>(
        std::forward<Func_>(func), std::move(existing_socket), remote, timeout);
    LOG_DEBUG("Connector: Initializing with existing socket for " << remote.source());
    op->run();
}

// =============================================================================
// C++23 Coroutine Support
// =============================================================================

#ifdef __cpp_impl_coroutine
// Coroutines are available (C++20/23)

#include <coroutine>
#include <optional>
#include <chrono>

/**
 * @defgroup CoroutineTCP Coroutine TCP Connectors
 * @brief C++23 coroutine awaiters for TCP connections
 *
 * These classes enable `co_await` style programming for TCP connections,
 * wrapping the callback-based connector with a modern coroutine interface.
 *
 * @code
 * auto socket = co_await qb::io::async::tcp::connect(
 *     uri{"tcp://localhost:6379"}, 5s
 * );
 * if (socket) { // use socket
 *     // ...
 * }
 * @endcode
 */

/**
 * @brief Coroutine awaiter for TCP connection establishment
 * @ingroup CoroutineTCP
 * @tparam Socket_ The socket type
 *
 * This awaiter wraps the callback-based tcp::connect with a C++23 coroutine
 * interface. It suspends the coroutine until connection completes and resumes
 * with std::optional<Socket_>.
 */
template <typename Socket_>
class connect_awaiter {
    uri _remote;
    std::chrono::milliseconds _timeout;
    std::optional<Socket_> _result;
    std::coroutine_handle<> _handle;
    bool _ready = false;

public:
    explicit connect_awaiter(uri remote,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
        : _remote(std::move(remote))
        , _timeout(timeout) {}

    [[nodiscard]] bool await_ready() const noexcept { return _ready; }

    void await_suspend(std::coroutine_handle<> h) {
        _handle = h;

        double timeout_sec = _timeout.count() > 0
            ? static_cast<double>(_timeout.count()) / 1000.0
            : 0.0;

        ::qb::io::async::tcp::connect<Socket_>(_remote, [this](Socket_&& socket) {
            if (socket.is_open()) {
                _result = std::move(socket);
            }
            _ready = true;
            if (_handle) {
                ::qb::io::async::CoroutineScheduler::current().schedule_resume(_handle);
            }
        }, timeout_sec);
    }

    [[nodiscard]] std::optional<Socket_> await_resume() {
        return std::move(_result);
    }
};

/**
 * @brief Factory function for TCP connection awaiter
 * @ingroup CoroutineTCP
 * @tparam Transport The transport type (default: transport::tcp)
 * @param remote The remote endpoint URI
 * @param timeout Connection timeout (default: 0ms = no timeout)
 * @return connect_awaiter with appropriate socket type
 */
template <typename Transport = qb::io::transport::tcp>
[[nodiscard]] auto connect(uri remote,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) {
    using socket_type = typename Transport::transport_io_type;
    return connect_awaiter<socket_type>{std::move(remote), timeout};
}

/**
 * @brief Awaiter for connecting with existing socket
 * @ingroup CoroutineTCP
 * @tparam Socket_ The socket type
 */
template <typename Socket_>
class connect_with_socket_awaiter {
    Socket_ _socket;
    uri _remote;
    std::chrono::milliseconds _timeout;
    std::optional<Socket_> _result;
    std::coroutine_handle<> _handle;
    bool _ready = false;

public:
    connect_with_socket_awaiter(Socket_&& sock, uri remote, std::chrono::milliseconds timeout)
        : _socket(std::move(sock))
        , _remote(std::move(remote))
        , _timeout(timeout) {}

    [[nodiscard]] bool await_ready() const noexcept { return _ready; }

    void await_suspend(std::coroutine_handle<> h) {
        _handle = h;

        double timeout_sec = _timeout.count() > 0
            ? static_cast<double>(_timeout.count()) / 1000.0
            : 0.0;

        ::qb::io::async::tcp::connect<Socket_>(std::move(_socket), _remote, [this](Socket_&& socket) {
            if (socket.is_open()) {
                _result = std::move(socket);
            }
            _ready = true;
            if (_handle) {
                ::qb::io::async::CoroutineScheduler::current().schedule_resume(_handle);
            }
        }, timeout_sec);
    }

    [[nodiscard]] std::optional<Socket_> await_resume() {
        return std::move(_result);
    }
};

/**
 * @brief Factory function for connecting with existing socket
 * @ingroup CoroutineTCP
 * @tparam Transport The transport type (default: transport::tcp)
 * @param existing_socket Socket to use for the connection (will be moved)
 * @param remote The remote endpoint URI
 * @param timeout Connection timeout (default: 0ms = no timeout)
 */
template <typename Transport = qb::io::transport::tcp>
[[nodiscard]] auto connect_with_socket(typename Transport::transport_io_type&& existing_socket,
                                         uri remote,
                                         std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) {
    using socket_type = typename Transport::transport_io_type;
    return connect_with_socket_awaiter<socket_type>{std::move(existing_socket), std::move(remote), timeout};
}

#endif // __cpp_impl_coroutine

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_CONNECTOR_H
