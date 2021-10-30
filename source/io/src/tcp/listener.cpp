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

#include <qb/io/tcp/listener.h>

namespace qb::io::tcp {

int
listener::listen(io::endpoint const &ep) noexcept {
    const auto ret = pserve(ep);
    set_optval<int>(IPPROTO_TCP, TCP_NODELAY, 1);
    return ret;
}

int
listener::listen(io::uri const &u) noexcept {
    switch (u.af()) {
    case AF_INET:
    case AF_INET6:
        return listen(io::endpoint().as_in(std::string(u.host()).c_str(), u.u_port()));
    case AF_UNIX:
        const auto path = std::string(u.path()) + std::string(u.host());
        return listen_un(path.c_str());
    }
    return -1;
}

int
listener::listen_v4(uint16_t port, std::string const &host) noexcept {
    return listen(io::endpoint().as_in(host.c_str(), port));
}

int
listener::listen_v6(uint16_t port, std::string const &host) noexcept {
    return listen(io::endpoint().as_in(host.c_str(), port));
}

int
listener::listen_un(std::string const &path) noexcept {
    return listen(io::endpoint().as_un(path.c_str()));
}

socket
listener::accept() const noexcept {
    return {io::socket::accept()};
}

int
listener::accept(tcp::socket &sock) const noexcept {
    socket_type nt_sock = qb::io::inet::invalid_socket;

    auto ret = io::socket::accept_n(nt_sock);
    sock = nt_sock;

    return ret;
}

int
listener::disconnect() const noexcept {
    return shutdown();
}

} // namespace qb::io::tcp
