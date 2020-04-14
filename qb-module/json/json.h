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

#ifndef QB_MODULE_JSON_H_
#define QB_MODULE_JSON_H_
#include <json/single_include/nlohmann/json.hpp>
#include <qb/io/protocol/base.h>

namespace qb {
namespace protocol {

template <typename _IO_>
class json : public base::byte_terminated<_IO_, '\0'> {
public:
    json() = delete;
    json(_IO_ &io) noexcept
        : base::byte_terminated<_IO_, '\0'>(io) {}

    struct message {
        const std::size_t size;
        const char *data;
        nlohmann::json json;
    };

    void
    onMessage(std::size_t size) noexcept final {
        const auto parsed = this->shiftSize(size);
        const auto data = this->_io.in().cbegin();

        this->_io.on(message{
            parsed, data,
            nlohmann::json::parse(std::string_view(data, parsed), nullptr, false)});
    }
};

template <typename _IO_>
class json_packed : public base::byte_terminated<_IO_, '\0'> {
public:
    json_packed() = delete;
    json_packed(_IO_ &io) noexcept
        : base::byte_terminated<_IO_, '\0'>(io) {}

    struct message {
        const std::size_t size;
        const char *data;
        nlohmann::json json;
    };

    void
    onMessage(std::size_t size) noexcept final {
        const auto parsed = this->shiftSize(size);
        const auto data = this->_io.in().cbegin();
        this->_io.on(message{
            parsed, data, nlohmann::json::from_msgpack(std::string_view(data, parsed))});
    }
};

} // namespace protocol

namespace json {

using object = nlohmann::json;

template <typename _IO_>
using protocol = qb::protocol::json<_IO_>;

template <typename _IO_>
using protocol_packed = qb::protocol::json_packed<_IO_>;

} // namespace json

namespace allocator {

template <>
pipe<char> &pipe<char>::put<qb::json::object>(const qb::json::object &c);

} // namespace allocator

} // namespace qb

#endif // QB_MODULE_JSON_H_
