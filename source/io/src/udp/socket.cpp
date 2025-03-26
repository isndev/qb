/**
 * @file qb/io/src/udp/socket.cpp
 * @brief Implementation of UDP socket functionality
 * 
 * This file contains the implementation of UDP socket operations in the QB framework,
 * including initialization, reading, writing, and binding operations for UDP sockets.
 * It provides cross-platform socket handling with support for IPv4, IPv6, and Unix domains.
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
 * @ingroup IO
 */

#include <qb/io/udp/socket.h>

namespace qb::io::udp {

socket::socket(io::socket &&sock) noexcept
    : io::socket(sock.release_handle()) {}

socket &
socket::operator=(io::socket &&sock) noexcept {
    static_cast<io::socket &>(*this) = sock.release_handle();
    return *this;
}

bool
socket::init(int af) noexcept {
    auto ret = open(af, SOCK_DGRAM, 0);

    set_optval(SOL_SOCKET, SO_BROADCAST, 1);

    return ret;
}

int
socket::read(void *dest, std::size_t len, qb::io::endpoint &peer) const noexcept {
    return recvfrom(dest, static_cast<int>(len), peer);
}

int
socket::write(const void *data, std::size_t len,
              qb::io::endpoint const &to) const noexcept {
    return sendto(data, static_cast<int>(len), to);
}

int
socket::bind(qb::io::endpoint const &ep) noexcept {
    if (is_open()) {
        auto af = get_optval<int>(SOL_SOCKET, SO_TYPE);
        if (af != ep.af())
            return -1;
    } else
        init(ep.af());

    return qb::io::inet::socket::bind(ep);
}

int
socket::bind(io::uri const &u) noexcept {
    switch (u.af()) {
    case AF_INET:
    case AF_INET6:
        return bind(io::endpoint().as_in(std::string(u.host()).c_str(), u.u_port()));
    case AF_UNIX:
        const auto path = std::string(u.path()) + std::string(u.host());
        return bind_un(path.c_str());
    }
    return -1;
}

int
socket::bind_v4(uint16_t port, std::string const &host) noexcept {
    return bind(qb::io::endpoint().as_in(host.c_str(), port));
}

int
socket::bind_v6(uint16_t port, std::string const &host) noexcept {
    return bind(qb::io::endpoint().as_in(host.c_str(), port));
}

int
socket::bind_un(std::string const &path) noexcept {
    return bind(qb::io::endpoint().as_un(path.c_str()));
}

int
socket::disconnect() const noexcept {
    return shutdown();
}

} // namespace qb::io::udp
