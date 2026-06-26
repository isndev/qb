/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/tcp-connector-loopback.cpp
 * @brief The coroutine TCP connector against a *real* loopback peer — end-to-end.
 *
 * Proves `co_await qb::io::async::tcp::connect()` / `connect_with_socket()` drives a genuine
 * non-blocking TCP handshake on the qb-io event loop and resumes the coroutine with a
 * `std::optional<socket>` that is engaged iff the kernel actually connected. Every test binds an
 * *ephemeral* (`:0`) v4 loopback listener and reads the OS-assigned port back — there is not a
 * single fixed port, so the whole file is collision-free under `ctest -j` (see the dossier's
 * FIXED-PORT FRAGILITY finding against the legacy `test-io.cpp` CORO_TCP suite). It merges the
 * unique real-socket scenarios that were spread across three former monoliths
 * (test-coroutine-tcp-connector.cpp, test-coroutine-tcp-client-integration.cpp,
 * test-coroutine-tcp-client-io.cpp); the connector's *internal* state machine — fake socket,
 * multi-turn handshake, verify_peer/set_insecure — lives in its mock peer at
 * system/tcp/tcp-connector-state-machine.cpp, and the channel-backpressure unit moved to
 * coro-data.
 *
 * Scenarios (each a distinct real-socket behaviour, deduped — the bare "connect to a local
 * listener happy path" appears exactly once, in ConnectResumesWithOpenSocket):
 *   - ConnectResumesWithOpenSocket            : co_await connect → engaged + is_open(); server accepts.
 *   - ConnectToRefusedPortResumesEmpty        : connect to a just-closed ephemeral port → disengaged.
 *   - RequestResponseRoundTrip                : connect, write "PING", read "PONG" back (dual oracle:
 *                                               client sees PONG *and* server confirms it saw PING).
 *   - ConnectedSocketWritesToServer           : client write is observed verbatim server-side.
 *   - CoalescedServerWritesReadInOneRecv      : 3 newline-framed records sent as one write arrive
 *                                               coalesced in a single recv (renamed from the
 *                                               misleading "MultipleFrames" — TCP has no frames).
 *   - FailedConnectDoesNotPoisonLaterConnect  : a refused connect followed by a good one, same coro.
 *   - ParallelClientsAllConnect               : N coroutines connect to one listener concurrently.
 *   - ConnectWithExistingSocketSucceeds       : connect_with_socket over a pre-init'd fd.
 *   - SequentialAttemptsReuseScheduler        : three back-to-back connects on the same loop.
 *
 * De-flake: every wait is the shared `qb::io::test::pump_until` (loud bounded timeout, never a
 * silent hang); `read_until_data` surfaces a read timeout as an *explicit* failure flag the test
 * asserts on (so a stalled read fails loudly instead of comparing against ""). Each test asserts a
 * completion flag AFTER the pump, so a coroutine that never ran FAILS rather than passes vacuously.
 *
 * @ingroup Tests
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <qb/io/async/coroutine.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/loopback_fixture.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

// ---------------------------------------------------------------------------
// Fixture: fresh per-test event loop in SetUp(), full coroutine-scheduler +
// watcher teardown in TearDown() (the established coroutine-suite idiom —
// reset the scheduler first, then clear the listener's registered events).
// ---------------------------------------------------------------------------
class TcpConnectorLoopbackTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }

    void
    TearDown() override {
        qb::io::async::listener::current.reset_coro_scheduler();
        qb::io::async::listener::current.clear();
    }
};

std::string
tcp_uri(unsigned short port) {
    return "tcp://127.0.0.1:" + std::to_string(port);
}

// Bind an ephemeral v4 loopback listener, close it immediately, and return its
// (now free, refusing) port. The brief reuse window is TOCTOU-racy by nature —
// acceptable for "connect must fail" loopback scaffolding, same contract as the
// shared reserve_free_tcp_port().
unsigned short
refused_port() {
    qb::io::tcp::listener listener;
    EXPECT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    listener.disconnect();
    listener.close();
    return port;
}

// A coroutine read that polls the connected socket until at least one byte
// arrives or a bounded deadline elapses. On timeout it sets `timed_out` so the
// caller can assert the read actually delivered data (vs. silently comparing an
// empty string and "passing"). co_await sleep(1ms) keeps the event loop turning.
task<std::string>
read_until_data(qb::io::tcp::socket &socket, bool &timed_out, qb::duration timeout = 1s) {
    const auto deadline    = qb::mono_now() + timeout;
    char       buffer[256] = {};

    while (qb::mono_now() < deadline) {
        const int n = socket.read(buffer, sizeof(buffer));
        if (n > 0) {
            timed_out = false;
            co_return std::string(buffer, static_cast<std::size_t>(n));
        }
        co_await sleep(1ms);
    }

    timed_out = true;
    co_return std::string{};
}

} // namespace

// ---------------------------------------------------------------------------
// Happy path: the single canonical "connect to a local listener" success.
// (Deduped — this is the one place the bare success path lives.)
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorLoopbackTest, ConnectResumesWithOpenSocket) {
    std::atomic<int>  server_port{0};
    std::atomic<bool> accepted{false};

    std::thread server([&] {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        qb::io::tcp::socket sock = listener.accept();
        accepted.store(sock.is_open());
        sock.close();
        listener.disconnect();
        listener.close();
    });

    ASSERT_TRUE(pump_until([&] { return server_port.load() > 0; }, 1s)) << "server never bound";

    std::atomic<bool> done{false};
    bool              connected = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(
            qb::io::uri{tcp_uri(static_cast<unsigned short>(server_port.load()))}, 1s);
        connected = socket.has_value() && socket->is_open();
        if (socket)
            socket->close();
        done.store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "connect coroutine never completed";
    if (server.joinable())
        server.join();

    ASSERT_TRUE(done.load());
    EXPECT_TRUE(connected);
    EXPECT_TRUE(accepted.load());
}

// ---------------------------------------------------------------------------
// A connect to a port nobody is listening on resumes with a disengaged optional.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorLoopbackTest, ConnectToRefusedPortResumesEmpty) {
    const unsigned short port = refused_port();
    ASSERT_GT(port, 0u);

    std::atomic<bool> done{false};
    bool              connected = true;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(port)}, 500ms);
        connected   = socket.has_value();
        done.store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "refused-connect coroutine never completed";

    ASSERT_TRUE(done.load());
    EXPECT_FALSE(connected);
}

// ---------------------------------------------------------------------------
// Full request/response: client connects, writes "PING", reads "PONG".
// Dual oracle — the client asserts it received PONG, AND the server asserts it
// saw exactly "PING" before replying, so neither side can pass on a half-flow.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorLoopbackTest, RequestResponseRoundTrip) {
    std::atomic<int>  server_port{0};
    std::atomic<bool> server_done{false};
    std::atomic<bool> server_saw_ping{false};

    std::thread server([&] {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        qb::io::tcp::socket sock = listener.accept();
        if (sock.is_open()) {
            char      request[64] = {};
            const int n           = sock.read(request, sizeof(request));
            if (n > 0 && std::string_view(request, static_cast<std::size_t>(n)) == "PING") {
                server_saw_ping.store(true);
                const char reply[] = "PONG";
                (void) sock.write(reply, std::strlen(reply));
            }
            sock.close();
        }
        listener.disconnect();
        listener.close();
        server_done.store(true);
    });

    ASSERT_TRUE(pump_until([&] { return server_port.load() > 0; }, 1s)) << "server never bound";

    std::atomic<bool> client_done{false};
    std::string       response;
    bool              connected   = false;
    bool              write_ok    = false;
    bool              read_timeout = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(
            qb::io::uri{tcp_uri(static_cast<unsigned short>(server_port.load()))}, 1s);
        connected = socket.has_value();
        if (!socket) {
            client_done.store(true);
            co_return;
        }

        const char request[] = "PING";
        write_ok = socket->write(request, std::strlen(request)) == static_cast<int>(std::strlen(request));
        if (!write_ok) {
            socket->close();
            client_done.store(true);
            co_return;
        }

        response = co_await read_until_data(*socket, read_timeout);
        socket->close();
        client_done.store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return client_done.load() && server_done.load(); }))
        << "request/response never completed";
    if (server.joinable())
        server.join();

    ASSERT_TRUE(client_done.load());
    ASSERT_TRUE(server_done.load());
    EXPECT_TRUE(connected);
    EXPECT_TRUE(write_ok);
    EXPECT_FALSE(read_timeout) << "client never read the server reply";
    EXPECT_EQ(response, "PONG");
    EXPECT_TRUE(server_saw_ping.load());
}

// ---------------------------------------------------------------------------
// A bare write on the connected socket reaches the server byte-for-byte.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorLoopbackTest, ConnectedSocketWritesToServer) {
    std::atomic<int>  server_port{0};
    std::atomic<bool> server_done{false};
    std::string       received;

    std::thread server([&] {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        qb::io::tcp::socket sock = listener.accept();
        if (sock.is_open()) {
            char      buffer[64] = {};
            const int n          = sock.read(buffer, sizeof(buffer));
            if (n > 0)
                received.assign(buffer, static_cast<std::size_t>(n));
            sock.close();
        }
        listener.disconnect();
        listener.close();
        server_done.store(true);
    });

    ASSERT_TRUE(pump_until([&] { return server_port.load() > 0; }, 1s)) << "server never bound";

    std::atomic<bool> client_done{false};
    bool              write_ok = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(
            qb::io::uri{tcp_uri(static_cast<unsigned short>(server_port.load()))}, 1s);
        if (socket) {
            const char payload[] = "PING";
            write_ok = socket->write(payload, std::strlen(payload)) == static_cast<int>(std::strlen(payload));
            socket->close();
        }
        client_done.store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return client_done.load() && server_done.load(); }))
        << "write coroutine never completed";
    if (server.joinable())
        server.join();

    ASSERT_TRUE(client_done.load());
    ASSERT_TRUE(server_done.load());
    EXPECT_TRUE(write_ok);
    EXPECT_EQ(received, "PING");
}

// ---------------------------------------------------------------------------
// Three newline-delimited records sent in ONE server write arrive coalesced in a
// single client recv. Renamed from the legacy "MultipleFrames" — TCP is a byte
// stream with no frame boundaries; the contract is that the bytes arrive intact
// and in order, NOT that they are delivered as three separate reads.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorLoopbackTest, CoalescedServerWritesReadInOneRecv) {
    std::atomic<int> server_port{0};

    std::thread server([&] {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        qb::io::tcp::socket sock = listener.accept();
        if (sock.is_open()) {
            const char payload[] = "ALPHA\nBETA\nGAMMA\n";
            (void) sock.write(payload, std::strlen(payload));
            sock.close();
        }
        listener.disconnect();
        listener.close();
    });

    ASSERT_TRUE(pump_until([&] { return server_port.load() > 0; }, 1s)) << "server never bound";

    std::atomic<bool> done{false};
    std::string       data;
    bool              read_timeout = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(
            qb::io::uri{tcp_uri(static_cast<unsigned short>(server_port.load()))}, 1s);
        if (!socket) {
            done.store(true);
            co_return;
        }
        data = co_await read_until_data(*socket, read_timeout);
        socket->close();
        done.store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "coalesced-read coroutine never completed";
    if (server.joinable())
        server.join();

    ASSERT_TRUE(done.load());
    EXPECT_FALSE(read_timeout) << "client never read the coalesced payload";
    EXPECT_EQ(data, "ALPHA\nBETA\nGAMMA\n");
}

// ---------------------------------------------------------------------------
// A refused connect must not poison a later good connect on the same coroutine /
// the same event loop (each connect attempt is independently torn down).
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorLoopbackTest, FailedConnectDoesNotPoisonLaterConnect) {
    const unsigned short dead_port = refused_port();
    ASSERT_GT(dead_port, 0u);

    std::atomic<int> server_port{0};
    std::thread      server([&] {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));
        qb::io::tcp::socket sock = listener.accept();
        sock.close();
        listener.disconnect();
        listener.close();
    });

    ASSERT_TRUE(pump_until([&] { return server_port.load() > 0; }, 1s)) << "server never bound";

    std::atomic<bool> done{false};
    bool              first_failed     = false;
    bool              second_succeeded = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto failed  = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(dead_port)}, 500ms);
        first_failed = !failed.has_value();

        auto socket = co_await qb::io::async::tcp::connect(
            qb::io::uri{tcp_uri(static_cast<unsigned short>(server_port.load()))}, 1s);
        second_succeeded = socket.has_value() && socket->is_open();
        if (socket)
            socket->close();
        done.store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "fail-then-succeed coroutine never completed";
    if (server.joinable())
        server.join();

    ASSERT_TRUE(done.load());
    EXPECT_TRUE(first_failed);
    EXPECT_TRUE(second_succeeded);
}

// ---------------------------------------------------------------------------
// N coroutines connect to a single listener concurrently; every one completes
// and connects (exact counts, not "at least one").
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorLoopbackTest, ParallelClientsAllConnect) {
    constexpr int    kClients = 3;
    std::atomic<int> server_port{0};
    std::atomic<int> accepted{0};

    std::thread server([&] {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        for (int i = 0; i < kClients; ++i) {
            qb::io::tcp::socket sock = listener.accept();
            if (sock.is_open()) {
                ++accepted;
                sock.close();
            }
        }
        listener.disconnect();
        listener.close();
    });

    ASSERT_TRUE(pump_until([&] { return server_port.load() > 0; }, 1s)) << "server never bound";

    std::atomic<int> completed{0};
    std::atomic<int> connected{0};

    for (int i = 0; i < kClients; ++i) {
        coro_scheduler().spawn([&]() -> task<void> {
            auto socket = co_await qb::io::async::tcp::connect(
                qb::io::uri{tcp_uri(static_cast<unsigned short>(server_port.load()))}, 1s);
            if (socket) {
                ++connected;
                socket->close();
            }
            ++completed;
            co_return;
        });
    }

    EXPECT_TRUE(pump_until([&] { return completed.load() == kClients && accepted.load() == kClients; }, 3s))
        << "not all parallel clients completed";
    if (server.joinable())
        server.join();

    EXPECT_EQ(completed.load(), kClients);
    EXPECT_EQ(connected.load(), kClients);
    EXPECT_EQ(accepted.load(), kClients);
}

// ---------------------------------------------------------------------------
// connect_with_socket over a caller-supplied, pre-init'd socket succeeds and the
// resumed optional is engaged + open.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorLoopbackTest, ConnectWithExistingSocketSucceeds) {
    std::atomic<int>  server_port{0};
    std::atomic<bool> accepted{false};

    std::thread server([&] {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        qb::io::tcp::socket sock = listener.accept();
        accepted.store(sock.is_open());
        sock.close();
        listener.disconnect();
        listener.close();
    });

    ASSERT_TRUE(pump_until([&] { return server_port.load() > 0; }, 1s)) << "server never bound";

    std::atomic<bool> done{false};
    bool              connected = false;
    bool              init_ok   = false;

    coro_scheduler().spawn([&]() -> task<void> {
        qb::io::tcp::socket existing_socket;
        init_ok = existing_socket.init(AF_INET) == qb::io::SocketStatus::Done;
        if (!init_ok) {
            done.store(true);
            co_return;
        }

        auto socket = co_await qb::io::async::tcp::connect_with_socket(
            std::move(existing_socket),
            qb::io::uri{tcp_uri(static_cast<unsigned short>(server_port.load()))}, 1s);
        connected = socket.has_value() && socket->is_open();
        if (socket)
            socket->close();
        done.store(true);
        co_return;
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "connect_with_socket coroutine never completed";
    if (server.joinable())
        server.join();

    ASSERT_TRUE(done.load());
    EXPECT_TRUE(init_ok);
    EXPECT_TRUE(connected);
    EXPECT_TRUE(accepted.load());
}

// ---------------------------------------------------------------------------
// Three back-to-back connect attempts reuse the same event loop / scheduler
// without leaking watchers — all three connect.
// ---------------------------------------------------------------------------
TEST_F(TcpConnectorLoopbackTest, SequentialAttemptsReuseScheduler) {
    constexpr int    kAttempts = 3;
    std::atomic<int> completions{0};
    std::atomic<int> successes{0};

    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        std::atomic<int> server_port{0};
        std::thread      server([&] {
            qb::io::tcp::listener listener;
            ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
            server_port.store(static_cast<int>(listener.local_endpoint().port()));

            qb::io::tcp::socket sock = listener.accept();
            sock.close();
            listener.disconnect();
            listener.close();
        });

        ASSERT_TRUE(pump_until([&] { return server_port.load() > 0; }, 1s)) << "server never bound";

        coro_scheduler().spawn([&, port = static_cast<unsigned short>(server_port.load())]() -> task<void> {
            auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(port)}, 1s);
            if (socket) {
                ++successes;
                socket->close();
            }
            ++completions;
            co_return;
        });

        EXPECT_TRUE(pump_until([&] { return completions.load() == attempt + 1; }))
            << "attempt " << attempt << " never completed";
        if (server.joinable())
            server.join();
    }

    EXPECT_EQ(completions.load(), kAttempts);
    EXPECT_EQ(successes.load(), kAttempts);
}
