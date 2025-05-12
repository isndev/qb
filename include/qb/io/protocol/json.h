/**
 * @file qb/io/protocol/json.h
 * @brief JSON protocol implementations for the QB IO system.
 *
 * This file contains protocol implementations for parsing and handling JSON messages.
 * It leverages the `nlohmann/json` library for JSON manipulation and provides
 * protocols for handling null-terminated JSON strings and MessagePack encoded JSON.
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

#ifndef QB_IO_PROTOCOL_JSON_H
#define QB_IO_PROTOCOL_JSON_H
#include "base.h"
#include "nlohmann/json.hpp"

namespace qb {
namespace protocol {

/**
 * @class json
 * @ingroup Protocol
 * @brief Protocol for parsing null-terminated JSON messages.
 *
 * This class implements a protocol to handle JSON messages that are expected
 * to be terminated by a NULL character (`'\0'`). It uses the
 * `qb::protocol::base::byte_terminated` protocol as its base and parses the received
 * data (excluding the terminator) as a JSON object using `nlohmann::json`.
 *
 * The `onMessage` method, when invoked by the framework, will provide a
 * `message` struct containing the raw data, its size, and the parsed `nlohmann::json` object
 * to the associated I/O component's handler.
 *
 * @tparam IO_ The I/O component type (e.g., a TCP session class) that will use this protocol.
 *             It must be compatible with `base::byte_terminated`.
 */
template <typename IO_>
class json : public base::byte_terminated<IO_, '\0'> {
public:
    /**
     * @brief Default constructor (deleted)
     */
    json() = delete;
    
    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object that uses this protocol
     */
    explicit json(IO_ &io) noexcept
        : base::byte_terminated<IO_, '\0'>(io) {}

    /**
     * @struct message
     * @brief Structure representing a JSON message
     *
     * This structure contains information about a complete JSON message,
     * including its size, raw data, and the parsed JSON object.
     */
    struct message {
        const std::size_t size; /**< Message size */
        const char       *data; /**< Pointer to the raw data */
        nlohmann::json    json; /**< Parsed JSON object */
    };

    /**
     * @brief Process a received message
     *
     * This method is called when a complete message is received.
     * It builds a message object with the parsed JSON and passes it to the I/O object.
     *
     * @param size Message size with the delimiter
     */
    void
    onMessage(std::size_t size) noexcept final {
        const auto parsed = this->shiftSize(size);
        const auto data   = this->_io.in().cbegin();

        this->_io.on(message{
            parsed, data,
            nlohmann::json::parse(std::string_view(data, parsed), nullptr, false)});
    }
};

/**
 * @class json_packed
 * @ingroup Protocol
 * @brief Protocol for parsing null-terminated, MessagePack encoded JSON messages.
 *
 * This class implements a protocol to handle messages that are MessagePack encoded JSON,
 * terminated by a NULL character (`'\0'`). It uses `base::byte_terminated` for framing
 * and then deserializes the MessagePack data into an `nlohmann::json` object.
 *
 * The `onMessage` method provides a `message` struct containing the raw MessagePack data,
 * its size, and the deserialized `nlohmann::json` object to the I/O component's handler.
 *
 * @tparam IO_ The I/O component type that will use this protocol.
 *             It must be compatible with `base::byte_terminated`.
 */
template <typename IO_>
class json_packed : public base::byte_terminated<IO_, '\0'> {
public:
    /**
     * @brief Default constructor (deleted)
     */
    json_packed() = delete;
    
    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object that uses this protocol
     */
    explicit json_packed(IO_ &io) noexcept
        : base::byte_terminated<IO_, '\0'>(io) {}

    /**
     * @struct message
     * @brief Structure representing a MessagePack encoded JSON message
     *
     * This structure contains information about a complete MessagePack message,
     * including its size, raw data, and the parsed JSON object.
     */
    struct message {
        const std::size_t size; /**< Message size */
        const char       *data; /**< Pointer to the raw data */
        nlohmann::json    json; /**< Parsed JSON object */
    };

    /**
     * @brief Process a received message
     *
     * This method is called when a complete message is received.
     * It builds a message object with the JSON parsed from MessagePack format
     * and passes it to the I/O object.
     *
     * @param size Message size with the delimiter
     */
    void
    onMessage(std::size_t size) noexcept final {
        const auto parsed = this->shiftSize(size);
        const auto data   = this->_io.in().cbegin();
        this->_io.on(message{
            parsed, data, nlohmann::json::from_msgpack(std::string_view(data, parsed))});
    }
};

} // namespace protocol
} // namespace qb
#endif // QB_IO_PROTOCOL_JSON_H