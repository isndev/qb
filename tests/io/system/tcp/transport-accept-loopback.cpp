/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the specific terms.
 */

/**
 * @file system/tcp/transport-accept-loopback.cpp
 * @brief `qb::io::transport::accept` + `qb::io::async::io_handler` — the acceptor mixin and the
 *        session registry, over real loopback.
 *
 * Two transport-layer surfaces that the existing suites only touch through the full async session
 * stack (and so leave their direct branches thin) are exercised here against real kernel sockets:
 *
 *  PART A — `qb::io::transport::accept` (transport/accept.h), driven WITHOUT the event loop:
 *    - `transport()` exposes the inner `tcp::listener`; `listen_v4` binds an ephemeral port.
 *    - `read()` returns the *native handle* of a freshly accepted socket when a client is waiting,
 *      and `getAccepted()` then wraps that connected socket (a real read/write round-trip proves it).
 *    - `read()` with no pending connection (non-blocking listener) returns `(size_t)-1`.
 *    - `flush()` releases the accepted handle so the transport's destructor does not close the
 *      descriptor that was handed off to a session (the moved-out semantics).
 *    - `close()` shuts the listener so a subsequent `read()` no longer accepts.
 *    - `is_secure()` is a compile-time `false`.
 *
 *  PART B — `qb::io::async::io_handler` (async/io_handler.h), driven through a real async TCP server:
 *    - `session_count()` / `max_sessions()` / `set_max_sessions()` accounting as clients connect.
 *    - the DoS cap: with `set_max_sessions(N)`, the (N+1)-th connection is refused — `registerSession`
 *      closes the incoming socket and never grows the registry past N.
 *    - `session(id)` lookup, `sessions()` map access, `extractSession()` removing a session and
 *      returning its live transport, and `unregisterSession()` disconnecting one.
 *    - `stream()` / `stream_if()` broadcast fan-out reaching every (or a filtered subset of) client.
 *
 * The accept half needs no daemon (single accept syscall over a loopback pair); the handler half runs
 * the in-process event loop bound to a loopback listener — no external server. Ephemeral `:0` ports
 * throughout, collision-free under `ctest -j`.
 *
 * Signatures relied on:
 *   transport::accept: tcp::listener& transport(); std::size_t read(); void flush(std::size_t);
 *                      void close(); tcp::socket& getAccepted(); static constexpr bool is_secure();
 *   io_handler:        std::size_t session_count() const; std::size_t max_sessions() const;
 *                      void set_max_sessions(std::size_t); std::shared_ptr<_Session> session(uuid);
 *                      session_map_t& sessions(); std::pair<transport_io_type,bool> extractSession(uuid);
 *                      void unregisterSession(uuid const&); _Derived& stream(...); _Derived& stream_if(f,...);
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
 * @ingroup Tests
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/transport/accept.h>

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;
using qb::io::test::pump_until;

// ===========================================================================
// PART A — transport::accept mixin, direct loopback (no event loop)
// ===========================================================================

TEST(TransportAccept, IsNotSecureAtCompileTime) {
    static_assert(!qb::io::transport::accept::is_secure());
    EXPECT_FALSE(qb::io::transport::accept::is_secure());
}

TEST(TransportAccept, ReadAcceptsAPendingConnectionAndGetAcceptedRoundTrips) {
    qb::io::transport::accept acceptor;
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = acceptor.transport().local_endpoint().port();
    ASSERT_NE(port, 0);

    // A client connects on a worker thread and exchanges one line with the
    // accepted server-side socket.
    std::thread client_thread([&] {
        qb::io::tcp::socket client;
        ASSERT_EQ(client.connect_v4("127.0.0.1", port), qb::io::SocketStatus::Done);
        EXPECT_GE(client.write("ping", 4), 4);

        char buffer[8] = {};
        // Bounded read for the echoed reply.
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        int        got      = 0;
        while (got < 4 && std::chrono::steady_clock::now() < deadline) {
            const int n = client.read(buffer + got, 4 - got);
            if (n > 0)
                got += n;
            else
                std::this_thread::sleep_for(1ms);
        }
        EXPECT_EQ(got, 4);
        EXPECT_EQ(std::string(buffer, 4), "pong");
        client.disconnect();
    });

    // Block-accept the pending connection via the mixin's read(): it returns the
    // native handle of the freshly accepted socket (never (size_t)-1 here).
    const std::size_t handle = acceptor.read();
    ASSERT_NE(handle, static_cast<std::size_t>(-1)) << "read() failed to accept the pending connection";

    qb::io::tcp::socket &accepted = acceptor.getAccepted();
    ASSERT_TRUE(accepted.is_open());
    EXPECT_EQ(static_cast<std::size_t>(accepted.native_handle()), handle) << "getAccepted() must wrap the exact handle read() returned";

    // Drain the client's "ping" and echo "pong" back.
    accepted.set_nonblocking(false);
    char in[8] = {};
    int  got   = 0;
    while (got < 4) {
        const int n = accepted.read(in + got, 4 - got);
        ASSERT_GT(n, 0);
        got += n;
    }
    EXPECT_EQ(std::string(in, 4), "ping");
    EXPECT_GE(accepted.write("pong", 4), 4);

    client_thread.join();

    // flush() releases the accepted handle: after a handoff to a session the
    // transport must NOT close the descriptor it gave away. Prove the handle is
    // detached (native_handle reverts to the invalid sentinel).
    acceptor.flush(0);
    EXPECT_EQ(acceptor.getAccepted().native_handle(), qb::io::inet::invalid_socket)
        << "flush() must release the accepted handle (moved-out semantics)";

    // We still own the connection here; close it ourselves.
    qb::io::socket reclaimed(static_cast<::socket_type>(handle));
    reclaimed.close();
}

TEST(TransportAccept, ReadWithNoPendingConnectionReportsFailure) {
    qb::io::transport::accept acceptor;
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    // Non-blocking listener with an empty accept queue: read() cannot accept and
    // must report failure as (size_t)-1.
    acceptor.transport().set_nonblocking(true);

    EXPECT_EQ(acceptor.read(), static_cast<std::size_t>(-1)) << "read() with no pending connection must return (size_t)-1";
}

TEST(TransportAccept, CloseStopsTheListenerFromAccepting) {
    qb::io::transport::accept acceptor;
    ASSERT_EQ(acceptor.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    EXPECT_TRUE(acceptor.transport().is_open());

    acceptor.close();
    EXPECT_FALSE(acceptor.transport().is_open()) << "close() must shut the underlying listener";

    // A read() on the closed listener cannot accept.
    EXPECT_EQ(acceptor.read(), static_cast<std::size_t>(-1));

    // eof() is a documented no-op; calling it must be harmless.
    acceptor.eof();
}

// ===========================================================================
// PART B — io_handler via a real async TCP server
// ===========================================================================

namespace {

// Minimal echo server session: a newline-delimited command protocol whose
// handler echoes each received line back. Broadcasts land here too (the server's
// stream()/stream_if() write through `*session << ...`).
class HandlerServer;

class HandlerSession : public qb::io::use<HandlerSession>::tcp::client<HandlerServer> {
public:
    using Protocol = qb::protocol::text::command<HandlerSession>;

    explicit HandlerSession(IOServer &server)
        : client(server) {}

    void
    on(Protocol::message &&msg) {
        // Echo the line back to the originator.
        *this << msg.text << Protocol::end;
    }
};

class HandlerServer : public qb::io::use<HandlerServer>::tcp::server<HandlerSession> {
public:
    // io_handler invokes on(session&) right after a SUCCESSFUL registerSession,
    // i.e. once per admitted session (a capped/refused connection never lands here).
    std::atomic<std::size_t> admitted{0};

    void
    on(IOSession &) {
        ++admitted;
    }
};

// A throwaway client that connects, optionally sends a line, and collects echoes.
class ProbeClient : public qb::io::use<ProbeClient>::tcp::client<> {
public:
    using Protocol = qb::protocol::text::command<ProbeClient>;
    std::atomic<std::size_t> received{0};
    std::string              last;

    void
    on(Protocol::message &&msg) {
        last = std::string(msg.text);
        ++received;
    }
};

} // namespace

TEST(IoHandler, MaxSessionsAccountingAndDosCap) {
    qb::io::async::init();

    HandlerServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    // Default is unlimited (0); set a hard cap of 2 sessions.
    EXPECT_EQ(server.max_sessions(), 0u);
    server.set_max_sessions(2);
    EXPECT_EQ(server.max_sessions(), 2u);
    EXPECT_EQ(server.session_count(), 0u);

    // Connect three clients on a worker thread; the third must be refused by the cap.
    // The clients live for the whole test so all three TCP connections stay established
    // while the server decides which to admit.
    std::atomic<int>  connected{0};
    std::atomic<bool> stop{false};
    std::thread       worker([&] {
        qb::io::async::init();
        std::vector<std::unique_ptr<ProbeClient>> clients;
        for (int i = 0; i < 3; ++i) {
            auto c = std::make_unique<ProbeClient>();
            if (c->transport().connect_v4("127.0.0.1", port) == qb::io::SocketStatus::Done) {
                c->start();
                ++connected;
                clients.push_back(std::move(c));
            }
        }
        while (!stop.load())
            qb::io::async::run_for(10ms);
    });

    // All three TCP connects established on the client side...
    EXPECT_TRUE(pump_until([&] { return connected.load() == 3; }, 3s)) << "not all three clients connected at the TCP layer";

    // ...but the server admits exactly the cap (2), and on(session&) fired for those 2 only.
    EXPECT_TRUE(pump_until([&] { return server.admitted.load() == 2u; }, 3s))
        << "server did not admit exactly the cap; admitted=" << server.admitted.load();
    EXPECT_LE(server.session_count(), 2u) << "session_count must never exceed the configured cap";
    EXPECT_EQ(server.session_count(), 2u) << "the cap must admit exactly 2 sessions";

    stop.store(true);
    worker.join();
    // Pump out any pending disconnects from the joined worker's dropped clients.
    for (int i = 0; i < 20; ++i)
        qb::io::async::run_for(10ms);

    qb::io::async::listener::current.clear();
}

TEST(IoHandler, BroadcastReachesAllSessionsAndExtractRemovesOne) {
    qb::io::async::init();

    HandlerServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();
    server.set_max_sessions(0); // unlimited

    constexpr int     kClients = 3;
    std::atomic<int>  ready{0};
    std::atomic<bool> stop{false};
    std::atomic<int>  total_received{0};

    std::thread worker([&] {
        qb::io::async::init();
        std::vector<std::unique_ptr<ProbeClient>> clients;
        for (int i = 0; i < kClients; ++i) {
            auto c = std::make_unique<ProbeClient>();
            if (c->transport().connect_v4("127.0.0.1", port) == qb::io::SocketStatus::Done) {
                c->start();
                clients.push_back(std::move(c));
                ++ready;
            }
        }
        while (!stop.load()) {
            qb::io::async::run_for(10ms);
            int sum = 0;
            for (auto &c : clients)
                sum += static_cast<int>(c->received.load());
            total_received.store(sum);
        }
    });

    // Wait for all clients to land in the server registry.
    EXPECT_TRUE(pump_until([&] { return server.session_count() == static_cast<std::size_t>(kClients); }, 3s))
        << "not all clients registered; session_count=" << server.session_count();
    EXPECT_EQ(server.sessions().size(), static_cast<std::size_t>(kClients));

    // Broadcast to ALL sessions; every client must receive the line.
    server.stream(std::string("broadcast"), '\n');
    EXPECT_TRUE(pump_until([&] { return total_received.load() >= kClients; }, 3s))
        << "broadcast did not reach all clients; total_received=" << total_received.load();

    // stream_if to a filtered subset: send only to the first session by id.
    const auto first_id = server.sessions().begin()->first;
    server.stream_if([&](HandlerSession &s) { return s.id() == first_id; }, std::string("filtered"), '\n');
    // The filtered fan-out is best-effort observed: at least the all-broadcast
    // count holds, and at least one more line is delivered to the chosen client.
    EXPECT_TRUE(pump_until([&] { return total_received.load() >= kClients + 1; }, 3s))
        << "stream_if did not deliver to the filtered session; total_received=" << total_received.load();

    // session(id) lookup resolves to a live session.
    EXPECT_NE(server.session(first_id), nullptr);

    // extractSession() removes a session and returns its live transport (still open).
    // The session's transport here is a qb::io::tcp::socket; read liveness on it directly.
    auto [io, ok] = server.extractSession(first_id);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(io.is_open()) << "extracted transport must still own a live descriptor";
    EXPECT_EQ(server.session_count(), static_cast<std::size_t>(kClients - 1));
    EXPECT_EQ(server.session(first_id), nullptr) << "extracted session must no longer resolve";
    io.close();

    // extractSession on a now-missing id reports failure with a default transport.
    auto [missing_io, missing_ok] = server.extractSession(first_id);
    EXPECT_FALSE(missing_ok);
    EXPECT_FALSE(missing_io.is_open());

    // unregisterSession on a remaining session disconnects it without throwing.
    if (!server.sessions().empty()) {
        const auto another_id = server.sessions().begin()->first;
        server.unregisterSession(another_id);
    }

    stop.store(true);
    worker.join();
    for (int i = 0; i < 20; ++i)
        qb::io::async::run_for(10ms);

    qb::io::async::listener::current.clear();
}
