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
 * @test Destroy a channel while a receiver is parked
 * @brief A coroutine suspended on `co_await ch.recv()` must not use-after-free when the
 *        channel is destroyed before the deferred close()-resume runs (the lifetime of a
 *        `co_await consumer.receive()` that outlives the consumer). ~channel marks the
 *        liveness token dead; the resumed await_resume() and the awaiter destructor then
 *        skip the freed channel and the recv resolves to nullopt. Under AddressSanitizer
 *        the unguarded code reports heap-use-after-free here.
 */
TEST_F(ChannelBasicTests, DestroyChannelWhileRecvParkedNoUAF) {
    auto ch = std::make_unique<channel<int>>(4);
    bool done = false;
    bool got_value = true; // must become false (nullopt) once the channel is gone

    coro_scheduler().spawn([&]() -> task<void> {
        auto r = co_await ch->recv(); // parks: buffer empty, channel open
        got_value = r.has_value();
        done = true;
    });

    run_for(10ms); // let the receiver park
    EXPECT_FALSE(done);

    ch.reset(); // destroy the channel while the receiver is parked

    run_for(50ms); // deferred resume now runs against the freed channel
    EXPECT_TRUE(done);
    EXPECT_FALSE(got_value); // resolved to nullopt, no use-after-free
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

/**
 * @test Unbuffered rendezvous sender-first
 * @brief sender parks first on channel(0), receiver arrives later and must wake it
 */
TEST_F(ChannelEdgeCases, UnbufferedSenderFirstRendezvous) {
    channel<int> ch(0);
    std::atomic<bool> sender_done{false};
    std::atomic<bool> receiver_done{false};
    std::atomic<int> received{-1};

    auto sender = [&]() -> task<void> {
        co_await ch.send(7);
        sender_done = true;
    };

    auto receiver = [&]() -> task<void> {
        auto v = co_await ch.recv();
        if (v) received = *v;
        receiver_done = true;
    };

    auto orchestrator = [&]() -> task<void> {
        coro_scheduler().spawn(sender());
        co_await sleep(5ms); // ensure sender suspends first
        coro_scheduler().spawn(receiver());
    };

    coro_scheduler().spawn(orchestrator());
    run_for(200ms);

    EXPECT_TRUE(sender_done.load());
    EXPECT_TRUE(receiver_done.load());
    EXPECT_EQ(received.load(), 7);
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
// TEST SUITE: channel select()
// =============================================================================

class ChannelSelectTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override { qb::io::async::listener::current.clear(); }
};

TEST_F(ChannelSelectTests, FastPath_ValueAlreadyInBuffer) {
    channel<int>    ch_a(4), ch_b(4);
    select_result   result;
    bool done = false;

    ch_a.try_send(42);

    auto reader = [&ch_a, &ch_b, &result, &done]() -> task<void> {
        result = co_await select(ch_a, ch_b);
        done = true;
    };

    coro_scheduler().spawn(reader());
    run_for(20ms);
    EXPECT_TRUE(done);
    EXPECT_EQ(result.index, 0u);
    EXPECT_EQ(result.get<int>(), 42);
}

TEST_F(ChannelSelectTests, SlowPath_FirstSenderWins) {
    channel<int>  ch_a(1), ch_b(1);
    select_result result;
    bool done = false;

    auto reader = [&ch_a, &ch_b, &result, &done]() -> task<void> {
        result = co_await select(ch_a, ch_b);
        done = true;
    };
    coro_scheduler().spawn(reader());

    // Send to ch_b first (shorter delay)
    auto sender_b = [&ch_b]() -> task<void> {
        co_await sleep(5ms);
        co_await ch_b.send(99);
    };
    auto sender_a = [&ch_a]() -> task<void> {
        co_await sleep(20ms);
        co_await ch_a.send(77);
    };
    coro_scheduler().spawn(sender_b());
    coro_scheduler().spawn(sender_a());

    run_for(100ms);
    EXPECT_TRUE(done);
    EXPECT_EQ(result.index, 1u);  // ch_b won
    EXPECT_EQ(result.get<int>(), 99);
}

TEST_F(ChannelSelectTests, ClosedChannel_ResolvedAsClosed) {
    channel<int>  ch_a(1), ch_b(1);
    select_result result;
    bool done = false;

    ch_a.close();

    auto reader = [&ch_a, &ch_b, &result, &done]() -> task<void> {
        result = co_await select(ch_a, ch_b);
        done = true;
    };
    coro_scheduler().spawn(reader());
    run_for(10ms);
    EXPECT_TRUE(done);
    EXPECT_EQ(result.index, 0u);
    EXPECT_TRUE(result.closed);
}

TEST_F(ChannelSelectTests, VectorSelect_PicksFirstReady) {
    channel<int> ch0(1), ch1(1), ch2(1);
    std::vector<channel<int>*> channels = {&ch0, &ch1, &ch2};
    select_result result;
    bool done = false;

    auto reader = [&channels, &result, &done]() -> task<void> {
        result = co_await select(channels);
        done = true;
    };
    coro_scheduler().spawn(reader());

    auto sender = [&ch2]() -> task<void> {
        co_await sleep(5ms);
        co_await ch2.send(7);
    };
    coro_scheduler().spawn(sender());
    run_for(50ms);

    EXPECT_TRUE(done);
    EXPECT_EQ(result.index, 2u);
    EXPECT_EQ(result.get<int>(), 7);
}

TEST_F(ChannelSelectTests, SelectInLoop_DrainsMultipleChannels) {
    channel<int>  ch_a(4), ch_b(4);
    std::vector<int> received;

    auto producer = [&ch_a, &ch_b]() -> task<void> {
        co_await ch_a.send(1);
        co_await ch_b.send(2);
        co_await ch_a.send(3);
        ch_a.close();
        ch_b.close();
    };

    // Use select() while both channels are still open, fall back to
    // individual recv() once one of them closes. This avoids the
    // deterministic left-to-right bias of select picking the same
    // closed channel on every iteration.
    auto consumer = [&ch_a, &ch_b, &received]() -> task<void> {
        bool a_open = true, b_open = true;
        while (a_open || b_open) {
            if (a_open && b_open) {
                auto res = co_await select(ch_a, ch_b);
                if (res.closed) {
                    (res.index == 0 ? a_open : b_open) = false;
                } else {
                    received.push_back(res.get<int>());
                }
            } else if (a_open) {
                auto v = co_await ch_a.recv();
                if (!v) a_open = false; else received.push_back(*v);
            } else {
                auto v = co_await ch_b.recv();
                if (!v) b_open = false; else received.push_back(*v);
            }
        }
    };

    coro_scheduler().spawn(producer());
    coro_scheduler().spawn(consumer());
    run_for(100ms);

    EXPECT_EQ(received.size(), 3u);
}

// =============================================================================
// TEST SUITE: channel recv_for / send_for timeouts
// =============================================================================

class ChannelTimeoutTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override { qb::io::async::listener::current.clear(); }
};

TEST_F(ChannelTimeoutTests, RecvFor_ValueArrivesBeforeTimeout) {
    channel<int>       ch(1);
    std::optional<int> result;
    bool done = false;

    auto sender = [&ch]() -> task<void> {
        co_await sleep(10ms);
        co_await ch.send(42);
    };

    auto recver = [&ch, &result, &done]() -> task<void> {
        result = co_await ch.recv_for(200ms);
        done   = true;
    };

    coro_scheduler().spawn(sender());
    coro_scheduler().spawn(recver());
    run_for(300ms);

    EXPECT_TRUE(done);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST_F(ChannelTimeoutTests, RecvFor_TimesOutWhenNoValue) {
    channel<int>       ch(1);
    std::optional<int> result{999};
    bool done = false;

    auto recver = [&ch, &result, &done]() -> task<void> {
        result = co_await ch.recv_for(20ms);
        done   = true;
    };

    coro_scheduler().spawn(recver());
    run_for(100ms);

    EXPECT_TRUE(done);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ChannelTimeoutTests, RecvFor_BufferedFastPath) {
    channel<int>       ch(4);
    ch.try_send(55);
    std::optional<int> result;
    bool done = false;

    auto recver = [&ch, &result, &done]() -> task<void> {
        result = co_await ch.recv_for(50ms);
        done   = true;
    };
    coro_scheduler().spawn(recver());
    run_for(20ms);

    EXPECT_TRUE(done);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 55);
}

TEST_F(ChannelTimeoutTests, RecvFor_ClosedChannelReturnsNullopt) {
    channel<int>       ch(1);
    ch.close();
    std::optional<int> result{999};
    bool done = false;

    auto recver = [&ch, &result, &done]() -> task<void> {
        result = co_await ch.recv_for(50ms);
        done   = true;
    };
    coro_scheduler().spawn(recver());
    run_for(20ms);

    EXPECT_TRUE(done);
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// TEST SUITE: Channel Advanced APIs
// =============================================================================

class ChannelAdvancedTests : public ::testing::Test {
protected:
    void SetUp() override { qb::io::async::init(); }
    void TearDown() override { qb::io::async::listener::current.clear(); }
};

TEST_F(ChannelAdvancedTests, ChannelRangeIteratesBufferedItems) {
    channel<int> ch(10);
    ch.try_send(1);
    ch.try_send(2);
    ch.try_send(3);

    std::vector<int> collected;
    for (auto val : channel_range(ch)) {
        collected.push_back(val);
    }
    EXPECT_EQ(collected.size(), 3u);
    EXPECT_EQ(collected[0], 1);
    EXPECT_EQ(collected[2], 3);
    EXPECT_TRUE(ch.empty());
}

TEST_F(ChannelAdvancedTests, ChannelRangeEmptyChannel) {
    channel<int> ch(10);
    std::vector<int> collected;
    for (auto val : channel_range(ch)) {
        collected.push_back(val);
    }
    EXPECT_TRUE(collected.empty());
}

TEST_F(ChannelAdvancedTests, MakeChannelFactory) {
    auto ch = make_channel<std::string>(5);
    ASSERT_NE(ch, nullptr);
    EXPECT_EQ(ch->capacity(), 5u);
    ch->try_send("hello");
    auto val = ch->try_recv();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "hello");
}

TEST_F(ChannelAdvancedTests, MPSCMultipleSenders) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        channel<int> ch(10);
        for (int i = 0; i < 5; ++i) {
            co_await ch.send(i);
        }
        std::vector<int> received;
        while (auto v = ch.try_recv()) {
            received.push_back(*v);
        }
        EXPECT_EQ(received.size(), 5u);
        std::sort(received.begin(), received.end());
        for (int i = 0; i < 5; ++i) {
            EXPECT_EQ(received[i], i);
        }
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(ChannelAdvancedTests, SendForSuccessPath) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        channel<int> ch(1);
        bool sent = co_await ch.send_for(42, 100ms);
        EXPECT_TRUE(sent);
        auto v = ch.try_recv();
        EXPECT_TRUE(v.has_value());
        if (v.has_value()) EXPECT_EQ(*v, 42);
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(ChannelAdvancedTests, RecvDrainsBufferOnClose) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        channel<int> ch(5);
        co_await ch.send(10);
        co_await ch.send(20);
        ch.close();

        auto v1 = co_await ch.recv();
        auto v2 = co_await ch.recv();
        auto v3 = co_await ch.recv();

        EXPECT_TRUE(v1.has_value());
        EXPECT_EQ(*v1, 10);
        EXPECT_TRUE(v2.has_value());
        EXPECT_EQ(*v2, 20);
        EXPECT_FALSE(v3.has_value());
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
