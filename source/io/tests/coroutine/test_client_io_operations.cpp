/**
 * @file test_client_io_operations.cpp
 * @brief Integration tests for coro_client I/O operations
 *
 * Tests complete client lifecycle and operations.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/tcp/listener.h>
#include <chrono>
#include <atomic>
#include <string>
#include <thread>

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
// TEST SUITE: Client Lifecycle
// =============================================================================

class ClientLifecycle : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Client instantiation
 * @brief Verifies client can be created and initial state is correct
 */
TEST_F(ClientLifecycle, ClientInstantiation) {
    tcp_client client;

    EXPECT_FALSE(client.is_connected());
    EXPECT_TRUE(client.stream().is_closed());
}

/**
 * @test Connect to unreachable host fails gracefully
 * @brief Verifies connection failure is handled correctly
 */
TEST_F(ClientLifecycle, ConnectToUnreachableHost) {
    auto target = qb::io::uri("tcp://192.0.2.1:12345");  // TEST-NET-1
    tcp_client client;
    std::atomic<bool> done{false};
    bool connect_result = true;  // Should become false

    auto task = [&client, &target, &connect_result, &done]() -> qb::io::async::task<void> {
        connect_result = co_await client.connect(target, 1s);
        done.store(true);
    };

    coro_scheduler().spawn(task());
    // OS may take long (e.g. 75s+) to report unreachable host; connector has no timeout timer
    int iterations = 0;
    while (!done.load() && iterations < 12000) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_FALSE(connect_result);
    EXPECT_FALSE(client.is_connected());
}

/**
 * @test Connect to refused port
 * @brief Verifies connection refused is handled correctly
 */
TEST_F(ClientLifecycle, ConnectToRefusedPort) {
    auto [host, port] = get_refused_endpoint();
    ASSERT_GT(port, 0) << "Failed to get refused port";
    auto target = qb::io::uri("tcp://" + host + ":" + std::to_string(port));
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
    EXPECT_FALSE(client.is_connected());
}

/**
 * @test Disconnect without connection is safe
 * @brief Verifies disconnect() is safe when not connected
 */
TEST_F(ClientLifecycle, DisconnectWithoutConnection) {
    tcp_client client;
    
    EXPECT_FALSE(client.is_connected());
    
    // Should not crash
    client.disconnect();
    client.disconnect();
    
    EXPECT_FALSE(client.is_connected());
    EXPECT_TRUE(client.stream().is_closed());
}

/**
 * @test Multiple connect attempts
 * @brief Verifies client can attempt multiple connections
 */
TEST_F(ClientLifecycle, MultipleConnectAttempts) {
    auto [host, port] = get_refused_endpoint();
    ASSERT_GT(port, 0) << "Failed to get refused port";
    tcp_client client;
    std::atomic<bool> done{false};
    int failure_count = 0;
    std::string uri = "tcp://" + host + ":" + std::to_string(port);

    auto task = [&client, &failure_count, &done, uri]() -> qb::io::async::task<void> {
        for (int i = 0; i < 3; ++i) {
            if (!co_await client.connect(qb::io::uri(uri), 1s)) {
                ++failure_count;
            }
        }
        done.store(true);
    };

    coro_scheduler().spawn(task());

    int iterations = 0;
    while (!done.load() && iterations < 200) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_EQ(failure_count, 3);
}

// =============================================================================
// TEST SUITE: Client Stream Integration
// =============================================================================

class ClientStream : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Stream accessor returns valid stream
 * @brief Verifies client.stream() returns usable coro_stream
 */
TEST_F(ClientStream, StreamAccessor) {
    tcp_client client;
    client.stream().open();

    auto& stream = client.stream();
    EXPECT_EQ(stream.size(), 0u);
    EXPECT_FALSE(stream.is_closed());

    // Deliver a message manually
    coro_message msg;
    msg.data = {'H', 'e', 'l', 'l', 'o'};
    client.deliver_message(std::move(msg));
    
    EXPECT_EQ(stream.size(), 1u);
}

/**
 * @test Deliver message to stream
 * @brief Verifies deliver_message() works with client stream
 */
TEST_F(ClientStream, DeliverMessageToStream) {
    tcp_client client;
    client.stream().open();
    std::atomic<bool> done{false};
    std::optional<coro_message> received;

    // Deliver message manually (simulating protocol layer)
    coro_message msg;
    msg.data = {'H', 'e', 'l', 'l', 'o'};
    client.deliver_message(std::move(msg));

    auto task = [&client, &received, &done]() -> qb::io::async::task<void> {
        received = co_await client.stream().receive();
        done.store(true);
    };

    coro_scheduler().spawn(task());
    run_for(50ms);

    EXPECT_TRUE(done.load());
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->size(), 5u);
}

/**
 * @test Multiple messages to stream
 * @brief Verifies multiple messages can be delivered and received
 */
TEST_F(ClientStream, MultipleMessagesToStream) {
    tcp_client client;
    client.stream().open();
    std::atomic<bool> done{false};
    int received_count = 0;

    // Deliver multiple messages
    for (int i = 0; i < 5; ++i) {
        coro_message msg;
        msg.data = {static_cast<char>('0' + i)};
        client.deliver_message(std::move(msg));
    }
    client.stream().close();

    auto task = [&client, &received_count, &done]() -> qb::io::async::task<void> {
        while (true) {
            auto msg = co_await client.stream().receive();
            if (!msg) break;
            ++received_count;
        }
        done.store(true);
    };

    coro_scheduler().spawn(task());
    run_for(100ms);

    EXPECT_TRUE(done.load());
    EXPECT_EQ(received_count, 5);
}

/**
 * @test Stream closed on disconnect
 * @brief Verifies stream is closed when client disconnects
 */
TEST_F(ClientStream, StreamClosedOnDisconnect) {
    tcp_client client;
    client.stream().open();

    // Deliver a message first
    coro_message msg;
    msg.data = {'t', 'e', 's', 't'};
    client.deliver_message(std::move(msg));
    
    EXPECT_FALSE(client.stream().is_closed());
    EXPECT_EQ(client.stream().size(), 1u);
    
    client.disconnect();
    
    EXPECT_TRUE(client.stream().is_closed());
}

/**
 * @test Try receive from stream
 * @brief Verifies try_receive() works with client stream
 */
TEST_F(ClientStream, TryReceiveFromStream) {
    tcp_client client;
    client.stream().open();

    // Should return nullopt when empty
    auto result1 = client.stream().try_receive();
    EXPECT_FALSE(result1.has_value());
    
    // Deliver a message
    coro_message msg;
    msg.data = {'d', 'a', 't', 'a'};
    client.deliver_message(std::move(msg));
    
    // Should return the message
    auto result2 = client.stream().try_receive();
    EXPECT_TRUE(result2.has_value());
    EXPECT_EQ(result2->size(), 4u);
    
    // Should be empty again
    auto result3 = client.stream().try_receive();
    EXPECT_FALSE(result3.has_value());
}

// =============================================================================
// TEST SUITE: Client Transport Access
// =============================================================================

class ClientTransport : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Transport accessor exists
 * @brief Verifies transport() method exists and returns reference
 */
TEST_F(ClientTransport, TransportAccessorExists) {
    tcp_client client;
    
    // Should be able to get transport reference even before connection
    auto& transport = client.transport();
    EXPECT_FALSE(transport.is_open());
}

// =============================================================================
// TEST SUITE: Multiple Clients
// =============================================================================

class ClientMultiple : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Multiple independent clients
 * @brief Verifies multiple clients can exist independently
 */
TEST_F(ClientMultiple, MultipleIndependentClients) {
    tcp_client client1;
    tcp_client client2;
    tcp_client client3;
    client1.stream().open();
    client2.stream().open();
    client3.stream().open();

    // All should be independent
    EXPECT_FALSE(client1.is_connected());
    EXPECT_FALSE(client2.is_connected());
    EXPECT_FALSE(client3.is_connected());
    
    // Deliver to different streams
    coro_message msg1{{'1'}};
    coro_message msg2{{'2'}};
    coro_message msg3{{'3'}};
    
    client1.deliver_message(std::move(msg1));
    client2.deliver_message(std::move(msg2));
    client3.deliver_message(std::move(msg3));
    
    EXPECT_EQ(client1.stream().size(), 1u);
    EXPECT_EQ(client2.stream().size(), 1u);
    EXPECT_EQ(client3.stream().size(), 1u);
}

/**
 * @test Parallel client operations
 * @brief Verifies multiple clients can operate in parallel coroutines
 */
TEST_F(ClientMultiple, ParallelClientOperations) {
    auto [host, port] = get_refused_endpoint();
    ASSERT_GT(port, 0) << "Failed to get refused port";
    std::string connect_uri = "tcp://" + host + ":" + std::to_string(port);

    std::atomic<int> completion_count{0};
    constexpr int NUM_CLIENTS = 3;

    auto worker = [&completion_count, connect_uri](int id) -> qb::io::async::task<void> {
        tcp_client client;
        (void)co_await client.connect(qb::io::uri(connect_uri), 1s);
        client.stream().open();
        coro_message msg;
        msg.data = {static_cast<char>('A' + id)};
        client.deliver_message(std::move(msg));
        auto received = co_await client.stream().receive();
        if (received) {
            completion_count.fetch_add(1);
        }
        co_return;
    };

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        coro_scheduler().spawn(worker(i));
    }

    constexpr int max_iterations = 400;
    for (int i = 0; i < max_iterations; ++i) {
        run_for(25ms);
        if (completion_count.load() == NUM_CLIENTS) {
            break;
        }
    }

    EXPECT_EQ(completion_count.load(), NUM_CLIENTS) << "Expected " << NUM_CLIENTS
        << " workers to deliver and receive; got " << completion_count.load();
}

// =============================================================================
// TEST SUITE: Error Handling
// =============================================================================

class ClientErrors : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Invalid URI handling
 * @brief Verifies client handles invalid URIs gracefully
 */
TEST_F(ClientErrors, InvalidUriHandling) {
    std::vector<std::string> bad_uris = {
        "not-a-uri",
        "tcp://",
        "",
    };

    for (const auto& uri_str : bad_uris) {
        tcp_client client;
        std::atomic<bool> done{false};
        bool result = true;

        auto task = [&client, &uri_str, &result, &done]() -> qb::io::async::task<void> {
            result = co_await client.connect(qb::io::uri(uri_str), 1s);
            done.store(true);
        };

        coro_scheduler().spawn(task());
        
        int iterations = 0;
        while (!done.load() && iterations < 100) {
            run_for(10ms);
            ++iterations;
        }

        EXPECT_TRUE(done.load()) << "URI: " << uri_str;
        EXPECT_FALSE(result) << "URI should fail: " << uri_str;
        EXPECT_FALSE(client.is_connected()) << "URI: " << uri_str;

        // Clean up
        qb::io::async::listener::current.clear();
    }
}

/**
 * @test Operations on disconnected client
 * @brief Verifies all operations handle disconnected state safely
 */
TEST_F(ClientErrors, OperationsOnDisconnectedClient) {
    tcp_client client;
    
    // All operations should fail gracefully when not connected
    EXPECT_FALSE(client.is_connected());
    EXPECT_TRUE(client.stream().is_closed());
    
    // These should not crash
    client.disconnect();
    client.disconnect();
    
    // Transport should exist but not be open
    EXPECT_FALSE(client.transport().is_open());
}

/**
 * @test Receive when not connected returns nullopt
 * @brief Verifies client.receive() returns nullopt when not connected
 */
TEST_F(ClientErrors, ReceiveWhenNotConnected) {
    tcp_client client;
    std::atomic<bool> done{false};
    std::optional<coro_message> received = coro_message{{'x'}};  // non-nullopt to ensure we overwrite

    auto task = [&client, &received, &done]() -> qb::io::async::task<void> {
        received = co_await client.receive(1s);
        done.store(true);
    };

    coro_scheduler().spawn(task());
    run_for(50ms);

    ASSERT_TRUE(done.load());
    EXPECT_FALSE(received.has_value());
}

/**
 * @test Receive on closed stream
 * @brief Verifies receive handles closed/empty stream correctly
 */
TEST_F(ClientErrors, ReceiveOnClosedStream) {
    tcp_client client;
    std::atomic<bool> done{false};
    bool got_nullopt = false;

    // Close stream immediately
    client.stream().close();

    auto task = [&client, &got_nullopt, &done]() -> qb::io::async::task<void> {
        auto msg = co_await client.stream().receive();
        got_nullopt = !msg.has_value();
        done.store(true);
    };

    coro_scheduler().spawn(task());
    run_for(50ms);

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(got_nullopt);
}

// =============================================================================
// TEST SUITE: Timeout Variations
// =============================================================================

class ClientTimeouts : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Zero timeout behavior
 * @brief Verifies connect with short timeout to refused port fails (API treats 0s as no timeout)
 */
TEST_F(ClientTimeouts, ZeroTimeout) {
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
// TEST SUITE: Move Semantics
// =============================================================================

class ClientMove : public ::testing::Test {
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
TEST_F(ClientMove, MoveConstruction) {
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
    EXPECT_FALSE(client2.stream().is_closed());
}

// =============================================================================
// TEST SUITE: Reconnection and stream state
// =============================================================================

class ClientReconnect : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Connect then disconnect then connect again
 * @brief Verifies disconnect clears state and a second connect attempt is independent
 */
TEST_F(ClientReconnect, ConnectDisconnectReconnect) {
    auto [host, port] = get_refused_endpoint();
    ASSERT_GT(port, 0);
    std::string uri = "tcp://" + host + ":" + std::to_string(port);
    tcp_client client;
    std::atomic<bool> done{false};
    int failure_count = 0;

    auto task = [&client, &uri, &failure_count, &done]() -> qb::io::async::task<void> {
        bool ok1 = co_await client.connect(qb::io::uri(uri), 1s);
        if (!ok1) ++failure_count;
        EXPECT_FALSE(client.is_connected());
        client.disconnect();
        EXPECT_TRUE(client.stream().is_closed());

        bool ok2 = co_await client.connect(qb::io::uri(uri), 1s);
        if (!ok2) ++failure_count;
        EXPECT_FALSE(client.is_connected());
        done.store(true);
    };

    coro_scheduler().spawn(task());
    int iterations = 0;
    while (!done.load() && iterations < 250) {
        run_for(10ms);
        ++iterations;
    }

    ASSERT_TRUE(done.load());
    EXPECT_EQ(failure_count, 2);
}

/**
 * @test Stream open after close allows delivery again
 * @brief Verifies stream can be reopened after close for manual delivery use cases
 */
TEST_F(ClientReconnect, StreamReopenAfterClose) {
    tcp_client client;
    client.stream().open();
    client.deliver_message(coro_message{{'a'}});
    EXPECT_EQ(client.stream().size(), 1u);
    auto a = client.stream().try_receive();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->data[0], 'a');
    client.stream().close();
    EXPECT_TRUE(client.stream().is_closed());
    EXPECT_EQ(client.stream().try_receive(), std::nullopt);  // closed and empty

    client.stream().open();
    EXPECT_FALSE(client.stream().is_closed());
    client.deliver_message(coro_message{{'b'}});
    EXPECT_EQ(client.stream().size(), 1u);
    auto msg = client.stream().try_receive();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->size(), 1u);
    EXPECT_EQ(msg->data[0], 'b');
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
