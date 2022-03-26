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

#ifndef QB_IO_ASYNC_TCP_SESSION_H
#define QB_IO_ASYNC_TCP_SESSION_H

#include "../../../uuid.h"
#include "../io.h"

namespace qb::io::async::tcp {

template <typename _Derived, typename _Transport, typename _Server = void>
class client
    : public io<_Derived>
    , _Transport {
    using base_t = io<_Derived>;
    friend base_t;

public:
    using transport_io_type = typename _Transport::transport_io_type;
    using _Transport::in;
    using _Transport::out;
    using _Transport::transport;
    using base_t::publish;

protected:
    const uuid _uuid;
    _Server &_server;

public:
    using IOServer = _Server;
    constexpr static const bool has_server = true;

    client() = delete;
    explicit client(_Server &server)
        : _uuid(generate_random_uuid())
        , _server(server) {
        if constexpr (has_member_Protocol<_Derived>::value) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(
                    static_cast<_Derived &>(*this));
            }
        }
    }

    inline _Server &
    server() {
        return _server;
    }

    inline uuid const &id() const noexcept {
        return _uuid;
    }

};

template <typename _Derived, typename _Transport>
class client<_Derived, _Transport, void>
    : public io<_Derived>
    , _Transport {
    using base_t = io<_Derived>;
    friend base_t;

public:
    using transport_io_type = typename _Transport::transport_io_type;
    using _Transport::in;
    using _Transport::out;
    using _Transport::transport;
    using base_t::publish;

public:
    client() noexcept {
        if constexpr (has_member_Protocol<_Derived>::value) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(
                    static_cast<_Derived &>(*this));
            }
        }
    }
};

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_SESSION_H
