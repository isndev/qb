/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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

#include "../../ip.h"
#include "../../uri.h"
#include "../listener.h"
#include "../event/io.h"

namespace qb::io::async::tcp {

    template <typename Socket_, typename Func_>
    class connector {
        Func_ _func;
        const double _timeout;
        Socket_ _socket;
    public:
        connector(uri const &remote, Func_ &&func, double timeout = 0.)
        : _func(std::forward<Func_>(func))
        , _timeout(timeout > 0. ? ev_time() + timeout : 0.) {
            static_cast<sys::socket<SocketType::TCP>&>(_socket).init();
            _socket.setBlocking(false);
            auto ret = _socket.connect(remote);
            if (ret == SocketStatus::Done) {
                _func(_socket);
            } else if (ret == SocketStatus::NotReady) {
                listener::current.registerEvent<event::io>(*this, _socket.fd(), EV_WRITE).start();
                return;
            } else
                _socket.disconnect();
            delete this;
        }
        void on(event::io const &event) {
            if (!(event._revents & EV_WRITE))
                _socket.disconnect();
            else if (_socket.getRemoteAddress() == ip::None) {
                if (!_timeout || ev_time() < _timeout)
                    return;
                else
                    _socket.disconnect();
            }
            listener::current.unregisterEvent(event._interface);
            _func(_socket);
            delete this;
        }
    };

    template <typename Socket_, typename Func_>
    void connect(uri const &remote, Func_ &&func, double timeout = 0.) {
        new connector<Socket_, Func_>(remote, std::forward<Func_>(func), timeout);
    }

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_CONNECTOR_H
