/**
 * @file qb/io/transport/udp.h
 * @brief UDP datagram transport implementation for the QB IO library.
 *
 * This file provides a transport implementation for UDP sockets,
 * extending the `qb::io::stream` class with UDP-specific functionality including
 * support for datagram-based communication, endpoint identity management (`udp::identity`),
 * and message buffering with destination tracking.
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

#ifndef QB_IO_TRANSPORT_UDP_H_
#define QB_IO_TRANSPORT_UDP_H_
#include <qb/utility/functional.h>
#include "../stream.h"
#include "../udp/socket.h"

namespace qb::io::transport {

/**
 * @class udp
 * @ingroup Transport
 * @brief UDP transport providing connectionless, datagram-based communication.
 *
 * This class implements a transport layer for UDP sockets by extending
 * the generic `qb::io::stream` class, specializing it with `qb::io::udp::socket`.
 * It provides support for identity tracking of remote endpoints, message buffering
 * specific to datagrams, and methods for sending and receiving UDP packets.
 */
class udp : public stream<io::udp::socket> {
    using base_t = stream<io::udp::socket>;

public:
    /** @brief Indicates that this transport implementation is not secure */
    constexpr bool is_secure() const noexcept { return false; }
    /**
     * @brief Indicates that this transport implementation resets its input buffer state
     *        when a read operation is pending (characteristic of datagram processing).
     */
    constexpr static const bool has_reset_on_pending_read = true;

    /**
     * @struct identity
     * @ingroup Transport
     * @brief Identifies a UDP endpoint, extending `qb::io::endpoint` with hashing support.
     *
     * This structure is used to represent the source or destination of a UDP datagram.
     * It inherits from `qb::io::endpoint` and adds a custom hasher for use in
     * unordered collections like `qb::unordered_map`.
     */
    struct identity : public qb::io::endpoint {
        /** @brief Default constructor. */
        identity() = default;

        /**
         * @brief Construct from an existing `qb::io::endpoint`.
         * @param ep The endpoint to copy identity information from.
         */
        identity(qb::io::endpoint const &ep)
            : qb::io::endpoint(ep) {}

        /**
         * @struct hasher
         * @brief Hash function for `udp::identity` objects.
         *
         * This struct provides a hashing operation suitable for using `udp::identity`
         * objects as keys in standard C++ unordered containers.
         * It hashes the raw memory representation of the underlying endpoint data.
         */
        struct hasher {
            /**
             * @brief Hash operator.
             * @param id The `udp::identity` to hash.
             * @return `std::size_t` hash value.
             */
            std::size_t
            operator()(const identity &id) const noexcept {
                return std::hash<std::string_view>{}(
                    std::string_view(reinterpret_cast<const char *>(&id), id.len()));
            }
        };

        /**
         * @brief Inequality operator
         * @param rhs Right-hand side identity
         * @return true if identities differ, false otherwise
         */
        bool
        operator!=(identity const &rhs) const noexcept {
            return std::string_view(reinterpret_cast<const char *>(this), len()) !=
                   std::string_view(reinterpret_cast<const char *>(&rhs), rhs.len());
        }
    };

    /**
     * @class ProxyOut
     * @ingroup Transport
     * @brief Proxy class providing a stream-like interface for sending UDP datagrams.
     *
     * This class allows data to be written using `operator<<` and ensures that
     * the data is correctly associated with the current destination `udp::identity`
     * and buffered as a distinct datagram in the UDP transport's output buffer.
     */
    class ProxyOut {
        udp &proxy; /**< Reference to the parent UDP transport instance. */

    public:
        /**
         * @brief Constructor.
         * @param prx Reference to the `udp` transport this proxy will operate on.
         */
        ProxyOut(udp &prx)
            : proxy(prx) {}

        /**
         * @brief Stream output operator to append data to the current datagram being built.
         * @tparam T Type of data to output.
         * @param data Data to send.
         * @return Reference to this `ProxyOut` for chaining.
         * @details Appends data to the current message in the UDP transport's output buffer.
         *          Manages the size tracking for the current datagram being constructed.
         */
        template <typename T>
        auto &
        operator<<(T &&data) {
            auto      &out_buffer = static_cast<udp::base_t &>(proxy).out();
            const auto start_size = out_buffer.size();
            out_buffer << std::forward<T>(data);
            auto p = reinterpret_cast<udp::pushed_message *>(out_buffer.begin() +
                                                             proxy._last_pushed_offset);
            p->size += (out_buffer.size() - start_size);
            return *this;
        }

        /**
         * @brief Get the total size of data currently pending in the transport's output buffer for writing.
         * @return Number of bytes pending for write across all datagrams in the buffer.
         */
        std::size_t
        size() {
            return static_cast<udp::base_t &>(proxy).pendingWrite();
        }
    };

    /** @brief ProxyOut needs access to private members */
    friend ProxyOut;

private:
    ProxyOut      _out{*this};    /**< Output proxy for stream-like sending. */
    udp::identity _remote_source; /**< Source `udp::identity` of the last received datagram. */
    udp::identity _remote_dest;   /**< Destination `udp::identity` for outgoing datagrams when using the `_out` proxy. */

    /**
     * @struct pushed_message
     * @brief Internal structure representing a datagram message in the output buffer.
     * @private
     */
    struct pushed_message {
        udp::identity ident;      /**< Destination identity */
        int           size   = 0; /**< Size of the message in bytes */
        int           offset = 0; /**< Current offset in the message for partial sends */
    };

    int _last_pushed_offset = -1; /**< Offset of the last pushed message in the buffer */

public:
    /**
     * @brief Get the source `udp::identity` (endpoint) of the last successfully received datagram.
     * @return Constant reference to the source `udp::identity`.
     * @note This is updated by the `read()` method upon successful reception of a datagram.
     */
    const udp::identity &
    getSource() const noexcept {
        return _remote_source;
    }

    /**
     * @brief Set the destination `udp::identity` for subsequent outgoing datagrams sent via `out()` or `operator<<`.
     * @param to The `udp::identity` of the remote endpoint to send to.
     * @details If the destination changes from the previously set one, or if the output buffer is empty,
     *          a new datagram header (`pushed_message`) will be created in the output buffer
     *          upon the next write operation via `out()`.
     */
    void
    setDestination(udp::identity const &to) noexcept {
        if (to != _remote_dest || !_out_buffer.size()) {
            _remote_dest        = to;
            _last_pushed_offset = -1;
        }
    }

    /**
     * @brief Get the output proxy (`ProxyOut`) for stream-like sending to the current destination.
     * @return Reference to the `ProxyOut` instance.
     * @details If no datagram is currently being constructed for the current `_remote_dest`,
     *          this method initializes a new `pushed_message` header in the output buffer.
     *          Subsequent writes via the returned proxy will append to this datagram.
     */
    auto &
    out() {
        if (_last_pushed_offset < 0) {
            _last_pushed_offset = static_cast<int>(_out_buffer.size());
            auto &m             = _out_buffer.allocate_back<pushed_message>();
            m.ident             = _remote_dest;
            m.size              = 0;
        }

        return _out;
    }

    /**
     * @brief Read a single datagram from the UDP socket.
     * @return Number of bytes read on success (size of the datagram).
     *         Returns a negative value on error (e.g., from `socket::recvfrom`).
     *         Returns 0 if the read operation would block (in non-blocking mode and no data available).
     * @details Reads a datagram into the internal input buffer (`_in_buffer`).
     *          Upon successful read, `_remote_source` is updated with the sender's endpoint,
     *          and `setDestination(_remote_source)` is called to set this as the default reply-to target.
     *          The maximum datagram size read is `io::udp::socket::MaxDatagramSize`.
     */
    int
    read() noexcept {
        const auto ret =
            transport().read(_in_buffer.allocate_back(io::udp::socket::MaxDatagramSize),
                             io::udp::socket::MaxDatagramSize, _remote_source);
        if (qb::likely(ret > 0)) {
            _in_buffer.free_back(io::udp::socket::MaxDatagramSize - ret);
            setDestination(_remote_source);
        }
        return ret;
    }

    /**
     * @brief Write the next complete datagram from the output buffer to its destination.
     * @return Number of bytes successfully written from the datagram.
     *         Returns a negative value on error (e.g., from `socket::sendto`).
     *         Returns 0 if the output buffer is empty.
     * @details Attempts to send the first datagram queued in the `_out_buffer`.
     *          A datagram might be sent in multiple chunks if it exceeds `io::udp::socket::MaxDatagramSize`,
     *          though typically UDP sends entire datagrams or fails.
     *          If a datagram is completely sent, it's removed from the `_out_buffer`.
     *          Manages partial sends by updating `pushed_message::offset`.
     */
    int
    write() noexcept {
        if (!_out_buffer.size())
            return 0;

        auto &msg   = *reinterpret_cast<pushed_message *>(_out_buffer.begin());
        auto  begin = _out_buffer.begin() + sizeof(pushed_message) + msg.offset;

        const auto ret = transport().write(
            begin,
            std::min(msg.size - msg.offset,
                     static_cast<int>(io::udp::socket::MaxDatagramSize)),
            msg.ident);
        if (qb::likely(ret > 0)) {
            msg.offset += ret;

            if (msg.offset == msg.size) {
                _out_buffer.free_front(msg.size + sizeof(pushed_message));
                if (_out_buffer.size()) {
                    _out_buffer.reorder();
                    _last_pushed_offset = -1;
                } else
                    _out_buffer.reset();
            }
        }

        return ret;
    }

    /**
     * @brief Publish (enqueue) data to be sent to the current default destination (`_remote_dest`).
     * @param data Pointer to the data to publish.
     * @param size Size of the data in bytes.
     * @return Pointer to the copied data within the output buffer.
     * @details This is a convenience method that calls `publish_to(_remote_dest, data, size)`.
     *          The data is added as a new datagram or appended to the current one being built for `_remote_dest`.
     */
    char *
    publish(char const *data, std::size_t size) noexcept {
        return publish_to(_remote_dest, data, size);
    }

    /**
     * @brief Publish (enqueue) data to be sent to a specific `udp::identity` destination.
     * @param to The destination `udp::identity` (endpoint).
     * @param data Pointer to the data to publish.
     * @param size Size of the data in bytes.
     * @return Pointer to the copied data within the output buffer.
     * @details Adds the data as a new datagram in the `_out_buffer`, targeting the specified `to` endpoint.
     *          A `pushed_message` header is prepended to manage this datagram.
     */
    char *
    publish_to(udp::identity const &to, char const *data, std::size_t size) noexcept {
        auto &m = _out_buffer.allocate_back<pushed_message>();
        m.ident = to;
        m.size  = static_cast<int>(size);

        return static_cast<char *>(
            std::memcpy(_out_buffer.allocate_back(size), data, size));
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_UDP_H_
