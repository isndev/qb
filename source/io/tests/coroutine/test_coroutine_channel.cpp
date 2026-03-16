/**
 * @file test_coroutine_channel.cpp
 * @brief Channel tests for coroutine communication
 *
 * Tests for MPSC channel:
 * - send/recv
 * - try_send/try_recv
 * - buffered and unbuffered channels
 * - close semantics
 *
 * @author qb - C++ Actor Framework
 */

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <atomic>
#include <string>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Channel Basic Operations
// =============================================================================

class ChannelBasicTests : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Buffered channel send/receive
 * @brief Basic producer-consumer pattern
 */
TEST_F(ChannelBasicTests, BufferedSendReceive) {
    channel<int> ch(5);
    std::atomic<int> received{0};

    auto producer = [&ch]() -> task<void> {
        for (int i = 1; i <= 5; ++i) {
            co_await ch.send(i);
        }
        ch.close();
    };

    auto consumer = [&ch, &received]() -> task<void> {
        while (true) {
            auto val = co_await ch.recv();
            if (!val) break;
            received += *val;
        }
    };

    coro_scheduler().spawn(producer());
    coro_scheduler().spawn(consumer());

    run_for(100ms);

    EXPECT_EQ(received, 15);  // 1+2+3+4+5
}

/**
 * @test Try send on buffered channel
 * @brief Non-blocking send when buffer has space
 */
TEST_F(ChannelBasicTests, TrySendBuffered) {
    channel<int> ch(2);

    EXPECT_TRUE(ch.try_send(1));
    EXPECT_TRUE(ch.try_send(2));
    EXPECT_FALSE(ch.try_send(3));  // Buffer full

    EXPECT_EQ(ch.size(), 2);
}

/**
 * @test Try receive
 * @brief Non-blocking receive
 */
TEST_F(ChannelBasicTests, TryReceive) {
    channel<int> ch(2);

    ch.try_send(42);

    auto val = ch.try_recv();
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);

    auto empty = ch.try_recv();
    EXPECT_FALSE(empty.has_value());
}

/**
 * @test Channel capacity
 * @brief Buffer respects capacity
 */
TEST_F(ChannelBasicTests, ChannelCapacity) {
    channel<int> ch(3);

    EXPECT_EQ(ch.capacity(), 3);
    EXPECT_TRUE(ch.empty());

    ch.try_send(1);
    ch.try_send(2);
    ch.try_send(3);

    EXPECT_EQ(ch.size(), 3);
    EXPECT_FALSE(ch.empty());
}

/**
 * @test Channel close
 * @brief Close signals end of stream
 */
TEST_F(ChannelBasicTests, ChannelClose) {
    channel<int> ch(5);

    EXPECT_FALSE(ch.is_closed());

    ch.close();

    EXPECT_TRUE(ch.is_closed());
}

// =============================================================================
// TEST SUITE: Channel Edge Cases
// =============================================================================

class ChannelEdgeCases : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Receive from closed empty channel
 * @brief Returns empty optional
 */
TEST_F(ChannelEdgeCases, ReceiveFromClosedEmpty) {
    channel<int> ch(5);
    ch.close();

    auto result = ch.try_recv();
    EXPECT_FALSE(result.has_value());
}

/**
 * @test Send to closed channel
 * @brief try_send returns false
 */
TEST_F(ChannelEdgeCases, SendToClosed) {
    channel<int> ch(5);
    ch.try_send(1);
    ch.close();

    EXPECT_FALSE(ch.try_send(2));
}

// =============================================================================
// TEST SUITE: Channel Fast Path (await_ready avoids suspend)
// =============================================================================

class ChannelFastPathTests : public ::testing::Test {
protected:
    void SetUp() override {
        qb::io::async::init();
    }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test Send when buffer has space
 * @brief Multiple sends with capacity; fast path should complete without suspending
 */
TEST_F(ChannelFastPathTests, SendWhenBufferHasSpace) {
    channel<int> ch(20);
    std::atomic<int> sent{0};

    auto producer = [&ch, &sent]() -> task<void> {
        for (int i = 0; i < 10; ++i) {
            co_await ch.send(i);
            sent++;
        }
        ch.close();
    };

    coro_scheduler().spawn(producer());
    run_for(50ms);

    EXPECT_EQ(sent, 10);
    EXPECT_EQ(ch.size(), 10u);
}

/**
 * @test Recv when buffer has data
 * @brief Consumer receives pre-filled buffer; fast path for recv
 */
TEST_F(ChannelFastPathTests, RecvWhenBufferHasData) {
    channel<int> ch(10);
    for (int i = 0; i < 5; ++i) {
        ch.try_send(i + 1);
    }

    std::atomic<int> sum{0};
    auto consumer = [&ch, &sum]() -> task<void> {
        for (int i = 0; i < 5; ++i) {
            auto v = co_await ch.recv();
            if (v) sum += *v;
        }
    };

    coro_scheduler().spawn(consumer());
    run_for(50ms);

    EXPECT_EQ(sum, 15);  // 1+2+3+4+5
}

// =============================================================================
// TEST SUITE: make_pipeline
// Exercises the fixed pipeline_worker free function that replaced the
// local-lambda-coroutine capturing &in and &out (moved-from local unique_ptrs).
// =============================================================================

class PipelineTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

/**
 * @test make_pipeline basic transform
 * @brief Sends values into the input channel and reads transformed values from
 *        the output channel. Verifies that the pipeline_worker coroutine runs
 *        correctly after make_pipeline returns (i.e., the raw-pointer fix works).
 */
TEST_F(PipelineTests, BasicTransform) {
    auto [in, out] = make_pipeline<int, int>([](int v) { return v * 10; }, 8);

    std::atomic<int> sum{0};
    std::atomic<bool> done{false};

    // Producer + consumer coroutines share the channels by raw pointer; the
    // channels are kept alive because in/out unique_ptrs live in this scope.
    auto producer = [&in]() -> task<void> {
        for (int i = 1; i <= 5; ++i) co_await in->send(i);
        in->close();
    };
    auto consumer = [&out, &sum, &done]() -> task<void> {
        while (true) {
            auto v = co_await out->recv();
            if (!v) break;
            sum += *v;
        }
        done = true;
    };

    coro_scheduler().spawn(producer());
    coro_scheduler().spawn(consumer());
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(sum.load(), (1 + 2 + 3 + 4 + 5) * 10);  // 150
}

/**
 * @test make_pipeline string transform
 * @brief Type-changing transform (int→string) to verify template instantiation.
 */
TEST_F(PipelineTests, TypeChangingTransform) {
    auto [in, out] = make_pipeline<int, std::string>([](int v) {
        return "item" + std::to_string(v);
    }, 4);

    std::atomic<int>  received{0};
    std::atomic<bool> done{false};

    auto producer = [&in]() -> task<void> {
        for (int i = 0; i < 3; ++i) co_await in->send(i);
        in->close();
    };
    auto consumer = [&out, &received, &done]() -> task<void> {
        while (true) {
            auto v = co_await out->recv();
            if (!v) break;
            ++received;
        }
        done = true;
    };

    coro_scheduler().spawn(producer());
    coro_scheduler().spawn(consumer());
    run_for(200ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(received.load(), 3);
}

/**
 * @test make_pipeline close propagates
 * @brief When the input channel is closed, the worker closes the output channel
 *        and the consumer exits cleanly.
 */
TEST_F(PipelineTests, ClosePropagates) {
    auto [in, out] = make_pipeline<int, int>([](int v) { return v; }, 4);

    std::atomic<bool> consumer_done{false};

    auto consumer = [&out, &consumer_done]() -> task<void> {
        int count = 0;
        while (true) {
            auto v = co_await out->recv();
            if (!v) break;
            ++count;
        }
        EXPECT_EQ(count, 2);
        consumer_done = true;
    };

    coro_scheduler().spawn(consumer());

    // Send two items then close the input.
    auto sender = [&in]() -> task<void> {
        co_await in->send(1);
        co_await in->send(2);
        in->close();
    };
    coro_scheduler().spawn(sender());
    run_for(200ms);

    EXPECT_TRUE(consumer_done);
    EXPECT_TRUE(out->is_closed());
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
