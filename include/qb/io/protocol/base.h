/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#ifndef QB_IO_PROT_BASE_H_
#define QB_IO_PROT_BASE_H_
#include "../async/protocol.h"
#include <cstring>
#include <qb/system/allocator/pipe.h>

namespace qb::protocol::base {

template <typename _IO_, char _EndByte = '\0'>
class byte_terminated : public io::async::AProtocol<_IO_> {
    std::size_t _offset = 0u;

public:
    constexpr static const std::size_t delimiter_size = 1;
    constexpr static const char end = _EndByte;

    byte_terminated() = delete;
    virtual ~byte_terminated() = default;
    byte_terminated(_IO_ &io) noexcept
        : io::async::AProtocol<_IO_>(io) {}

    inline std::size_t
    shiftSize(std::size_t const size) const noexcept {
        return static_cast<std::size_t>(size) - delimiter_size;
    }

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

    void
    reset() noexcept final {
        _offset = 0;
    }
};

template <typename _IO_, typename _Trait>
class bytes_terminated : public io::async::AProtocol<_IO_> {
    constexpr static const std::size_t _SizeBytes = sizeof(_Trait::_EndBytes) - 1;
    std::size_t _offset = 0u;

public:
    constexpr static const std::size_t delimiter_size = _SizeBytes;
    constexpr static const auto end = _Trait::_EndBytes;

    bytes_terminated() = delete;
    virtual ~bytes_terminated() = default;
    bytes_terminated(_IO_ &io) noexcept
        : io::async::AProtocol<_IO_>(io) {}

    inline std::size_t
    shiftSize(std::size_t const size) const noexcept {
        return static_cast<std::size_t>(size) - delimiter_size;
    }

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

    void
    reset() noexcept final {
        _offset = 0;
    }
};

template <typename _IO_, typename _Size = uint16_t>
class size_as_header : public io::async::AProtocol<_IO_> {
    constexpr static const std::size_t SIZEOF = sizeof(_Size);
    _Size _size = 0u;

public:
    inline std::size_t
    shiftSize() const noexcept {
        return SIZEOF;
    }

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

    void
    reset() noexcept final {
        _size = 0;
    }
};

} // namespace qb::protocol::base

#endif // QB_IO_PROT_BASE_H_
