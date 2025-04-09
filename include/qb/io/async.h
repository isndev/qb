/**
 * @file qb/io/async.h
 * @brief Main include file for the QB asynchronous I/O library
 *
 * This file provides a convenient single include point for all asynchronous I/O
 * functionality in the QB framework. It includes all the necessary headers for
 * implementing asynchronous TCP and UDP clients and servers, file operations, and
 * I/O event handling.
 *
 * The file also defines a 'use' template struct that simplifies the creation of
 * various asynchronous I/O components through type aliases, enabling a consistent
 * interface for different transport implementations.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup IO
 */

#ifndef QB_IO_ASYNC_H_
#define QB_IO_ASYNC_H_

#include "async/event/all.h"
#include "async/file.h"
#include "async/io.h"
#include "async/tcp/client.h"
#include "async/tcp/server.h"
#include "async/udp/client.h"
#include "async/udp/server.h"
#include "config.h"

#include "transport/accept.h"
#include "transport/tcp.h"

#ifdef QB_IO_WITH_SSL
#include "transport/saccept.h"
#include "transport/stcp.h"
#endif

namespace qb::io {

template <typename _Derived>
struct use {
    template <typename _Protocol>
    using input = async::input<_Derived>;
    template <typename _Protocol>
    using output = async::output<_Derived>;
    template <typename _Protocol>
    using io = async::io<_Derived>;

    struct tcp {
        using acceptor = async::tcp::acceptor<_Derived, transport::accept>;

        template <typename _Client>
        using io_handler = async::io_handler<_Derived, _Client>;

        template <typename _Client>
        using server = async::tcp::server<_Derived, _Client, transport::accept>;

        template <typename _Server = void>
        using client = async::tcp::client<_Derived, transport::tcp, _Server>;

#ifdef QB_IO_WITH_SSL
        struct ssl {
            using acceptor = async::tcp::acceptor<_Derived, transport::saccept>;

            template <typename _Client>
            using io_handler = async::io_handler<_Derived, _Client>;

            template <typename _Client>
            using server = async::tcp::server<_Derived, _Client, transport::saccept>;

            template <typename _Server = void>
            using client = async::tcp::client<_Derived, transport::stcp, _Server>;
        };
#endif
    };

    struct udp {
        using server = async::udp::server<_Derived>;
        using client = async::udp::client<_Derived>;

        //        template <typename _Client>
        //        using server = async::udp::server<_Derived, _Client>;
        //
        //        template <template <typename _BaseProtocol> typename _Protocol,
        //                  typename _Server = void>
        //        using client = async::udp::client<_Derived, _Protocol, _Server>;

#ifdef QB_IO_WITH_SSL
        // Todo: implement dtls
#endif
    };

    using timeout = async::with_timeout<_Derived>;
    using file    = async::file<_Derived>;
};

} // namespace qb::io

#endif // QB_IO_ASYNC_H_
