/**
 * @file qb/io/protocol/base.h
 * @brief Base protocol implementations for the IO system
 * 
 * This file defines the basic protocols used for communication in the IO system,
 * including size-based message framing and data transformations.
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

#ifndef QB_IO_PROT_BASE_H_
#define QB_IO_PROT_BASE_H_
#include "../async/protocol.h"
#include <cstring>
#include <qb/system/allocator/pipe.h>

namespace qb::protocol::base {

/**
 * @class byte_terminated
 * @brief Protocol for messages terminated by a specific byte
 * 
 * This class implements a protocol where messages are delimited by
 * a specific termination byte (e.g., '\0' or '\n').
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 * @tparam _EndByte The byte that marks the end of a message (default '\0')
 */
template <typename _IO_, char _EndByte = '\0'>
class byte_terminated : public io::async::AProtocol<_IO_> {
    std::size_t _offset = 0u; /**< Position to resume delimiter search */

public:
    constexpr static const std::size_t delimiter_size = 1; /**< Delimiter size (1 byte) */
    constexpr static const char end = _EndByte; /**< End character */

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
    inline std::size_t
    shiftSize(std::size_t const size) const noexcept {
        return static_cast<std::size_t>(size) - delimiter_size;
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
        auto it = buffer.begin() + _offset;
        while (it != buffer.end()) {
            if (*it == _EndByte) {
                _offset = 0; // reset
                return (it - buffer.begin()) + delimiter_size;
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
 * @brief Protocol for messages terminated by a sequence of bytes
 * 
 * This class implements a protocol where messages are delimited by
 * a specific sequence of bytes (e.g., "\r\n").
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 * @tparam _Trait Trait containing the termination sequence (_EndBytes)
 */
template <typename _IO_, typename _Trait>
class bytes_terminated : public io::async::AProtocol<_IO_> {
    constexpr static const std::size_t _SizeBytes = sizeof(_Trait::_EndBytes) - 1; /**< Size of the termination sequence */
    std::size_t _offset = 0u; /**< Position to resume delimiter search */

public:
    constexpr static const std::size_t delimiter_size = _SizeBytes; /**< Delimiter size */
    constexpr static const auto end = _Trait::_EndBytes; /**< End sequence */

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
    inline std::size_t
    shiftSize(std::size_t const size) const noexcept {
        return static_cast<std::size_t>(size) - delimiter_size;
    }

    /**
     * @brief Determines the size of the next complete message
     * 
     * Searches for the termination sequence in the input buffer.
     * 
     * @return Message size if found, 0 otherwise
     */
    std::size_t
    getMessageSize() {
        const auto &buffer = this->_in_buffer;
        auto i = buffer.begin() + _offset;

        if ((buffer.end() - i) < _SizeBytes)
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
 * @brief Protocol for messages preceded by their size
 * 
 * This class implements a protocol where each message is preceded
 * by a header containing its size. The format can be uint8_t, uint16_t,
 * or uint32_t depending on needs.
 * 
 * @tparam _IO_ The I/O type that will use this protocol
 * @tparam _Size Header size type (default uint16_t)
 */
template <typename _IO_, typename _Size = uint16_t>
class size_as_header : public io::async::AProtocol<_IO_> {
    constexpr static const std::size_t SIZEOF = sizeof(_Size); /**< Header size */
    _Size _size = 0u; /**< Size of the message being analyzed */

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
            if constexpr (SIZEOF == 2)
                _size = ntohs(*reinterpret_cast<_Size *>(buffer.begin()));
            else if constexpr (SIZEOF == 4)
                _size = ntohl(*reinterpret_cast<_Size *>(buffer.begin()));
            else
                _size = *reinterpret_cast<_Size *>(buffer.begin());
            buffer.free_front(SIZEOF);
        }
        if (buffer.size() >= _size) {
            const auto ret = _size;
            _size = 0;
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
    Header(std::size_t size) noexcept {
        if constexpr (SIZEOF == 2) {
            return htons(static_cast<_Size>(size));
        } else if constexpr (SIZEOF == 4) {
            return htonl(static_cast<_Size>(size));
        } else {
            return size;
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
