/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/ssl/listener.h>

#include "../../shared/loopback_fixture.h"
#include "../../shared/ssl_fixtures.h"
#include "../../shared/tls_deadline.h"

using namespace std::chrono_literals;

namespace tls_worker_deadline_test {

class TlsWorkerDeadline : public ::testing::TestWithParam<bool> {
protected:
    bool
    connect(qb::io::tcp::ssl::socket &socket, std::uint16_t port, std::chrono::steady_clock::time_point deadline) {
        socket.set_insecure();
        if (GetParam()) {
            const std::atomic<bool> stop{false};
            return qb::io::test::connect_tls_worker_until(socket, port, stop, deadline);
        }
        return qb::io::test::connect_tls_blocking_until(socket, port, deadline) == 0 && socket.handshake_complete();
    }

    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

class ReleaseAndJoin {
    qb::io::test::teardown_rendezvous &_hold;
    std::thread                       &_thread;

public:
    ReleaseAndJoin(qb::io::test::teardown_rendezvous &hold, std::thread &thread)
        : _hold(hold)
        , _thread(thread) {}
    ~ReleaseAndJoin() {
        _hold.release_all();
        if (_thread.joinable())
            _thread.join();
    }
    ReleaseAndJoin(const ReleaseAndJoin &)            = delete;
    ReleaseAndJoin &operator=(const ReleaseAndJoin &) = delete;
};

TEST_P(TlsWorkerDeadline, LateStartDoesNotOpenConnection) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    qb::io::tcp::ssl::socket client;
    EXPECT_FALSE(connect(client, listener.local_endpoint().port(), std::chrono::steady_clock::now() - 1s));
    EXPECT_FALSE(client.is_open());
    EXPECT_FALSE(qb::io::test::wait_acceptable_within(listener.native_handle(), 0ms));
}

TEST_P(TlsWorkerDeadline, HandshakeStallReturnsBeforeSilentPeerCloses) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto                        port = listener.local_endpoint().port();
    qb::io::test::teardown_rendezvous hold;
    qb::io::tcp::ssl::socket          client;
    std::atomic<bool>                 accepted{false};
    std::atomic<bool>                 peer_finished{false};
    std::thread                       peer([&] {
        qb::io::tcp::socket socket;
        if (qb::io::test::accept_within(listener, socket)) {
            accepted.store(true);
            hold.server_done();
            // The peer deliberately never enters TLS. This outer bound lets a
            // broken client fail its elapsed-time oracle instead of wedging CTest.
            (void) hold.wait_client(6s);
        }
        peer_finished.store(true);
    });
    const ReleaseAndJoin              cleanup{hold, peer};
    const auto                        started = std::chrono::steady_clock::now();
    EXPECT_FALSE(connect(client, port, started + 500ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(hold.wait_server(3s)) << "silent peer never accepted the client";
    EXPECT_TRUE(accepted.load());
    EXPECT_FALSE(peer_finished.load()) << "the fixture deadline must release the client while its silent peer remains open";
    EXPECT_LT(elapsed, 3s);
    EXPECT_FALSE(client.handshake_complete());
}

TEST_P(TlsWorkerDeadline, RefusedConnectReturnsWithoutHandshake) {
    // A bound, non-listening socket keeps the port reserved throughout the test.
    qb::io::tcp::socket reservation;
    ASSERT_EQ(reservation.bind(qb::io::uri{"tcp://127.0.0.1:0"}), 0);
    qb::io::tcp::ssl::socket client;
    const auto               started = std::chrono::steady_clock::now();
    EXPECT_FALSE(connect(client, reservation.local_endpoint().port(), started + 500ms));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
    EXPECT_FALSE(client.handshake_complete());
}

TEST_P(TlsWorkerDeadline, HealthyHandshakeCompletes) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    qb::io::tcp::ssl::listener listener{qb::io::ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"),
                                                                     qb::io::test::ssl_resource_path("key.pem"))};
    ASSERT_TRUE(listener.context().ok());
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto                        port = listener.local_endpoint().port();
    qb::io::test::teardown_rendezvous hold;
    qb::io::tcp::ssl::socket          client;
    std::atomic<bool>                 handshook{false};
    std::thread                       peer([&] {
        qb::io::tcp::ssl::socket socket;
        if (!qb::io::test::accept_within(listener, socket))
            return;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!socket.handshake_complete() && std::chrono::steady_clock::now() < deadline) {
            if (socket.handshake_status() < 0)
                return;
            std::this_thread::sleep_for(1ms);
        }
        handshook.store(socket.handshake_complete());
        hold.server_done();
        (void) hold.wait_client();
    });
    const ReleaseAndJoin              cleanup{hold, peer};
    EXPECT_TRUE(connect(client, port, std::chrono::steady_clock::now() + 5s));
    EXPECT_TRUE(client.handshake_complete());
    EXPECT_TRUE(hold.wait_server());
    EXPECT_TRUE(handshook.load());
}

TEST(TlsWorkerStop, AlreadyStoppedWorkerDoesNotConnect) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const std::atomic<bool>  stop{true};
    qb::io::tcp::ssl::socket client;
    EXPECT_FALSE(qb::io::test::connect_tls_worker_until(client, listener.local_endpoint().port(), stop, std::chrono::steady_clock::now() + 5s));
    EXPECT_FALSE(client.is_open());
    EXPECT_FALSE(qb::io::test::wait_acceptable_within(listener.native_handle(), 0ms));
}

TEST(TlsWorkerStop, StopDuringHandshakeReturnsBeforePeerCloses) {
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), 0);
    const auto                        port = listener.local_endpoint().port();
    std::atomic<bool>                 stop{false};
    std::atomic<bool>                 done{false};
    bool                              connected = true;
    qb::io::tcp::socket               peer;
    std::thread                       worker([&] {
        qb::io::tcp::ssl::socket client;
        connected = qb::io::test::connect_tls_worker_until(client, port, stop, std::chrono::steady_clock::now() + 5s);
        done.store(true);
    });
    const qb::io::test::thread_joiner cleanup{worker};
    ASSERT_TRUE(qb::io::test::accept_within(listener, peer));
    // TCP admission is the barrier: the worker has entered connect, and this peer
    // deliberately provides no TLS progress. Stop must cancel before its 5s timeout.
    stop.store(true);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(done.load()) << "stop must release a worker while its silent peer remains open";
    peer.disconnect(); // release a defective worker before joining/reporting the failure
    worker.join();
    EXPECT_FALSE(connected);
}

INSTANTIATE_TEST_SUITE_P(BlockingAndAsync, TlsWorkerDeadline, ::testing::Bool());

} // namespace tls_worker_deadline_test
