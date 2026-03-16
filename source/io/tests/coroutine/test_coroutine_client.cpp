/**
 * @file test_coroutine_client.cpp
 * @brief Feasibility test for coroutine client functionality
 *
 * Tests the new coroutine client components:
 * - coro_stream: message streaming with co_await
 * - coroutine_connector: async connection with co_await
 * - coro_client: complete client interface
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: coro_stream
// =============================================================================

class CoroStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Stream deliver and receive
 * @brief Basic test of stream message delivery
 */
TEST_F(CoroStreamTest, StreamDeliverAndReceive) {
    using message_type = std::string;
    coro_stream<message_type> stream;
    stream.open();

    // Deliver a message
    std::string msg = "Hello, World!";
    EXPECT_TRUE(stream.deliver(std::move(msg)));

    // Check queue size
    EXPECT_EQ(stream.size(), 1u);

    // Receive the message (non-blocking)
    auto received = stream.try_receive();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, "Hello, World!");

    // Queue should be empty now
    EXPECT_EQ(stream.size(), 0u);
}

/**
 * @test Stream receive awaiter
 * @brief Tests co_await on stream receive
 */
TEST_F(CoroStreamTest, StreamReceiveAwaiter) {
    using message_type = int;
    coro_stream<message_type> stream;
    stream.open();

    std::atomic<bool> received{false};
    std::atomic<int> value{0};

    // Spawn coroutine that waits for message
    auto receiver_fn = [&stream, &received, &value]() -> task<void> {
        auto msg = co_await stream.receive();
        if (msg) {
            value.store(*msg);
            received.store(true);
        }
        co_return;
    };

    auto t = receiver_fn();
    coro_scheduler().spawn(std::move(t));

    // Run for a bit - coroutine should be suspended
    run_for(10ms);
    EXPECT_FALSE(received.load());

    // Deliver message
    stream.deliver(42);

    // Run again - coroutine should resume
    run_for(10ms);

    EXPECT_TRUE(received.load());
    EXPECT_EQ(value.load(), 42);
}

/**
 * @test Stream close while waiting
 * @brief Tests that receive returns nullopt when stream closed
 */
TEST_F(CoroStreamTest, StreamCloseWhileWaiting) {
    using message_type = std::string;
    coro_stream<message_type> stream;
    stream.open();

    std::atomic<bool> completed{false};
    std::atomic<bool> got_nullopt{false};

    // Spawn coroutine waiting for message
    auto waiter_fn = [&stream, &completed, &got_nullopt]() -> task<void> {
        auto msg = co_await stream.receive();
        if (!msg) {
            got_nullopt.store(true);
        }
        completed.store(true);
        co_return;
    };

    auto t = waiter_fn();
    coro_scheduler().spawn(std::move(t));

    // Run - coroutine should be suspended
    run_for(5ms);
    EXPECT_FALSE(completed.load());

    // Close the stream
    stream.close();

    // Run again - coroutine should resume with nullopt
    run_for(5ms);

    EXPECT_TRUE(completed.load());
    EXPECT_TRUE(got_nullopt.load());
}

/**
 * @test Multiple messages in stream
 * @brief Tests FIFO order of stream
 */
TEST_F(CoroStreamTest, StreamMultipleMessages) {
    using message_type = int;
    coro_stream<message_type> stream;
    stream.open();

    // Deliver multiple messages
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(stream.deliver(message_type(i)));
    }

    // Receive in order
    for (int i = 0; i < 5; ++i) {
        auto msg = stream.try_receive();
        ASSERT_TRUE(msg.has_value());
        EXPECT_EQ(*msg, i);
    }

    // Stream empty
    EXPECT_EQ(stream.size(), 0u);
}

// =============================================================================
// TEST SUITE: coroutine_connector (Mock Tests)
// =============================================================================

class CoroutineConnectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Connector creation
 * @brief Basic connector instantiation
 */
TEST_F(CoroutineConnectorTest, ConnectorCreation) {
    // Just test that connector can be created
    // Real connection test requires server
    auto connector = make_coroutine_connector<qb::io::transport::tcp>();

    EXPECT_FALSE(connector.is_connected());
}

// =============================================================================
// TEST SUITE: coro_client (Compilation Test)
// =============================================================================

class CoroClientTest : public ::testing::Test {
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
 * @brief Tests that client types can be instantiated
 */
TEST_F(CoroClientTest, ClientInstantiation) {
    // Test that coro_client can be instantiated
    // Note: These just test compilation, actual usage requires connection

    tcp_client client;
    EXPECT_FALSE(client.is_connected());
}

/**
 * @test Stream receive in coroutine
 * @brief End-to-end test of stream + coroutine
 */
TEST_F(CoroClientTest, StreamInCoroutine) {
    using message_type = std::string;
    coro_stream<message_type> stream;
    stream.open();

    std::vector<std::string> received_messages;
    std::atomic<bool> done{false};

    // Consumer coroutine
    auto consumer_fn = [&stream, &received_messages, &done]() -> task<void> {
        // Receive 3 messages
        for (int i = 0; i < 3; ++i) {
            auto msg = co_await stream.receive();
            if (msg) {
                received_messages.push_back(*msg);
            } else {
                break;  // Stream closed
            }
        }
        done.store(true);
        co_return;
    };

    auto t = consumer_fn();
    coro_scheduler().spawn(std::move(t));

    // Producer (deliver messages with delays)
    auto producer_fn = [&stream]() -> task<void> {
        co_await sleep(10ms);
        stream.deliver(std::string("Message 1"));

        co_await sleep(10ms);
        stream.deliver(std::string("Message 2"));

        co_await sleep(10ms);
        stream.deliver(std::string("Message 3"));

        co_await sleep(10ms);
        stream.close();

        co_return;
    };

    auto pt = producer_fn();
    coro_scheduler().spawn(std::move(pt));

    // Run until completion
    int max_iterations = 100;
    while (!done.load() && max_iterations-- > 0) {
        run_for(5ms);
    }

    ASSERT_TRUE(done.load());
    ASSERT_EQ(received_messages.size(), 3u);
    EXPECT_EQ(received_messages[0], "Message 1");
    EXPECT_EQ(received_messages[1], "Message 2");
    EXPECT_EQ(received_messages[2], "Message 3");
}

// =============================================================================
// TEST SUITE: Integration
// =============================================================================

class CoroClientIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }

    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Multiple streams in parallel
 * @brief Tests that multiple streams work independently
 */
TEST_F(CoroClientIntegration, MultipleStreamsParallel) {
    using message_type = int;

    coro_stream<message_type> stream1;
    coro_stream<message_type> stream2;
    stream1.open();
    stream2.open();

    std::atomic<int> sum1{0};
    std::atomic<int> sum2{0};
    std::atomic<bool> done1{false};
    std::atomic<bool> done2{false};

    // Consumer 1
    auto c1 = [&stream1, &sum1, &done1]() -> task<void> {
        for (int i = 0; i < 3; ++i) {
            auto msg = co_await stream1.receive();
            if (msg) sum1 += *msg;
        }
        done1 = true;
        co_return;
    };

    // Consumer 2
    auto c2 = [&stream2, &sum2, &done2]() -> task<void> {
        for (int i = 0; i < 3; ++i) {
            auto msg = co_await stream2.receive();
            if (msg) sum2 += *msg;
        }
        done2 = true;
        co_return;
    };

    coro_scheduler().spawn(c1());
    coro_scheduler().spawn(c2());

    // Deliver to both streams
    stream1.deliver(1);
    stream1.deliver(2);
    stream1.deliver(3);
    stream1.close();

    stream2.deliver(10);
    stream2.deliver(20);
    stream2.deliver(30);
    stream2.close();

    // Run until both complete
    for (int i = 0; i < 50 && (!done1 || !done2); ++i) {
        run_for(5ms);
    }

    EXPECT_TRUE(done1.load());
    EXPECT_TRUE(done2.load());
    EXPECT_EQ(sum1.load(), 6);   // 1+2+3
    EXPECT_EQ(sum2.load(), 60); // 10+20+30
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
