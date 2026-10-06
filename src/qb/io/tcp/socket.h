/**
 * @file qb/io/tcp/socket.h
 * @brief Implementation of TCP sockets for the QB IO library.
 *
 * This file provides the implementation of TCP sockets supporting
 * synchronous and asynchronous connections to IPv4, IPv6, and Unix sockets.
 * It builds upon the generic `qb::io::socket` wrapper.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * @ingroup TCP
 */

#ifndef QB_IO_TCP_SOCKET_H_
#define QB_IO_TCP_SOCKET_H_
#include "../system/sys__socket.h"
#include "../uri.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
#include <qb/system/time.h>

namespace qb::io::tcp {

/*!
 * @class socket
 * @ingroup TCP
 * @brief Class implementing TCP socket functionality for reliable, stream-oriented communication.
 *
 * This class provides TCP socket functionality, inheriting protectedly from the base `qb::io::socket`
 * and exposing a TCP-specific interface. It supports connecting to and communicating over
 * TCP/IPv4, TCP/IPv6, and Unix Domain Sockets (if enabled).
 * It is used as the underlying I/O primitive for `qb::io::transport::tcp`.
 */
class QB_API socket : protected qb::io::socket {
    /**
     * @brief Internal method to connect to an address with a specific address family.
     * @param af Address family (AF_INET for IPv4, AF_INET6 for IPv6).
     * @param host Host address string (IP or hostname).
     * @param port Port number to connect to.
     * @return 0 on success, a non-zero error code (typically negative) on failure.
     * @private
     */
    int connect_in(int af, std::string const &host, uint16_t port) noexcept;

    int connect_in(int af, std::string const &host, uint16_t port, qb::duration wtimeout) noexcept;

    /**
     * @brief Internal method to perform a non-blocking connect.
     * @param af Address family (AF_INET for IPv4, AF_INET6 for IPv6).
     * @param host Host address string (IP or hostname).
     * @param port Port number to connect to.
     * @return 0 if connection is in progress or succeeded immediately, a non-zero error code on immediate failure.
     * @private
     */
    int n_connect_in(int af, std::string const &host, uint16_t port) noexcept;

public:
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

    /** @brief Indicates that this socket implementation is not secure */
    constexpr static bool
    is_secure() noexcept {
        return false;
    }
    /**
     * @brief Default constructor. Creates an uninitialized TCP socket.
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
     * @param sock A generic `qb::io::socket` instance, typically already opened with `SOCK_STREAM`.
     *             The state of `sock` is moved into this `tcp::socket`.
     */
    socket(io::socket &&sock) noexcept;

    /**
     * @brief Move assignment operator from a generic `qb::io::socket`.
     * @param sock A generic `qb::io::socket` to move from.
     * @return Reference to this `tcp::socket`.
     */
    socket &operator=(io::socket &&sock) noexcept;

    /**
     * @brief Initialize (open) the TCP socket.
     * @param af Address family (e.g., `AF_INET`, `AF_INET6`, `AF_UNIX`). Defaults to `AF_INET`.
     * @return 0 on success, or a non-zero error code on failure.
     * @details This method calls the base `qb::io::socket::open()` with `SOCK_STREAM` type.
     */
    int init(int af = AF_INET) noexcept;

    /**
     * @brief Bind the socket to a specific local endpoint.
     * @param ep The `qb::io::endpoint` to bind to.
     * @return 0 on success, or a non-zero error code on failure.
     * @see qb::io::socket::bind(const endpoint &)
     */
    int bind(qb::io::endpoint const &ep) noexcept;

    /**
     * @brief Bind the socket to an endpoint specified by a URI.
     * @param u The `qb::io::uri` specifying the local address and port to bind to.
     * @return 0 on success, or a non-zero error code on failure.
     */
    int bind(qb::io::uri const &u) noexcept;

    /**
     * @brief Connect to a remote TCP endpoint.
     * @param ep The `qb::io::endpoint` of the remote server.
     * @return 0 on success (blocking connect), or a non-zero error code on failure.
     * @see qb::io::socket::connect(const endpoint &)
     */
    int connect(qb::io::endpoint const &ep) noexcept;

    /**
     * @brief Connect to a remote TCP endpoint with a wall-clock bound on the TCP handshake.
     * @param ep Remote endpoint.
     * @param wtimeout Maximum time to wait; non-positive values are clamped to zero (poll once).
     * @return 0 on success; non-zero on failure (including timeout; see `get_last_errno()`).
     */
    int connect(qb::io::endpoint const &ep, qb::duration wtimeout) noexcept;

    /**
     * @brief Connect to the first of `endpoints` that answers, trying them in order (since 3.3).
     * @param endpoints Addresses to try; an empty list is a failure.
     * @return 0 once one connected; otherwise the last attempt's result (`get_last_errno()` is its
     *         error), -1 for an empty list.
     * @details The fallback every connect by name runs (Huly QB-164): an address that refuses or does
     *          not route is closed and the next is tried. Only a descriptor this socket opens itself is
     *          replaced between two attempts -- a socket already open when the call begins may carry
     *          options a fresh descriptor would not, so it gets the first address only.
     */
    int connect(std::vector<qb::io::endpoint> const &endpoints) noexcept;

    /**
     * @brief `connect(endpoints)` within `wtimeout`, shared between the attempts.
     * @details Each attempt gets `connect_attempt_budget(time left, addresses left)`: an address that does
     *          not answer gives way to the next before the whole bound is spent. Non-positive `wtimeout`:
     *          one immediate check of the first address, as `connect(ep, wtimeout)` does.
     */
    int connect(std::vector<qb::io::endpoint> const &endpoints, qb::duration wtimeout) noexcept;

    /**
     * @brief Connect to a remote TCP endpoint specified by a URI.
     * @param u The `qb::io::uri` of the remote server.
     * @return 0 on success, or a non-zero error code on failure.
     * @details A host name is resolved to every address of the URI's family, tried in order with
     *          `connect(endpoints)` (since 3.3; before, the first address only).
     */
    int connect(uri const &u) noexcept;

    /**
     * @brief Connect using a URI with a bound TCP handshake duration.
     */
    int connect(uri const &u, qb::duration wtimeout) noexcept;

    /**
     * @brief Connect to an IPv4 TCP server.
     * @param host The hostname or IP address string of the server.
     * @param port The port number of the server.
     * @return 0 on success, or a non-zero error code on failure.
     */
    int connect_v4(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Connect to an IPv6 TCP server.
     * @param host The hostname or IP address string of the server.
     * @param port The port number of the server.
     * @return 0 on success, or a non-zero error code on failure.
     */
    int connect_v6(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Connect to a Unix domain TCP-style socket.
     * @param path The file system path of the Unix domain socket.
     * @return 0 on success, or a non-zero error code on failure.
     * @note This is only effective if `QB_ENABLE_UDS` is active and the system supports AF_UNIX.
     */
    int connect_un(std::filesystem::path const &path) noexcept;

    /**
     * @brief Initiate a non-blocking connect to a remote TCP endpoint.
     * @param ep The `qb::io::endpoint` of the remote server.
     * @return 0 if connection is in progress (use `select` or an event loop to check for completion)
     *         or succeeded immediately. Returns a non-zero error code on immediate failure.
     * @details Sets the socket to non-blocking before attempting to connect.
     *          The `connected()` method (or `handle_write_ready`) should be checked later to confirm connection.
     * @see qb::io::socket::connect_n(const endpoint &)
     */
    int n_connect(qb::io::endpoint const &ep) noexcept;

    /**
     * @brief Called after a non-blocking connect attempt succeeds or needs to be finalized.
     * @details For TCP, this method typically checks for socket errors to confirm successful connection after `n_connect` indicated progress.
     *          In this base `tcp::socket`, it's a no-op but can be overridden by derived classes (like `ssl::socket` for handshake).
     */
    inline void
    connected() noexcept {}

    /**
     * @brief Initiate a non-blocking connect to a remote TCP endpoint specified by a URI.
     * @param u The `qb::io::uri` of the remote server.
     * @return 0 if connection is in progress or succeeded immediately, non-zero error code on immediate failure.
     */
    int n_connect(uri const &u) noexcept;

    /**
     * @brief Initiate a non-blocking connect to an IPv4 TCP server.
     * @param host The hostname or IP address string of the server.
     * @param port The port number of the server.
     * @return 0 if connection is in progress or succeeded immediately, non-zero error code on immediate failure.
     */
    int n_connect_v4(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Initiate a non-blocking connect to an IPv6 TCP server.
     * @param host The hostname or IP address string of the server.
     * @param port The port number of the server.
     * @return 0 if connection is in progress or succeeded immediately, non-zero error code on immediate failure.
     */
    int n_connect_v6(std::string const &host, uint16_t port) noexcept;

    /**
     * @brief Initiate a non-blocking connect to a Unix domain TCP-style socket.
     * @param path The file system path of the Unix domain socket.
     * @return 0 if connection is in progress or succeeded immediately, non-zero error code on immediate failure.
     */
    int n_connect_un(std::filesystem::path const &path) noexcept;

    /**
     * @brief Read data from the connected TCP socket.
     * @param dest Pointer to the buffer where received data will be stored.
     * @param len Maximum number of bytes to read into the buffer.
     * @return Number of bytes actually read. `0` indicates graceful shutdown by the peer.
     *         A negative value indicates an error (e.g., `WSAEWOULDBLOCK` or `EAGAIN` if non-blocking and no data).
     * @see qb::io::socket::recv(void*, int, int)
     */
    int read(void *dest, std::size_t len) const noexcept;

    /**
     * @brief Write data to the connected TCP socket.
     * @param data Pointer to the data to be sent.
     * @param size Number of bytes to send from the `data` buffer.
     * @return Number of bytes actually written. This can be less than `size` if the send buffer is full.
     *         A negative value indicates an error.
     * @see qb::io::socket::send(const void*, int, int)
     */
    int write(const void *data, std::size_t size) const noexcept;

    /**
     * @brief Disconnect the TCP socket.
     * @return 0 on success, or a non-zero error code on failure.
     * @details This typically calls `qb::io::socket::shutdown()` with `QB_SD_BOTH` and then `close()`.
     */
    int disconnect() const noexcept;
};

/**
 * @brief The endpoints `host:port` resolves to in family `af`, in the resolver's order.
 * @param af   `AF_INET` or `AF_INET6`: only addresses of that family are kept (the family a URI names --
 *             `AF_INET` by default for a host name).
 * @param host A host name or a numeric address.
 * @param port The port every endpoint carries.
 * @return Every address the resolver returns for that family; empty when the name does not resolve.
 * @details The list a connect falls back through, since 3.3 (Huly QB-164): each `connect_in` and the
 *          asynchronous connector try the next address when one refuses or does not answer, where they
 *          used to stop at the first. A blocking `getaddrinfo` call, like the resolution it replaces.
 */
std::vector<qb::io::endpoint> resolve_endpoints(int af, std::string const &host, uint16_t port);

/**
 * @brief How long one connect attempt may take when `left` endpoints remain to try within `remaining`.
 * @return `remaining` shared equally between the `left` attempts, but never less than two seconds unless
 *         `remaining` itself is shorter; all of `remaining` when `left` is 1 or 0.
 * @details The rule of Go's dialer: an address that drops SYNs no longer eats the whole deadline before
 *          the next is tried, and a slow but live one still gets a fair attempt. Non-positive `remaining`:
 *          zero.
 */
qb::duration connect_attempt_budget(qb::duration remaining, std::size_t left) noexcept;

} // namespace qb::io::tcp

#endif // QB_IO_TCP_SOCKET_H_
