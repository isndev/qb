/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#ifndef QB_IO_ASYNC_TCP_CONNECTOR_H
#define QB_IO_ASYNC_TCP_CONNECTOR_H

#include "../../uri.h"
#include "../event/io.h"
#include "../listener.h"
#include <qb/io.h>

namespace qb::io::async::tcp {

template <typename Socket_, typename Func_>
class connector {
    Func_ _func;
    const double _timeout;
    Socket_ _socket;
    uri _remote;

public:
    connector(uri const &remote, Func_ &&func, double timeout = 0.)
        : _func(std::forward<Func_>(func))
        , _timeout(timeout > 0. ? ev_time() + timeout : 0.)
        , _remote{remote} {
        LOG_DEBUG("Started async connect to " << remote.source());
        //_socket.set_nonblocking(true);
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

    void
    on(event::io const &event) {
        int err = 0;
        if (!(event._revents & EV_WRITE) ||
            _socket.template get_optval<int>(SOL_SOCKET, SO_ERROR, err)) {
            _socket.disconnect();
            err = 1;
        } else if ((err && err != EISCONN) && (!_timeout || ev_time() < _timeout))
            return;
        listener::current.unregisterEvent(event._interface);
        if (!err || err == EISCONN) {
            LOG_DEBUG("Connected async to " << _remote.source());
            _func(std::move(_socket));
        } else {
            LOG_DEBUG("Failed to connect to " << _remote.source() << " err="
                                              << qb::io::socket::get_last_errno());
            _func(Socket_{});
        }
        delete this;
    }
};

template <typename Socket_, typename Func_>
void
connect(uri const &remote, Func_ &&func, double timeout = 0.) {
    new connector<Socket_, Func_>(remote, std::forward<Func_>(func), timeout);
}

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_CONNECTOR_H
