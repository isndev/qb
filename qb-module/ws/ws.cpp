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

#include "ws.h"

namespace qb::http::ws {
std::string
generateKey() noexcept {
    // Make random 16-byte nonce
    char nonce[16] = "";
    std::uniform_int_distribution<unsigned short> dist(0, 255);
    std::random_device rd;
    for (auto i = 0u; i < 16u; ++i)
        nonce[i] = static_cast<char>(dist(rd));

    return crypto::Base64::encode(nonce);
}
} // namespace qb::http::ws

namespace qb::allocator {

static void
fill_unmasked_message(pipe<char> &pipe, const qb::http::ws::Message &msg) {
    std::size_t length = msg.size();
    pipe.reserve(length + 10);

    pipe << static_cast<char>(msg.fin_rsv_opcode);
    // Masked (first length byte>=128)
    if (length >= 126) {
        std::size_t num_bytes;
        if (length > 0xffff) {
            num_bytes = 8;
            pipe << static_cast<char>(127);
        } else {
            num_bytes = 2;
            pipe << static_cast<char>(126);
        }

        for (std::size_t c = num_bytes - 1; c != static_cast<std::size_t>(-1); --c)
            pipe << static_cast<char>(
                (static_cast<unsigned long long>(length) >> (8 * c)) % 256);
    } else
        pipe << static_cast<char>(length);

    pipe << msg._data;
}

static void
fill_masked_message(pipe<char> &pipe, const qb::http::ws::Message &msg) {
    // Create mask
    std::array<unsigned char, 4> mask{};
    std::uniform_int_distribution<unsigned short> dist(0, 255);
    std::random_device rd;
    for (std::size_t c = 0; c < 4; ++c)
        mask[c] = static_cast<unsigned char>(dist(rd));

    std::size_t length = msg.size();
    pipe.reserve(length + 14);

    pipe << static_cast<char>(msg.fin_rsv_opcode);
    // Masked (first length byte>=128)
    if (length >= 126) {
        std::size_t num_bytes;
        if (length > 0xffff) {
            num_bytes = 8;
            pipe << static_cast<char>(127 + 128);
        } else {
            num_bytes = 2;
            pipe << static_cast<char>(126 + 128);
        }

        for (std::size_t c = num_bytes - 1; c != static_cast<std::size_t>(-1); --c)
            pipe << static_cast<char>(
                (static_cast<unsigned long long>(length) >> (8 * c)) % 256);
    } else
        pipe << static_cast<char>(length + 128);

    pipe.write(reinterpret_cast<char *>(mask.data()), 4);

    const auto msg_begin = msg._data.cbegin();
    auto out_begin = pipe.allocate_back(length);
    for (std::size_t i = 0; i < length; ++i)
        out_begin[i] = static_cast<const char>(
            static_cast<const unsigned char>(msg_begin[i]) ^ mask[i % 4]);
}

template <>
pipe<char> &
pipe<char>::put<qb::http::ws::Message>(const qb::http::ws::Message &msg) {
    if (msg.masked)
        fill_masked_message(*this, msg);
    else
        fill_unmasked_message(*this, msg);
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<qb::http::ws::MessageText>(const qb::http::ws::MessageText &msg) {
    return put(static_cast<qb::http::ws::Message const &>(msg));
}

template <>
pipe<char> &
pipe<char>::put<qb::http::ws::MessageBinary>(const qb::http::ws::MessageBinary &msg) {
    return put(static_cast<qb::http::ws::Message const &>(msg));
}

template <>
pipe<char> &
pipe<char>::put<qb::http::ws::MessageClose>(const qb::http::ws::MessageClose &msg) {
    return put(static_cast<qb::http::ws::Message const &>(msg));
}

template <>
pipe<char> &
pipe<char>::put<qb::http::WebSocketRequest>(const qb::http::WebSocketRequest &msg) {
    return put(static_cast<const qb::http::Request<> &>(msg));
}

} // namespace qb::allocator

//#include <qb/io/async.h>