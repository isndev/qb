/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/tcp/connect-fallback.cpp
 * @brief A connect falls back through a list of addresses (Huly QB-164), asynchronously and blocking.
 *
 * Every case is hermetic: the addresses are given, not resolved -- a closed loopback port (nothing listens
 * on it: refused), a loopback listener, and on Linux an address that drops SYNs (a listener whose accept
 * queue is full). What is pinned:
 *  - the async connector, callback and coroutine forms, tries the next address when one is refused, and
 *    delivers exactly one failure when every address refuses; an empty list fails;
 *  - an address that does not answer gives way within its share of the deadline
 *    (`tcp::connect_attempt_budget`), not at the deadline;
 *  - a socket handed over OPEN gets the first address only; one handed over not open falls back;
 *  - TLS: the fallback is the TCP connect's, and a TLS failure on an address that answered is final;
 *  - the blocking `tcp::socket::connect(endpoints)` (and its TLS twin) run the same fallback;
 *  - `connect_attempt_budget` shares the time the way it says.
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#ifdef QB_HAS_SSL
#include <qb/io/tcp/ssl/listener.h>
#include <qb/io/tcp/ssl/socket.h>
#include "../../shared/ssl_fixtures.h"
#endif

#include "../../shared/coroutine_test_support.h"

using namespace std::chrono_literals;
using qb::io::endpoint;

// A named namespace: these helpers are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace connect_fallback_test {

/// A loopback port nothing listens on: bound once to learn a free port, then closed.
[[nodiscard]] std::uint16_t
closed_port() {
    qb::io::tcp::listener probe;
    if (probe.listen_v4(0, "127.0.0.1") != 0)
        return 0;
    const auto port = probe.local_endpoint().port();
    probe.disconnect();
    probe.close();
    return port;
}

[[nodiscard]] endpoint
loopback(std::uint16_t port) {
    return endpoint().as_in("127.0.0.1", port);
}

/// A plain loopback listener: the kernel completes the TCP handshake into its backlog, no accept needed.
struct PlainServer {
    qb::io::tcp::listener lst;
    std::uint16_t         port = 0;
    PlainServer() {
        if (lst.listen_v4(0, "127.0.0.1") == 0)
            port = lst.local_endpoint().port();
    }
};

class ConnectFallback : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

/// Wake a thread blocked in `accept()` on `port`: a shutdown of the listener does so on Linux and not on
/// Windows, a connection does everywhere.
void
wake_accept(std::uint16_t port) {
    qb::io::tcp::socket waker;
    (void) waker.connect(loopback(port), 1s);
}

} // namespace connect_fallback_test

using connect_fallback_test::closed_port;
using connect_fallback_test::ConnectFallback;
using connect_fallback_test::loopback;
using connect_fallback_test::PlainServer;

// ---------------------------------------------------------------------------
// The attempt budget
// ---------------------------------------------------------------------------

TEST(ConnectAttemptBudget, SharesTheTimeLeftWithATwoSecondFloor) {
    using qb::io::tcp::connect_attempt_budget;
    EXPECT_EQ(connect_attempt_budget(6s, 2), qb::duration{3s}) << "an equal share";
    EXPECT_EQ(connect_attempt_budget(3s, 2), qb::duration{2s}) << "never under two seconds";
    EXPECT_EQ(connect_attempt_budget(1s, 3), qb::duration{1s}) << "never over what is left";
    EXPECT_EQ(connect_attempt_budget(5s, 1), qb::duration{5s}) << "the last address gets all of it";
    EXPECT_EQ(connect_attempt_budget(0s, 3), qb::duration::zero());
    EXPECT_EQ(connect_attempt_budget(-1s, 2), qb::duration::zero());
}

// ---------------------------------------------------------------------------
// Asynchronous: the connector
// ---------------------------------------------------------------------------

TEST_F(ConnectFallback, TheConnectorFallsBackPastARefusedAddress) {
    PlainServer server;
    const auto  refused = closed_port();
    ASSERT_NE(server.port, 0);
    ASSERT_NE(refused, 0);

    std::optional<qb::io::tcp::socket> got;
    std::atomic<bool>                  done{false};
    qb::io::async::listener::current.coro_scheduler().spawn([&]() -> qb::io::async::task<void> {
        got  = co_await qb::io::async::tcp::connect(std::vector<endpoint>{loopback(refused), loopback(server.port)}, "", 5s);
        done = true;
    });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 10s)) << "the connect never completed";
    ASSERT_TRUE(got.has_value()) << "the connector did not fall back to the listening address";
    EXPECT_EQ(got->peer_endpoint().port(), server.port);
}

TEST_F(ConnectFallback, EveryAddressRefusedIsOneFailure) {
    const auto a = closed_port();
    const auto b = closed_port();
    ASSERT_NE(a, 0);
    ASSERT_NE(b, 0);
    int  calls = 0;
    bool open  = true;
    qb::io::async::tcp::connect<qb::io::tcp::socket>(
        std::vector<endpoint>{loopback(a), loopback(b)}, "",
        [&](qb::io::tcp::socket &&s) {
            ++calls;
            open = s.is_open();
        },
        5s);
    ASSERT_TRUE(qb::io::test::pump_until([&] { return calls > 0; }, 10s)) << "no completion";
    EXPECT_FALSE(qb::io::test::pump_until([&] { return calls > 1; }, 50ms));
    EXPECT_EQ(calls, 1) << "the failure was delivered more than once";
    EXPECT_FALSE(open);
}

TEST_F(ConnectFallback, AnEmptyListFails) {
    int  calls = 0;
    bool open  = true;
    qb::io::async::tcp::connect<qb::io::tcp::socket>(std::vector<endpoint>{}, "", [&](qb::io::tcp::socket &&s) {
        ++calls;
        open = s.is_open();
    });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return calls > 0; })) << "no completion";
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(open);
}

TEST_F(ConnectFallback, ASocketHandedOverOpenGetsTheFirstAddressOnly) {
    PlainServer server;
    const auto  refused = closed_port();
    ASSERT_NE(server.port, 0);
    ASSERT_NE(refused, 0);

    // Handed over OPEN: its descriptor may carry options a fresh one would not -- one attempt.
    {
        qb::io::tcp::socket opened;
        ASSERT_EQ(opened.init(AF_INET), 0);
        int  calls = 0;
        bool open  = true;
        qb::io::async::tcp::connect(
            std::move(opened), std::vector<endpoint>{loopback(refused), loopback(server.port)}, "",
            [&](qb::io::tcp::socket &&s) {
                ++calls;
                open = s.is_open();
            },
            5s);
        ASSERT_TRUE(qb::io::test::pump_until([&] { return calls > 0; }, 10s)) << "no completion";
        EXPECT_FALSE(open) << "a socket handed over open was replaced to reach the second address";
    }
    // Handed over NOT open -- what a socket built from a TLS Context is until it connects: it falls back.
    {
        qb::io::tcp::socket fresh;
        int                 calls = 0;
        std::uint16_t       peer  = 0;
        qb::io::async::tcp::connect(
            std::move(fresh), std::vector<endpoint>{loopback(refused), loopback(server.port)}, "",
            [&](qb::io::tcp::socket &&s) {
                ++calls;
                if (s.is_open())
                    peer = s.peer_endpoint().port();
            },
            5s);
        ASSERT_TRUE(qb::io::test::pump_until([&] { return calls > 0; }, 10s)) << "no completion";
        EXPECT_EQ(peer, server.port) << "a socket handed over not open did not fall back";
    }
}

TEST_F(ConnectFallback, AnAddressThatDoesNotAnswerGivesWayWithinItsShare) {
    // A listener whose accept queue is full: Linux (and the BSDs) then DROP an incoming SYN -- the address
    // neither refuses nor answers, and only the attempt's share of the deadline moves the connect on.
    PlainServer silent;
    ASSERT_NE(silent.port, 0);
    ASSERT_EQ(::listen(silent.lst.native_handle(), 0), 0);
    qb::io::tcp::socket filler;
    ASSERT_EQ(filler.connect(loopback(silent.port)), 0) << "the first connection fills the queue";

    // Is the address really silent here? A probe that connects means this system answers instead
    // (Windows completes the handshake into a minimum backlog): the share has nothing to prove.
    qb::io::tcp::socket probe;
    const int           pr = probe.connect(loopback(silent.port), 300ms);
    probe.disconnect();
    if (pr == 0)
        GTEST_SKIP() << "this system accepts past a full queue: no address here drops SYNs";

    PlainServer server;
    ASSERT_NE(server.port, 0);
    std::optional<qb::io::tcp::socket> got;
    std::atomic<bool>                  done{false};
    const auto                         t0 = std::chrono::steady_clock::now();
    qb::io::async::listener::current.coro_scheduler().spawn([&]() -> qb::io::async::task<void> {
        got  = co_await qb::io::async::tcp::connect(std::vector<endpoint>{loopback(silent.port), loopback(server.port)}, "", 6s);
        done = true;
    });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 10s)) << "the connect never completed";
    const auto took = std::chrono::steady_clock::now() - t0;
    ASSERT_TRUE(got.has_value()) << "the connect waited out the whole deadline on the silent address";
    EXPECT_EQ(got->peer_endpoint().port(), server.port);
    EXPECT_LT(took, 5s) << "the silent address did not give way within its share (3 s of 6)";
}

// ---------------------------------------------------------------------------
// Blocking: tcp::socket::connect(endpoints)
// ---------------------------------------------------------------------------

TEST(ConnectFallbackBlocking, TheBlockingConnectFallsBackPastARefusedAddress) {
    PlainServer server;
    const auto  refused = closed_port();
    ASSERT_NE(server.port, 0);
    ASSERT_NE(refused, 0);
    {
        qb::io::tcp::socket s;
        ASSERT_EQ(s.connect(std::vector<endpoint>{loopback(refused), loopback(server.port)}), 0);
        EXPECT_EQ(s.peer_endpoint().port(), server.port);
    }
    {
        qb::io::tcp::socket s;
        ASSERT_EQ(s.connect(std::vector<endpoint>{loopback(refused), loopback(server.port)}, 5s), 0) << "the timed form";
        EXPECT_EQ(s.peer_endpoint().port(), server.port);
    }
    {
        qb::io::tcp::socket s;
        ASSERT_EQ(s.init(AF_INET), 0);
        EXPECT_NE(s.connect(std::vector<endpoint>{loopback(refused), loopback(server.port)}), 0)
            << "a socket open before the call was replaced to reach the second address";
    }
    {
        qb::io::tcp::socket s;
        EXPECT_NE(s.connect(std::vector<endpoint>{}), 0);
    }
}

#ifdef QB_HAS_SSL
// ---------------------------------------------------------------------------
// TLS: the fallback is the TCP connect's; a TLS failure is final
// ---------------------------------------------------------------------------

namespace connect_fallback_test {

std::filesystem::path
fixture(const char *name) {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "resources" / "ssl" / name;
}

/// A TLS loopback server on a thread: accepts up to `n` connections and drives each handshake.
struct TlsServer {
    qb::io::tcp::ssl::listener        lst;
    std::uint16_t                     port = 0;
    std::atomic<int>                  accepted{0};
    std::atomic<bool>                 stop{false};
    qb::io::test::teardown_rendezvous rendezvous;
    std::thread                       thread;

    explicit TlsServer(int n, std::chrono::milliseconds handshake_delay = 0ms) {
        lst.init(qb::io::ssl::create_server_context(TLS_server_method(), fixture("cert.pem"), fixture("key.pem")));
        if (lst.listen_v4(0, "127.0.0.1") != 0)
            return;
        port   = lst.local_endpoint().port();
        thread = std::thread([this, n, handshake_delay] {
            for (int i = 0; i < n && !stop.load(); ++i) {
                qb::io::tcp::ssl::socket s;
                if (lst.accept(s) != 0 || stop.load())
                    continue;
                ++accepted;
                std::this_thread::sleep_for(handshake_delay); // the TCP connect is up; the TLS handshake waits
                const auto deadline = std::chrono::steady_clock::now() + 3s;
                while (!s.handshake_complete() && std::chrono::steady_clock::now() < deadline && s.handshake_status() >= 0)
                    std::this_thread::sleep_for(1ms);
                (void) rendezvous.wait_client();
            }
            rendezvous.server_done();
        });
    }
    ~TlsServer() {
        stop = true;
        rendezvous.release_all();
        if (thread.joinable() && port != 0)
            wake_accept(port);
        lst.disconnect();
        if (thread.joinable())
            thread.join();
    }
};

/// Accepts one connection and closes it at once: the TCP connect succeeds, the TLS handshake fails.
struct ClosingServer {
    qb::io::tcp::listener lst;
    std::uint16_t         port = 0;
    std::thread           thread;
    ClosingServer() {
        if (lst.listen_v4(0, "127.0.0.1") != 0)
            return;
        port   = lst.local_endpoint().port();
        thread = std::thread([this] {
            qb::io::tcp::socket s;
            if (lst.accept(s) == 0)
                s.disconnect();
        });
    }
    ~ClosingServer() {
        if (thread.joinable() && port != 0)
            wake_accept(port);
        lst.disconnect();
        if (thread.joinable())
            thread.join();
    }
};

/// Accepts one connection, reads the client's first TLS bytes, then RESETS it -- SO_LINGER 0 and no shutdown, so a
/// reset and no FIN: the TCP connect succeeded and the reset lands in the middle of the handshake, deterministically.
struct ResettingServer {
    qb::io::tcp::listener lst;
    std::uint16_t         port = 0;
    std::thread           thread;
    std::atomic<bool>     hello_read{false};
    ResettingServer() {
        if (lst.listen_v4(0, "127.0.0.1") != 0)
            return;
        port   = lst.local_endpoint().port();
        thread = std::thread([this] {
            qb::io::tcp::socket s;
            if (lst.accept(s) != 0)
                return;
            char       buf[512];
            const auto until = std::chrono::steady_clock::now() + 5s;
            while (!hello_read && std::chrono::steady_clock::now() < until) {
                if (s.read(buf, sizeof(buf)) > 0)
                    hello_read = true;
                else
                    std::this_thread::sleep_for(1ms);
            }
            const ::linger hard{1, 0};
            (void) s.set_optval(SOL_SOCKET, SO_LINGER, hard);
            s.close(QB_SD_NONE);
        });
    }
    ~ResettingServer() {
        if (thread.joinable() && port != 0)
            wake_accept(port);
        lst.disconnect();
        if (thread.joinable())
            thread.join();
    }
};

} // namespace connect_fallback_test

using connect_fallback_test::ClosingServer;
using connect_fallback_test::ResettingServer;
using connect_fallback_test::TlsServer;

TEST_F(ConnectFallback, TheTlsConnectorFallsBackPastARefusedAddress) {
    TlsServer  server(1);
    const auto refused = closed_port();
    ASSERT_NE(server.port, 0);
    ASSERT_NE(refused, 0);

    std::optional<qb::io::tcp::ssl::socket> got;
    std::atomic<bool>                       done{false};
    qb::io::async::listener::current.coro_scheduler().spawn([&]() -> qb::io::async::task<void> {
        got  = co_await qb::io::async::tcp::connect<qb::io::transport::stcp>(std::vector<endpoint>{loopback(refused), loopback(server.port)},
                                                                             "localhost", 5s, /*verify_peer*/ false);
        done = true;
    });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 10s)) << "the connect never completed";
    ASSERT_TRUE(got.has_value()) << "the TLS connector did not fall back to the listening address";
    EXPECT_TRUE(got->handshake_complete());
    server.rendezvous.client_done();
    EXPECT_TRUE(server.rendezvous.wait_server());
}

TEST_F(ConnectFallback, ATlsFailureOnAnAddressThatAnsweredIsFinal) {
    ClosingServer first;
    TlsServer     second(1);
    ASSERT_NE(first.port, 0);
    ASSERT_NE(second.port, 0);

    std::optional<qb::io::tcp::ssl::socket> got;
    std::atomic<bool>                       done{false};
    qb::io::async::listener::current.coro_scheduler().spawn([&]() -> qb::io::async::task<void> {
        got  = co_await qb::io::async::tcp::connect<qb::io::transport::stcp>(std::vector<endpoint>{loopback(first.port), loopback(second.port)},
                                                                             "localhost", 5s, /*verify_peer*/ false);
        done = true;
    });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 10s)) << "the connect never completed";
    EXPECT_FALSE(got.has_value()) << "a TLS failure was taken as a reason to try the next address";
    EXPECT_EQ(second.accepted.load(), 0) << "the second address was tried after a TLS failure";
}

// The same rule when the peer RESETS the connection during the handshake. The connector read SO_ERROR on every event,
// handshake events included, so the reset read as a failed TCP connect and the next address was tried -- what the
// case above did on a CI runner whenever the closing server's reset beat the client's read (Huly QB-164). Whether the
// reset reaches SO_ERROR or the TLS read first is a race, lost about one time in three on Linux: the case runs it
// twenty times, which a connector that reads SO_ERROR past the TCP connect fails with near certainty.
TEST_F(ConnectFallback, AResetDuringTheTlsHandshakeIsFinal) {
    for (int round = 0; round < 20; ++round) {
        SCOPED_TRACE("round " + std::to_string(round));
        ResettingServer first;
        TlsServer       second(1);
        ASSERT_NE(first.port, 0);
        ASSERT_NE(second.port, 0);

        std::optional<qb::io::tcp::ssl::socket> got;
        std::atomic<bool>                       done{false};
        qb::io::async::listener::current.coro_scheduler().spawn([&]() -> qb::io::async::task<void> {
            got = co_await qb::io::async::tcp::connect<qb::io::transport::stcp>(
                std::vector<endpoint>{loopback(first.port), loopback(second.port)}, "localhost", 5s, /*verify_peer*/ false);
            done = true;
        });
        ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 10s)) << "the connect never completed";
        EXPECT_TRUE(first.hello_read.load()) << "the first server never read the client's hello: the reset did not land mid-handshake";
        ASSERT_FALSE(got.has_value()) << "a reset during the TLS handshake was taken as a failed TCP connect";
        ASSERT_EQ(second.accepted.load(), 0) << "the second address was tried after a reset during the handshake";
    }
}

TEST_F(ConnectFallback, ATlsHandshakeLongerThanTheShareIsNotCutShort) {
    // The TCP connect is up at once; the server then holds the handshake 4 s -- longer than the attempt's
    // share (3 s of 6). Past the TCP connect nothing moves to the next address: the connect completes on
    // the FIRST one, within the deadline.
    TlsServer slow(1, 4000ms);
    TlsServer fast(1);
    ASSERT_NE(slow.port, 0);
    ASSERT_NE(fast.port, 0);

    std::optional<qb::io::tcp::ssl::socket> got;
    std::atomic<bool>                       done{false};
    qb::io::async::listener::current.coro_scheduler().spawn([&]() -> qb::io::async::task<void> {
        got  = co_await qb::io::async::tcp::connect<qb::io::transport::stcp>(std::vector<endpoint>{loopback(slow.port), loopback(fast.port)},
                                                                             "localhost", 6s, /*verify_peer*/ false);
        done = true;
    });
    ASSERT_TRUE(qb::io::test::pump_until([&] { return done.load(); }, 10s)) << "the connect never completed";
    ASSERT_TRUE(got.has_value()) << "the handshake was cut short";
    EXPECT_EQ(got->peer_endpoint().port(), slow.port) << "an attempt's share moved a connect that was past TCP";
    EXPECT_EQ(fast.accepted.load(), 0);
    slow.rendezvous.client_done();
    EXPECT_TRUE(slow.rendezvous.wait_server());
}

TEST(ConnectFallbackBlocking, TheBlockingTlsConnectFallsBackPastARefusedAddress) {
    TlsServer  server(1);
    const auto refused = closed_port();
    ASSERT_NE(server.port, 0);
    ASSERT_NE(refused, 0);
    qb::io::tcp::ssl::socket s;
    s.set_insecure();
    ASSERT_EQ(s.connect(std::vector<endpoint>{loopback(refused), loopback(server.port)}, "localhost"), 0);
    EXPECT_TRUE(s.handshake_complete());
    server.rendezvous.client_done();
    EXPECT_TRUE(server.rendezvous.wait_server());
}
#endif // QB_HAS_SSL
