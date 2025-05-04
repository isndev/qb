/**
 * @file qb/io/protocol/json.h
 * @brief JSON protocol implementation
 *
 * This file contains the implementation of the JSON protocol for the IO system.
 * It provides protocols for parsing and handling JSON messages in different formats.
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
 * @ingroup Transport
 */

#ifndef QB_IO_PROTOCOL_JSON_H
#define QB_IO_PROTOCOL_JSON_H
#include "base.h"
#include "nlohmann/json.hpp"

namespace qb {
namespace protocol {

/**
 * @class json
 * @brief Protocol for parsing JSON messages
 *
 * This class implements a protocol to handle JSON messages
 * that are terminated by a NULL character. It uses the
 * byte_terminated protocol as a base and parses the received
 * data as JSON.
 *
 * @tparam IO_ The I/O type that will use this protocol
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
 * @brief Protocol for parsing MessagePack encoded JSON
 *
 * This class implements a protocol to handle MessagePack encoded JSON
 * that is terminated by a NULL character. It uses the byte_terminated
 * protocol as a base and parses the received data from MessagePack format.
 *
 * @tparam IO_ The I/O type that will use this protocol
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