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

#include <qb/io.h>
#include <qb/io/system/sys__socket.h>
#include "../../uri.h"
#include "../../transport/tcp.h"
#include "../event/io.h"
#include "../listener.h"

namespace qb::io::async::tcp {

/**
 * @class connector
 * @brief Handles asynchronous TCP connection establishment
 *
 * This class template manages the process of establishing an asynchronous
 * TCP connection. It attempts to connect immediately and, if that fails but
 * the socket is in progress, sets up event handling to complete the connection
 * when the socket becomes writable.
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type that will be called on connection completion
 */
template <typename Socket_, typename Func_>
class connector {
    Func_        _func;    /**< Callback function to call when connection completes */
    const double _timeout; /**< Connection timeout value (0 = no timeout) */
    Socket_      _socket;  /**< Socket for the connection */
    uri          _remote;  /**< URI of the remote endpoint */

private:
    /**
     * @brief Establishes the connection using the initialized _socket and _remote.
     *
     * This method contains the common logic for connecting, handling immediate success,
     * in-progress connections, or immediate failures.
     */
    void _establish_connection() {
        LOG_DEBUG("Started async connect to " << _remote.source());
        auto ret = _socket.n_connect(_remote);
        if (!ret) {
            LOG_DEBUG("Connected directly to " << _remote.source());
            _func(std::move(_socket));
        } else if (socket_no_error(qb::io::socket::get_last_errno())) {
            listener::current
                .registerEvent<event::io>(*this, _socket.native_handle(), EV_WRITE)
                .start();
            return;
        } else {
            _socket.disconnect();
            LOG_DEBUG("Failed to connect to "
                      << _remote.source() << " err=" << qb::io::socket::get_last_errno());
            _func(Socket_{});
        }
        delete this;
    }

public:
    /**
     * @brief Constructor
     *
     * Initiates an asynchronous connection to the specified remote endpoint.
     * If the connection succeeds immediately, the callback is called right away.
     * If the connection is in progress, event handling is set up.
     * If the connection fails immediately, the callback is called with an empty socket.
     *
     * @param remote URI of the remote endpoint to connect to
     * @param func Callback function to call when connection completes
     * @param timeout Connection timeout in seconds (0 = no timeout)
     */
    connector(uri const &remote, Func_ &&func, double timeout = 0.)
        : _func(std::forward<Func_>(func))
        , _timeout(timeout > 0. ? ev_time() + timeout : 0.)
        , _remote{remote} {
        LOG_DEBUG("Connector: Initializing for " << remote.source());
        // _socket is default-initialized here
        _establish_connection();
    }

    /**
     * @brief Constructor with an existing socket
     *
     * Initiates an asynchronous connection to the specified remote endpoint using an existing socket.
     * The provided socket is moved into the connector.
     * If the connection succeeds immediately, the callback is called right away.
     * If the connection is in progress, event handling is set up.
     * If the connection fails immediately, the callback is called with an empty socket.
     *
     * @param existing_socket An rvalue reference to an existing socket to be used for the connection
     * @param remote URI of the remote endpoint to connect to
     * @param func Callback function to call when connection completes
     * @param timeout Connection timeout in seconds (0 = no timeout)
     */
    connector(Socket_&& existing_socket, uri const &remote, Func_ &&func, double timeout = 0.)
        : _func(std::forward<Func_>(func))
        , _timeout(timeout > 0. ? ev_time() + timeout : 0.)
        , _socket(std::move(existing_socket)) // Move the existing socket
        , _remote{remote} {
        LOG_DEBUG("Connector: Initializing with existing socket for " << remote.source());
        _establish_connection();
    }

    /**
     * @brief I/O event handler
     *
     * This method is called when the socket becomes writable, indicating that
     * the connection has completed or failed. It checks the socket for errors,
     * completes the connection if successful, and calls the callback with the
     * socket. The object deletes itself after completion.
     *
     * @param event The I/O event
     */
    void
    on(event::io const &event) {
        int err = 0;
        if (!(event._revents & EV_WRITE) ||
            _socket.template get_optval<int>(SOL_SOCKET, SO_ERROR, err)) {
            _socket.disconnect();
            err = 1;
        }
        // Always complete: once writable we have a definitive result (success or SO_ERROR).
        // Do not wait for timeout when SO_ERROR is already set (e.g. ECONNREFUSED).
        listener::current.unregisterEvent(event._interface);
        if (!err || err == EISCONN) {
            LOG_DEBUG("Connected async to " << _remote.source());
            _socket.connected();
            _func(std::move(_socket));
        } else {
            LOG_DEBUG("Failed to connect to " << _remote.source() << " err="
                                              << qb::io::socket::get_last_errno());
            _func(Socket_{});
        }
        delete this;
    }
};

/**
 * @brief Initiates an asynchronous TCP connection
 *
 * This function creates a new connector object to establish an asynchronous
 * TCP connection to the specified remote endpoint. When the connection
 * completes or fails, the provided callback function is called with the
 * socket. The connector object manages its own lifetime.
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type that will be called on connection completion
 * @param remote URI of the remote endpoint to connect to
 * @param func Callback function to call when connection completes
 * @param timeout Connection timeout in seconds (0 = no timeout)
 */
template <typename Socket_, typename Func_>
void
connect(uri const &remote, Func_ &&func, double timeout = 0.) {
    new connector<Socket_, Func_>(remote, std::forward<Func_>(func), timeout);
}

/**
 * @brief Initiates an asynchronous TCP connection using an existing socket
 *
 * This function creates a new connector object to establish an asynchronous
 * TCP connection to the specified remote endpoint, using an existing socket instance.
 * The provided socket is moved. When the connection completes or fails,
 * the provided callback function is called with the socket.
 * The connector object manages its own lifetime.
 *
 * @tparam Socket_ The socket class type to use for the connection
 * @tparam Func_ The callback function type that will be called on connection completion
 * @param existing_socket An rvalue reference to an existing socket to be used for the connection
 * @param remote URI of the remote endpoint to connect to
 * @param func Callback function to call when connection completes
 * @param timeout Connection timeout in seconds (0 = no timeout)
 */
template <typename Socket_, typename Func_>
void
connect(Socket_&& existing_socket, uri const &remote, Func_ &&func, double timeout = 0.) {
    new connector<Socket_, Func_>(std::move(existing_socket), remote, std::forward<Func_>(func), timeout);
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

        // Use function from outer namespace (before coroutine section)
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

        // Use function from outer namespace (before coroutine section)
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
