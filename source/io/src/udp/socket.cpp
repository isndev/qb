/**
 * @file qb/io/src/udp/socket.cpp
 * @brief Implementation of UDP socket functionality
 *
 * This file contains the implementation of UDP socket operations in the QB framework,
 * including initialization, reading, writing, and binding operations for UDP sockets.
 * It provides cross-platform socket handling with support for IPv4, IPv6, and Unix
 * domains.
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
#include <cstring>

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

    // Enable broadcasting by default
    set_optval(SOL_SOCKET, SO_BROADCAST, 1);

    return ret;
}

int
socket::read(void *dest, std::size_t len, qb::io::endpoint &peer) const noexcept {
    return recvfrom(dest, static_cast<int>(len), peer);
}

int
socket::read_timeout(void *dest, std::size_t len, qb::io::endpoint &peer, 
                   const std::chrono::microseconds &timeout) const noexcept {
    if (!is_open()) {
        return -1;
    }

    // Set temporary non-blocking mode
    bool was_blocking = (test_nonblocking() == 0);
    if (was_blocking) {
        set_nonblocking(true);
    }

    // Wait for data availability
    int result = handle_read_ready(timeout);
    if (result <= 0) {
        // Restore blocking mode if needed
        if (was_blocking) {
            set_nonblocking(false);
        }
        return (result == 0) ? -ETIMEDOUT : -1;
    }

    // Read the data
    result = recvfrom(dest, static_cast<int>(len), peer);

    // Restore blocking mode if needed
    if (was_blocking) {
        set_nonblocking(false);
    }

    return result;
}

int
socket::try_read(void *dest, std::size_t len, qb::io::endpoint &peer) const noexcept {
    if (!is_open()) {
        return -1;
    }

    // Set temporary non-blocking mode
    bool was_blocking = (test_nonblocking() == 0);
    if (was_blocking) {
        set_nonblocking(true);
    }

    // Try to read without waiting
    int result = recvfrom(dest, static_cast<int>(len), peer);

    // Restore blocking mode if needed
    if (was_blocking) {
        set_nonblocking(false);
    }

    return result;
}

int
socket::write(const void *data, std::size_t len,
              qb::io::endpoint const &to) const noexcept {
    return sendto(data, static_cast<int>(len), to);
}

int
socket::set_buffer_size(std::size_t size) noexcept {
    if (!is_open()) {
        return -1;
    }

    // Set both send and receive buffer sizes
    int recv_result = set_optval(SOL_SOCKET, SO_RCVBUF, static_cast<int>(size));
    int send_result = set_optval(SOL_SOCKET, SO_SNDBUF, static_cast<int>(size));

    return (recv_result != 0 || send_result != 0) ? -1 : 0;
}

int
socket::set_broadcast(bool enable) noexcept {
    if (!is_open()) {
        return -1;
    }

    return set_optval(SOL_SOCKET, SO_BROADCAST, enable ? 1 : 0);
}

int
socket::join_multicast_group(const std::string &group, 
                           const std::string &iface) noexcept {
    if (!is_open()) {
        return -1;
    }

    int af = address_family();
    
    if (af == AF_INET) {
        // IPv4 multicast
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        
        // Set multicast group
        if (inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) {
            return -1;
        }
        
        // Set interface
        if (iface.empty()) {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, iface.c_str(), &mreq.imr_interface) != 1) {
            return -1;
        }
        
        return set_optval(IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    } 
    else if (af == AF_INET6) {
        // IPv6 multicast
        struct ipv6_mreq mreq6;
        memset(&mreq6, 0, sizeof(mreq6));
        
        // Set multicast group
        if (inet_pton(AF_INET6, group.c_str(), &mreq6.ipv6mr_multiaddr) != 1) {
            return -1;
        }
        
        // Set interface index (0 means any interface)
        mreq6.ipv6mr_interface = !iface.empty() ? static_cast<unsigned int>(std::stoi(iface)) : 0;
        
        return set_optval(IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6));
    }
    
    return -1;
}

int
socket::leave_multicast_group(const std::string &group,
                            const std::string &iface) noexcept {
    if (!is_open()) {
        return -1;
    }

    int af = address_family();
    
    if (af == AF_INET) {
        // IPv4 multicast
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        
        // Set multicast group
        if (inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) {
            return -1;
        }
        
        // Set interface
        if (iface.empty()) {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, iface.c_str(), &mreq.imr_interface) != 1) {
            return -1;
        }
        
        return set_optval(IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    } 
    else if (af == AF_INET6) {
        // IPv6 multicast
        struct ipv6_mreq mreq6;
        memset(&mreq6, 0, sizeof(mreq6));
        
        // Set multicast group
        if (inet_pton(AF_INET6, group.c_str(), &mreq6.ipv6mr_multiaddr) != 1) {
            return -1;
        }
        
        // Set interface index (0 means any interface)
        mreq6.ipv6mr_interface = !iface.empty() ? static_cast<unsigned int>(std::stoi(iface)) : 0;
        
        return set_optval(IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq6, sizeof(mreq6));
    }
    
    return -1;
}

int
socket::set_multicast_ttl(int ttl) noexcept {
    if (!is_open()) {
        return -1;
    }

    int af = address_family();
    
    if (af == AF_INET) {
        // Ensure TTL is in valid range
        unsigned char ttl_value = static_cast<unsigned char>(std::max(1, std::min(255, ttl)));
        return set_optval(IPPROTO_IP, IP_MULTICAST_TTL, ttl_value);
    } 
    else if (af == AF_INET6) {
        // For IPv6, the option is different
        unsigned int hop_limit = static_cast<unsigned int>(std::max(1, std::min(255, ttl)));
        return set_optval(IPPROTO_IPV6, IPV6_MULTICAST_HOPS, hop_limit);
    }
    
    return -1;
}

int
socket::set_multicast_loopback(bool enable) noexcept {
    if (!is_open()) {
        return -1;
    }

    int af = address_family();
    
    if (af == AF_INET) {
        unsigned char loop = enable ? 1 : 0;
        return set_optval(IPPROTO_IP, IP_MULTICAST_LOOP, loop);
    } 
    else if (af == AF_INET6) {
        unsigned int loop = enable ? 1 : 0;
        return set_optval(IPPROTO_IPV6, IPV6_MULTICAST_LOOP, loop);
    }
    
    return -1;
}

int
socket::address_family() const noexcept {
    if (!is_open()) {
        return -1;
    }
    
    return get_optval<int>(SOL_SOCKET, SO_TYPE);
}

bool
socket::is_bound() const noexcept {
    if (!is_open()) {
        return false;
    }
    
    try {
        auto local = local_endpoint();
        return local.port() != 0;
    } catch (...) {
        return false;
    }
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
