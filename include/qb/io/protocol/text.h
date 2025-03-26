/**
 * @file qb/io/protocol/text.h
 * @brief Protocols for processing text and binary messages
 * 
 * This file defines specialized protocols based on the base protocols
 * to handle different types of text and binary messages. It provides
 * implementations for strings terminated by various delimiters and
 * binary data preceded by their size.
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

#ifndef QB_IO_PROT_TEXT_H_
#define QB_IO_PROT_TEXT_H_
#include "base.h"

namespace qb::protocol::text {

/**
 * @class basic_text
 * @brief Protocol for text messages delimited by a character
 * 
 * This class implements a protocol to handle text messages
 * that are delimited by a specific character. It uses the
 * byte_terminated protocol as a base and exposes the message
 * content as a structure containing data and text.
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 * @tparam _StringTrait Text type (std::string, std::string_view, etc.)
 * @tparam _Sep Delimiter character (default '\0')
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
        const char *data;       /**< Pointer to the raw data */
        _StringTrait text;      /**< The message as text */
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
        const auto parsed = this->shiftSize(size);
        this->_io.on(message{parsed, buffer.cbegin(), {buffer.cbegin(), parsed}});
    }
};

/**
 * @class basic_binary
 * @brief Protocol for binary messages preceded by their size
 * 
 * This class implements a protocol to handle binary messages
 * that are preceded by a header containing their size. It uses the
 * size_as_header protocol as a base.
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 * @tparam _SizeHeader Size header type (default uint16_t)
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
        const char *data;       /**< Pointer to the raw data */
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
 * @brief Binary protocol with 8-bit header
 * 
 * Specialization of basic_binary using a uint8_t size header (0-255 bytes).
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 */
template <typename _IO_>
using binary8 = basic_binary<_IO_, uint8_t>;

/**
 * @typedef binary16
 * @brief Binary protocol with 16-bit header
 * 
 * Specialization of basic_binary using a uint16_t size header (0-65535 bytes).
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 */
template <typename _IO_>
using binary16 = basic_binary<_IO_, uint16_t>;

/**
 * @typedef binary32
 * @brief Binary protocol with 32-bit header
 * 
 * Specialization of basic_binary using a uint32_t size header (0-4GB).
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 */
template <typename _IO_>
using binary32 = basic_binary<_IO_, uint32_t>;

/**
 * @typedef string
 * @brief Protocol for NULL-terminated strings
 * 
 * Specialization of basic_text for std::string strings terminated by '\0'.
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 */
template <typename _IO_>
using string = basic_text<_IO_, std::string, '\0'>;

/**
 * @typedef command
 * @brief Protocol for newline-terminated commands
 * 
 * Specialization of basic_text for std::string commands terminated by '\n'.
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 */
template <typename _IO_>
using command = basic_text<_IO_, std::string, '\n'>;

/**
 * @typedef string_view
 * @brief Protocol for NULL-terminated strings with view
 * 
 * Specialization of basic_text for std::string_view strings terminated by '\0'.
 * Used for copy-free operations.
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 */
template <typename _IO_>
using string_view = basic_text<_IO_, const std::string_view, '\0'>;

/**
 * @typedef command_view
 * @brief Protocol for newline-terminated commands with view
 * 
 * Specialization of basic_text for std::string_view commands terminated by '\n'.
 * Used for copy-free operations.
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 */
template <typename _IO_>
using command_view = basic_text<_IO_, const std::string_view, '\n'>;

} // namespace qb::protocol::text

#endif // QB_IO_PROT_TEXT_H_
