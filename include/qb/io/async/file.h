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

#ifndef QB_IO_ASYNC_FILE_H
#define QB_IO_ASYNC_FILE_H

#include "../transport/file.h"
#include "io.h"

namespace qb::io::async {

template <typename _Derived>
class file
    : public file_watcher<_Derived>
    , qb::io::transport::file {
    using base_t = file_watcher<_Derived>;
    friend base_t;
public:
    using transport_io_type = typename qb::io::transport::file::transport_io_type;
    using qb::io::transport::file::in;
    using qb::io::transport::file::out;
    using qb::io::transport::file::transport;
public:
    explicit file() {
        if constexpr (has_member_Protocol<_Derived>::value) {
            if constexpr (!std::is_void_v<typename _Derived::Protocol>) {
                this->template switch_protocol<typename _Derived::Protocol>(
                        static_cast<_Derived &>(*this));
            }
        }
    }

    inline uint64_t
    ident() noexcept {
        return static_cast<_Derived &>(*this).transport().native_handle();
    }
};

} // namespace qb::io::async::file

#endif // QB_IO_ASYNC_FILE_H
