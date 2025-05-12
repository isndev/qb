/**
 * @file qb/io/udp/socket.h
 * @brief Implementation of UDP sockets for datagram communication in the QB IO library.
 *
 * This file provides the implementation of UDP sockets supporting
 * both IPv4, IPv6, and Unix sockets (if enabled) for connectionless, datagram-based communication.
 * It builds upon the generic `qb::io::socket` wrapper.
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
 * @class socket
 * @ingroup UDP
 * @brief Class implementing UDP socket functionality for datagram-based communication.
 *
 * This class provides UDP socket functionality, inheriting protectedly from `qb::io::socket`.
 * It supports sending and receiving datagrams over UDP/IPv4, UDP/IPv6, and Unix Domain Sockets (datagram type).
 * It also includes methods for managing multicast group memberships.
 * Used as the underlying I/O primitive for `qb::io::transport::udp`.
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

    // Methods inherited from the base socket (made public via using-declarations)
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
     * @brief Default constructor. Creates an uninitialized UDP socket.
     *        Call `init()` before use.
     */
    socket() = default;

    /**
     * @brief Deleted copy constructor. Sockets are not copyable.
     */
    socket(socket const &) = delete;

    /**
     * @brief Default move constructor.
     */
    socket(socket &&) = default;

    /**
     * @brief Default move assignment operator.
     * @return Reference to this socket.
     */
    socket &operator=(socket &&) = default;

    /**
     * @brief Constructor from a generic `qb::io::socket` (move semantics).
     * @param sock A generic `qb::io::socket` instance, typically already opened with `SOCK_DGRAM`.
     *             The state of `sock` is moved into this `udp::socket`.
     */
    socket(io::socket &&sock) noexcept;

    /**
     * @brief Move assignment operator from a generic `qb::io::socket`.
     * @param sock A generic `qb::io::socket` to move from.
     * @return Reference to this `udp::socket`.
     */
    socket &operator=(io::socket &&sock) noexcept;

    /**
     * @brief Initialize (open) the UDP socket.
     * @param af Address family (e.g., `AF_INET`, `AF_INET6`, `AF_UNIX`). Defaults to `AF_INET`.
     * @return `true` on success, `false` on failure to open the socket.
     * @details This method calls the base `qb::io::socket::open()` with `SOCK_DGRAM` type.
     */
    bool init(int af = AF_INET) noexcept;

    /**
     * @brief Bind the socket to a specific local endpoint.
     * @param ep The `qb::io::endpoint` to bind to. For UDP, this sets the local address and port
     *           from which packets will be sent and received.
     * @return 0 on success, or a non-zero error code on failure.
     * @see qb::io::socket::bind(const endpoint &)
     */
    int bind(qb::io::endpoint const &ep) noexcept;

    /**
     * @brief Bind the socket to an endpoint specified by a URI.
     * @param u The `qb::io::uri` specifying the local address and port to bind to.
     *          Scheme should typically be "udp".
     * @return 0 on success, or a non-zero error code on failure.
     */
    int bind(qb::io::uri const &u) noexcept;

    /**
     * @brief Bind the socket to a specific IPv4 address and port.
     * @param port The port number to bind to.
     * @param host The IPv4 host address string (e.g., "0.0.0.0" for all interfaces).
     *             Defaults to "0.0.0.0".
     * @return 0 on success, or a non-zero error code on failure.
     */
    int bind_v4(uint16_t port, std::string const &host = "0.0.0.0") noexcept;

    /**
     * @brief Bind the socket to a specific IPv6 address and port.
     * @param port The port number to bind to.
     * @param host The IPv6 host address string (e.g., "::" for all interfaces).
     *             Defaults to "::".
     * @return 0 on success, or a non-zero error code on failure.
     */
    int bind_v6(uint16_t port, std::string const &host = "::") noexcept;

    /**
     * @brief Bind the socket to a Unix domain socket path (for datagrams).
     * @param path The file system path for the Unix domain socket.
     * @return 0 on success, or a non-zero error code on failure.
     * @note This is only effective if `QB_ENABLE_UDS` is active and the system supports AF_UNIX with SOCK_DGRAM.
     */
    int bind_un(std::string const &path) noexcept;

    /**
     * @brief Read a datagram from the socket and get the sender's endpoint.
     * @param dest Pointer to the buffer where the received datagram will be stored.
     * @param len Maximum number of bytes to read (size of the `dest` buffer).
     * @param peer Output parameter: A `qb::io::endpoint` reference that will be populated
     *             with the source address of the received datagram.
     * @return Number of bytes actually read. Returns a negative value on error
     *         (e.g., `WSAEWOULDBLOCK` or `EAGAIN` if non-blocking and no data available).
     * @see qb::io::socket::recvfrom(void*, int, endpoint&, int)
     */
    int read(void *dest, std::size_t len, qb::io::endpoint &peer) const noexcept;

    /**
     * @brief Read a datagram from the socket with a specified timeout.
     * @param dest Pointer to the buffer for received data.
     * @param len Maximum number of bytes to read.
     * @param peer Output parameter for the sender's endpoint.
     * @param timeout Maximum time to wait for data. If the timeout expires before data arrives,
     *                an error (typically `ETIMEDOUT` or equivalent) is returned.
     * @return Number of bytes read, or a negative value on error or timeout.
     * @details Uses `qb::io::socket::recv_n` which internally uses `select` for timeout handling.
     */
    int read_timeout(void *dest, std::size_t len, qb::io::endpoint &peer, 
                     const std::chrono::microseconds &timeout) const noexcept;

    /**
     * @brief Try to read a datagram from the socket (non-blocking attempt).
     * @param dest Pointer to the buffer for received data.
     * @param len Maximum number of bytes to read.
     * @param peer Output parameter for the sender's endpoint if data is successfully read.
     * @return Number of bytes read if data was available immediately.
     *         Returns 0 if no data was available (and socket is non-blocking, would otherwise block).
     *         Returns a negative value on error.
     * @details Sets the socket to non-blocking, attempts a read, then restores its original blocking state.
     */
    int try_read(void *dest, std::size_t len, qb::io::endpoint &peer) const noexcept;

    /**
     * @brief Write (send) a datagram to a specific remote endpoint.
     * @param data Pointer to the data to be sent.
     * @param len Number of bytes to send from the `data` buffer.
     * @param to The destination `qb::io::endpoint`.
     * @return Number of bytes actually sent. For UDP, this is usually `len` on success.
     *         Returns a negative value on error.
     * @see qb::io::socket::sendto(const void*, int, const endpoint&, int)
     */
    int write(const void *data, std::size_t len,
              qb::io::endpoint const &to) const noexcept;

    /**
     * @brief Set the socket's send and receive buffer sizes (SO_SNDBUF, SO_RCVBUF).
     * @param size The desired buffer size in bytes for both send and receive buffers.
     * @return 0 on success for both options, or a non-zero error code if setting either option fails.
     */
    int set_buffer_size(std::size_t size) noexcept;

    /**
     * @brief Enable or disable the SO_BROADCAST socket option.
     * @param enable `true` to enable broadcasting, `false` to disable.
     * @return 0 on success, or a non-zero error code on failure.
     * @details Enabling this allows the socket to send packets to a broadcast address.
     */
    int set_broadcast(bool enable) noexcept;

    /**
     * @brief Join an IPv4 or IPv6 multicast group.
     * @param group Multicast group address string (e.g., "224.0.0.1" or "ff02::1").
     * @param iface Optional local interface address string or interface name to join the group on.
     *              If empty or null, the system chooses a default interface.
     * @return 0 on success, or a non-zero error code on failure.
     * @details Uses `IP_ADD_MEMBERSHIP` or `IPV6_JOIN_GROUP` socket options.
     */
    int join_multicast_group(const std::string &group, 
                             const std::string &iface = "") noexcept;

    /**
     * @brief Leave an IPv4 or IPv6 multicast group.
     * @param group Multicast group address string.
     * @param iface Optional local interface address string or interface name used when joining.
     * @return 0 on success, or a non-zero error code on failure.
     * @details Uses `IP_DROP_MEMBERSHIP` or `IPV6_LEAVE_GROUP` socket options.
     */
    int leave_multicast_group(const std::string &group,
                              const std::string &iface = "") noexcept;

    /**
     * @brief Set the multicast Time-To-Live (TTL) for outgoing IPv4 packets or hop limit for IPv6.
     * @param ttl The TTL (for IPv4, typically 1-255) or hop limit (for IPv6) value.
     *            A TTL of 0 restricts packets to the local host.
     *            A TTL of 1 restricts packets to the local subnet.
     * @return 0 on success, or a non-zero error code on failure.
     * @details Uses `IP_MULTICAST_TTL` or `IPV6_MULTICAST_HOPS` socket options.
     */
    int set_multicast_ttl(int ttl) noexcept;

    /**
     * @brief Set whether multicast packets sent from this socket are looped back to the local host.
     * @param enable `true` to enable multicast loopback (sender receives its own packets),
     *               `false` to disable (default is usually enabled).
     * @return 0 on success, or a non-zero error code on failure.
     * @details Uses `IP_MULTICAST_LOOP` or `IPV6_MULTICAST_LOOP` socket options.
     */
    int set_multicast_loopback(bool enable) noexcept;

    /**
     * @brief Get the address family of this UDP socket (e.g., AF_INET, AF_INET6).
     * @return The integer value representing the address family.
     * @details Retrieves this from the underlying `qb::io::socket`'s endpoint information if bound,
     *          or the family it was initialized with.
     */
    int address_family() const noexcept;

    /**
     * @brief Check if the UDP socket is currently bound to a local address and port.
     * @return `true` if the socket is bound, `false` otherwise.
     * @details A UDP socket must be bound to receive unicast messages or to send packets
     *          from a specific local port.
     */
    bool is_bound() const noexcept;

    /**
     * @brief Disconnect a previously connected UDP socket or clear default remote endpoint.
     * @return 0 on success, or a non-zero error code on failure.
     * @details For UDP, `connect` can be used to set a default destination for `send()` and to filter
     *          incoming packets to only those from that destination. This `disconnect` call removes
     *          that association. It then calls `qb::io::socket::close()`.
     */
    int disconnect() const noexcept;
};

} // namespace qb::io::udp

#endif // QB_IO_UDP_SOCKET_H_
