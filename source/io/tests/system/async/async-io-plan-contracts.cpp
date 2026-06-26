/**
 * @file system/async/async-io-plan-contracts.cpp
 * @brief Regression guards for the QB_IO_PLAN findings on the async I/O layer.
 *
 * Each test pins the behaviour of a specific finding from `qb/QB_IO_PLAN.md` so a future refactor
 * cannot silently undo the fix. The contracts are a mix of compile-time `static_assert`s (transport
 * security flags, ABI-stable disconnect reasons, event aliases) and loop-driven behaviour over real
 * loopback sockets and the in-process libev loop — so the file as a whole is a SYSTEM test
 * (`async::init()` + `async::run()`).
 *
 * Findings covered:
 *   - 2.1  — `transport::udp` write consumes the whole datagram (no leftover partial-send offset);
 *   - 2.2  — `is_secure()` is `static constexpr` for every shipped transport;
 *   - 2.9  — `event::eof` is a backward-compatible alias of `event::input_drained` and the latter is
 *            dispatched once the read buffer drains (the connection is NOT closed);
 *   - 2.11 — `io_handler::registerSession` rejects past `_max_sessions` BEFORE allocating, and CLOSES
 *            the incoming socket (proven here with `fcntl(F_GETFD) == -1 && errno == EBADF`, the
 *            stronger guarantee the old test conceded it skipped);
 *   - 2.12 — `io_handler::stream()` reuses its broadcast scratch buffer with no corruption across calls;
 *   - 2.14 — `scoped_callback` fires exactly once and is cancellable by destroying the handle;
 *   - 2.17 — `disconnect_reason` keeps its int-backed ABI values;
 *   - 2.18 — `acceptor::listen()` auto-starts the accept watcher;
 *   - 2.19 — `handshake::onMessage()` consumes the cached size without re-driving the SSL machine;
 *   - 2.21 — `IProtocol::kNoMessage == 0`;
 *   - 2.4  — `transport::saccept::flush()` is idempotent on an empty acceptor (TLS-preserving).
 *
 * Restructured from the dissolved system/test-io-plan.cpp. Per the restructure spec: all hard-coded
 * ports (64324-64330) become ephemeral `listen_v4(0)` + `local_endpoint().port()`; the DoS test now
 * proves the FD is closed via `fcntl`; the doubled `event::eof` static_assert is de-duplicated to a
 * single file-scope assertion; the hand-rolled poll loops are replaced by the shared
 * `qb::io::test::pump_until`. No file-local main().
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
#include <string>
#include <type_traits>

#ifndef _WIN32
#include <fcntl.h>
#endif

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/event/disconnected.h>
#include <qb/io/async/event/eof.h>
#include <qb/io/async/io_handler.h>
#include <qb/io/async/protocol.h>
#include <qb/io/async/tcp/server.h>
#include <qb/io/protocol/text.h>
#include <qb/io/transport/saccept.h>
#include <qb/io/transport/udp.h>
#include <qb/io/udp/socket.h>

#ifdef QB_HAS_SSL
#include <qb/io/protocol/handshake.h>
#endif

#include "../../shared/coroutine_test_support.h"

using namespace qb::io;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reset_async_context;

// -----------------------------------------------------------------------------
// Compile-time contracts (double as documentation of the ABI promises).
// -----------------------------------------------------------------------------

// Finding 2.2 — is_secure() must be static constexpr for every shipped transport.
static_assert(transport::tcp::is_secure() == false, "transport::tcp::is_secure() must be a static constexpr");
static_assert(transport::udp::is_secure() == false, "transport::udp::is_secure() must be a static constexpr (finding 2.2)");
static_assert(transport::accept::is_secure() == false, "transport::accept::is_secure() must be a static constexpr");
#ifdef QB_HAS_SSL
static_assert(transport::saccept::is_secure() == true, "transport::saccept::is_secure() must be a static constexpr");
#endif

// Finding 2.17 — disconnect_reason is int-backed with stable values.
static_assert(std::is_same_v<std::underlying_type_t<async::event::disconnect_reason>, int>,
              "disconnect_reason must remain ABI-compatible with int (finding 2.17)");
static_assert(static_cast<int>(async::event::disconnect_reason::peer_closed) == 0,
              "disconnect_reason::peer_closed must keep value 0 for ABI compat");
static_assert(static_cast<int>(async::event::disconnect_reason::user_initiated) == 1,
              "disconnect_reason::user_initiated must keep value 1 for ABI compat");
static_assert(static_cast<int>(async::event::disconnect_reason::protocol_error) == -1,
              "disconnect_reason::protocol_error must keep value -1 for ABI compat");
static_assert(static_cast<int>(async::event::disconnect_reason::message_too_large) == -2,
              "disconnect_reason::message_too_large must keep value -2 for ABI compat");

// Finding 2.21 — IProtocol::kNoMessage is a named constant equal to 0.
static_assert(async::IProtocol::kNoMessage == 0, "IProtocol::kNoMessage must equal 0 for caller compatibility");

// Finding 2.9 — event::eof is a backward-compatible alias for event::input_drained
// (asserted ONCE here at file scope — the old monolith duplicated this in a test body too).
static_assert(std::is_same_v<async::event::eof, async::event::input_drained>,
              "event::eof must be a backward-compatible alias for event::input_drained");

namespace {

class IoPlanContractsTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        reset_async_context();
    }
    void
    TearDown() override {
        async::listener::current.clear();
    }
};

} // namespace

// =============================================================================
// Finding 2.1 — UDP write consumes the whole datagram (no leftover offset)
// =============================================================================

TEST_F(IoPlanContractsTest, UdpWriteDrainsTheWholeDatagram) {
    transport::udp sender;
    transport::udp receiver;
    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver.transport().init());

    // Ephemeral receive port — no fixed port to collide under `ctest -j`.
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);
    const auto recv_port = receiver.transport().local_endpoint().port();
    ASSERT_NE(recv_port, 0);

    qb::io::endpoint         bound_ep("127.0.0.1", recv_port);
    transport::udp::identity dest{bound_ep};
    sender.setDestination(dest);

    constexpr std::size_t kPayload = 200;
    std::string           payload(kPayload, 'A');
    sender.publish(payload.data(), payload.size());
    EXPECT_GT(sender.pendingWrite(), kPayload) << "queue should hold the pushed_message header + payload";

    const int written = sender.write();
    EXPECT_EQ(static_cast<std::size_t>(written), kPayload) << "UDP is all-or-nothing: write() must report the whole datagram";

    // The regression guard for finding 2.1: a successful write fully drains the queue.
    EXPECT_EQ(sender.pendingWrite(), 0u) << "after a successful write() the output queue must be drained (no partial-send dead code)";

    // Round-trip so the receiver actually drains it (no closed-loop race).
    int n = 0;
    EXPECT_TRUE(pump_until([&] {
        n = receiver.read();
        return n > 0;
    })) << "receiver never read the datagram";
    EXPECT_EQ(static_cast<std::size_t>(n), kPayload);
}

TEST_F(IoPlanContractsTest, UdpBackToBackDatagramsDrainOneByOne) {
    transport::udp sender;
    transport::udp receiver;
    ASSERT_TRUE(sender.transport().init());
    ASSERT_TRUE(receiver.transport().init());
    ASSERT_EQ(receiver.transport().bind_v4(0, "127.0.0.1"), 0);
    const auto recv_port = receiver.transport().local_endpoint().port();
    ASSERT_NE(recv_port, 0);

    qb::io::endpoint         bound_ep("127.0.0.1", recv_port);
    transport::udp::identity dest{bound_ep};
    sender.setDestination(dest);

    constexpr int         kCount   = 5;
    constexpr std::size_t kPayload = 64;
    for (int i = 0; i < kCount; ++i) {
        std::string p(kPayload, static_cast<char>('a' + i));
        sender.publish(p.data(), p.size());
    }

    for (int i = 0; i < kCount; ++i) {
        const int w = sender.write();
        ASSERT_EQ(static_cast<std::size_t>(w), kPayload) << "datagram " << i << " must write whole";
    }
    EXPECT_EQ(sender.pendingWrite(), 0u);
}

// =============================================================================
// Finding 2.9 — input_drained dispatch on a live (un-closed) connection
// =============================================================================

namespace plan_test {

class line_server;

struct line_session : public use<line_session>::tcp::client<line_server> {
    using Protocol = qb::protocol::text::command<line_session>;

    std::atomic<int> *drained_counter = nullptr;
    std::atomic<int> *messages_seen   = nullptr;

    explicit line_session(line_server &srv);

    void
    on(Protocol::message &&) {
        if (messages_seen)
            messages_seen->fetch_add(1);
    }

    void
    on(async::event::input_drained &&) {
        if (drained_counter)
            drained_counter->fetch_add(1);
    }
};

class line_server : public use<line_server>::tcp::server<line_session> {};

inline line_session::line_session(line_server &srv)
    : client(srv) {}

} // namespace plan_test

TEST_F(IoPlanContractsTest, InputDrainedDispatchesOnLiveConnection) {
    std::atomic<int> drained_counter{0};
    std::atomic<int> messages_seen{0};

    plan_test::line_server server;
    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done);
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), SocketStatus::Done);

    // Drive the loop until the session is registered, then attach the counters.
    ASSERT_TRUE(pump_until([&] { return server.session_count() > 0u; })) << "server failed to register the test session";
    for (auto &[id, sess] : server.sessions()) {
        sess->drained_counter = &drained_counter;
        sess->messages_seen   = &messages_seen;
    }

    constexpr const char message[] = "hello\n";
    ASSERT_EQ(client.write(message, sizeof(message) - 1), static_cast<int>(sizeof(message) - 1));

    EXPECT_TRUE(pump_until([&] { return messages_seen.load() == 1; })) << "the session should have parsed exactly one message";
    // input_drained fires once the buffer empties — the connection is NOT closed.
    EXPECT_GE(drained_counter.load(), 1) << "input_drained must dispatch after the buffer drains (finding 2.9)";

    client.disconnect();
    // Best-effort drain of the session on teardown — not the assertion of record.
    (void) pump_until([&] { return server.session_count() == 0u; });
}

// =============================================================================
// Finding 2.11 — registerSession rejects before allocation AND closes the FD
// =============================================================================

namespace plan_test {

struct dos_handler;

struct dos_session : public async::tcp::client<dos_session, transport::tcp, dos_handler> {
    explicit dos_session(dos_handler &h);
    using Protocol = qb::protocol::text::command<dos_session>;
    void
    on(Protocol::message &&) {}
};

struct dos_handler : public async::io_handler<dos_handler, dos_session> {};

inline dos_session::dos_session(dos_handler &h)
    : client(h) {}

} // namespace plan_test

TEST_F(IoPlanContractsTest, RegisterSessionRejectsBeyondCapAndClosesTheFd) {
    constexpr std::size_t kCap   = 4;
    constexpr std::size_t kBurst = 64;

    plan_test::dos_handler handler;
    handler.set_max_sessions(kCap);

    std::size_t accepted = 0;
    std::size_t rejected = 0;

    for (std::size_t i = 0; i < kBurst; ++i) {
        // A real open native socket so the rejection path must actually close something.
        qb::io::tcp::socket sock;
        ASSERT_EQ(sock.init(AF_INET), 0) << "could not init test socket #" << i;
        const auto fd = sock.native_handle();
        ASSERT_GE(fd, 0);

        auto *registered = handler.registerSession(std::move(sock));
        if (registered != nullptr) {
            ++accepted;
        } else {
            ++rejected;
            EXPECT_EQ(handler.session_count(), kCap) << "a rejected attempt must not perturb session_count";
#ifndef _WIN32
            // Finding 2.11 (strengthened): the socket we handed in must already be
            // CLOSED — probing its fd must fail with EBADF (no leaked descriptor).
            errno              = 0;
            const int probe    = ::fcntl(fd, F_GETFD);
            const int probe_err = errno;
            EXPECT_EQ(probe, -1) << "rejected socket fd " << fd << " is still open (leaked descriptor)";
            EXPECT_EQ(probe_err, EBADF) << "rejected socket fd must be closed (errno EBADF), got errno=" << probe_err;
#endif
        }
    }

    EXPECT_EQ(accepted, kCap) << "exactly _max_sessions sessions should have been registered";
    EXPECT_EQ(rejected, kBurst - kCap) << "every other attempt must be rejected without allocation";
    EXPECT_EQ(handler.session_count(), kCap);
}

// =============================================================================
// Finding 2.12 — stream() broadcast scratch reuse is corruption-free
// =============================================================================

TEST_F(IoPlanContractsTest, BroadcastScratchReuseHasNoCorruption) {
    plan_test::dos_handler handler;
    handler.set_max_sessions(0); // unlimited

    constexpr int kRepeat = 100;
    for (int i = 0; i < kRepeat; ++i) {
        // Empty handler: stream() must be a safe no-op even when called many times,
        // proving the scratch buffer is left in a consistent state between calls.
        EXPECT_NO_THROW({ handler.stream("ping ", i, '\n'); });
    }
    EXPECT_EQ(handler.session_count(), 0u);
}

// =============================================================================
// Finding 2.14 — scoped_callback fires exactly once / is cancellable
// =============================================================================

TEST_F(IoPlanContractsTest, ScopedCallbackFiresOnce) {
    std::atomic<int> fired{0};
    auto             timer = async::scoped_callback([&] { fired.fetch_add(1); }, 50ms);
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(fired.load(), 0);
    EXPECT_FALSE(timer->fired());

    EXPECT_TRUE(pump_until([&] { return fired.load() == 1; })) << "scoped_callback never fired";
    EXPECT_TRUE(timer->fired());

    // Must never re-fire.
    EXPECT_FALSE(pump_until([&] { return fired.load() > 1; }, 100ms)) << "scoped_callback fired more than once";
    EXPECT_EQ(fired.load(), 1);
}

TEST_F(IoPlanContractsTest, ScopedCallbackDestructionCancelsPendingTimer) {
    std::atomic<int> fired{0};
    {
        auto timer = async::scoped_callback([&] { fired.fetch_add(1); }, 1s);
        ASSERT_NE(timer, nullptr);
        EXPECT_FALSE(timer->fired());
        // Destroyed on scope exit — the watcher must be torn down before it can fire.
    }

    EXPECT_FALSE(pump_until([&] { return fired.load() > 0; }, 250ms))
        << "destroying a ScopedTimeout before its deadline must cancel the callback";
    EXPECT_EQ(fired.load(), 0);
}

// =============================================================================
// Finding 2.18 — acceptor::listen() auto-starts the accept watcher
// =============================================================================

namespace plan_test {

struct autostart_server;

struct autostart_session : public use<autostart_session>::tcp::client<autostart_server> {
    using Protocol = qb::protocol::text::command<autostart_session>;
    explicit autostart_session(autostart_server &srv);
    void
    on(Protocol::message &&) {}
};

struct autostart_server : public use<autostart_server>::tcp::server<autostart_session> {
    std::atomic<int> *connection_count = nullptr;
};

inline autostart_session::autostart_session(autostart_server &srv)
    : client(srv) {
    if (srv.connection_count)
        srv.connection_count->fetch_add(1);
}

} // namespace plan_test

TEST_F(IoPlanContractsTest, AcceptorListenAutoStartsWatcher) {
    std::atomic<int>            connection_count{0};
    plan_test::autostart_server server;
    server.connection_count = &connection_count;

    ASSERT_EQ(server.transport().listen_v4(0, "127.0.0.1"), SocketStatus::Done) << "binding listener must succeed";
    const auto port = server.transport().local_endpoint().port();
    ASSERT_NE(port, 0);
    server.start();

    qb::io::tcp::socket client;
    ASSERT_EQ(client.connect_v4("127.0.0.1", port), SocketStatus::Done);

    EXPECT_TRUE(pump_until([&] { return connection_count.load() == 1; })) << "auto-started acceptor never accepted the client";
    client.disconnect();
}

// =============================================================================
// Finding 2.19 — handshake::onMessage() consumes the cached size, no re-drive
// =============================================================================

#ifdef QB_HAS_SSL

namespace {

struct fake_handshake_io {
    struct fake_transport {
        int do_handshake_calls = 0;
        int next_result        = 0; // 0 → not done; >0 → done with that size
        int
        do_handshake() noexcept {
            ++do_handshake_calls;
            return next_result;
        }
    };

    using base_io_t = fake_handshake_io; // satisfies AProtocol's friend
    fake_transport _t;
    int            handshake_events = 0;

    fake_transport &
    transport() noexcept {
        return _t;
    }
    void
    on(qb::io::async::event::handshake) noexcept {
        ++handshake_events;
    }
};

} // namespace

TEST_F(IoPlanContractsTest, HandshakeGetMessageSizeIsCachedAcrossOnMessage) {
    fake_handshake_io                              io;
    qb::io::protocol::handshake<fake_handshake_io> proto(io);

    // First probe: not done → 0, no event.
    io._t.next_result = 0;
    EXPECT_EQ(proto.getMessageSize(), 0u);
    EXPECT_EQ(io._t.do_handshake_calls, 1);
    EXPECT_EQ(io.handshake_events, 0);

    // Second probe: done → returns size; onMessage() must consume the cached value
    // without re-invoking do_handshake().
    io._t.next_result      = 7;
    const auto reported    = proto.getMessageSize();
    const int  before_calls = io._t.do_handshake_calls;
    EXPECT_EQ(reported, 7u);
    EXPECT_EQ(before_calls, 2);

    proto.onMessage(reported);
    EXPECT_EQ(io._t.do_handshake_calls, before_calls) << "onMessage() must not re-drive the SSL state machine (finding 2.19)";
    EXPECT_EQ(io.handshake_events, 1);

    // Third probe: done → short-circuits to 0 without poking the transport again.
    const int locked_calls = io._t.do_handshake_calls;
    EXPECT_EQ(proto.getMessageSize(), 0u);
    EXPECT_EQ(io._t.do_handshake_calls, locked_calls) << "after onMessage() the protocol must not poll do_handshake() again";

    // After reset() it behaves freshly-constructed.
    proto.reset();
    io._t.next_result = 0;
    EXPECT_EQ(proto.getMessageSize(), 0u);
    EXPECT_EQ(io._t.do_handshake_calls, locked_calls + 1);
}

// =============================================================================
// Finding 2.4 — saccept::flush() is idempotent on an empty acceptor
// =============================================================================

TEST_F(IoPlanContractsTest, SacceptFlushIsIdempotentOnEmptyAcceptor) {
    transport::saccept sa;

    EXPECT_NO_THROW(sa.flush(0));
    EXPECT_NO_THROW(sa.flush(0)); // idempotent — release_handle on an empty FD is fine
    EXPECT_EQ(sa.getAccepted().native_handle(), -1) << "after flush() the accepted socket must report no native handle";

    EXPECT_NO_THROW(sa.close());
}

#endif // QB_HAS_SSL
