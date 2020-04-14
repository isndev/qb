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

#ifndef QB_MODULE_XML_H_
#define QB_MODULE_XML_H_
#include <qb/io/protocol/base.h>
#include <xml/src/pugixml.hpp>

namespace qb {
namespace protocol {

template <typename _IO_>
class xml : public base::byte_terminated<_IO_, '\0'> {
    using base_t = base::byte_terminated<_IO_, '\0'>;

public:
    xml() = delete;
    xml(_IO_ &io) noexcept
        : base::byte_terminated<_IO_, '\0'>(io) {}

    struct message {
        const std::size_t size;
        const char *data;
        pugi::xml_document &xml;
    };

    void
    onMessage(std::size_t size) noexcept {
        const auto parsed = this->shiftSize(size);
        auto &buffer = this->_io.in();
        pugi::xml_document doc;

        doc.load_buffer(buffer.begin(), size);
        this->_io.on(message{parsed, buffer.cbegin(), doc});
    }
};

template <typename _IO_>
class xml_view : public base::byte_terminated<_IO_, '\0'> {
    using base_t = base::byte_terminated<_IO_, '\0'>;

public:
    xml_view() = delete;
    xml_view(_IO_ &io) noexcept
        : base::byte_terminated<_IO_, '\0'>(io) {}

    struct message {
        const std::size_t size;
        const char *data;
        const pugi::xml_document &xml;
    };

    void
    onMessage(std::size_t size) noexcept {
        const auto parsed = this->shiftSize(size);
        auto &buffer = this->_io.in();
        pugi::xml_document doc;

        doc.load_buffer_inplace(buffer.begin(), size);
        this->_io.on(message{parsed, buffer.cbegin(), doc});
    }
};

} // namespace protocol

namespace xml {

using document = pugi::xml_document;
using node = pugi::xml_node;
using attribute = pugi::xml_attribute;
using text = pugi::xml_text;

template <typename _IO_>
using protocol = qb::protocol::xml<_IO_>;

template <typename _IO_>
using protocol_view = qb::protocol::xml_view<_IO_>;

} // namespace xml

namespace allocator {

template <>
pipe<char> &pipe<char>::put<qb::xml::document>(const qb::xml::document &c);
template <>
pipe<char> &pipe<char>::put<qb::xml::node>(const qb::xml::node &c);
template <>
pipe<char> &pipe<char>::put<qb::xml::attribute>(const qb::xml::attribute &c);
template <>
pipe<char> &pipe<char>::put<qb::xml::text>(const qb::xml::text &c);

} // namespace allocator

} // namespace qb
#endif // QB_MODULE_XML_H_
