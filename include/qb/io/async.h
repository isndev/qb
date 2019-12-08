/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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

#ifndef             QB_IO_ASYNC_H_
# define            QB_IO_ASYNC_H_

#include "async/io.h"
#include "async/event/all.h"
#include "async/tcp/server.h"
#include "async/tcp/client.h"
#include "async/udp/server.h"
#include "async/udp/client.h"

#include "transport/accept.h"
#include "transport/tcp.h"

#ifdef QB_IO_WITH_SSL
#include "transport/saccept.h"
#include "transport/stcp.h"
#endif

namespace qb {
    namespace io {

        template <typename _Derived>
        struct use {
            template <typename _Protocol>
            using input = async::input<_Derived, _Protocol>;
            template <typename _Protocol>
            using output = async::output<_Derived, _Protocol>;
            template <typename _Protocol>
            using io = async::io<_Derived, _Protocol>;

            struct tcp {

                template <typename _Client>
                using server = async::tcp::server<_Derived, _Client, transport::accept>;

                template <template<typename _Transport> typename _Protocol, typename _Server = void>
                using client = async::tcp::client<_Derived, _Protocol, transport::tcp, _Server>;

#ifdef QB_IO_WITH_SSL
                struct ssl {
                    template <typename _Client>
                    using server = async::tcp::server<_Derived, _Client, transport::saccept>;

                    template <template<typename _BaseProtocol> typename _Protocol, typename _Server = void>
                    using client = async::tcp::client<_Derived, _Protocol, transport::stcp, _Server>;
                };
#endif
            };

            struct udp {

                template <typename _Client>
                using server = async::udp::server<_Derived, _Client>;

                template <template<typename _BaseProtocol> typename _Protocol, typename _Server = void>
                using client = async::udp::client<_Derived, _Protocol, _Server>;

#ifdef QB_IO_WITH_SSL
#endif
            };

            using timeout = async::with_timeout<_Derived>;

        };

    } // namespace io
} // namespace qb

#endif // QB_IO_ASYNC_H_
