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

#include "http.h"

namespace qb::allocator {
template <>
pipe<char> &
pipe<char>::put<qb::http::Request<std::string>>(
    const qb::http::Request<std::string> &r) {
    // HTTP Status Line
    *this << ::http_method_str(static_cast<http_method>(r.method)) << qb::http::sep
          << r.path << qb::http::sep << "HTTP/" << r.major_version << "."
          << r.minor_version << qb::http::endl;
    // HTTP Headers
    for (const auto &it : r.headers) {
        for (const auto value : it.second)
            *this << it.first << ": " << value << qb::http::endl;
    }
    // Body
    auto length = r.content_length + r.body.size();
    if (length) {
        *this << "Content-Length: " << length << qb::http::endl
              << qb::http::endl
              << r.body;
    } else
        *this << qb::http::endl;
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<qb::http::Response<std::string>>(
    const qb::http::Response<std::string> &r) {
    // HTTP Status Line
    *this << "HTTP/" << r.major_version << "." << r.minor_version << qb::http::sep
          << r.status_code << qb::http::sep
          << (r.status.empty()
                  ? ::http_status_str(static_cast<http_status>(r.status_code))
                  : r.status.c_str())
          << qb::http::endl;
    // HTTP Headers
    for (const auto &it : r.headers) {
        for (const auto value : it.second)
            *this << it.first << ": " << value << qb::http::endl;
    }
    // Body
    auto length = r.content_length + r.body.size();
    if (length) {
        *this << "Content-Length: " << length << qb::http::endl
              << qb::http::endl
              << r.body;
    } else
        *this << qb::http::endl;
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<qb::http::Chunk>(const qb::http::Chunk &c) {
    constexpr static const std::size_t hex_len = sizeof(std::size_t) << 1u;
    static const char digits[] = "0123456789ABCDEF";
    if (c.size()) {
        std::string rc(hex_len, '0');
        auto f_pos = 0u;
        for (size_t i = 0u, j = (hex_len - 1u) * 4u; i < hex_len; ++i, j -= 4u) {
            const auto offset = (c.size() >> j) & 0x0fu;
            rc[i] = digits[offset];
            if (!offset)
                ++f_pos;
        }
        std::string_view hex_view(rc.c_str() + f_pos, rc.size() - f_pos);
        *this << hex_view << qb::http::endl;
        put(c.data(), c.size());
    } else {
        *this << '0' << qb::http::endl;
    }

    *this << qb::http::endl;
    return *this;
}
} // namespace qb::allocator

extern "C" {
#include <http/http_parser.c>
}

std::string
qb::http::urlDecode(const char *str, std::size_t const size) {
    return urlDecode(str, str + size);
}

template <>
const http_parser_settings
    qb::http::template Parser<qb::http::Request<std::string>>::settings{
        &Parser::on_message_begin, &Parser::on_url,
        &Parser::on_status,        &Parser::on_header_field,
        &Parser::on_header_value,  &Parser::on_headers_complete,
        &Parser::on_body,          &Parser::on_message_complete,
        &Parser::on_chunk_header,  &Parser::on_chunk_complete};

template <>
const http_parser_settings
    qb::http::template Parser<qb::http::Request<std::string_view>>::settings{
        &Parser::on_message_begin, &Parser::on_url,
        &Parser::on_status,        &Parser::on_header_field,
        &Parser::on_header_value,  &Parser::on_headers_complete,
        &Parser::on_body,          &Parser::on_message_complete,
        &Parser::on_chunk_header,  &Parser::on_chunk_complete};

template <>
const http_parser_settings
    qb::http::template Parser<qb::http::Response<std::string>>::settings{
        &Parser::on_message_begin, &Parser::on_url,
        &Parser::on_status,        &Parser::on_header_field,
        &Parser::on_header_value,  &Parser::on_headers_complete,
        &Parser::on_body,          &Parser::on_message_complete,
        &Parser::on_chunk_header,  &Parser::on_chunk_complete};

template <>
const http_parser_settings
    qb::http::template Parser<qb::http::Response<std::string_view>>::settings{
        &Parser::on_message_begin, &Parser::on_url,
        &Parser::on_status,        &Parser::on_header_field,
        &Parser::on_header_value,  &Parser::on_headers_complete,
        &Parser::on_body,          &Parser::on_message_complete,
        &Parser::on_chunk_header,  &Parser::on_chunk_complete};

// templates instantiation
// objects
template class qb::http::Request<std::string>;
template class qb::http::Request<std::string_view>;
template class qb::http::Response<std::string>;
template class qb::http::Response<std::string_view>;
// functions
template std::string qb::http::urlDecode<std::string>(const std::string &str);
template std::string qb::http::urlDecode<std::string_view>(const std::string_view &str);