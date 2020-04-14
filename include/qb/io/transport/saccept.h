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

#ifndef QB_IO_TRANSPORT_SACCEPT_H_
#define QB_IO_TRANSPORT_SACCEPT_H_
#include "../tcp/ssl/listener.h"

namespace qb::io::transport {

class saccept {
    io::tcp::ssl::listener _io;
    io::tcp::ssl::socket _accepted_io;

public:
    using input_io_type = io::tcp::ssl::listener;
    using socket_type = io::tcp::ssl::socket;

    io::tcp::ssl::listener &
    transport() noexcept {
        return _io;
    }

    std::size_t
    read() noexcept {
        if (_io.accept(_accepted_io) == io::SocketStatus::Done)
            return static_cast<std::size_t>(_accepted_io.ident());
        return static_cast<std::size_t>(-1);
    }

    void
    flush(std::size_t) noexcept {
        _accepted_io = io::tcp::ssl::socket();
    }

    void
    eof() const noexcept {}

    void
    close() noexcept {
        _io.close();
    }

    io::tcp::ssl::socket
    getAccepted() const {
        return _accepted_io;
    }
};

} // namespace qb::io::transport

#endif // QB_IO_TRANSPORT_SACCEPT_H_
