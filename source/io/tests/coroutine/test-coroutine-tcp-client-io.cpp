/**
 * @file qb/io/tests/coroutine/test-coroutine-tcp-client-io.cpp
 * @brief Coroutine TCP client I/O tests
 *
 * This file contains tests for coroutine TCP client socket operations, including
 * writing to a server, reading server replies, connecting with an existing socket,
 * reusing the scheduler across sequential connection attempts, and channel-modeled
 * client message backpressure.
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

class CoroutineClientIOOperationsTest : public ::testing::Test {
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
pump_until(Predicate done, qb::duration timeout = 2s) {
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

} // namespace

TEST_F(CoroutineClientIOOperationsTest, ConnectedSocketCanWriteToServer) {
    std::atomic<int>  server_port{0};
    std::atomic<bool> server_done{false};
    std::string       received;

    std::thread server([&]() {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        auto socket = listener.accept();
        if (socket.is_open()) {
            char      buffer[64] = {};
            const int n          = socket.read(buffer, sizeof(buffer));
            if (n > 0) {
                received.assign(buffer, static_cast<std::size_t>(n));
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
    bool              write_ok = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(server_port.load())}, 1s);
        if (socket) {
            const char payload[] = "PING";
            write_ok             = socket->write(payload, std::strlen(payload)) == static_cast<int>(std::strlen(payload));
            socket->close();
        }
        client_done.store(true);
        co_return;
    });

    pump_until([&] { return client_done.load() && server_done.load(); }, 2s);

    if (server.joinable()) {
        server.join();
    }

    ASSERT_TRUE(client_done.load());
    ASSERT_TRUE(server_done.load());
    EXPECT_TRUE(write_ok);
    EXPECT_EQ(received, "PING");
}

TEST_F(CoroutineClientIOOperationsTest, ConnectedSocketCanReadServerReply) {
    std::atomic<int> server_port{0};

    std::thread server([&]() {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        auto socket = listener.accept();
        if (socket.is_open()) {
            const char reply[] = "PONG";
            (void) socket.write(reply, std::strlen(reply));
            socket.close();
        }
        listener.disconnect();
        listener.close();
    });

    pump_until([&] { return server_port.load() > 0; }, 1s);
    ASSERT_GT(server_port.load(), 0);

    std::atomic<bool> done{false};
    std::string       reply;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(server_port.load())}, 1s);
        if (socket) {
            char      buffer[64] = {};
            const int n          = socket->read(buffer, sizeof(buffer));
            if (n > 0) {
                reply.assign(buffer, static_cast<std::size_t>(n));
            }
            socket->close();
        }
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); }, 2s);

    if (server.joinable()) {
        server.join();
    }

    ASSERT_TRUE(done.load());
    EXPECT_EQ(reply, "PONG");
}

TEST_F(CoroutineClientIOOperationsTest, ConnectWithExistingSocketSucceeds) {
    std::atomic<int>  server_port{0};
    std::atomic<bool> accepted{false};

    std::thread server([&]() {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        auto socket = listener.accept();
        accepted.store(socket.is_open());
        socket.close();
        listener.disconnect();
        listener.close();
    });

    pump_until([&] { return server_port.load() > 0; }, 1s);
    ASSERT_GT(server_port.load(), 0);

    std::atomic<bool> done{false};
    bool              connected = false;

    coro_scheduler().spawn([&]() -> task<void> {
        qb::io::tcp::socket existing_socket;
        if (existing_socket.init(AF_INET) != qb::io::SocketStatus::Done) {
            done.store(true);
            co_return;
        }

        auto socket =
            co_await qb::io::async::tcp::connect_with_socket(std::move(existing_socket), qb::io::uri{tcp_uri(server_port.load())}, 1s);
        connected = socket.has_value() && socket->is_open();
        if (socket) {
            socket->close();
        }
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); }, 2s);

    if (server.joinable()) {
        server.join();
    }

    ASSERT_TRUE(done.load());
    EXPECT_TRUE(connected);
    EXPECT_TRUE(accepted.load());
}

TEST_F(CoroutineClientIOOperationsTest, SequentialConnectionAttemptsCanReuseTheScheduler) {
    std::atomic<int> completions{0};
    std::atomic<int> successes{0};

    for (int attempt = 0; attempt < 3; ++attempt) {
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

        coro_scheduler().spawn([&, port = server_port.load()]() -> task<void> {
            auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{tcp_uri(port)}, 1s);
            if (socket) {
                ++successes;
                socket->close();
            }
            ++completions;
            co_return;
        });

        pump_until([&] { return completions.load() == attempt + 1; }, 2s);
        if (server.joinable()) {
            server.join();
        }
    }

    EXPECT_EQ(completions.load(), 3);
    EXPECT_EQ(successes.load(), 3);
}

TEST_F(CoroutineClientIOOperationsTest, ChannelModelsClientMessageQueueBackpressure) {
    channel<std::vector<char>> outbound{2};

    EXPECT_TRUE(outbound.try_send(std::vector<char>{'a'}));
    EXPECT_TRUE(outbound.try_send(std::vector<char>{'b'}));
    EXPECT_FALSE(outbound.try_send(std::vector<char>{'c'}));

    auto first = outbound.try_recv();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, (std::vector<char>{'a'}));

    EXPECT_TRUE(outbound.try_send(std::vector<char>{'c'}));
    EXPECT_EQ(outbound.size(), 2u);
}
