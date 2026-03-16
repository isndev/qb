/**
 * @file test_coro_client_integration.cpp
 * @brief Integration tests for coro_client with real TCP connections
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 */

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/protocol/text.h>
#include <chrono>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

using namespace qb::io;
using namespace qb::io::async;
using namespace std::chrono_literals;

/**
 * Returns (host, port) for an address that will refuse connections.
 * Binds a listener to 127.0.0.1:0, reads the assigned port, then closes and
 * destroys the listener so the port is released; a subsequent connect() will get ECONNREFUSED.
 */
static std::pair<std::string, int> get_refused_endpoint() {
    constexpr const char* host = "127.0.0.1";
    int port = 0;
    {
        qb::io::tcp::listener l;
        if (l.listen_v4(0, host) != 0) {
            return {"", 0};
        }
        port = static_cast<int>(l.local_endpoint().port());
        l.disconnect();
        l.close();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return {host, port};
}

// =============================================================================
// TEST SUITE: Coro Client Connection Tests
// =============================================================================

class CoroClientConnection : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Connection to localhost refused (no server)
 * @brief Verifies connection refused handling
 */
TEST_F(CoroClientConnection, ConnectionRefused) {
    auto [host, port] = get_refused_endpoint();
    ASSERT_GT(port, 0) << "Failed to get refused port";
    auto target = qb::io::uri("tcp://" + host + ":" + std::to_string(port));
    tcp_client client;
    std::atomic<bool> done{false};
    bool connect_result = true;  // Should become false

    auto task = [&client, &target, &connect_result, &done]() -> qb::io::async::task<void> {
        connect_result = co_await client.connect(target, 2s);
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 300) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_FALSE(connect_result);
    EXPECT_FALSE(client.is_connected());
}

/**
 * @test Connection to refused port completes and returns false
 * @brief Verifies failed connection (e.g. refused) completes quickly and returns false.
 *        Uses a refused port instead of unreachable host to avoid long OS timeouts (75s+).
 */
TEST_F(CoroClientConnection, ConnectionTimeout) {
    auto [host, port] = get_refused_endpoint();
    ASSERT_GT(port, 0) << "Failed to get refused port";
    auto target = qb::io::uri("tcp://" + host + ":" + std::to_string(port));
    tcp_client client;
    std::atomic<bool> done{false};
    bool connect_result = true;

    auto task = [&client, &target, &connect_result, &done]() -> qb::io::async::task<void> {
        connect_result = co_await client.connect(target, 2s);
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 500) {
        run_for(10ms);
        ++iterations;
    }

    ASSERT_TRUE(done.load()) << "Connect task did not complete";
    EXPECT_FALSE(connect_result);
    EXPECT_FALSE(client.is_connected());
}

/**
 * @test Invalid URI handling
 * @brief Verifies invalid URI handling
 */
TEST_F(CoroClientConnection, InvalidUri) {
    auto target = qb::io::uri("invalid://uri");
    tcp_client client;
    std::atomic<bool> done{false};
    bool connect_result = true;

    auto task = [&client, &target, &connect_result, &done]() -> qb::io::async::task<void> {
        connect_result = co_await client.connect(target, 1s);
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 100) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_FALSE(connect_result);
}

/**
 * @test Empty URI handling
 * @brief Verifies empty URI handling
 */
TEST_F(CoroClientConnection, EmptyUri) {
    auto target = qb::io::uri("");
    tcp_client client;
    std::atomic<bool> done{false};
    bool connect_result = true;

    auto task = [&client, &target, &connect_result, &done]() -> qb::io::async::task<void> {
        connect_result = co_await client.connect(target, 1s);
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 100) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_FALSE(connect_result);
}

/**
 * @test Short timeout to refused port fails
 * @brief Verifies connect with short timeout to unreachable port returns false
 * @note API treats 0s as "no timeout"; we use 1s to refused port for deterministic failure
 */
TEST_F(CoroClientConnection, ZeroTimeout) {
    auto [host, port] = get_refused_endpoint();
    ASSERT_GT(port, 0) << "Failed to get refused port";
    auto target = qb::io::uri("tcp://" + host + ":" + std::to_string(port));
    tcp_client client;
    std::atomic<bool> done{false};
    bool result = true;

    auto task = [&client, &target, &result, &done]() -> qb::io::async::task<void> {
        result = co_await client.connect(target, 1s);
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 200) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_FALSE(result);
}

// =============================================================================
// TEST SUITE: Client State Tests
// =============================================================================

class CoroClientState : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Initial state before connection
 * @brief Verifies initial state of client
 */
TEST_F(CoroClientState, InitialState) {
    tcp_client client;

    EXPECT_FALSE(client.is_connected());
    EXPECT_TRUE(client.stream().is_closed());  // Stream starts closed until connected
    EXPECT_EQ(client.stream().size(), 0u);
}

/**
 * @test Disconnect without connection is safe
 * @brief Verifies disconnect() is safe when not connected
 */
TEST_F(CoroClientState, DisconnectWithoutConnection) {
    tcp_client client;

    EXPECT_FALSE(client.is_connected());

    // Should not crash
    client.disconnect();
    client.disconnect();

    EXPECT_FALSE(client.is_connected());
}

/**
 * @test Transport access without connection
 * @brief Verifies transport() works before connection
 */
TEST_F(CoroClientState, TransportAccessWithoutConnection) {
    tcp_client client;

    auto& transport = client.transport();
    EXPECT_FALSE(transport.is_open());
}

/**
 * @test Multiple connect attempts without reset
 * @brief Verifies client handles multiple attempts
 */
TEST_F(CoroClientState, MultipleConnectAttemptsFail) {
    auto [host, port] = get_refused_endpoint();
    ASSERT_GT(port, 0) << "Failed to get refused port";
    tcp_client client;
    std::atomic<bool> done{false};
    int failure_count = 0;
    std::string uri_prefix = "tcp://" + host + ":";

    auto task = [&client, &failure_count, &done, uri_prefix, port]() -> qb::io::async::task<void> {
        for (int i = 0; i < 3; ++i) {
            if (!co_await client.connect(qb::io::uri(uri_prefix + std::to_string(port)), 1s)) {
                ++failure_count;
            }
        }
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 300) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_EQ(failure_count, 3);
}

/**
 * @test Stream delivery without connection
 * @brief Verifies deliver_message() works when stream is opened without a connection
 */
TEST_F(CoroClientState, StreamDeliveryWithoutConnection) {
    tcp_client client;
    client.stream().open();

    coro_message msg;
    msg.data = {'t', 'e', 's', 't'};
    client.deliver_message(std::move(msg));

    EXPECT_EQ(client.stream().size(), 1u);

    auto received = client.stream().try_receive();
    EXPECT_TRUE(received.has_value());
    EXPECT_EQ(received->size(), 4u);
}

// =============================================================================
// TEST SUITE: Parallel Client Tests
// =============================================================================

class CoroClientParallel : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Multiple parallel clients creation
 * @brief Verifies multiple clients can be created and run in parallel
 */
TEST_F(CoroClientParallel, ParallelClientCreation) {
    constexpr int NUM_CLIENTS = 3;
    std::atomic<int> completed_count{0};
    std::atomic<bool> done{false};

    auto worker = [&completed_count](int id) -> qb::io::async::task<void> {
        (void)id;
        tcp_client client;
        auto [host, port] = get_refused_endpoint();
        if (port > 0) {
            (void)co_await client.connect(qb::io::uri("tcp://" + host + ":" + std::to_string(port)), 1s);
        }
        client.stream().open();  // Allow delivery so we can receive below
        // Deliver a message regardless of connection result
        coro_message msg;
        msg.data = {'A'};
        client.deliver_message(std::move(msg));

        // Receive it back
        auto received = co_await client.stream().receive();
        if (received) {
            completed_count.fetch_add(1);
        }

        co_return;
    };

    // Launch multiple clients
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        coro_scheduler().spawn(worker(i));
    }

    // Monitor
    auto monitor = [&done, &completed_count]() -> qb::io::async::task<void> {
        while (completed_count.load() < NUM_CLIENTS) {
            co_await sleep(10ms);
        }
        done.store(true);
    };
    coro_scheduler().spawn(monitor());

    int iterations = 0;
    while (!done.load() && iterations < 100) {
        run_for(20ms);
        ++iterations;
    }

    EXPECT_EQ(completed_count.load(), NUM_CLIENTS);
}

/**
 * @test Multiple clients with independent streams
 * @brief Verifies each client has independent stream
 */
TEST_F(CoroClientParallel, IndependentStreams) {
    tcp_client client1;
    tcp_client client2;
    tcp_client client3;
    client1.stream().open();
    client2.stream().open();
    client3.stream().open();

    // Deliver different messages to each
    coro_message msg1{{'1'}};
    coro_message msg2{{'2'}};
    coro_message msg3{{'3'}};

    client1.deliver_message(std::move(msg1));
    client2.deliver_message(std::move(msg2));
    client3.deliver_message(std::move(msg3));

    EXPECT_EQ(client1.stream().size(), 1u);
    EXPECT_EQ(client2.stream().size(), 1u);
    EXPECT_EQ(client3.stream().size(), 1u);

    // Verify independence
    auto r1 = client1.stream().try_receive();
    auto r2 = client2.stream().try_receive();
    auto r3 = client3.stream().try_receive();

    EXPECT_EQ(r1->data[0], '1');
    EXPECT_EQ(r2->data[0], '2');
    EXPECT_EQ(r3->data[0], '3');
}

// =============================================================================
// TEST SUITE: Line protocol round-trip
// =============================================================================

class LineProtocolRoundTrip : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Line protocol: server sends "PONG\n", client receives one line message
 * @brief Verifies switch_protocol<line> + connect + receive() round-trip
 */
TEST_F(LineProtocolRoundTrip, ReceiveLineFromServer) {
    std::atomic<int> server_port{0};
    std::atomic<bool> server_done{false};

    std::thread server_thread([&server_port, &server_done]() {
        qb::io::tcp::listener listener;
        if (listener.listen_v4(0, "127.0.0.1") != 0) {
            return;
        }
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        qb::io::tcp::socket client_socket = listener.accept();
        if (client_socket.is_open()) {
            const char* pong = "PONG\n";
            (void)client_socket.write(pong, 5);
            client_socket.close();
        }
        listener.disconnect();
        listener.close();
        server_done.store(true);
    });

    int port = 0;
    for (int i = 0; i < 100 && (port = server_port.load()) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GT(port, 0) << "Server did not bind";

    tcp_client client;
    client.switch_protocol<qb::protocol::text::command>();

    std::atomic<bool> done{false};
    std::optional<coro_message> received;

    auto task = [&client, port, &received, &done]() -> qb::io::async::task<void> {
        auto target = qb::io::uri("tcp://127.0.0.1:" + std::to_string(port));
        if (!co_await client.connect(target, 5s)) {
            done.store(true);
            co_return;
        }
        received = co_await client.receive(5s);
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 600) {
        run_for(10ms);
        ++iterations;
    }

    if (server_thread.joinable()) {
        server_thread.join();
    }

    ASSERT_TRUE(done.load()) << "Client task did not complete";
    ASSERT_TRUE(received.has_value()) << "Expected one line message";
    EXPECT_EQ(received->size(), 4u) << "Line protocol delivers payload without delimiter";
    EXPECT_EQ(std::string(received->begin(), received->end()), "PONG");
}

/**
 * @test Line protocol: server sends multiple lines, client receives them one by one
 * @brief Verifies protocol parses multiple messages correctly
 */
TEST_F(LineProtocolRoundTrip, ReceiveMultipleLines) {
    std::atomic<int> server_port{0};

    std::thread server_thread([&server_port]() {
        qb::io::tcp::listener listener;
        if (listener.listen_v4(0, "127.0.0.1") != 0) return;
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        qb::io::tcp::socket client_socket = listener.accept();
        if (client_socket.is_open()) {
            const char* lines = "ALPHA\nBETA\nGAMMA\n";
            (void)client_socket.write(lines, 17);
            client_socket.close();
        }
        listener.disconnect();
        listener.close();
    });

    int port = 0;
    for (int i = 0; i < 100 && (port = server_port.load()) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GT(port, 0) << "Server did not bind";

    tcp_client client;
    client.switch_protocol<qb::protocol::text::command>();

    std::atomic<bool> done{false};
    std::vector<std::string> received_lines;

    auto task = [&client, port, &received_lines, &done]() -> qb::io::async::task<void> {
        auto target = qb::io::uri("tcp://127.0.0.1:" + std::to_string(port));
        if (!co_await client.connect(target, 5s)) {
            done.store(true);
            co_return;
        }
        for (int i = 0; i < 3; ++i) {
            auto msg = co_await client.receive(5s);
            if (!msg) break;
            received_lines.emplace_back(msg->begin(), msg->end());
        }
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 800) {
        run_for(10ms);
        ++iterations;
    }

    if (server_thread.joinable()) {
        server_thread.join();
    }

    ASSERT_TRUE(done.load()) << "Client task did not complete";
    ASSERT_EQ(received_lines.size(), 3u) << "Expected 3 line messages";
    EXPECT_EQ(received_lines[0], "ALPHA");
    EXPECT_EQ(received_lines[1], "BETA");
    EXPECT_EQ(received_lines[2], "GAMMA");
}

/**
 * @test Line protocol: client sends "PING\n", server replies "PONG\n", client receives it
 * @brief Full request/response with text protocol (send + receive)
 */
TEST_F(LineProtocolRoundTrip, PingPongRequestResponse) {
    std::atomic<int> server_port{0};

    std::thread server_thread([&server_port]() {
        qb::io::tcp::listener listener;
        if (listener.listen_v4(0, "127.0.0.1") != 0) return;
        server_port.store(static_cast<int>(listener.local_endpoint().port()));

        qb::io::tcp::socket client_socket = listener.accept();
        if (client_socket.is_open()) {
            char buf[32];
            int n = client_socket.read(buf, sizeof(buf));
            (void)n;
            const char* pong = "PONG\n";
            (void)client_socket.write(pong, 5);
            client_socket.close();
        }
        listener.disconnect();
        listener.close();
    });

    int port = 0;
    for (int i = 0; i < 100 && (port = server_port.load()) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GT(port, 0) << "Server did not bind";

    tcp_client client;
    client.switch_protocol<qb::protocol::text::command>();

    std::atomic<bool> done{false};
    std::optional<coro_message> response;
    bool send_ok = false;

    auto task = [&client, port, &response, &send_ok, &done]() -> qb::io::async::task<void> {
        auto target = qb::io::uri("tcp://127.0.0.1:" + std::to_string(port));
        if (!co_await client.connect(target, 5s)) {
            done.store(true);
            co_return;
        }
        send_ok = co_await client.send("PING\n");
        if (send_ok) {
            response = co_await client.receive(5s);
        }
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 600) {
        run_for(10ms);
        ++iterations;
    }

    if (server_thread.joinable()) {
        server_thread.join();
    }

    ASSERT_TRUE(done.load()) << "Client task did not complete";
    EXPECT_TRUE(send_ok) << "Send PING failed";
    ASSERT_TRUE(response.has_value()) << "Expected PONG response";
    EXPECT_EQ(response->size(), 4u);
    EXPECT_EQ(std::string(response->begin(), response->end()), "PONG");
}

// =============================================================================
// TEST SUITE: Move Semantics
// =============================================================================

class CoroClientMove : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Client move construction
 * @brief Verifies client can be move-constructed
 */
TEST_F(CoroClientMove, MoveConstruction) {
    tcp_client client1;
    client1.stream().open();

    // Deliver a message
    coro_message msg;
    msg.data = {'m', 'o', 'v', 'e'};
    client1.deliver_message(std::move(msg));

    // Move construct
    tcp_client client2 = std::move(client1);

    // client2 should have the message
    EXPECT_EQ(client2.stream().size(), 1u);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
