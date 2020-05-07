/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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

#ifndef QB_IO_PROT_TEXT_H_
#define QB_IO_PROT_TEXT_H_
#include "base.h"

namespace qb::protocol::text {

template <typename _IO_, typename _StringTrait, char _Sep = '\0'>
class basic_text : public base::byte_terminated<_IO_, _Sep> {
public:
    basic_text() = delete;
    basic_text(_IO_ &io) noexcept
    : base::byte_terminated<_IO_, _Sep>(io) {}

    struct message {
        const std::size_t size;
        const char *data;
        _StringTrait text;
    };

    void
    onMessage(std::size_t size) noexcept {
        const auto &buffer = this->_io.in();
        const auto parsed = this->shiftSize(size);
        this->_io.on(message{parsed, buffer.cbegin(), {buffer.cbegin(), parsed}});
    }
};

template <typename _IO_, typename _SizeHeader = uint16_t>
class basic_binary : public base::size_as_header<_IO_, _SizeHeader> {
public:
    basic_binary() = delete;
    basic_binary(_IO_ &io) noexcept
    : base::size_as_header<_IO_, _SizeHeader>(io) {}

    struct message {
        const std::size_t size;
        const char *data;
    };

    void
    onMessage(std::size_t size) const noexcept {
        this->_io.on(message{size, this->_io.in().cbegin() + this->shiftSize()});
    }
};

template <typename _IO_>
using binary8 = basic_binary<_IO_, uint8_t>;
template <typename _IO_>
using binary16 = basic_binary<_IO_, uint16_t>;
template <typename _IO_>
using binary32 = basic_binary<_IO_, uint32_t>;
template <typename _IO_>
using string = basic_text<_IO_, std::string, '\0'>;
template <typename _IO_>
using command = basic_text<_IO_, std::string, '\n'>;
template <typename _IO_>
using string_view = basic_text<_IO_, const std::string_view, '\0'>;
template <typename _IO_>
using command_view = basic_text<_IO_, const std::string_view, '\n'>;

} // namespace qb::protocol::text

#endif // QB_IO_PROT_TEXT_H_
