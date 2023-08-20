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

#ifndef QB_IO_ASYNC_TCP_ACCEPTOR_H
#define QB_IO_ASYNC_TCP_ACCEPTOR_H

#include "../../protocol/accept.h"
#include "../io.h"

namespace qb::io::async::tcp {

template <typename _Derived, typename _Prot>
class acceptor
    : public input<acceptor<_Derived, _Prot>>
    , public _Prot {

    friend class input<acceptor<_Derived, _Prot>>;
    using base_t = input<acceptor<_Derived, _Prot>>;
    using Protocol = protocol::accept<acceptor, typename _Prot::socket_type>;

public:
    void
    on(event::disconnected &&e) {
        if constexpr (has_method_on<_Derived, void, event::disconnected>::value)
            static_cast<_Derived &>(*this).on(std::forward<event::disconnected>(e));
        else
            throw std::runtime_error("Acceptor has been disconnected");
    }

public:
    using accepted_socket_type = typename _Prot::socket_type;

public:
    acceptor() noexcept
        : base_t(new Protocol(*this)) {}

    ~acceptor() = default;

    void
    on(typename Protocol::message &&new_socket) {
        static_cast<_Derived &>(*this).on(
            std::forward<typename Protocol::message>(new_socket));
    }
};

} // namespace qb::io::async::tcp

#endif // QB_IO_ASYNC_TCP_ACCEPTOR_H
