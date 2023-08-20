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

#include <qb/io/tcp/socket.h>

namespace qb::io::tcp {

socket::socket(io::socket &&sock) noexcept
    : io::socket(sock.release_handle()) {}

socket &
socket::operator=(io::socket &&sock) noexcept {
    static_cast<io::socket &>(*this) = sock.release_handle();
    return *this;
}

int
socket::init(int af) noexcept {
    if (io::socket::open(af, SOCK_STREAM, 0)) {
        set_optval<int>(IPPROTO_TCP, TCP_NODELAY, 1);
        return 0;
    }
    return -1;
}

int
socket::bind(qb::io::endpoint const &ep) noexcept {
    if (is_open()) {
        const auto af = get_optval<int>(SOL_SOCKET, SO_TYPE);
        if (af != ep.af())
            return -1;
    } else if (init(ep.af()))
        return -1;

    return qb::io::socket::bind(ep);
}

int
socket::bind(io::uri const &u) noexcept {
    switch (u.af()) {
    case AF_INET:
    case AF_INET6:
        return bind(io::endpoint().as_in(std::string(u.host()).c_str(), u.u_port()));
    case AF_UNIX:
        const auto path = std::string(u.path()) + std::string(u.host());
        return bind(io::endpoint().as_un(path.c_str()));
    }
    return -1;
}

int
socket::connect_in(int af, std::string const &host, uint16_t port) noexcept {
    auto ret = -1;
    qb::io::socket::resolve_i(
        [&, this](const auto &ep) {
            if (ep.af() == af) {
                ret = connect(ep);
                return true;
            }
            return false;
        },
        host.c_str(), port, af, SOCK_STREAM);

    return ret;
}

int
socket::connect(qb::io::endpoint const &ep) noexcept {
    if (is_open()) {
        const auto af = get_optval<int>(SOL_SOCKET, SO_TYPE);
        if (af != ep.af())
            return -1;
    } else if (init(ep.af()))
        return -1;

    return qb::io::socket::connect(ep);
}

int
socket::connect(uri const &u) noexcept {
    switch (u.af()) {
    case AF_INET:
    case AF_INET6:
        return connect_in(u.af(), std::string(u.host()), u.u_port());
    case AF_UNIX:
        const auto path = std::string(u.path()) + std::string(u.host());
        return connect_un(path);
    }
    return -1;
}

int
socket::connect_v4(std::string const &host, uint16_t port) noexcept {
    return connect_in(AF_INET, host, port);
}

int
socket::connect_v6(std::string const &host, uint16_t port) noexcept {
    return connect_in(AF_INET6, host, port);
}

int
socket::connect_un(std::string const &path) noexcept {
    return connect(qb::io::endpoint().as_un(path.c_str()));
}

// non blocking version

int
socket::n_connect_in(int af, std::string const &host, uint16_t port) noexcept {
    auto ret = -1;
    qb::io::socket::resolve_i(
        [&, this](const auto &ep) {
            if (ep.af() == af) {
                ret = n_connect(ep);
                return true;
            }
            return false;
        },
        host.c_str(), port, af, SOCK_STREAM);

    return ret;
}

int
socket::n_connect(qb::io::endpoint const &ep) noexcept {
    if (is_open()) {
        const auto af = get_optval<int>(SOL_SOCKET, SO_TYPE);
        if (af != ep.af())
            return -1;
    } else if (init(ep.af()))
        return -1;

    return qb::io::socket::connect_n(ep);
}

int
socket::n_connect(uri const &u) noexcept {
    switch (u.af()) {
    case AF_INET:
    case AF_INET6:
        return n_connect_in(u.af(), std::string(u.host()), u.u_port());
    case AF_UNIX:
        const auto path = std::string(u.path()) + std::string(u.host());
        return n_connect_un(path);
    }
    return -1;
}

int
socket::n_connect_v4(std::string const &host, uint16_t port) noexcept {
    return n_connect_in(AF_INET, host, port);
}

int
socket::n_connect_v6(std::string const &host, uint16_t port) noexcept {
    return n_connect_in(AF_INET6, host, port);
}

int
socket::n_connect_un(std::string const &path) noexcept {
    return n_connect(qb::io::endpoint().as_un(path.c_str()));
}

int
socket::read(void *dest, std::size_t len) const noexcept {
    int ret = recv(dest, static_cast<int>(len));
    return ret > 0 ? ret : -1;
}

int
socket::write(const void *data, std::size_t size) const noexcept {
    return send(data, static_cast<int>(size));
}

int
socket::disconnect() const noexcept {
    return shutdown();
}

} // namespace qb::io::tcp