/**
 * @file qb/io/tests/coroutine/test-coroutine-tcp-connector.cpp
 * @brief Coroutine TCP connector tests
 *
 * This file contains tests for the asynchronous TCP connector coroutine and callback
 * APIs, including invalid URI handling, refused connection handling, callback
 * completion guarantees, local listener success paths, and parallel awaiter completion.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
#include <string>
#include <thread>

#include <qb/io/async/coroutine.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/tcp/listener.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class ConnectorIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

template <typename Predicate>
void pump_until(Predicate done, qb::duration timeout = 2s) {
    const auto deadline = qb::mono_now() + timeout;
    while (!done() && qb::mono_now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
}

std::pair<std::string, int> refused_endpoint() {
    constexpr const char *host = "127.0.0.1";
    qb::io::tcp::listener listener;
    if (listener.listen_v4(0, host) != qb::io::SocketStatus::Done) {
        return {"", 0};
    }
    const int port = static_cast<int>(listener.local_endpoint().port());
    listener.disconnect();
    listener.close();
    std::this_thread::sleep_for(50ms);
    return {host, port};
}

std::string tcp_uri(int port) {
    return "tcp://127.0.0.1:" + std::to_string(port);
}

} // namespace

TEST_F(ConnectorIntegrationTest, AwaiterReturnsEmptyOptionalForInvalidUri) {
    std::atomic<bool> done{false};
    bool connected = true;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(
            qb::io::uri{"invalid://uri"}, 100ms);
        connected = socket.has_value();
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); });

    ASSERT_TRUE(done.load());
    EXPECT_FALSE(connected);
}

TEST_F(ConnectorIntegrationTest, AwaiterReturnsEmptyOptionalForRefusedPort) {
    auto [host, port] = refused_endpoint();
    ASSERT_GT(port, 0);

    std::atomic<bool> done{false};
    bool connected = true;

    coro_scheduler().spawn([&, uri = "tcp://" + host + ":" + std::to_string(port)]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{uri}, 500ms);
        connected = socket.has_value();
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); });

    ASSERT_TRUE(done.load());
    EXPECT_FALSE(connected);
}

TEST_F(ConnectorIntegrationTest, CallbackConnectorCompletesExactlyOnceOnRefusedPort) {
    auto [host, port] = refused_endpoint();
    ASSERT_GT(port, 0);

    std::atomic<int> completions{0};
    bool connected = true;

    qb::io::async::tcp::connect<qb::io::tcp::socket>(
        qb::io::uri{"tcp://" + host + ":" + std::to_string(port)},
        [&](qb::io::tcp::socket &&socket) {
            connected = socket.is_open();
            completions.fetch_add(1);
        },
        500ms);

    pump_until([&] { return completions.load() > 0; });

    EXPECT_EQ(completions.load(), 1);
    EXPECT_FALSE(connected);
}

TEST_F(ConnectorIntegrationTest, AwaiterConnectsToLocalListener) {
    std::atomic<int> server_port{0};
    std::atomic<bool> accepted{false};

    std::thread server([&]() {
        qb::io::tcp::listener listener;
        ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        qb::io::tcp::socket socket = listener.accept();
        accepted.store(socket.is_open());
        socket.close();
        listener.disconnect();
        listener.close();
    });

    pump_until([&] { return server_port.load() > 0; }, 1s);
    ASSERT_GT(server_port.load(), 0);

    std::atomic<bool> done{false};
    bool connected = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect(
            qb::io::uri{tcp_uri(server_port.load())}, 1s);
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

TEST_F(ConnectorIntegrationTest, ParallelAwaitersAllCompleteOnRefusedPort) {
    auto [host, port] = refused_endpoint();
    ASSERT_GT(port, 0);

    constexpr int kConnectors = 5;
    std::atomic<int> completions{0};
    std::atomic<int> successes{0};
    const std::string uri = "tcp://" + host + ":" + std::to_string(port);

    for (int i = 0; i < kConnectors; ++i) {
        coro_scheduler().spawn([&, uri]() -> task<void> {
            auto socket = co_await qb::io::async::tcp::connect(qb::io::uri{uri}, 500ms);
            if (socket) {
                ++successes;
            }
            ++completions;
            co_return;
        });
    }

    pump_until([&] { return completions.load() == kConnectors; }, 3s);

    EXPECT_EQ(completions.load(), kConnectors);
    EXPECT_EQ(successes.load(), 0);
}
