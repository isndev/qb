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

#ifndef QB_IO_ASYNC_PROTOCOL_ACCEPT_H
#define QB_IO_ASYNC_PROTOCOL_ACCEPT_H

namespace qb::io::protocol {

template <typename _IO_, typename _Socket>
class accept : public async::AProtocol<_IO_> {
public:
    using message = _Socket;

    accept() = delete;

    accept(_IO_ &io) noexcept
        : async::AProtocol<_IO_>(io) {}

    std::size_t
    getMessageSize() noexcept final {
        return static_cast<size_t>(this->_io.getAccepted().is_open());
    }

    void
    onMessage(std::size_t) noexcept final {
        this->_io.on(std::move(this->_io.getAccepted()));
    }

    void
    reset() noexcept final {}
};

} // namespace qb::io::protocol

#endif // QB_IO_ASYNC_PROTOCOL_ACCEPT_H
