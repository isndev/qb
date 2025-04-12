/**
 * @file qb/io/async/tcp/connector.h
 * @brief Asynchronous TCP connection establishment utilities
 *
 * This file provides utilities for establishing asynchronous TCP connections.
 * It defines the connector class template which handles the async connection
 * process and a connect function for initiating asynchronous connections.
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
        LOG_DEBUG("Started async connect to " << remote.source());
        auto ret = _socket.n_connect(remote);
        if (!ret) {
            LOG_DEBUG("Connected directly to " << remote.source());
            _func(std::move(_socket));
        } else if (socket_no_error(qb::io::socket::get_last_errno())) {
            listener::current
                .registerEvent<event::io>(*this, _socket.native_handle(), EV_WRITE)
                .start();
            return;
        } else {
            _socket.disconnect();
            LOG_DEBUG("Failed to connect to "
                      << remote.source() << " err=" << qb::io::socket::get_last_errno());
            _func(Socket_{});
        }
        delete this;
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
        } else if (err && (err != EISCONN) && (!_timeout || ev_time() < _timeout))
            return;
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

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_CONNECTOR_H
