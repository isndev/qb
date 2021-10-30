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

#ifndef QB_IO_ASYNC_PROTOCOL_H
#define QB_IO_ASYNC_PROTOCOL_H

#include <qb/utility/type_traits.h>

namespace qb::io::async {

class IProtocol {
public:
    virtual ~IProtocol() = default;
    virtual std::size_t getMessageSize() noexcept = 0;
    virtual void onMessage(std::size_t) noexcept = 0;
    virtual void reset() noexcept = 0;
};

template <typename _IO_>
class AProtocol : public IProtocol {
    friend typename _IO_::base_io_t;

    bool _status = true;
protected:
    _IO_ &_io;

    AProtocol() = delete;
    AProtocol(_IO_ &io) noexcept
        : _io(io) {}
    virtual ~AProtocol() = default;
    virtual std::size_t getMessageSize() noexcept = 0;
    virtual void onMessage(std::size_t) noexcept = 0;
    virtual void reset() noexcept = 0;

public:
    [[nodiscard]] bool
    ok() const noexcept {
        return _status;
    }

    void not_ok() noexcept {
        _status = false;
    }
};

} // namespace qb::io::async

#endif // QB_IO_ASYNC_PROTOCOL_H
