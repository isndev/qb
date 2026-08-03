/**
 * @file qb/io/protocol/base.h
 * @brief Base protocol implementations for message framing in the QB IO system.
 *
 * This file defines fundamental protocol templates used for common message framing
 * techniques, such as messages terminated by specific byte(s) or messages
 * prefixed by their size. These serve as building blocks for more specific
 * application-level or data-format-level protocols.
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
 * @ingroup IO
 */

#ifndef QB_IO_PROT_BASE_H_
#define QB_IO_PROT_BASE_H_
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <qb/system/allocator/pipe.h>
#include "../config.h"
#include "../async/protocol.h"

namespace qb::protocol::base {

/**
 * @class byte_terminated
 * @ingroup Protocol
 * @brief Protocol for messages delimited by a specific single byte character.
 *
 * This class implements a protocol where messages are framed by a specific
 * termination byte (e.g., '\0' for null-terminated strings, '\n' for line-based messages).
 * It inherits from `qb::io::async::AProtocol`.
 *
 * @tparam _IO_ The I/O component type (e.g., a TCP session class) that will use this protocol.
 *              It needs to provide an `in()` method returning a reference to its input buffer (e.g., `qb::allocator::pipe<char>`).
 * @tparam _EndByte The single byte character that marks the end of a message. Defaults to '\0'.
 */
template <typename _IO_, char _EndByte = '\0'>
class byte_terminated : public io::async::AProtocol<_IO_> {
    std::size_t _offset = 0u; /**< Position to resume delimiter search */

public:
    static constexpr std::size_t delimiter_size = 1;        /**< Delimiter size (1 byte) */
    static constexpr char        end            = _EndByte; /**< End character */

    /**
     * @brief Default constructor (deleted)
     */
    byte_terminated() = delete;

    /**
     * @brief Virtual destructor
     */
    virtual ~byte_terminated() = default;

    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object that uses this protocol
     */
    byte_terminated(_IO_ &io) noexcept
        : io::async::AProtocol<_IO_>(io) {}

    /**
     * @brief Calculates the message size without the delimiter
     *
     * @param size Total size including the delimiter
     * @return Message size without the delimiter
     */
    [[nodiscard]] inline std::size_t
    shiftSize(std::size_t const size) const noexcept {
        return size >= delimiter_size ? size - delimiter_size : 0;
    }

    /**
     * @brief Determines the size of the next complete message
     *
     * Searches for the termination byte in the input buffer.
     *
     * @return Message size if found, 0 otherwise
     */
    std::size_t
    getMessageSize() noexcept final {
        const auto &buffer = this->_io.in();
        auto        it     = buffer.begin() + _offset;
        while (it != buffer.end()) {
            if (*it == _EndByte) {
                _offset = 0;
                return static_cast<std::size_t>(it - buffer.begin()) + delimiter_size;
            }
            ++it;
        }
        _offset = it - buffer.begin();
        return 0;
    }

    /**
     * @brief Resets the protocol state
     */
    void
    reset() noexcept final {
        _offset = 0;
    }
};

/**
 * @class bytes_terminated
 * @ingroup Protocol
 * @brief Protocol for messages delimited by a specific sequence of bytes (a string literal).
 *
 * This class implements a protocol where messages are framed by a specific sequence
 * of bytes, defined via a trait struct providing the `_EndBytes` sequence.
 * For example, it can be used for HTTP-like messages ending in "\r\n\r\n".
 * It inherits from `qb::io::async::AProtocol`.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 *              It needs to provide an `in()` method for input buffer access.
 * @tparam _Trait A trait class that must define a static constexpr char array `_EndBytes`
 *                representing the termination sequence (e.g., `struct CRLF { static constexpr char _EndBytes[] = "\r\n"; };`).
 */
template <typename _IO_, typename _Trait>
class bytes_terminated : public io::async::AProtocol<_IO_> {
    static constexpr std::size_t _SizeBytes = sizeof(_Trait::_EndBytes) - 1; /**< Size of the termination sequence */
    static_assert(_SizeBytes > 0, "Delimiter sequence must not be empty");
    std::size_t _offset = 0u; /**< Position to resume delimiter search */

public:
    static constexpr std::size_t delimiter_size = _SizeBytes;        /**< Delimiter size */
    static constexpr auto        end            = _Trait::_EndBytes; /**< End sequence */

    /**
     * @brief Default constructor (deleted)
     */
    bytes_terminated() = delete;

    /**
     * @brief Virtual destructor
     */
    virtual ~bytes_terminated() = default;

    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object that uses this protocol
     */
    bytes_terminated(_IO_ &io) noexcept
        : io::async::AProtocol<_IO_>(io) {}

    /**
     * @brief Calculates the message size without the delimiter
     *
     * @param size Total size including the delimiter
     * @return Message size without the delimiter
     */
    [[nodiscard]] inline std::size_t
    shiftSize(std::size_t const size) const noexcept {
        return size >= delimiter_size ? size - delimiter_size : 0;
    }

    /**
     * @brief Determines the size of the next complete message
     *
     * Searches for the termination sequence in the input buffer.
     *
     * @return Message size if found, 0 otherwise
     */
    std::size_t
    getMessageSize() noexcept final {
        const auto &buffer = this->_io.in();
        auto        i      = buffer.begin() + _offset;

        if (static_cast<std::size_t>(buffer.end() - i) < _SizeBytes)
            return 0;

        const auto end = buffer.end() - _SizeBytes + 1;
        while (i != end) {
            if (!std::memcmp(i, _Trait::_EndBytes, _SizeBytes)) {
                _offset = 0; // reset
                return i - buffer.begin() + _SizeBytes;
            }
            ++i;
        }
        _offset = i - buffer.begin();
        return 0;
    }

    /**
     * @brief Resets the protocol state
     */
    void
    reset() noexcept final {
        _offset = 0;
    }
};

/**
 * @class size_as_header
 * @ingroup Protocol
 * @brief Protocol for messages where the payload is preceded by a fixed-size header indicating its length.
 *
 * This class implements a protocol where each message payload is prefixed
 * by an integer header (e.g., `uint8_t`, `uint16_t`, `uint32_t`) that specifies
 * the size of the upcoming payload. It handles network byte order conversion (e.g., `ntohs`, `ntohl`)
 * for 16-bit and 32-bit headers.
 * It inherits from `qb::io::async::AProtocol`.
 *
 * @tparam _IO_ The I/O component type that will use this protocol.
 *              It needs to provide an `in()` method for input buffer access.
 * @tparam _Size The integer type of the size header (e.g., `uint16_t`, `uint32_t`). Defaults to `uint16_t`.
 */
template <typename _IO_, typename _Size = uint16_t>
class size_as_header : public io::async::AProtocol<_IO_> {
    static constexpr std::size_t SIZEOF = sizeof(_Size); /**< Header size */
    _Size                        _size  = 0u;            /**< Size of the message being analyzed */

public:
    /**
     * @brief Default constructor
     */
    size_as_header() = delete;

    /**
     * @brief Constructor with I/O reference
     *
     * @param io Reference to the I/O object that uses this protocol
     */
    size_as_header(_IO_ &io) noexcept
        : io::async::AProtocol<_IO_>(io) {}

    /**
     * @brief Returns the size of the header
     *
     * @return Header size in bytes
     */
    inline std::size_t
    shiftSize() const noexcept {
        return SIZEOF;
    }

    /**
     * @brief Determines the size of the next complete message
     *
     * First reads the header to determine the message size,
     * then waits for the complete message to be available.
     *
     * @return Size of the message if complete, 0 otherwise
     */
    std::size_t
    getMessageSize() noexcept final {
        auto &buffer = this->_io.in();

        if (!_size && buffer.size() >= SIZEOF) {
            _Size raw;
            std::memcpy(&raw, buffer.begin(), SIZEOF);
            if constexpr (SIZEOF == 2)
                _size = ntohs(raw);
            else if constexpr (SIZEOF == 4)
                _size = ntohl(raw);
            else
                _size = raw;
            if (!_size) {
                this->not_ok();
                return 0;
            }
            buffer.free_front(SIZEOF);
        }
        if (_size && buffer.size() >= _size) {
            const auto ret = _size;
            _size          = 0;
            return ret;
        }
        return 0;
    }

    /**
     * @brief Creates a size header for a message
     *
     * Converts the message size into an appropriate header,
     * with endianness conversion if necessary.
     *
     * @param size Size of the message
     * @return Formatted size header
     */
    static _Size
    Header(std::size_t size) {
        static_assert(std::is_unsigned_v<_Size>, "size header must be unsigned");
        if (size > static_cast<std::size_t>(std::numeric_limits<_Size>::max()))
            throw std::runtime_error("payload size exceeds binary protocol header capacity");
        if constexpr (SIZEOF == 2) {
            return htons(static_cast<_Size>(size));
        } else if constexpr (SIZEOF == 4) {
            return htonl(static_cast<_Size>(size));
        } else {
            return static_cast<_Size>(size);
        }
    }

    /**
     * @brief Resets the protocol state
     */
    void
    reset() noexcept final {
        _size = 0;
    }
};

} // namespace qb::protocol::base

#endif // QB_IO_PROT_BASE_H_
