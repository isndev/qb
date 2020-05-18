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

#ifndef QB_IO_TRANSPORT_STCP_H
#define QB_IO_TRANSPORT_STCP_H
#include "../stream.h"
#include "../tcp/ssl/socket.h"
#include <iostream>
namespace qb::io::transport {

class stcp : public stream<io::tcp::ssl::socket> {
public:
    [[nodiscard]] int
    read() noexcept {
        static constexpr const std::size_t bucket_read = 4096;

        auto ret = _in.read(_in_buffer.allocate_back(bucket_read), bucket_read);
        if (likely(ret >= 0)) {
            _in_buffer.free_back(bucket_read - ret);
            const auto pending = SSL_pending(transport().ssl());
            if (pending) {
                ret += _in.read(_in_buffer.allocate_back(pending), pending);
            }
        }
        return ret;
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_STCP_H
