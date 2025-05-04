/**
 * @file qb/io/udp/socket.h
 * @brief Implementation of UDP sockets for the QB IO library
 *
 * This file provides the implementation of UDP sockets supporting
 * both IPv4, IPv6, and Unix sockets for datagram communication.
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
 * @ingroup UDP
 */

#include "../system/sys__socket.h"
#include "../uri.h"
#include <optional>
#include <chrono>
#include <string>

#ifndef QB_IO_UDP_SOCKET_H_
#define QB_IO_UDP_SOCKET_H_

namespace qb::io::udp {

/*!
 * @class socket udp/socket.h qb/io/udp/socket.h
 * @ingroup UDP
 * @brief Class implementing UDP socket functionality
 *
 * This class provides UDP socket functionality for datagram-based communication.
 * It inherits from the base qb::io::socket class and provides methods for
 * binding, sending, and receiving UDP datagrams, as well as multicast operations.
 */
class QB_API socket : private qb::io::socket {
public:
    /**
     * @brief Default maximum size of a UDP datagram
     *
     * The theoretical maximum size of a UDP datagram is 65507 bytes,
     * but this implementation uses a more conservative default value of 512 bytes.
     * This can be adjusted by using set_buffer_size() method.
     */
    constexpr static const std::size_t DefaultDatagramSize = 512;
    
    /**
     * @brief Maximum possible size of a UDP datagram
     *
     * The theoretical maximum size of a UDP datagram is 65507 bytes (IPv4).
     */
    constexpr static const std::size_t MaxDatagramSize = 65507;

    // Methods inherited from the base socket
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

    /**
     * @brief Default constructor
     */
    socket() = default;

    /**
     * @brief Copy constructor (deleted)
     */
    socket(socket const &) = delete;

    /**
     * @brief Move constructor
     */
    socket(socket &&) = default;

    /**
     * @brief Move assignment operator
     * @return Reference to the moved socket
     */
    socket &operator=(socket &&) = default;

    /**
     * @brief Constructor from an IO socket
     * @param sock Socket to move from
     */
    socket(io::socket &&sock) noexcept;

    /**
     * @brief Move assignment operator from an IO socket
     * @param sock Socket to move from
     * @return Reference to the moved socket
     */
    socket &operator=(io::socket &&sock) noexcept;

    /**
     * @brief Initialize the UDP socket
     * @param af Address family (default: AF_INET for IPv4)
     * @return true on success, false on failure
     */
    bool init(int af = AF_INET) noexcept;

    /**
     * @brief Bind the socket to an endpoint
     * @param ep Endpoint to bind to
     * @return 0 on success, error code on failure
     */
    int bind(qb::io::endpoint const &ep) noexcept;

    /**
     * @brief Bind the socket to a URI
     * @param u URI to bind to
     * @return 0 on success, error code on failure
     */
    int bind(qb::io::uri const &u) noexcept;

    /**
     * @brief Bind the socket to an IPv4 address
     * @param port Port number to bind to
     * @param host Host address (default: "0.0.0.0" for all interfaces)
     * @return 0 on success, error code on failure
     */
    int bind_v4(uint16_t port, std::string const &host = "0.0.0.0") noexcept;

    /**
     * @brief Bind the socket to an IPv6 address
     * @param port Port number to bind to
     * @param host Host address (default: "::" for all interfaces)
     * @return 0 on success, error code on failure
     */
    int bind_v6(uint16_t port, std::string const &host = "::") noexcept;

    /**
     * @brief Bind the socket to a Unix domain socket
     * @param path Path to the Unix domain socket
     * @return 0 on success, error code on failure
     */
    int bind_un(std::string const &path) noexcept;

    /**
     * @brief Read data from the socket and get the sender's endpoint
     * @param dest Destination buffer for the data
     * @param len Maximum number of bytes to read
     * @param peer Output parameter that will contain the sender's endpoint
     * @return Number of bytes read on success, error code on failure
     */
    int read(void *dest, std::size_t len, qb::io::endpoint &peer) const noexcept;

    /**
     * @brief Read data from the socket with timeout
     * @param dest Destination buffer for the data
     * @param len Maximum number of bytes to read
     * @param peer Output parameter that will contain the sender's endpoint
     * @param timeout Maximum time to wait for data
     * @return Number of bytes read on success, error code on failure
     */
    int read_timeout(void *dest, std::size_t len, qb::io::endpoint &peer, 
                     const std::chrono::microseconds &timeout) const noexcept;

    /**
     * @brief Try to read data from the socket (non-blocking)
     * @param dest Destination buffer for the data
     * @param len Maximum number of bytes to read
     * @param peer Output parameter that will contain the sender's endpoint if data is available
     * @return Number of bytes read on success, 0 if no data available, error code on failure
     */
    int try_read(void *dest, std::size_t len, qb::io::endpoint &peer) const noexcept;

    /**
     * @brief Write data to a specific endpoint
     * @param data Data to send
     * @param len Number of bytes to send
     * @param to Destination endpoint
     * @return Number of bytes sent on success, error code on failure
     */
    int write(const void *data, std::size_t len,
              qb::io::endpoint const &to) const noexcept;

    /**
     * @brief Set socket's send and receive buffer sizes
     * @param size Buffer size to set
     * @return 0 on success, error code on failure
     */
    int set_buffer_size(std::size_t size) noexcept;

    /**
     * @brief Enable or disable broadcast capability for this socket
     * @param enable true to enable, false to disable
     * @return 0 on success, error code on failure
     */
    int set_broadcast(bool enable) noexcept;

    /**
     * @brief Join a multicast group
     * @param group Multicast group address to join
     * @param iface Interface address to use (optional)
     * @return 0 on success, error code on failure
     */
    int join_multicast_group(const std::string &group, 
                             const std::string &iface = "") noexcept;

    /**
     * @brief Leave a multicast group
     * @param group Multicast group address to leave
     * @param iface Interface address to use (optional)
     * @return 0 on success, error code on failure
     */
    int leave_multicast_group(const std::string &group,
                              const std::string &iface = "") noexcept;

    /**
     * @brief Set the multicast TTL value
     * @param ttl TTL value to set (1-255)
     * @return 0 on success, error code on failure
     */
    int set_multicast_ttl(int ttl) noexcept;

    /**
     * @brief Set whether multicast packets are looped back to the sender
     * @param enable true to enable loopback, false to disable
     * @return 0 on success, error code on failure
     */
    int set_multicast_loopback(bool enable) noexcept;

    /**
     * @brief Get the address family of this socket
     * @return Address family (AF_INET, AF_INET6, etc.)
     */
    int address_family() const noexcept;

    /**
     * @brief Check if the socket is bound to an address
     * @return true if bound, false otherwise
     */
    bool is_bound() const noexcept;

    /**
     * @brief Disconnect the socket
     * @return 0 on success, error code on failure
     */
    int disconnect() const noexcept;
};

} // namespace qb::io::udp

#endif // QB_IO_UDP_SOCKET_H_
