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

#include "../uri.h"
#include "../system/sys__socket.h"

#ifndef QB_IO_UDP_SOCKET_H_
#    define QB_IO_UDP_SOCKET_H_

namespace qb::io::udp {

/*!
 * @class socket udp/socket.h qb/io/udp/socket.h
 * @ingroup UDP
 */
class QB_API socket : private qb::io::socket {
public:
    constexpr static const std::size_t MaxDatagramSize = 65507;
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

    socket() = default;
    socket(socket const &) = delete;
    socket(socket &&) = default;
    socket &operator=(socket &&) = default;
    socket(io::socket &&sock) noexcept;

    socket &operator=(io::socket &&sock) noexcept;

    bool init(int af = AF_INET) noexcept;
    int bind(qb::io::endpoint const &ep) noexcept;
    int bind(qb::io::uri const &u) noexcept;
    int bind_v4(uint16_t port, std::string const &host = "0.0.0.0") noexcept;
    int bind_v6(uint16_t port, std::string const &host = "::") noexcept;
    int bind_un(std::string const &path) noexcept;

    int read(void *dest, std::size_t len, qb::io::endpoint &peer) const noexcept;
    int write(const void *data, std::size_t len,
              qb::io::endpoint const &to) const noexcept;
    int disconnect() const noexcept;
};

} // namespace qb::io::udp

#endif // QB_IO_UDP_SOCKET_H_
