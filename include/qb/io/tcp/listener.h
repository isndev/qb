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

#include "socket.h"

#ifndef QB_IO_TCP_LISTENER_H_
#    define QB_IO_TCP_LISTENER_H_

namespace qb::io::tcp {

/*!
 * @class listener tcp/listener.h qb/io/tcp/listener.h
 * @ingroup TCP
 */
class QB_API listener : private io::socket {
public:
    using qb::io::socket::close;
    using qb::io::socket::get_optval;
    using qb::io::socket::is_open;
    using qb::io::socket::local_endpoint;
    using qb::io::socket::native_handle;
    using qb::io::socket::peer_endpoint;
    using qb::io::socket::release_handle;
    using qb::io::socket::set_nonblocking;
    using qb::io::socket::set_optval;
    using qb::io::socket::test_nonblocking;

    int listen(io::endpoint const &ep) noexcept;
    int listen(io::uri const &uri) noexcept;
    int listen_v4(uint16_t port, std::string const &host = "0.0.0.0") noexcept;
    int listen_v6(uint16_t port, std::string const &host = "::") noexcept;
    int listen_un(std::string const &path) noexcept;

    tcp::socket accept() const noexcept;
    int accept(tcp::socket &sock) const noexcept;

    int disconnect() const noexcept;
};

} // namespace qb::io::tcp

#endif // QB_IO_TCP_LISTENER_H_
