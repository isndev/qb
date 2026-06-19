/**
 * @file qb/io/async/tcp/connector.h
 * @brief Asynchronous TCP connection establishment utilities
 *
 * This file provides utilities for establishing asynchronous TCP connections.
 * It defines the connector class template which handles the async connection
 * process and a connect function for initiating asynchronous connections.
 *
 * C++20 Coroutine Support:
 * ========================
 *
 * This file also provides C++20 coroutine awaiters for async TCP connections,
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
 * @ingroup TCP
 */

#ifndef QB_IO_ASYNC_TCP_CONNECTOR_H
#define QB_IO_ASYNC_TCP_CONNECTOR_H

#include <atomic>
#include <concepts>
#include <memory>
#include <type_traits>

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
    Func_   func_;   /**< Callback function to call when connection completes */
    Socket_ socket_; /**< Socket for the connection */
    uri     remote_; /**< URI of the remote endpoint */
    /** Absolute libev time `ev_time() + timeout` when `timeout > 0`; else `0` (no deadline). */
    const double deadline_;
    /** When false, disable TLS peer verification on a secure socket (opt-out). */
    bool verify_peer_{true};

    bool                       completed_{false};
    bool                       deadline_armed_{false};
    IRegisteredKernelEvent    *io_iface_{nullptr};
    std::shared_ptr<connector> self_hold_;

    /**
     * @brief Marks this connect attempt as finished for callback purposes.
     * @return true if this call is the first completion; false if already completed.
     */
    [[nodiscard]] bool
    mark_completed_once() noexcept {
        if (completed_)
            return false;
        completed_ = true;
        return true;
    }

    /**
     * @brief Invokes the user callback and releases the self-retention ref.
     * @private
     */
    void
    deliver(Socket_ &&s) {
        auto guard = std::exchange(self_hold_, nullptr);
        func_(std::move(s));
    }

    void
    deliver_failure() {
        socket_.disconnect();
        if (mark_completed_once())
            deliver(Socket_{});
    }

    enum class finalize_result { done, pending, failed };

    [[nodiscard]] bool
    arm_io(int events) {
        if (!socket_.is_open() || socket_.native_handle() < 0)
            return false;

        if (!self_hold_)
            self_hold_ = this->shared_from_this();

        if (io_iface_)
            return true;

        auto &io_ev = listener::current.registerEvent<event::io>(*this, socket_.native_handle(), events);
        io_iface_   = io_ev._interface;
        io_ev.start();
        return true;
    }

    void
    arm_deadline() {
        if (deadline_ <= 0. || deadline_armed_)
            return;
        deadline_armed_                 = true;
        const double             remain = deadline_ - ev_time();
        std::weak_ptr<connector> w      = this->shared_from_this();
        qb::io::async::callback(
            [w]() {
                if (auto self = w.lock())
                    self->on_deadline();
            },
            qb::detail::from_ev_seconds(remain > 0. ? remain : 0.));
    }

    [[nodiscard]] finalize_result
    finalize_transport_connect() noexcept {
        if constexpr (requires(Socket_ &s) {
                          { s.handshake_status() } -> std::same_as<int>;
                      }) {
            const auto status = socket_.handshake_status();
            if (status > 0)
                return finalize_result::done;
            if (status == 0)
                return finalize_result::pending;
            return finalize_result::failed;
        } else if constexpr (std::is_same_v<decltype(std::declval<Socket_ &>().connected()), int>) {
            return socket_.connected() == 0 ? finalize_result::done : finalize_result::failed;
        } else {
            socket_.connected();
            return finalize_result::done;
        }
    }

public:
    /**
     * @brief Constructs a connector and stores parameters (does not connect yet).
     * @param func Callback invoked exactly once with the connected socket or an empty socket
     * @param remote Remote URI
     * @param timeout_sec Connection deadline in seconds from construction (`ev_time()`);
     *                    `0` means no deadline timer (wait indefinitely for writability).
     */
    connector(Func_ &&func, uri remote, double timeout_sec, bool verify_peer = true)
        : func_(std::forward<Func_>(func))
        , remote_(std::move(remote))
        , deadline_(timeout_sec > 0. ? ev_time() + timeout_sec : 0.)
        , verify_peer_(verify_peer) {}

    /**
     * @brief Constructs a connector with an existing socket (does not connect yet).
     * @param func Callback invoked exactly once on completion
     * @param existing Socket to use (moved from)
     * @param remote Remote URI
     * @param timeout_sec Same semantics as the other constructor
     * @param verify_peer When false, disables TLS peer verification (secure sockets only).
     */
    connector(Func_ &&func, Socket_ &&existing, uri remote, double timeout_sec, bool verify_peer = true)
        : func_(std::forward<Func_>(func))
        , socket_(std::move(existing))
        , remote_(std::move(remote))
        , deadline_(timeout_sec > 0. ? ev_time() + timeout_sec : 0.)
        , verify_peer_(verify_peer) {}

    /**
     * @brief Runs `n_connect` and either completes immediately or registers `EV_WRITE`
     *        (and optionally a deadline callback).
     */
    void
    run() {
        LOG_DEBUG("Started async connect to " << remote_.source());
        // Apply the TLS verification policy before the (non-blocking) connect so
        // it is in effect when the handshake starts. No-op for plain sockets.
        if constexpr (requires { socket_.set_insecure(); }) {
            if (!verify_peer_)
                socket_.set_insecure();
        }
        auto ret = socket_.n_connect(remote_);
        if (!ret) {
            switch (finalize_transport_connect()) {
                case finalize_result::done:
                    if (!mark_completed_once())
                        return;
                    LOG_DEBUG("Connected directly to " << remote_.source());
                    deliver(std::move(socket_));
                    break;
                case finalize_result::pending:
                    if (arm_io(EV_READ | EV_WRITE)) {
                        arm_deadline();
                    } else {
                        deliver_failure();
                    }
                    break;
                case finalize_result::failed:
                    LOG_DEBUG("Failed to finalize direct connect to " << remote_.source());
                    deliver_failure();
            }
            return;
        }
        if (socket_no_error(qb::io::socket::get_last_errno()) && arm_io(EV_WRITE)) {
            arm_deadline();
            return;
        }

        LOG_DEBUG("Failed to connect to " << remote_.source() << " err=" << qb::io::socket::get_last_errno());
        deliver_failure();
    }

    /**
     * @brief I/O event handler when the socket becomes writable (connect completion).
     * @param event The I/O event (must unregister `event._interface` here, always).
     */
    void
    on(event::io const &event) {
        int err = 0;
        if (!(event._revents & (EV_READ | EV_WRITE)) || socket_.template get_optval<int>(SOL_SOCKET, SO_ERROR, err)) {
            socket_.disconnect();
            err = 1;
        }

        if (!err || err == EISCONN) {
            switch (finalize_transport_connect()) {
                case finalize_result::done:
                    listener::current.unregisterEvent(event._interface);
                    io_iface_ = nullptr;
                    if (!mark_completed_once())
                        return;
                    LOG_DEBUG("Connected async to " << remote_.source());
                    deliver(std::move(socket_));
                    return;
                case finalize_result::pending:
                    static_cast<event::io &>(const_cast<event::io &>(event)).set(EV_READ | EV_WRITE);
                    return;
                case finalize_result::failed:
                    break;
            }

            socket_.disconnect();
        }

        listener::current.unregisterEvent(event._interface);
        io_iface_ = nullptr;
        if (!mark_completed_once())
            return;
        LOG_DEBUG("Failed to connect to " << remote_.source() << " err=" << err);
        deliver(Socket_{});
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
 * @param verify_peer For secure transports, whether to verify the server
 *                    certificate chain + hostname (default `true`, secure).
 *                    Pass `false` only for trusted/self-signed channels.
 */
template <typename Socket_, typename Func_>
void
connect(uri const &remote, Func_ &&func, qb::duration timeout = qb::duration::zero(), bool verify_peer = true) {
    auto op = std::make_shared<connector<Socket_, Func_>>(std::forward<Func_>(func), remote, qb::detail::to_ev_seconds(timeout), verify_peer);
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
connect(Socket_ &&existing_socket, uri const &remote, Func_ &&func, qb::duration timeout = qb::duration::zero(), bool verify_peer = true) {
    auto op = std::make_shared<connector<Socket_, Func_>>(std::forward<Func_>(func), std::move(existing_socket), remote,
                                                          qb::detail::to_ev_seconds(timeout), verify_peer);
    LOG_DEBUG("Connector: Initializing with existing socket for " << remote.source());
    op->run();
}

// =============================================================================
// C++20 Coroutine Support
// =============================================================================

#ifdef __cpp_impl_coroutine
// Coroutines are available (C++20/23)

#include <coroutine>
#include <optional>
#include <chrono>

/**
 * @defgroup CoroutineTCP Coroutine TCP Connectors
 * @brief C++20 coroutine awaiters for TCP connections
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
 * This awaiter wraps the callback-based tcp::connect with a C++20 coroutine
 * interface. It suspends the coroutine until connection completes and resumes
 * with std::optional<Socket_>.
 */
template <typename Socket_>
class connect_awaiter {
    struct state_t {
        std::optional<Socket_>               result;
        std::coroutine_handle<>              handle{};
        ::qb::io::async::CoroutineScheduler *scheduler{nullptr};
        bool                                 ready{false};
        bool                                 active{true};
    };

    uri                      _remote;
    qb::duration             _timeout;
    bool                     _verify_peer{true};
    std::shared_ptr<state_t> _state{std::make_shared<state_t>()};

public:
    explicit connect_awaiter(uri remote, qb::duration timeout = qb::duration::zero(), bool verify_peer = true)
        : _remote(std::move(remote))
        , _timeout(timeout)
        , _verify_peer(verify_peer) {}

    [[nodiscard]] bool
    await_ready() const noexcept {
        return _state->ready;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->handle    = h;
        _state->scheduler = ::qb::io::async::CoroutineScheduler::current_ptr();
        if (!_state->scheduler)
            _state->scheduler = &::qb::io::async::CoroutineScheduler::current();

        auto state = _state;
        ::qb::io::async::tcp::connect<Socket_>(
            _remote,
            [state](Socket_ &&socket) {
                if (!state->active)
                    return;
                if (socket.is_open()) {
                    state->result = std::move(socket);
                }
                state->ready = true;
                if (state->scheduler && state->handle) {
                    state->scheduler->schedule_resume(state->handle);
                }
            },
            _timeout, _verify_peer);
    }

    [[nodiscard]] std::optional<Socket_>
    await_resume() {
        _state->active = false;
        _state->handle = {};
        return std::move(_state->result);
    }

    ~connect_awaiter() {
        _state->active = false;
        _state->handle = {};
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
[[nodiscard]] auto
connect(uri remote, qb::duration timeout = qb::duration::zero(), bool verify_peer = true) {
    using socket_type = typename Transport::transport_io_type;
    return connect_awaiter<socket_type>{std::move(remote), timeout, verify_peer};
}

/**
 * @brief Awaiter for connecting with existing socket
 * @ingroup CoroutineTCP
 * @tparam Socket_ The socket type
 */
template <typename Socket_>
class connect_with_socket_awaiter {
    struct state_t {
        std::optional<Socket_>               result;
        std::coroutine_handle<>              handle{};
        ::qb::io::async::CoroutineScheduler *scheduler{nullptr};
        bool                                 ready{false};
        bool                                 active{true};
    };

    Socket_                  _socket;
    uri                      _remote;
    qb::duration             _timeout;
    std::shared_ptr<state_t> _state{std::make_shared<state_t>()};

public:
    connect_with_socket_awaiter(Socket_ &&sock, uri remote, qb::duration timeout)
        : _socket(std::move(sock))
        , _remote(std::move(remote))
        , _timeout(timeout) {}

    [[nodiscard]] bool
    await_ready() const noexcept {
        return _state->ready;
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        _state->handle    = h;
        _state->scheduler = ::qb::io::async::CoroutineScheduler::current_ptr();
        if (!_state->scheduler)
            _state->scheduler = &::qb::io::async::CoroutineScheduler::current();

        auto state = _state;
        ::qb::io::async::tcp::connect<Socket_>(
            std::move(_socket), _remote,
            [state](Socket_ &&socket) {
                if (!state->active)
                    return;
                if (socket.is_open()) {
                    state->result = std::move(socket);
                }
                state->ready = true;
                if (state->scheduler && state->handle) {
                    state->scheduler->schedule_resume(state->handle);
                }
            },
            _timeout);
    }

    [[nodiscard]] std::optional<Socket_>
    await_resume() {
        _state->active = false;
        _state->handle = {};
        return std::move(_state->result);
    }

    ~connect_with_socket_awaiter() {
        _state->active = false;
        _state->handle = {};
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
[[nodiscard]] auto
connect_with_socket(typename Transport::transport_io_type &&existing_socket, uri remote, qb::duration timeout = qb::duration::zero()) {
    using socket_type = typename Transport::transport_io_type;
    return connect_with_socket_awaiter<socket_type>{std::move(existing_socket), std::move(remote), timeout};
}

#endif // __cpp_impl_coroutine

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_CONNECTOR_H
