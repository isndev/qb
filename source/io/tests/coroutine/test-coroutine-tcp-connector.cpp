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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <unistd.h>

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

class FakeConnectorSocket {
public:
    enum class connect_result { direct, pending, fail };

    struct state {
        connect_result result = connect_result::direct;
        std::vector<int> handshake_results{1};
        std::size_t handshake_index = 0u;
        int fd = -1;
        int peer_fd = -1;
        int n_connect_calls = 0;
        int disconnect_calls = 0;
        int connected_calls = 0;
        int get_optval_calls = 0;
        int set_insecure_calls = 0;
        int so_error = 0;
        int get_optval_result = 0;
    };

private:
    std::shared_ptr<state> _state;

    void ensure_fd() {
        if (_state->fd >= 0)
            return;
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
            _state->fd = fds[0];
            _state->peer_fd = fds[1];
        }
    }

public:
    FakeConnectorSocket()
        : _state(std::make_shared<state>()) {}

    explicit FakeConnectorSocket(std::shared_ptr<state> shared_state)
        : _state(std::move(shared_state)) {}

    [[nodiscard]] std::shared_ptr<state> shared_state() const noexcept {
        return _state;
    }

    [[nodiscard]] bool is_open() const noexcept {
        return _state && _state->fd >= 0;
    }

    [[nodiscard]] int native_handle() const noexcept {
        return _state ? _state->fd : -1;
    }

    int n_connect(qb::io::uri const&) {
        ++_state->n_connect_calls;
        if (_state->result == connect_result::fail) {
            qb::io::socket::set_last_errno(ECONNREFUSED);
            return -1;
        }

        ensure_fd();
        if (_state->result == connect_result::pending) {
            qb::io::socket::set_last_errno(EINPROGRESS);
            return -1;
        }
        return 0;
    }

    int handshake_status() {
        auto index = std::min(_state->handshake_index,
                              _state->handshake_results.size() - 1u);
        const auto result = _state->handshake_results[index];
        if (_state->handshake_index + 1u < _state->handshake_results.size())
            ++_state->handshake_index;
        return result;
    }

    int connected() {
        ++_state->connected_calls;
        return 0;
    }

    template <typename T>
    int get_optval(int, int, T& out) {
        ++_state->get_optval_calls;
        out = static_cast<T>(_state->so_error);
        return _state->get_optval_result;
    }

    void set_insecure() {
        ++_state->set_insecure_calls;
    }

    void disconnect() {
        ++_state->disconnect_calls;
        if (_state->fd >= 0) {
            ::close(_state->fd);
            _state->fd = -1;
        }
        if (_state->peer_fd >= 0) {
            ::close(_state->peer_fd);
            _state->peer_fd = -1;
        }
    }
};

struct FakeConnectorTransport {
    using transport_io_type = FakeConnectorSocket;
};

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

TEST_F(ConnectorIntegrationTest, CallbackConnectorCoversDirectHandshakeSuccessAndFailure) {
    {
        auto shared = std::make_shared<FakeConnectorSocket::state>();
        shared->result = FakeConnectorSocket::connect_result::direct;
        shared->handshake_results = {1};
        std::atomic<int> completions{0};
        bool connected = false;

        qb::io::async::tcp::connect<FakeConnectorSocket>(
            FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:1"},
            [&](FakeConnectorSocket&& socket) {
                connected = socket.is_open();
                ++completions;
            },
            0ms);

        EXPECT_EQ(completions.load(), 1);
        EXPECT_TRUE(connected);
        EXPECT_EQ(shared->n_connect_calls, 1);
    }

    {
        auto shared = std::make_shared<FakeConnectorSocket::state>();
        shared->result = FakeConnectorSocket::connect_result::direct;
        shared->handshake_results = {-1};
        std::atomic<int> completions{0};
        bool connected = true;

        qb::io::async::tcp::connect<FakeConnectorSocket>(
            FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:2"},
            [&](FakeConnectorSocket&& socket) {
                connected = socket.is_open();
                ++completions;
            },
            0ms, false);

        EXPECT_EQ(completions.load(), 1);
        EXPECT_FALSE(connected);
        EXPECT_EQ(shared->n_connect_calls, 1);
        EXPECT_EQ(shared->set_insecure_calls, 1);
        EXPECT_EQ(shared->disconnect_calls, 1);
    }
}

TEST_F(ConnectorIntegrationTest, CallbackConnectorDeadlineCompletesPendingHandshakeOnce) {
    auto shared = std::make_shared<FakeConnectorSocket::state>();
    shared->result = FakeConnectorSocket::connect_result::pending;
    shared->handshake_results = {0};

    std::atomic<int> completions{0};
    bool connected = true;

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:3"},
        [&](FakeConnectorSocket&& socket) {
            connected = socket.is_open();
            ++completions;
        },
        1ms);

    pump_until([&] { return completions.load() > 0; }, 200ms);

    EXPECT_EQ(completions.load(), 1);
    EXPECT_FALSE(connected);
    EXPECT_EQ(shared->n_connect_calls, 1);
    EXPECT_EQ(shared->disconnect_calls, 1);
}

TEST_F(ConnectorIntegrationTest, CallbackConnectorCompletesPendingHandshakeFromIoEvent) {
    auto shared = std::make_shared<FakeConnectorSocket::state>();
    shared->result = FakeConnectorSocket::connect_result::pending;
    shared->handshake_results = {0, 1};

    std::atomic<int> completions{0};
    bool connected = false;

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:7"},
        [&](FakeConnectorSocket&& socket) {
            connected = socket.is_open();
            ++completions;
        },
        500ms);

    pump_until([&] { return completions.load() > 0; }, 500ms);

    EXPECT_EQ(completions.load(), 1);
    EXPECT_TRUE(connected);
    EXPECT_GE(shared->get_optval_calls, 2);
    EXPECT_EQ(shared->disconnect_calls, 0);
}

TEST_F(ConnectorIntegrationTest, CallbackConnectorFailsPendingHandshakeFromIoEvent) {
    auto shared = std::make_shared<FakeConnectorSocket::state>();
    shared->result = FakeConnectorSocket::connect_result::pending;
    shared->handshake_results = {-1};

    std::atomic<int> completions{0};
    bool connected = true;

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:8"},
        [&](FakeConnectorSocket&& socket) {
            connected = socket.is_open();
            ++completions;
        },
        500ms);

    pump_until([&] { return completions.load() > 0; }, 500ms);

    EXPECT_EQ(completions.load(), 1);
    EXPECT_FALSE(connected);
    EXPECT_EQ(shared->get_optval_calls, 1);
    EXPECT_EQ(shared->disconnect_calls, 1);
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

TEST_F(ConnectorIntegrationTest, AwaiterWithExistingSocketCompletesAndMovesResult) {
    auto shared = std::make_shared<FakeConnectorSocket::state>();
    shared->result = FakeConnectorSocket::connect_result::direct;
    shared->handshake_results = {1};

    std::atomic<bool> done{false};
    bool connected = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto socket = co_await qb::io::async::tcp::connect_with_socket<FakeConnectorTransport>(
            FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:4"}, 0ms);
        connected = socket.has_value() && socket->is_open();
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); }, 200ms);

    ASSERT_TRUE(done.load());
    EXPECT_TRUE(connected);
    EXPECT_EQ(shared->n_connect_calls, 1);
}

TEST_F(ConnectorIntegrationTest, DestroyedAwaiterIgnoresLateConnectorCallback) {
    auto shared = std::make_shared<FakeConnectorSocket::state>();
    shared->result = FakeConnectorSocket::connect_result::pending;
    shared->handshake_results = {0};

    {
        auto awaiter = qb::io::async::tcp::connect_awaiter<FakeConnectorSocket>{
            qb::io::uri{"tcp://fake.local:5"}, 1ms};
        auto ready_before_destroy = awaiter.await_ready();
        EXPECT_FALSE(ready_before_destroy);
    }

    qb::io::async::tcp::connect<FakeConnectorSocket>(
        FakeConnectorSocket{shared}, qb::io::uri{"tcp://fake.local:6"},
        [](FakeConnectorSocket&&) {},
        1ms);

    pump_until([&] { return shared->disconnect_calls > 0; }, 200ms);

    EXPECT_EQ(shared->disconnect_calls, 1);
}
