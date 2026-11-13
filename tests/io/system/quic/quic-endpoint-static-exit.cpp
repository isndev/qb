/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/quic/quic-endpoint-static-exit.cpp
 * @brief A QUIC endpoint closes without arming its timer -- so an endpoint with STATIC STORAGE, closed by its
 *        destructor after its thread's event loop, ends cleanly when the process exits (Huly QB-1246).
 *
 * `endpoint::close()` asks the backend to close, sends what that leaves to send, reports the connections it ends,
 * then stops its two watchers and closes its socket. A closed backend still has a deadline -- its closing period --
 * and the drain of its last packets armed the endpoint's timer for it, for the next line to stop: a start and a stop
 * of the same watcher, in the same call. In a running program nothing shows. But `exit()` destroys the main thread's
 * thread-locals -- `listener::current` among them -- before the objects with static storage: an endpoint a program
 * keeps there (an HTTP/3 client, a server) is closed by its destructor AFTER its thread's loop, and the timer it
 * started there was a write into the freed loop -- an abort (`free(): corrupted unsorted chunks`) or a segmentation
 * fault at exit in a Release build, a heap-use-after-free under AddressSanitizer. Stopping a watcher there is safe;
 * `close()` now holds its timer aside while the backend closes: its drains arm nothing.
 *
 * The cases after the loop each run in a CHILD process (a death test, re-executed): an `atexit` handler registered
 * before the endpoint is built is their judge (`static_exit_support.h`). The two in-process cases pin what the timer
 * held aside must not break: an endpoint that closes connects again and completes a handshake -- its timer is its
 * own again -- and a handler that closes and connects again from inside the close, whose `connect()` finds no timer
 * and registers one, leaves the endpoint the two registrations it had and none behind it once destroyed.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/quic.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/quic_test_doubles.h"
#include "../../shared/ssl_fixtures.h"
#include "../../shared/static_exit_support.h"

using namespace std::chrono_literals;

namespace quic_endpoint_static_exit_test {

using namespace qb::io::test::static_exit;
using qb::io::test::CallbackQuicServer;
using qb::io::test::ssl_resource_path;

using State = qb::io::async::quic::endpoint::state;

/// The server of a case in a child: on the heap, reachable to the end, never destroyed.
CallbackQuicServer *g_server = nullptr;

[[nodiscard]] qb::io::quic::tls_config
client_tls() {
    qb::io::quic::tls_config tls;
    tls.server_name = "localhost";
    tls.verify_peer = false;
    return tls;
}

[[nodiscard]] std::string
listen_on(CallbackQuicServer &server) {
    if (!server.listen(qb::io::uri{"quic://127.0.0.1:0"}, ssl_resource_path("cert.pem"), ssl_resource_path("key.pem"), {"h3"}))
        return {};
    return "quic://127.0.0.1:" + std::to_string(server.local_endpoint().port());
}

enum class state {
    never_connected,
    connecting, ///< connect() asked, no pass of the loop since
    connected,  ///< the handshake done, nothing sent
    closed,     ///< close() by the program, then `exit()`
};

[[noreturn]] void
run_endpoint(state const st, site const where) {
    std::atexit(judge);                                         // first: it runs after the destructor of `object`
    static_cast<void>(qb::io::async::listener::current.size()); // the thread's listener exists: the endpoint goes after it
    if (!qb::io::test::require_ssl_files())
        setup_failed("the TLS test certificate is missing");
    g_server              = new CallbackQuicServer;
    const std::string uri = listen_on(*g_server);
    if (uri.empty())
        setup_failed("the QUIC server did not start");
    static at_exit<qb::io::async::quic::endpoint> object;
    qb::io::async::quic::endpoint                &client = object.client;
    if (st != state::never_connected) {
        if (!client.connect(qb::io::uri{uri}, client_tls(), {"h3"}))
            setup_failed("connect() started nothing");
        if (st != state::connecting && !pump_until(nullptr, [&] {
                return g_server->current_state() == State::connected && client.current_state() == State::connected;
            }))
            setup_failed("the QUIC handshake did not complete");
        if (st == state::closed)
            client.close(0, "done");
    }
    leave(where);
}

class QuicEndpointStaticExitDeathTest : public in_a_child_from {};

TEST_P(QuicEndpointStaticExitDeathTest, NeverConnected) {
    EXPECT_EXIT(run_endpoint(state::never_connected, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(-1, 0, 0, 0, 0, 0, 0)));
}

// The endpoint's destructor closes a live connection after the loop: the backend's last packets leave, and no timer
// is started for its closing period.
TEST_P(QuicEndpointStaticExitDeathTest, Connected) {
    EXPECT_EXIT(run_endpoint(state::connected, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(-1, 0, 0, 0, 0, 0, 0)));
}

TEST_P(QuicEndpointStaticExitDeathTest, ClosedByTheProgram) {
    EXPECT_EXIT(run_endpoint(state::closed, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(-1, 0, 0, 0, 0, 0, 0)));
}

INSTANTIATE_TEST_SUITE_P(Exit, QuicEndpointStaticExitDeathTest, ::testing::Values(site::main, site::callback), site_name);

class QuicEndpointStaticExitOnceDeathTest : public in_a_child {};

TEST_F(QuicEndpointStaticExitOnceDeathTest, AHandshakeInFlight) {
    EXPECT_EXIT(run_endpoint(state::connecting, site::main), ::testing::ExitedWithCode(0), stderr_is(verdict(-1, 0, 0, 0, 0, 0, 0)));
}

// ---- What the timer held aside must not break ----------------------------------------------------------------------

/// A client whose handler, told its connection ended, closes and connects again -- once, and from inside the close
/// that reports it. Its own close() releases the socket, so the connect() is accepted (on a socket still bound it is
/// refused before anything is registered).
class ReconnectsFromItsClose : public qb::io::async::quic::endpoint {
public:
    std::string again;
    int         asked    = 0; ///< connect() calls made from inside a close
    int         accepted = 0; ///< ... that the endpoint accepted

protected:
    void
    dispatch(qb::io::async::quic::event::connection_closed const &) override {
        if (!again.empty() && asked == 0) {
            ++asked;
            this->close(0, "from the handler");
            if (this->connect(qb::io::uri{again}, client_tls(), {"h3"}))
                ++accepted;
        }
    }
};

} // namespace quic_endpoint_static_exit_test

using namespace quic_endpoint_static_exit_test;

// close() puts the timer back: the endpoint connects again and its handshake -- which the timer drives when a packet
// is lost, and whose idle teardown is the timer's alone -- completes on the same registration.
TEST(QuicEndpointClose, AnEndpointThatClosedConnectsAgainOnItsOwnTimer) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    CallbackQuicServer            server;
    qb::io::async::quic::endpoint client;
    const std::string             uri = listen_on(server);
    ASSERT_FALSE(uri.empty());
    const std::size_t registered_idle = qb::io::async::listener::current.size();
    for (int round = 0; round < 3; ++round) {
        ASSERT_TRUE(client.connect(qb::io::uri{uri}, client_tls(), {"h3"}));
        ASSERT_TRUE(pump_until(nullptr, [&] { return client.current_state() == State::connected && server.connected == round + 1; }))
            << "round " << round << ": the handshake did not complete";
        client.close(0, "again");
        EXPECT_EQ(client.current_state(), State::closed);
        ASSERT_TRUE(pump_until(nullptr, [&] { return server.closed == round + 1; })) << "round " << round << ": the server never saw the close";
    }
    // One io watcher and one timer for the client, registered at its first connect and kept: a close that lost its
    // timer would register another at every connect.
    EXPECT_EQ(qb::io::async::listener::current.size(), registered_idle + 2u);
}

// The close holds the endpoint's timer aside and gives it back as it returns, whatever its handlers did. A handler
// that closes and connects again from inside it finds no timer: its connect() registers one, and arms it. The close
// releases that one as it resumes -- kept, it would stay registered on the loop with the endpoint as its handler,
// armed, after the endpoint is destroyed -- and ends what the handler started, as it did before it held anything.
TEST(QuicEndpointClose, AHandlerThatConnectsAgainFromInsideTheCloseLeavesTheTwoRegistrations) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    CallbackQuicServer server;
    const std::string  uri = listen_on(server);
    ASSERT_FALSE(uri.empty());
    const std::size_t registered_before = qb::io::async::listener::current.size();
    {
        ReconnectsFromItsClose client;
        ASSERT_TRUE(client.connect(qb::io::uri{uri}, client_tls(), {"h3"}));
        ASSERT_TRUE(pump_until(nullptr, [&] { return client.current_state() == State::connected && server.connected == 1; }));
        EXPECT_EQ(qb::io::async::listener::current.size(), registered_before + 2u);
        client.again = uri;
        client.close(0, "and again");
        ASSERT_EQ(client.asked, 1) << "the close reported no connection ended: the case is no witness";
        ASSERT_EQ(client.accepted, 1) << "the connect() made from inside the close registered nothing: the case is no witness";
        EXPECT_EQ(qb::io::async::listener::current.size(), registered_before + 2u) << "the close kept a second timer registered";
        EXPECT_EQ(client.current_state(), State::closed);
    }
    EXPECT_EQ(qb::io::async::listener::current.size(), registered_before) << "a destroyed endpoint left a registration behind";
}
