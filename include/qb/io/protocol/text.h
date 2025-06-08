/**
 * @file qb/io/protocol/text.h
 * @brief Protocols for processing text and binary messages in the QB IO system.
 *
 * This file defines specialized protocols based on the `qb::protocol::base` templates
 * to handle different types of text and binary messages. It provides concrete
 * implementations for strings terminated by various delimiters (like null or newline)
 * and binary data preceded by fixed-size length headers.
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

#ifndef QB_IO_PROT_TEXT_H_
#define QB_IO_PROT_TEXT_H_
#include "base.h"

namespace qb::protocol::text {

/**
 * @class basic_text
 * @ingroup Protocol
 * @brief Protocol for text messages delimited by a specific character, yielding a specified string type.
 *
 * This class implements a protocol to handle text messages that are delimited by
 * a specific character (`_Sep`). It uses the `qb::protocol::base::byte_terminated`
 * protocol as its base and, upon receiving a complete message, provides a `message`
 * struct containing the raw data, its size, and the text formatted as `_StringTrait`
 * (e.g., `std::string`, `std::string_view`) to the I/O component's handler.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 * @tparam _StringTrait The type of string to produce (e.g., `std::string`, `std::string_view`).
 * @tparam _Sep The delimiter character. Defaults to '\0'.
 */
template <typename _IO_, typename _StringTrait, char _Sep = '\0'>
class basic_text : public base::byte_terminated<_IO_, _Sep> {
public:
    /**
     * @brief Default constructor (deleted)
     */
    basic_text() = delete;

    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object that uses this protocol
     */
    basic_text(_IO_ &io) noexcept
        : base::byte_terminated<_IO_, _Sep>(io) {}

    /**
     * @struct message
     * @brief Structure representing a text message
     *
     * This structure contains information about a complete text message,
     * including its size, raw data, and the text formatted according
     * to the specified _StringTrait.
     */
    struct message {
        const std::size_t size; /**< Message size without the delimiter */
        const char       *data; /**< Pointer to the raw data */
        _StringTrait      text; /**< The message as text */
    };

    /**
     * @brief Process a received message
     *
     * This method is called when a complete message is received.
     * It builds a message object and passes it to the I/O object.
     *
     * @param size Message size with the delimiter
     */
    void
    onMessage(std::size_t size) noexcept {
        const auto &buffer = this->_io.in();
        const auto  parsed = this->shiftSize(size);
        this->_io.on(message{parsed, buffer.cbegin(), {buffer.cbegin(), parsed}});
    }
};

/**
 * @class basic_binary
 * @ingroup Protocol
 * @brief Protocol for binary messages where the payload is preceded by a fixed-size header indicating its length.
 *
 * This class implements a protocol to handle binary messages that are prefixed
 * with a header of type `_SizeHeader` containing their payload size. It uses the
 * `qb::protocol::base::size_as_header` protocol as its base. Upon receiving a complete
 * message, it provides a `message` struct containing the raw payload data pointer and its size
 * to the I/O component's handler.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 * @tparam _SizeHeader The integer type of the size header (e.g., `uint8_t`, `uint16_t`). Defaults to `uint16_t`.
 */
template <typename _IO_, typename _SizeHeader = uint16_t>
class basic_binary : public base::size_as_header<_IO_, _SizeHeader> {
public:
    /**
     * @brief Default constructor (deleted)
     */
    basic_binary() = delete;

    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object that uses this protocol
     */
    basic_binary(_IO_ &io) noexcept
        : base::size_as_header<_IO_, _SizeHeader>(io) {}

    /**
     * @struct message
     * @brief Structure representing a binary message
     *
     * This structure contains information about a complete binary message,
     * including its size and raw data.
     */
    struct message {
        const std::size_t size; /**< Message size */
        const char       *data; /**< Pointer to the raw data */
    };

    /**
     * @brief Process a received message
     *
     * This method is called when a complete message is received.
     * It builds a message object and passes it to the I/O object.
     *
     * @param size Message size
     */
    void
    onMessage(std::size_t size) const noexcept {
        this->_io.on(message{size, this->_io.in().cbegin() + this->shiftSize()});
    }
};

/**
 * @typedef binary8
 * @ingroup Protocol
 * @brief Binary protocol with an 8-bit (uint8_t) size header.
 *
 * Specialization of `basic_binary` using a `uint8_t` size header, suitable for
 * messages with payloads up to 255 bytes.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 */
template <typename _IO_>
using binary8 = basic_binary<_IO_, uint8_t>;

/**
 * @typedef binary16
 * @ingroup Protocol
 * @brief Binary protocol with a 16-bit (uint16_t) size header.
 *
 * Specialization of `basic_binary` using a `uint16_t` size header, suitable for
 * messages with payloads up to 65535 bytes. Handles network byte order for the header.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 */
template <typename _IO_>
using binary16 = basic_binary<_IO_, uint16_t>;

/**
 * @typedef binary32
 * @ingroup Protocol
 * @brief Binary protocol with a 32-bit (uint32_t) size header.
 *
 * Specialization of `basic_binary` using a `uint32_t` size header, suitable for
 * messages with payloads up to 4GB. Handles network byte order for the header.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 */
template <typename _IO_>
using binary32 = basic_binary<_IO_, uint32_t>;

/**
 * @typedef string
 * @ingroup Protocol
 * @brief Protocol for NULL-terminated (`\0`) strings, yielding `std::string`.
 *
 * Specialization of `basic_text` for handling standard C-style null-terminated strings.
 * The received message payload (excluding the terminator) is provided as `std::string`.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 */
template <typename _IO_>
using string = basic_text<_IO_, std::string, '\0'>;

/**
 * @typedef command
 * @ingroup Protocol
 * @brief Protocol for newline-terminated (`\n`) commands, yielding `std::string`.
 *
 * Specialization of `basic_text` for handling line-based commands or messages
 * where each message is terminated by a newline character.
 * The received message payload (excluding the newline) is provided as `std::string`.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 */
template <typename _IO_>
using command = basic_text<_IO_, std::string, '\n'>;

/**
 * @typedef string_view
 * @ingroup Protocol
 * @brief Protocol for NULL-terminated (`\0`) strings, yielding `std::string_view` (zero-copy read).
 *
 * Specialization of `basic_text` for handling null-terminated strings where the payload
 * is provided as a `std::string_view`. This allows for reading the string data
 * without an additional copy, provided the underlying buffer remains valid during the view's lifetime.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 */
template <typename _IO_>
using string_view = basic_text<_IO_, const std::string_view, '\0'>;

/**
 * @typedef command_view
 * @ingroup Protocol
 * @brief Protocol for newline-terminated (`\n`) commands, yielding `std::string_view` (zero-copy read).
 *
 * Specialization of `basic_text` for handling newline-terminated messages where the payload
 * is provided as a `std::string_view`. This offers zero-copy reading of line-based commands.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 */
template <typename _IO_>
using command_view = basic_text<_IO_, const std::string_view, '\n'>;

} // namespace qb::protocol::text

#endif // QB_IO_PROT_TEXT_H_
