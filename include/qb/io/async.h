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
 * @note **Execution Model and Thread Safety:**
 *       - qb-io is designed for **single-threaded execution** within each VirtualCore.
 *       - Each `VirtualCore` (in qb-core) or thread (standalone usage) has its own
 *         `listener::current` event loop.
 *       - All I/O objects (clients, servers, sessions) created via `use<Derived>::*`
 *         **must not be shared between threads**. They are bound to the thread's listener.
 *       - Event handlers (`on()` methods) are called sequentially in the same thread,
 *         eliminating the need for mutexes or atomic operations for I/O state.
 *       - This design provides thread safety through isolation: each VirtualCore manages
 *         its own set of I/O objects independently.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
#include "async/tcp/connector.h"
#include "async/tcp/client.h"
#include "async/tcp/server.h"
#include "async/udp/client.h"
#include "async/udp/server.h"
#include "async/quic.h"
#include "async/coroutine.h" // C++20 coroutine support
#include "config.h"

#include "transport/accept.h"
#include "transport/tcp.h"

#ifdef QB_HAS_SSL
#include "transport/saccept.h"
#include "transport/stcp.h"
#endif

namespace qb::io {

/**
 * @struct use
 * @ingroup Async
 * @brief Helper template providing type aliases for integrating qb-io asynchronous components.
 * @details This CRTP helper simplifies the declaration of actors or classes that use qb-io's
 *          asynchronous features (like TCP clients/servers, UDP endpoints, file watchers)
 *          by providing convenient nested type aliases for the underlying asynchronous
 *          base classes and transports.
 * @tparam _Derived The class that will inherit from one of the `use` helper's nested types.
 */
template <typename _Derived>
struct use {
    /**
     * @brief CRTP-based asynchronous input helper.
     *
     * The protocol is picked up automatically from `_Derived::Protocol` when that
     * nested alias exists (handled inside `async::input`'s constructor via
     * `switch_protocol`). The previous `template <typename _Protocol>` parameter was
     * never consumed and is kept here only as a defaulted template for backward
     * compatibility; new code should simply write `use<Self>::input`.
     */
    template <typename _Protocol = void>
    using input = async::input<_Derived>;
    /**
     * @brief CRTP-based asynchronous output helper (see `input` note for `_Protocol`).
     */
    template <typename _Protocol = void>
    using output = async::output<_Derived>;
    /**
     * @brief CRTP-based asynchronous bidirectional I/O helper (see `input` note for `_Protocol`).
     */
    template <typename _Protocol = void>
    using io = async::io<_Derived>;

    /** @brief Provides type aliases for TCP-based asynchronous components. */
    struct tcp {
        using acceptor = async::tcp::acceptor<_Derived, transport::accept>;

        template <typename _Client>
        using io_handler = async::io_handler<_Derived, _Client>;

        template <typename _Client>
        using server = async::tcp::server<_Derived, _Client, transport::accept>;

        template <typename _Server = void>
        using client = async::tcp::client<_Derived, transport::tcp, _Server>;

#ifdef QB_HAS_SSL
        /** @brief Provides type aliases for SSL/TLS-secured TCP asynchronous components. */
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

    /**
     * @brief Provides type aliases for UDP-based asynchronous components.
     *
     * UDP servers are currently datagram-oriented (no per-peer session
     * demultiplexing): all datagrams funnel through the same `_Derived`
     * instance. If per-`identity` session demultiplexing is required, use
     * `transport::udp::identity` as a map key inside your own handler.
     */
    struct udp {
        using server = async::udp::server<_Derived>;
        using client = async::udp::client<_Derived>;

#ifdef QB_HAS_SSL
        // TODO(qb): implement DTLS once the underlying transport exposes it.
#endif
    };

    struct quic {
        template <typename _StreamSession>
        using io_handler = async::quic::io_handler<_Derived, _StreamSession>;

        using session        = async::quic::client<_Derived>;
        using stream_session = async::quic::client<_Derived>;

        template <typename _StreamSession>
        using server = async::quic::server<_Derived, _StreamSession>;

        template <typename _Server = void>
        using client = async::quic::client<_Derived, _Server>;

        template <typename _StreamSession = void>
        using connector = async::quic::connector<_Derived, _StreamSession>;
        using endpoint  = async::quic::endpoint;
        using stream    = async::quic::stream;
    };

    using timeout = async::with_timeout<_Derived>;
    using file    = async::file<_Derived>;
};

} // namespace qb::io

#endif // QB_IO_ASYNC_H_
