/**
 * @file qb/io/tests/coroutine/test-coroutine-tcp-client-integration.cpp
 * @brief Coroutine TCP client integration tests
 *
 * This file contains integration tests for coroutine TCP clients, including
 * request-response flows, multi-frame server replies, failed connection recovery, and
 * multiple parallel clients connecting to a local listener.
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

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <qb/io/async/coroutine.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/listener.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class CoroClientIntegrationTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::async::init();
    }

    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

template <typename Predicate>
void
pump_until(Predicate done, qb::duration timeout = 3s) {
    const auto deadline = qb::mono_now() + timeout;
    while (!done() && qb::mono_now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
}

std::string
tcp_uri(int port) {
    return "tcp://127.0.0.1:" + std::to_string(port);
}

task<std::string>
read_until_data(qb::io::tcp::socket &socket, qb::duration timeout = 1s) {
    const auto deadline    = qb::mono_now() + timeout;
    char       buffer[256] = {};

    while (qb::mono_now() < deadline) {
        const int n = socket.read(buffer, sizeof(buffer));
        if (n > 0) {
            co_return std::string(buffer, static_cast<std::size_t>(n));
        }
        co_await sleep(1ms);
    }

    co_return std::string{};
}

} // namespace

TEST_F(CoroClientIntegrationTest, RequestResponseOverConnectedSocket) {
    std::atomic<int>  server_port{0};
    std::atomic<bool> server_done{false};

    std::thread server([&]() {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        auto socket = listener.accept();
        if (socket.is_open()) {
            char      request[64] = {};
            const int n           = socket.read(request, sizeof(request));
            if (n > 0 && std::string_view(request, static_cast<std::size_t>(n)) == "PING") {
                const char reply[] = "PONG";
                (void) socket.write(reply, std::strlen(reply));
            }
            socket.close();
        }
        listener.disconnect();
        listener.close();
        server_done.store(true);
    });

    pump_until([&] { return server_port.load() > 0; }, 1s);
    ASSERT_GT(server_port.load(), 0);

    std::atomic<bool> client_done{false};
    std::string       response;
    bool              connected = false;
    bool              write_ok  = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(server_port.load())}, 1s);
        connected   = socket.has_value();
        if (!socket) {
            client_done.store(true);
            co_return;
        }

        const char request[] = "PING";
        write_ok             = socket->write(request, std::strlen(request)) == static_cast<int>(std::strlen(request));
        if (!write_ok) {
            socket->close();
            client_done.store(true);
            co_return;
        }

        response = co_await read_until_data(*socket);
        socket->close();
        client_done.store(true);
        co_return;
    });

    pump_until([&] { return client_done.load() && server_done.load(); }, 3s);

    if (server.joinable()) {
        server.join();
    }

    ASSERT_TRUE(client_done.load());
    ASSERT_TRUE(server_done.load());
    EXPECT_TRUE(connected);
    EXPECT_TRUE(write_ok);
    EXPECT_EQ(response, "PONG");
}

TEST_F(CoroClientIntegrationTest, ServerCanSendMultipleFramesAndClientReadsThem) {
    std::atomic<int> server_port{0};

    std::thread server([&]() {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        auto socket = listener.accept();
        if (socket.is_open()) {
            const char payload[] = "ALPHA\nBETA\nGAMMA\n";
            (void) socket.write(payload, std::strlen(payload));
            socket.close();
        }
        listener.disconnect();
        listener.close();
    });

    pump_until([&] { return server_port.load() > 0; }, 1s);
    ASSERT_GT(server_port.load(), 0);

    std::atomic<bool> done{false};
    std::string       data;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(server_port.load())}, 1s);
        if (!socket) {
            done.store(true);
            co_return;
        }

        data = co_await read_until_data(*socket);
        socket->close();
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); }, 3s);

    if (server.joinable()) {
        server.join();
    }

    ASSERT_TRUE(done.load());
    EXPECT_EQ(data, "ALPHA\nBETA\nGAMMA\n");
}

TEST_F(CoroClientIntegrationTest, FailedConnectDoesNotPoisonLaterSuccessfulConnect) {
    qb::io::tcp::listener refused_listener;
    ASSERT_EQ(refused_listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const int refused_port = static_cast<int>(refused_listener.local_endpoint().port());
    refused_listener.disconnect();
    refused_listener.close();
    std::this_thread::sleep_for(50ms);

    std::atomic<int> server_port{0};
    std::thread      server([&]() {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));
        auto socket = listener.accept();
        socket.close();
        listener.disconnect();
        listener.close();
    });

    pump_until([&] { return server_port.load() > 0; }, 1s);
    ASSERT_GT(server_port.load(), 0);

    std::atomic<bool> done{false};
    bool              first_failed     = false;
    bool              second_succeeded = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto failed  = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(refused_port)}, 500ms);
        first_failed = !failed.has_value();

        auto socket      = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(server_port.load())}, 1s);
        second_succeeded = socket.has_value() && socket->is_open();
        if (socket) {
            socket->close();
        }

        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); }, 3s);

    if (server.joinable()) {
        server.join();
    }

    ASSERT_TRUE(done.load());
    EXPECT_TRUE(first_failed);
    EXPECT_TRUE(second_succeeded);
}

TEST_F(CoroClientIntegrationTest, MultipleParallelClientsConnectToOneListener) {
    constexpr int    kClients = 3;
    std::atomic<int> server_port{0};
    std::atomic<int> accepted{0};

    std::thread server([&]() {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        for (int i = 0; i < kClients; ++i) {
            auto socket = listener.accept();
            if (socket.is_open()) {
                ++accepted;
                socket.close();
            }
        }
        listener.disconnect();
        listener.close();
    });

    pump_until([&] { return server_port.load() > 0; }, 1s);
    ASSERT_GT(server_port.load(), 0);

    std::atomic<int> completed{0};
    std::atomic<int> connected{0};

    for (int i = 0; i < kClients; ++i) {
        coro_scheduler().spawn([&]() -> task<void> {
            auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(server_port.load())}, 1s);
            if (socket) {
                ++connected;
                socket->close();
            }
            ++completed;
            co_return;
        });
    }

    pump_until([&] { return completed.load() == kClients && accepted.load() == kClients; }, 3s);

    if (server.joinable()) {
        server.join();
    }

    EXPECT_EQ(completed.load(), kClients);
    EXPECT_EQ(connected.load(), kClients);
    EXPECT_EQ(accepted.load(), kClients);
}
