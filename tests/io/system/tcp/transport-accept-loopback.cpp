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
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>
#include <qb/io/transport/accept.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/loopback_fixture.h"

using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::read_exact_within;

#ifndef _WIN32
namespace transport_accept_loopback_test {
thread_local int         watched_peer_fd = -1;
thread_local std::size_t peer_name_calls = 0;

// Observe the native call, below both client::ip() and the socket implementation.
// Every use first proves that qb's compiled socket object reaches this interposer.
class PeerNameScope {
    int         _previous_fd;
    std::size_t _previous_calls;

public:
    explicit PeerNameScope(int fd)
        : _previous_fd(watched_peer_fd)
        , _previous_calls(peer_name_calls) {
        watched_peer_fd = fd;
        peer_name_calls = 0;
    }

    ~PeerNameScope() {
        watched_peer_fd = _previous_fd;
        peer_name_calls = _previous_calls;
    }

    [[nodiscard]] std::size_t
    calls() const noexcept {
        return peer_name_calls;
    }
};
} // namespace transport_accept_loopback_test

extern "C" int
getpeername(int fd, sockaddr *address, socklen_t *length)
#ifdef __linux__
    noexcept
#endif
{
    using NativeGetPeerName              = decltype(&::getpeername);
    static const auto native_getpeername = reinterpret_cast<NativeGetPeerName>(::dlsym(RTLD_NEXT, "getpeername"));
    if (!native_getpeername) {
        errno = ENOSYS;
        return -1;
    }
    if (fd == transport_accept_loopback_test::watched_peer_fd)
        ++transport_accept_loopback_test::peer_name_calls;
    return native_getpeername(fd, address, length);
}
#endif

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
        // Bounded read for the echoed reply. The deadline is only a bound because the
        // read is non-blocking: `connect_v4` leaves the socket in the OS default
        // (blocking) mode, so the loop this replaced re-checked `deadline` between
        // reads it could never get back to — the 2s was decoration.
        EXPECT_EQ(read_exact_within(client, buffer, 4, 2s), 4u);
        EXPECT_EQ(std::string(buffer, 4), "pong");
        client.disconnect();
    });

    // Join on EVERY exit path. The explicit `client_thread.join()` further down is
    // still where the ordering matters (it must precede `flush()`), and leaves this
    // guard a no-op; the guard is for the path where a fatal ASSERT below returns from
    // the test body first. See qb::io::test::thread_joiner for why that matters.
    const qb::io::test::thread_joiner client_joiner{client_thread};

    // Accept the pending connection via the mixin's read(). Bounded: a client whose
    // connect never lands (its ASSERT above aborted the thread body) would park a
    // blocking accept on the MAIN thread forever, and `client_thread.join()` below
    // would never be reached to report it. With a non-blocking listener `read()`
    // returns (size_t)-1 until a connection is queued — the documented behaviour the
    // next test in this file pins — so polling it is the mixin's own contract, and
    // the success branch taken here (`accept()==Done` -> native handle) is unchanged.
    acceptor.transport().set_nonblocking(true);
    std::size_t handle       = static_cast<std::size_t>(-1);
    const auto  accept_until = std::chrono::steady_clock::now() + 3s;
    while ((handle = acceptor.read()) == static_cast<std::size_t>(-1) && std::chrono::steady_clock::now() < accept_until)
        std::this_thread::sleep_for(1ms);
    ASSERT_NE(handle, static_cast<std::size_t>(-1)) << "read() failed to accept the pending connection";

    qb::io::tcp::socket &accepted = acceptor.getAccepted();
    ASSERT_TRUE(accepted.is_open());
    EXPECT_EQ(static_cast<std::size_t>(accepted.native_handle()), handle) << "getAccepted() must wrap the exact handle read() returned";

    // Drain the client's "ping" and echo "pong" back. Bounded: this loop had no
    // deadline at all, on the main thread, over a socket deliberately flipped to
    // blocking — a client that connected but never wrote wedged the binary.
    char in[8] = {};
    ASSERT_EQ(read_exact_within(accepted, in, 4), 4u) << "accepted socket never carried the client's 4-byte ping";
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

    struct stop_and_join {
        std::atomic<bool> &stop;
        std::thread       &worker;
        ~stop_and_join() {
            stop.store(true);
            if (worker.joinable())
                worker.join();
        }
    } cleanup{stop, worker};

    // Wait for all clients to land in the server registry.
    ASSERT_TRUE(pump_until([&] { return server.session_count() == static_cast<std::size_t>(kClients); }, 3s))
        << "not all clients registered; session_count=" << server.session_count();
    ASSERT_EQ(server.sessions().size(), static_cast<std::size_t>(kClients));

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

// ===========================================================================
// PART C — io_handler under re-entrant hooks (Huly QB-306) and the broadcast's admission (Huly QB-292)
// ===========================================================================

namespace {

class HookServer;

// A session whose `extracted` hook and whose server's registration hook run what the test hands them.
class HookSession : public qb::io::use<HookSession>::tcp::client<HookServer> {
public:
    using Protocol = qb::protocol::text::command<HookSession>;

    std::function<void()> on_extracted; // runs once
    std::size_t           write_cap = QB_MAX_WRITE_BUFFER_SIZE;

    explicit HookSession(IOServer &server)
        : client(server) {}

    // publish() reads the cap through the derived session (the transport's own setter is not public on
    // a tcp::client session).
    [[nodiscard]] std::size_t
    max_write_buffer_size() const noexcept {
        return write_cap;
    }

    void
    on(Protocol::message &&) {}

    void
    on(qb::io::async::event::extracted &&) {
        if (auto hook = std::exchange(on_extracted, nullptr))
            hook();
    }
};

class HookServer : public qb::io::use<HookServer>::tcp::server<HookSession> {
public:
    std::function<void(IOSession &)> on_registered; // runs once

    void
    on(IOSession &session) {
        if (auto hook = std::exchange(on_registered, nullptr))
            hook(session);
    }
};

// One connected client on a worker thread, for as long as the guard lives.
class OneClient {
    std::atomic<bool> _stop{false};
    std::atomic<bool> _connected{false};
    std::thread       _worker;

public:
    explicit OneClient(std::uint16_t port)
        : _worker([this, port] {
            qb::io::async::init();
            ProbeClient client;
            if (client.transport().connect_v4("127.0.0.1", port) == qb::io::SocketStatus::Done) {
                client.start();
                _connected.store(true);
            }
            while (!_stop.load())
                qb::io::async::run_for(5ms);
        }) {}

    ~OneClient() {
        _stop.store(true);
        _worker.join();
    }

    [[nodiscard]] bool
    connected() const noexcept {
        return _connected.load();
    }
};

void
drain_after_clients() {
    for (int i = 0; i < 20; ++i)
        qb::io::async::run_for(5ms);
    qb::io::async::listener::current.clear();
}

} // namespace

// The `extracted` hook grows the registry: a rehash frees the bucket array the extraction's iterator
// pointed into, and the erase that followed used it (ASan: heap-use-after-free).
TEST(IoHandler, ExtractSessionSurvivesAHookThatGrowsTheRegistry) {
    qb::io::async::init();
    HookServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    server.start();
    {
        OneClient client(server.transport().local_endpoint().port());
        ASSERT_TRUE(pump_until([&] { return server.session_count() == 1u; }, 3s));
        const auto id = server.sessions().begin()->first;

        std::vector<qb::uuid> added;
        server.session(id)->on_extracted = [&] {
            for (int i = 0; i < 64; ++i) { // enough to rehash the registry several times over
                auto extra = std::make_shared<HookSession>(server);
                added.push_back(extra->id());
                server.sessions().emplace(extra->id(), std::move(extra));
            }
        };

        auto [io, ok] = server.extractSession(id);
        EXPECT_TRUE(ok);
        EXPECT_TRUE(io.is_open()) << "the extracted transport is handed over live";
        EXPECT_EQ(server.session(id), nullptr) << "the extracted session no longer resolves";
        EXPECT_EQ(server.session_count(), added.size()) << "exactly the extracted session left the registry";
        for (auto const &extra : added)
            EXPECT_NE(server.session(extra), nullptr) << "every session the hook added stays registered";
        io.close();
        server.sessions().clear();
    }
    drain_after_clients();
}

// The `extracted` hook extracts the same session again: the inner call takes it, and the outer one --
// which erased the node the inner call had already freed -- must report it has nothing left.
TEST(IoHandler, ExtractSessionFromItsOwnHookHandsTheTransportOverOnce) {
    qb::io::async::init();
    HookServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    server.start();
    {
        OneClient client(server.transport().local_endpoint().port());
        ASSERT_TRUE(pump_until([&] { return server.session_count() == 1u; }, 3s));
        const auto id = server.sessions().begin()->first;

        decltype(server.extractSession(id)) inner{};
        server.session(id)->on_extracted = [&] {
            inner = server.extractSession(id);
        };

        auto [io, ok] = server.extractSession(id);
        EXPECT_TRUE(inner.second) << "the inner extraction took the session";
        EXPECT_TRUE(inner.first.is_open());
        EXPECT_FALSE(ok) << "the outer extraction has nothing left to hand over";
        EXPECT_FALSE(io.is_open());
        EXPECT_EQ(server.session_count(), 0u);
        inner.first.close();
    }
    drain_after_clients();
}

// The registration hook extracts the session it was handed: registerSession() then returned a pointer
// to a session that died as it returned.
TEST(IoHandler, RegisterSessionReportsASessionItsHookTookAway) {
    qb::io::async::init();
    HookServer server; // never listens: the test registers a connected socket itself

    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", listener.local_endpoint().port()), qb::io::SocketStatus::Done);
    qb::io::tcp::socket accepted;
    ASSERT_EQ(listener.accept(accepted), qb::io::SocketStatus::Done);

    decltype(server.extractSession(qb::uuid{})) taken{};
    server.on_registered = [&](HookSession &session) {
        taken = server.extractSession(session.id());
    };

    auto *registered = server.registerSession(std::move(accepted));
    EXPECT_TRUE(taken.second) << "the hook extracted the session";
    EXPECT_EQ(registered, nullptr) << "a session its hook took away is not reported as registered";
    EXPECT_EQ(server.session_count(), 0u);

    taken.first.close();
    client.disconnect();
    drain_after_clients();
}

// stream() writes each argument through `*session << ...`: every one of them, not only the first,
// must pass the session's write-buffer cap (Huly QB-292).
TEST(IoHandler, BroadcastAdmitsEveryArgumentAgainstTheWriteBufferCap) {
    qb::io::async::init();
    HookServer server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    server.start();
    {
        OneClient client(server.transport().local_endpoint().port());
        ASSERT_TRUE(pump_until([&] { return server.session_count() == 1u; }, 3s));
        auto session       = server.sessions().begin()->second;
        session->write_cap = 6u;

        server.stream(std::string("abcd"), std::string("efgh")); // the SECOND argument crosses the cap
        EXPECT_LE(session->out().size(), 6u) << "the cap holds for every argument of the broadcast";
        EXPECT_EQ(session->disconnection_reason(), static_cast<int>(qb::io::async::event::disconnect_reason::buffer_overflow));
    }
    drain_after_clients();
}

namespace transport_accept_loopback_test {

class PeerServer;

class PeerSession : public qb::io::use<PeerSession>::tcp::client<PeerServer> {
public:
    using Protocol = void;
    const std::string before_socket;

    explicit PeerSession(IOServer &server)
        : client(server)
        , before_socket(ip()) {}
};

class PeerServer : public qb::io::async::io_handler<PeerServer, PeerSession> {};

class LegacyPeerSocket : public qb::io::tcp::socket {
public:
    LegacyPeerSocket() = default;
    explicit LegacyPeerSocket(qb::io::tcp::socket &&socket)
        : qb::io::tcp::socket(std::move(socket)) {}
    LegacyPeerSocket(LegacyPeerSocket &&)            = default;
    LegacyPeerSocket &operator=(LegacyPeerSocket &&) = default;
    std::string       peer_ip() const                = delete;
};

class LegacyPeerTransport : public qb::io::stream<LegacyPeerSocket> {
public:
    static constexpr bool
    is_secure() noexcept {
        return false;
    }
};

class LegacyPeerServer;
class LegacyPeerSession : public qb::io::async::tcp::client<LegacyPeerSession, LegacyPeerTransport, LegacyPeerServer> {
    using base = qb::io::async::tcp::client<LegacyPeerSession, transport_accept_loopback_test::LegacyPeerTransport, LegacyPeerServer>;

public:
    using Protocol = void;
    explicit LegacyPeerSession(IOServer &server)
        : base(server) {}
};
class LegacyPeerServer : public qb::io::async::io_handler<LegacyPeerServer, LegacyPeerSession> {};

class PeerIpCache : public testing::Test {
protected:
    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

// A complete TCP handshake and bounded accept, without a worker thread or daemon.
bool
connect_pair(qb::io::tcp::listener &listener, qb::io::tcp::socket &client, qb::io::tcp::socket &accepted) {
    if (listener.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done)
        return false;
    if (client.connect(listener.local_endpoint(), qb::io::test::kLoopbackWaitBudget) != qb::io::SocketStatus::Done)
        return false;
    return qb::io::test::accept_within(listener, accepted);
}

// Linux normally routes all 127/8 locally; hosts without that alias use ::1.
// Bind the remote's source explicitly: a destination alias alone would still
// leave its accepted peer at 127.0.0.1 and weaken the cache-transfer assertions.
bool
connect_distinct_pair(qb::io::tcp::listener &listener, qb::io::tcp::socket &client, qb::io::tcp::socket &accepted) {
    const char *address = "127.0.0.2";
    if (listener.listen_v4(0, address) != qb::io::SocketStatus::Done) {
        address = "::1";
        if (listener.listen_v6(0, address) != qb::io::SocketStatus::Done)
            return false;
    }
    if (client.bind(qb::io::endpoint().as_in(address, 0)) != 0)
        return false;
    if (client.connect(listener.local_endpoint(), qb::io::test::kLoopbackWaitBudget) != qb::io::SocketStatus::Done)
        return false;
    return qb::io::test::accept_within(listener, accepted);
}

TEST_F(PeerIpCache, EmptyBeforeInstallationAndAfterExtraction) {
    static_assert(std::is_same_v<decltype(std::declval<const PeerSession &>().ip()), std::string>);
    qb::io::tcp::listener listener;
    qb::io::tcp::socket   remote;
    qb::io::tcp::socket   accepted;
    ASSERT_TRUE(connect_pair(listener, remote, accepted));
    PeerServer server;
    auto      *session = server.registerSession(std::move(accepted));
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(session->before_socket.empty());
    EXPECT_EQ(session->ip(), "127.0.0.1");
    auto retained = server.session(session->id());
    ASSERT_NE(retained, nullptr);
    auto [socket, extracted] = server.extractSession(session->id());
    ASSERT_TRUE(extracted);
    EXPECT_TRUE(retained->ip().empty()) << "a retained session no longer owns its extracted peer";
    EXPECT_EQ(socket.peer_endpoint().ip(), "127.0.0.1");
    auto *replacement = server.registerSession(std::move(socket));
    ASSERT_NE(replacement, nullptr);
    EXPECT_TRUE(replacement->before_socket.empty());
    EXPECT_EQ(replacement->ip(), "127.0.0.1");
    replacement->stop();
    replacement->transport().close();
    EXPECT_TRUE(replacement->ip().empty());
}

TEST_F(PeerIpCache, FailedLookupDoesNotPoisonTheLaterConnectionOnTheSameSocket) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    PeerServer  server;
    PeerSession session(server);
    ASSERT_EQ(session.transport().init(), 0);
    const auto fd = session.transport().native_handle();
    EXPECT_TRUE(session.ip().empty());
    ASSERT_EQ(session.transport().connect(listener.local_endpoint(), qb::io::test::kLoopbackWaitBudget), 0);
    qb::io::tcp::socket accepted;
    ASSERT_TRUE(qb::io::test::accept_within(listener, accepted));
    EXPECT_EQ(session.transport().native_handle(), fd);
    EXPECT_EQ(session.ip(), "127.0.0.1");
}

TEST_F(PeerIpCache, CustomTransportWithoutPeerIpKeepsThePeerEndpointFallback) {
    static_assert(std::is_same_v<decltype(std::declval<const LegacyPeerSession &>().ip()), std::string>);
    qb::io::tcp::listener listener;
    qb::io::tcp::socket   remote;
    qb::io::tcp::socket   accepted;
    ASSERT_TRUE(connect_pair(listener, remote, accepted));
    LegacyPeerSocket socket(std::move(accepted));
    LegacyPeerServer server;
    auto            *session = server.registerSession(std::move(socket));
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->ip(), "127.0.0.1");
}

#ifndef _WIN32
TEST_F(PeerIpCache, RepeatedIpReadsQueryTheNativePeerOnlyOnce) {
    qb::io::tcp::listener listener;
    qb::io::tcp::socket   remote;
    qb::io::tcp::socket   accepted;
    ASSERT_TRUE(connect_pair(listener, remote, accepted));
    PeerServer server;
    auto      *session = server.registerSession(std::move(accepted));
    ASSERT_NE(session, nullptr);
    const auto fd = session->transport().native_handle();
    {
        PeerNameScope probe(fd);
        const auto    peer = qb::io::socket::peer_endpoint(fd);
        ASSERT_TRUE(peer);
        ASSERT_EQ(probe.calls(), 1u) << "HARNESS REFUSED: the compiled socket call did not reach the native interposer";
    }
    PeerNameScope reads(fd);
    const auto    first  = session->ip();
    const auto    second = session->ip();
    const auto    third  = session->ip();
    EXPECT_EQ(first, "127.0.0.1");
    EXPECT_EQ(second, first);
    EXPECT_EQ(third, first);
    EXPECT_EQ(reads.calls(), 1u) << "three public ip() reads must share one formatted peer lookup";
}

TEST_F(PeerIpCache, ReplacingAClosedSocketAtTheSameDescriptorQueriesTheNewPeer) {
    qb::io::tcp::listener first_listener;
    qb::io::tcp::socket   first_remote;
    qb::io::tcp::socket   first_accepted;
    ASSERT_TRUE(connect_pair(first_listener, first_remote, first_accepted));
    qb::io::tcp::listener next_listener;
    qb::io::tcp::socket   next_remote;
    qb::io::tcp::socket   next_accepted;
    ASSERT_TRUE(connect_distinct_pair(next_listener, next_remote, next_accepted))
        << "distinct real peer requires the 127.0.0.2 alias or IPv6 loopback";
    const auto next_ip = next_remote.local_endpoint().ip();
    ASSERT_NE(next_ip, "127.0.0.1");
    PeerServer server;
    auto      *session = server.registerSession(std::move(first_accepted));
    ASSERT_NE(session, nullptr);
    auto      &transport = session->transport();
    const auto old_fd    = transport.native_handle();
    ASSERT_NE(next_accepted.native_handle(), old_fd);
    {
        PeerNameScope probe(old_fd);
        ASSERT_TRUE(qb::io::socket::peer_endpoint(old_fd));
        ASSERT_EQ(probe.calls(), 1u) << "HARNESS REFUSED: native getpeername interception is inactive";
    }
    ASSERT_EQ(session->ip(), "127.0.0.1");
    session->stop();
    // Keep the reference obtained BEFORE priming the cache, and do not read ip()
    // between close and adoption: neither a getter hook nor seeing fd=-1 may hide
    // the same-descriptor replacement from this witness.
    transport.close();
    const auto adopted = ::dup2(next_accepted.native_handle(), old_fd);
    ASSERT_EQ(adopted, old_fd);
    qb::io::socket owned(adopted);
    next_accepted.close(QB_SD_NONE); // do not shutdown the duplicated connection
    PeerNameScope replaced(old_fd);
    transport = std::move(owned);
    EXPECT_EQ(transport.native_handle(), old_fd);
    EXPECT_EQ(session->ip(), next_ip);
    EXPECT_EQ(session->ip(), next_ip);
    EXPECT_EQ(replaced.calls(), 1u) << "the reused descriptor names a new connection, not the cached old peer";
}

TEST_F(PeerIpCache, MovingConnectedSocketsTransfersTheirCachedPeerWithTheDescriptor) {
    qb::io::tcp::listener first_listener;
    qb::io::tcp::socket   first_remote;
    qb::io::tcp::socket   first_accepted;
    ASSERT_TRUE(connect_pair(first_listener, first_remote, first_accepted));
    qb::io::tcp::listener next_listener;
    qb::io::tcp::socket   next_remote;
    qb::io::tcp::socket   next_accepted;
    ASSERT_TRUE(connect_distinct_pair(next_listener, next_remote, next_accepted))
        << "distinct real peer requires the 127.0.0.2 alias or IPv6 loopback";
    const auto next_ip = next_remote.local_endpoint().ip();
    ASSERT_NE(next_ip, "127.0.0.1");
    PeerServer server;
    auto      *first = server.registerSession(std::move(first_accepted));
    auto      *next  = server.registerSession(std::move(next_accepted));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(next, nullptr);
    const auto first_fd = first->transport().native_handle();
    const auto next_fd  = next->transport().native_handle();
    {
        PeerNameScope probe(next_fd);
        ASSERT_TRUE(qb::io::socket::peer_endpoint(next_fd));
        ASSERT_EQ(probe.calls(), 1u) << "HARNESS REFUSED: native getpeername interception is inactive";
    }
    ASSERT_EQ(first->ip(), "127.0.0.1");
    ASSERT_EQ(next->ip(), next_ip);
    first->stop();
    next->stop();
    // Existing socket move-assignment swaps descriptors when both sides are open.
    first->transport() = std::move(next->transport());
    ASSERT_EQ(first->transport().native_handle(), next_fd);
    ASSERT_EQ(next->transport().native_handle(), first_fd);
    {
        PeerNameScope transferred(next_fd);
        EXPECT_EQ(first->ip(), next_ip);
        EXPECT_EQ(first->ip(), next_ip);
        EXPECT_EQ(transferred.calls(), 0u) << "moving a live connection must not repeat its cached peer lookup";
    }
    {
        PeerNameScope swapped(first_fd);
        EXPECT_EQ(next->ip(), "127.0.0.1");
        EXPECT_EQ(swapped.calls(), 0u) << "the source now owns the former destination connection and its cache";
    }
}

TEST_F(PeerIpCache, NativeUdpPeerRetargetAndDisconnectInvalidateTheCache) {
    qb::io::socket first(AF_INET, SOCK_DGRAM, 0);
    qb::io::socket next(AF_INET, SOCK_DGRAM, 0);
    qb::io::socket client(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(first.is_open());
    ASSERT_TRUE(next.is_open());
    ASSERT_TRUE(client.is_open());
    ASSERT_EQ(first.bind(qb::io::endpoint().as_in("127.0.0.1", 0)), 0);
    ASSERT_EQ(next.bind(qb::io::endpoint().as_in("127.0.0.1", 0)), 0);
    ASSERT_NE(first.local_endpoint().port(), next.local_endpoint().port());
    ASSERT_EQ(client.connect(first.local_endpoint()), 0);
    const auto fd = client.native_handle();
    {
        PeerNameScope probe(fd);
        ASSERT_TRUE(qb::io::socket::peer_endpoint(fd));
        ASSERT_EQ(probe.calls(), 1u) << "HARNESS REFUSED: native getpeername interception is inactive";
    }
    {
        PeerNameScope reads(fd);
        EXPECT_EQ(client.peer_ip(), "127.0.0.1");
        EXPECT_EQ(client.peer_ip(), "127.0.0.1");
        EXPECT_EQ(reads.calls(), 1u);
    }
    ASSERT_EQ(client.connect(next.local_endpoint()), 0);
    EXPECT_EQ(client.peer_endpoint().port(), next.local_endpoint().port());
    {
        PeerNameScope reads(fd);
        EXPECT_EQ(client.peer_ip(), "127.0.0.1");
        EXPECT_EQ(client.peer_ip(), "127.0.0.1");
        EXPECT_EQ(reads.calls(), 1u) << "retargeting a connected datagram socket changes its peer without changing fd";
    }
    ASSERT_EQ(qb::io::socket::connect(fd, first.local_endpoint()), 0);
    client.invalidate_peer_ip(); // raw/static helpers cannot find the owning C++ socket
    {
        PeerNameScope reads(fd);
        EXPECT_EQ(client.peer_ip(), "127.0.0.1");
        EXPECT_EQ(reads.calls(), 1u);
    }
    qb::io::endpoint invalid;
    invalid.len(0);
    EXPECT_NE(client.connect(invalid), 0);
    const auto after_failed_connect = client.peer_endpoint().ip();
    {
        PeerNameScope reads(fd);
        EXPECT_EQ(client.peer_ip(), after_failed_connect);
        EXPECT_EQ(reads.calls(), 1u) << "a failed connect attempt must not leave a cached association authoritative";
    }
    // AF_UNSPEC disconnect is not supported uniformly by host kernels. Compare
    // the member operation with a real uncached socket; even a failed native
    // attempt must invalidate cached peer information.
    ASSERT_EQ(client.connect(first.local_endpoint()), 0);
    EXPECT_EQ(client.peer_ip(), "127.0.0.1");
    qb::io::socket native_control;
    ASSERT_TRUE(native_control.open(AF_INET, SOCK_DGRAM, 0));
    ASSERT_EQ(native_control.connect(first.local_endpoint()), 0);
    const int native_result = qb::io::socket::disconnect(native_control.native_handle());
    const int native_error  = native_result == 0 ? 0 : qb::io::socket::get_last_errno();
    const int member_result = client.disconnect();
    const int member_error  = member_result == 0 ? 0 : qb::io::socket::get_last_errno();
    EXPECT_EQ(member_result, native_result);
    EXPECT_EQ(member_error, native_error);
    const auto actual_peer = qb::io::socket::peer_endpoint(fd).ip();
    {
        PeerNameScope reads(fd);
        EXPECT_EQ(client.peer_ip(), actual_peer);
        EXPECT_EQ(reads.calls(), 1u);
    }
    if (native_result == 0)
        EXPECT_TRUE(actual_peer.empty());
    ASSERT_EQ(client.connect(first.local_endpoint()), 0);
    EXPECT_EQ(client.peer_ip(), "127.0.0.1");
}

TEST_F(PeerIpCache, GenericSocketAdoptionClosesTheDestinationAndTransfersTheCache) {
    qb::io::tcp::listener first_listener;
    qb::io::tcp::socket   first_remote;
    qb::io::tcp::socket   first_accepted;
    ASSERT_TRUE(connect_pair(first_listener, first_remote, first_accepted));
    qb::io::tcp::listener next_listener;
    qb::io::tcp::socket   next_remote;
    qb::io::tcp::socket   next_accepted;
    ASSERT_TRUE(connect_distinct_pair(next_listener, next_remote, next_accepted));
    const auto next_ip = next_remote.local_endpoint().ip();
    ASSERT_NE(next_ip, "127.0.0.1");
    qb::io::socket incoming(next_accepted.release_handle());
    const auto     next_fd = incoming.native_handle();
    {
        PeerNameScope probe(next_fd);
        ASSERT_TRUE(qb::io::socket::peer_endpoint(next_fd));
        ASSERT_EQ(probe.calls(), 1u) << "HARNESS REFUSED: native getpeername interception is inactive";
    }
    ASSERT_EQ(incoming.peer_ip(), next_ip);
    PeerServer server;
    auto      *session = server.registerSession(std::move(first_accepted));
    ASSERT_NE(session, nullptr);
    const auto old_fd = session->transport().native_handle();
    ASSERT_NE(old_fd, next_fd);
    ASSERT_EQ(session->ip(), "127.0.0.1");
    session->stop();
    session->transport() = std::move(incoming);
    EXPECT_FALSE(incoming.is_open()) << "generic adoption must not swap the old destination back into its source";
    EXPECT_FALSE(qb::io::socket::peer_endpoint(old_fd)) << "generic adoption closes the previous destination";
    EXPECT_EQ(session->transport().native_handle(), next_fd);
    PeerNameScope transferred(next_fd);
    EXPECT_EQ(session->ip(), next_ip);
    EXPECT_EQ(session->ip(), next_ip);
    EXPECT_EQ(transferred.calls(), 0u);
}
#endif

} // namespace transport_accept_loopback_test
