/**
 * @file qb/io/transport/udp.h
 * @brief UDP transport implementation for the QB IO library
 * 
 * This file provides a transport implementation for UDP sockets,
 * extending the stream class with UDP-specific functionality including
 * support for datagram-based communication, endpoint identity management,
 * and message buffering.
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
#include "../stream.h"
#include "../udp/socket.h"
#include <qb/utility/functional.h>

namespace qb::io::transport {

/**
 * @class udp
 * @brief UDP transport class
 * 
 * This class implements a transport layer for UDP sockets by extending
 * the generic stream class with UDP-specific functionality. It provides
 * support for identity tracking, message buffering, and datagram-based
 * communication operations.
 */
class udp : public stream<io::udp::socket> {
    using base_t = stream<io::udp::socket>;

public:
    /**
     * @brief Indicates that the implementation resets pending reads
     * 
     * This flag indicates that this transport implementation resets
     * when a read operation is pending.
     */
    constexpr static const bool has_reset_on_pending_read = true;

    /**
     * @struct identity
     * @brief Identifies a UDP endpoint
     * 
     * This structure extends qb::io::endpoint to provide identity
     * management for UDP connections, including support for hashing
     * and comparison operations.
     */
    struct identity : public qb::io::endpoint {
        /** @brief Default constructor */
        identity() = default;
        
        /**
         * @brief Construct from an endpoint
         * @param ep Endpoint to copy from
         */
        identity(qb::io::endpoint const &ep)
            : qb::io::endpoint(ep) {}

        /**
         * @struct hasher
         * @brief Hash function for UDP identities
         * 
         * This struct provides a hashing operation for using identity
         * objects in unordered containers.
         */
        struct hasher {
            /**
             * @brief Hash operator
             * @param id Identity to hash
             * @return Hash value
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
     * @brief Proxy for output operations
     * 
     * This class provides a stream-like interface for sending data
     * through the UDP transport.
     */
    class ProxyOut {
        udp &proxy; /**< Reference to the UDP transport */

    public:
        /**
         * @brief Constructor
         * @param prx Reference to the UDP transport
         */
        ProxyOut(udp &prx)
            : proxy(prx) {}

        /**
         * @brief Stream output operator
         * @tparam T Type of data to output
         * @param data Data to send
         * @return Reference to this ProxyOut
         */
        template <typename T>
        auto &
        operator<<(T &&data) {
            auto &out_buffer = static_cast<udp::base_t &>(proxy).out();
            const auto start_size = out_buffer.size();
            out_buffer << std::forward<T>(data);
            auto p = reinterpret_cast<udp::pushed_message *>(out_buffer.begin() +
                                                             proxy._last_pushed_offset);
            p->size += (out_buffer.size() - start_size);
            return *this;
        }

        /**
         * @brief Get the size of the pending write buffer
         * @return Number of bytes pending for write
         */
        std::size_t
        size() {
            return static_cast<udp::base_t &>(proxy).pendingWrite();
        }
    };

    /** @brief ProxyOut needs access to private members */
    friend ProxyOut;

private:
    ProxyOut _out{*this};         /**< Output proxy */
    udp::identity _remote_source; /**< Source identity for received messages */
    udp::identity _remote_dest;   /**< Destination identity for outgoing messages */

    /**
     * @struct pushed_message
     * @brief Structure representing a message in the output buffer
     * 
     * This structure contains metadata about a message that has been
     * pushed to the output buffer but not yet sent.
     */
    struct pushed_message {
        udp::identity ident; /**< Destination identity */
        int size = 0;        /**< Size of the message in bytes */
        int offset = 0;      /**< Current offset in the message for partial sends */
    };

    int _last_pushed_offset = -1; /**< Offset of the last pushed message in the buffer */

public:
    /**
     * @brief Get the source identity of the last received message
     * @return Constant reference to the source identity
     */
    const udp::identity &
    getSource() const noexcept {
        return _remote_source;
    }

    /**
     * @brief Set the destination for outgoing messages
     * @param to Destination identity
     * 
     * Sets the destination for subsequent outgoing messages.
     * If the destination changes or the output buffer is empty,
     * a new message header will be created on the next write.
     */
    void
    setDestination(udp::identity const &to) noexcept {
        if (to != _remote_dest || !_out_buffer.size()) {
            _remote_dest = to;
            _last_pushed_offset = -1;
        }
    }

    /**
     * @brief Get the output proxy
     * @return Reference to the output proxy
     * 
     * Prepares the output buffer for writing if necessary by
     * creating a new message header if the last pushed offset
     * is negative.
     */
    auto &
    out() {
        if (_last_pushed_offset < 0) {
            _last_pushed_offset = static_cast<int>(_out_buffer.size());
            auto &m = _out_buffer.allocate_back<pushed_message>();
            m.ident = _remote_dest;
            m.size = 0;
        }

        return _out;
    }

    /**
     * @brief Read data from the UDP socket
     * @return Number of bytes read on success, error code on failure
     * 
     * Reads a datagram from the UDP socket into the input buffer
     * and sets the destination for replies to the source of the
     * received message.
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
     * @brief Write data to the UDP socket
     * @return Number of bytes written on success, error code on failure
     * 
     * Writes the next message from the output buffer to the UDP socket.
     * If the message is completely sent, it is removed from the output buffer.
     */
    int
    write() noexcept {
        if (!_out_buffer.size())
            return 0;

        auto &msg = *reinterpret_cast<pushed_message *>(_out_buffer.begin());
        auto begin = _out_buffer.begin() + sizeof(pushed_message) + msg.offset;

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
     * @brief Publish data to the current destination
     * @param data Pointer to the data to publish
     * @param size Size of the data to publish
     * @return Pointer to the copied data in the output buffer
     * 
     * Copies the data into the output buffer for sending to the
     * current destination.
     */
    char *
    publish(char const *data, std::size_t size) noexcept {
        return publish_to(_remote_dest, data, size);
    }

    /**
     * @brief Publish data to a specific destination
     * @param to Destination identity
     * @param data Pointer to the data to publish
     * @param size Size of the data to publish
     * @return Pointer to the copied data in the output buffer
     * 
     * Copies the data into the output buffer for sending to the
     * specified destination.
     */
    char *
    publish_to(udp::identity const &to, char const *data, std::size_t size) noexcept {
        auto &m = _out_buffer.allocate_back<pushed_message>();
        m.ident = to;
        m.size = static_cast<int>(size);

        return static_cast<char *>(
            std::memcpy(_out_buffer.allocate_back(size), data, size));
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_UDP_H_
