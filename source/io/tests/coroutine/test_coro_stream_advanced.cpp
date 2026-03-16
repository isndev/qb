/**
 * @file test_coro_stream_advanced.cpp
 * @brief Advanced tests for coro_stream
 *
 * Tests supplémentaires pour valider le comportement de coro_stream
 * dans des scénarios complexes et edge cases.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * @license Apache License, Version 2.0
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>
#include <thread>
#include <random>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Message Ordering & Sequencing
// =============================================================================

class StreamOrdering : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Messages received in FIFO order
 * @brief Verifies that messages are delivered in the same order they were sent
 */
TEST_F(StreamOrdering, FifoOrderingGuaranteed) {
    coro_stream<int> stream;
    stream.open();
    std::vector<int> received;
    std::atomic<bool> done{false};

    // Deliver 100 messages in sequence
    for (int i = 0; i < 100; ++i) {
        stream.deliver(int{i});
    }

    // Consumer coroutine
    auto consumer = [&stream, &received, &done]() -> task<void> {
        for (int i = 0; i < 100; ++i) {
            auto msg = co_await stream.receive();
            if (msg) {
                received.push_back(*msg);
            }
        }
        done.store(true);
        co_return;
    };

    coro_scheduler().spawn(consumer());
    run_for(100ms);

    ASSERT_TRUE(done.load());
    ASSERT_EQ(received.size(), 100u);

    // Verify FIFO ordering
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(received[i], i) << "Message at index " << i << " out of order";
    }
}

/**
 * @test Interleaved deliver and receive maintains order
 * @brief Producer and consumer operate concurrently, order must be preserved
 */
TEST_F(StreamOrdering, InterleavedOperationsMaintainOrder) {
    coro_stream<int> stream;
    stream.open();
    std::vector<int> received;
    std::atomic<bool> done{false};
    std::atomic<int> next_to_deliver{0};

    // Producer coroutine
    auto producer = [&stream, &next_to_deliver]() -> task<void> {
        for (int i = 0; i < 50; ++i) {
            stream.deliver(int{i});
            next_to_deliver.store(i + 1);
            co_await sleep(1ms);  // Small delay between deliveries
        }
        stream.close();
        co_return;
    };

    // Consumer coroutine
    auto consumer = [&stream, &received, &done]() -> task<void> {
        while (true) {
            auto msg = co_await stream.receive();
            if (!msg) break;
            received.push_back(*msg);
        }
        done.store(true);
        co_return;
    };

    coro_scheduler().spawn(producer());
    coro_scheduler().spawn(consumer());

    int iterations = 0;
    while (!done.load() && iterations < 200) {
        run_for(5ms);
        ++iterations;
    }

    ASSERT_TRUE(done.load());
    ASSERT_EQ(received.size(), 50u);

    // Verify order is maintained
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(received[i], i);
    }
}

// =============================================================================
// TEST SUITE: Stress & Performance
// =============================================================================

class StreamStress : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test High volume message throughput
 * @brief Tests handling of 10000 messages
 */
TEST_F(StreamStress, HighVolumeThroughput) {
    coro_stream<int> stream;
    stream.open();
    std::atomic<int> count{0};
    std::atomic<bool> done{false};
    constexpr int NUM_MESSAGES = 10000;

    // Pre-deliver all messages
    for (int i = 0; i < NUM_MESSAGES; ++i) {
        stream.deliver(int{i});
    }
    stream.close();

    auto consumer = [&stream, &count, &done]() -> task<void> {
        while (true) {
            auto msg = co_await stream.receive();
            if (!msg) break;
            count.fetch_add(1);
        }
        done.store(true);
        co_return;
    };

    coro_scheduler().spawn(consumer());
    run_for(500ms);

    EXPECT_TRUE(done.load());
    EXPECT_EQ(count.load(), NUM_MESSAGES);
}

/**
 * @test Rapid open/close cycles
 * @brief Tests stream lifecycle stability
 */
TEST_F(StreamStress, RapidLifecycleCycles) {
    std::atomic<int> total_received{0};

    for (int cycle = 0; cycle < 10; ++cycle) {
        coro_stream<int> stream;
        stream.open();
        std::atomic<int> received{0};
        std::atomic<bool> done{false};

        // Deliver messages
        for (int i = 0; i < 10; ++i) {
            stream.deliver(int{i});
        }
        stream.close();

        auto consumer = [&stream, &received, &done]() -> task<void> {
            while (true) {
                auto msg = co_await stream.receive();
                if (!msg) break;
                received.fetch_add(1);
            }
            done.store(true);
            co_return;
        };

        coro_scheduler().spawn(consumer());
        
        int iterations = 0;
        while (!done.load() && iterations < 50) {
            run_for(5ms);
            ++iterations;
        }

        ASSERT_TRUE(done.load()) << "Cycle " << cycle << " did not complete";
        ASSERT_EQ(received.load(), 10) << "Cycle " << cycle << " lost messages";
        
        total_received.fetch_add(received.load());
    }

    EXPECT_EQ(total_received.load(), 100);
}

/**
 * @test Large message handling
 * @brief Tests that large strings are handled correctly
 */
TEST_F(StreamStress, LargeMessageHandling) {
    coro_stream<std::string> stream;
    stream.open();
    std::string large_message(100000, 'X');  // 100KB string
    const std::size_t expected_size = large_message.size();
    std::atomic<bool> done{false};
    std::optional<std::string> received;

    stream.deliver(std::move(large_message));
    stream.close();

    auto consumer = [&stream, &received, &done]() -> task<void> {
        auto msg = co_await stream.receive();
        received = std::move(msg);
        done.store(true);
        co_return;
    };

    coro_scheduler().spawn(consumer());
    run_for(50ms);

    ASSERT_TRUE(done.load());
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->size(), expected_size);
    // Verify content is all 'X'
    EXPECT_EQ(received->find_first_not_of('X'), std::string::npos);
}

// =============================================================================
// TEST SUITE: Edge Cases & Error Handling
// =============================================================================

class StreamEdgeCases : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Double close is safe
 * @brief Calling close() twice should not cause issues
 */
TEST_F(StreamEdgeCases, DoubleCloseIsSafe) {
    coro_stream<int> stream;
    
    stream.close();
    stream.close();  // Should not crash or cause issues
    
    EXPECT_TRUE(stream.is_closed());
}

/**
 * @test Deliver after close is rejected
 * @brief Messages delivered after close should be rejected
 */
TEST_F(StreamEdgeCases, DeliverAfterCloseRejected) {
    coro_stream<int> stream;
    
    stream.close();
    
    bool delivered = stream.deliver(42);
    EXPECT_FALSE(delivered);
    EXPECT_EQ(stream.size(), 0u);
}

/**
 * @test Empty stream immediate close
 * @brief Consumer should immediately get nullopt on empty closed stream
 */
TEST_F(StreamEdgeCases, EmptyStreamImmediateClose) {
    coro_stream<int> stream;
    std::atomic<bool> done{false};
    bool got_nullopt = false;

    stream.close();  // Close before any messages

    auto consumer = [&stream, &done, &got_nullopt]() -> task<void> {
        auto msg = co_await stream.receive();
        got_nullopt = !msg.has_value();
        done.store(true);
        co_return;
    };

    coro_scheduler().spawn(consumer());
    run_for(10ms);

    EXPECT_TRUE(done.load());
    EXPECT_TRUE(got_nullopt);
}

/**
 * @test Move semantics work correctly
 * @brief Stream can be moved without losing messages
 */
TEST_F(StreamEdgeCases, MoveSemanticsPreserveMessages) {
    coro_stream<int> stream1;
    stream1.open();
    
    // Add some messages
    for (int i = 0; i < 5; ++i) {
        stream1.deliver(int{i});
    }
    
    // Move construct
    coro_stream<int> stream2 = std::move(stream1);
    
    // Messages should be in stream2
    EXPECT_EQ(stream2.size(), 5u);
    
    // Consume from stream2
    std::atomic<int> count{0};
    std::atomic<bool> done{false};
    
    auto consumer = [&stream2, &count, &done]() -> task<void> {
        while (true) {
            auto msg = co_await stream2.receive();
            if (!msg) break;
            count.fetch_add(1);
        }
        done.store(true);
        co_return;
    };
    
    stream2.close();
    coro_scheduler().spawn(consumer());
    run_for(50ms);
    
    EXPECT_TRUE(done.load());
    EXPECT_EQ(count.load(), 5);
}

/**
 * @test Try receive on empty stream
 * @brief try_receive should return nullopt on empty stream
 */
TEST_F(StreamEdgeCases, TryReceiveEmptyReturnsNone) {
    coro_stream<int> stream;
    
    auto result = stream.try_receive();
    EXPECT_FALSE(result.has_value());
}

/**
 * @test Try receive returns message without blocking
 * @brief try_receive should immediately return available message
 */
TEST_F(StreamEdgeCases, TryReceiveReturnsImmediately) {
    coro_stream<int> stream;
    stream.open();
    
    stream.deliver(42);
    
    auto result = stream.try_receive();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
    EXPECT_EQ(stream.size(), 0u);
}

// =============================================================================
// TEST SUITE: Multiple Coroutines Interaction
// =============================================================================

class StreamMultiCoro : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Multiple sequential receives in one coroutine
 * @brief Single coroutine can receive multiple messages
 */
TEST_F(StreamMultiCoro, SequentialReceivesInOneCoroutine) {
    coro_stream<int> stream;
    stream.open();
    std::vector<int> received;
    std::atomic<bool> done{false};

    // Deliver 5 messages
    for (int i = 0; i < 5; ++i) {
        stream.deliver(i * 10);
    }
    stream.close();

    auto consumer = [&stream, &received, &done]() -> task<void> {
        for (int i = 0; i < 10; ++i) {  // Try more than available
            auto msg = co_await stream.receive();
            if (!msg) break;
            received.push_back(*msg);
        }
        done.store(true);
        co_return;
    };

    coro_scheduler().spawn(consumer());
    run_for(50ms);

    ASSERT_TRUE(done.load());
    ASSERT_EQ(received.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(received[i], i * 10);
    }
}

/**
 * @test Consumer with timeout-like behavior using sleep
 * @brief Consumer can use sleep between receive attempts
 */
TEST_F(StreamMultiCoro, ConsumerWithPollingBehavior) {
    coro_stream<int> stream;
    stream.open();
    std::vector<int> received;
    std::atomic<bool> done{false};
    std::atomic<bool> producer_done{false};

    // Producer delivers with delays
    auto producer = [&stream, &producer_done]() -> task<void> {
        for (int i = 0; i < 3; ++i) {
            co_await sleep(20ms);
            stream.deliver(int{i});
        }
        co_await sleep(20ms);
        stream.close();
        producer_done.store(true);
        co_return;
    };

    // Consumer polls
    auto consumer = [&stream, &received, &done]() -> task<void> {
        while (true) {
            auto msg = co_await stream.receive();
            if (!msg) break;
            received.push_back(*msg);
            co_await sleep(5ms);  // Small processing delay
        }
        done.store(true);
        co_return;
    };

    coro_scheduler().spawn(producer());
    coro_scheduler().spawn(consumer());

    int iterations = 0;
    while (!done.load() && iterations < 100) {
        run_for(10ms);
        ++iterations;
    }

    EXPECT_TRUE(done.load());
    EXPECT_EQ(received.size(), 3u);
}

// =============================================================================
// TEST SUITE: Complex Message Types
// =============================================================================

class StreamComplexTypes : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

struct ComplexMessage {
    int id;
    std::string name;
    std::vector<double> data;
    
    bool operator==(const ComplexMessage& other) const {
        return id == other.id && name == other.name && data == other.data;
    }
};

/**
 * @test Complex message type handling
 * @brief Stream works with custom complex types
 */
TEST_F(StreamComplexTypes, ComplexTypeRoundTrip) {
    coro_stream<ComplexMessage> stream;
    stream.open();
    std::atomic<bool> done{false};
    std::optional<ComplexMessage> received;

    ComplexMessage original{42, "test", {1.0, 2.0, 3.0}};
    stream.deliver(std::move(original));
    stream.close();

    auto consumer = [&stream, &received, &done]() -> task<void> {
        received = co_await stream.receive();
        done.store(true);
        co_return;
    };

    coro_scheduler().spawn(consumer());
    run_for(50ms);

    ASSERT_TRUE(done.load());
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->id, 42);
    EXPECT_EQ(received->name, "test");
    EXPECT_EQ(received->data.size(), 3u);
}

/**
 * @test Move-only type handling
 * @brief Stream works with move-only types like unique_ptr
 */
TEST_F(StreamComplexTypes, MoveOnlyTypeHandling) {
    coro_stream<std::unique_ptr<int>> stream;
    stream.open();
    std::atomic<bool> done{false};
    int value = 0;

    stream.deliver(std::make_unique<int>(42));
    stream.close();

    auto consumer = [&stream, &done, &value]() -> task<void> {
        auto msg = co_await stream.receive();
        if (msg && *msg) {
            value = **msg;
        }
        done.store(true);
        co_return;
    };

    coro_scheduler().spawn(consumer());
    run_for(50ms);

    EXPECT_TRUE(done.load());
    EXPECT_EQ(value, 42);
}

// =============================================================================
// TEST SUITE: State Consistency
// =============================================================================

class StreamStateConsistency : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Size tracking is accurate
 * @brief Size reflects actual queue state
 */
TEST_F(StreamStateConsistency, SizeTrackingIsAccurate) {
    coro_stream<int> stream;
    stream.open();
    
    EXPECT_EQ(stream.size(), 0u);
    
    stream.deliver(1);
    EXPECT_EQ(stream.size(), 1u);
    
    stream.deliver(2);
    stream.deliver(3);
    EXPECT_EQ(stream.size(), 3u);
    
    auto msg = stream.try_receive();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(stream.size(), 2u);
    
    msg = stream.try_receive();
    msg = stream.try_receive();
    EXPECT_EQ(stream.size(), 0u);
}

/**
 * @test Error state propagation
 * @brief set_error marks stream correctly
 */
TEST_F(StreamStateConsistency, ErrorStatePropagation) {
    coro_stream<int> stream;
    stream.open();
    
    EXPECT_FALSE(stream.has_error());
    EXPECT_FALSE(stream.is_closed());
    
    stream.set_error();
    
    EXPECT_TRUE(stream.has_error());
    EXPECT_TRUE(stream.is_closed());
}

/**
 * @test Closed stream rejects new messages
 * @brief After close, deliver returns false
 */
TEST_F(StreamStateConsistency, ClosedStreamRejectsMessages) {
    coro_stream<int> stream;
    stream.open();
    
    EXPECT_TRUE(stream.deliver(1));
    EXPECT_EQ(stream.size(), 1u);
    
    stream.close();
    
    EXPECT_FALSE(stream.deliver(2));
    EXPECT_EQ(stream.size(), 1u);  // Original message still there
}
