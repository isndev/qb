/**
 * @file qb/io/tests/coroutine/test-coroutine-stream.cpp
 * @brief Coroutine async stream tests
 *
 * This file contains tests for async_stream creation, transformation, terminal
 * operations, range and repeat helpers, channel-backed streams, chained streams,
 * buffering, merge and zip operations, drain_to, timer streams, backpressure,
 * throttling, and shared channel consumption.
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
#include <qb/io/async/coroutine.h>
#include <chrono>
#include <vector>

using namespace qb::io::async;
using namespace std::chrono_literals;

// =============================================================================
// TEST SUITE: Stream Creation
// =============================================================================

class StreamCreationTests : public ::testing::Test {
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

/**
 * @test Stream from vector
 * @brief Create stream from container
 */
TEST_F(StreamCreationTests, FromVector) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{1, 2, 3, 4, 5};
        auto             stream = async_stream<int>::from_vector(data);

        auto result = co_await stream.collect();

        EXPECT_EQ(result, data);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(100ms);
}

/**
 * @test Empty stream
 * @brief Empty stream yields nothing
 */
TEST_F(StreamCreationTests, EmptyStream) {
    auto coro_fn = []() -> task<void> {
        auto stream = async_stream<int>::empty();

        auto result = co_await stream.collect();

        EXPECT_TRUE(result.empty());
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test Single value stream
 * @brief Stream with one element
 */
TEST_F(StreamCreationTests, SingleStream) {
    auto coro_fn = []() -> task<void> {
        auto stream = async_stream<int>::single(42);

        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{42}));
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

// =============================================================================
// TEST SUITE: Stream Transformations
// =============================================================================

class StreamTransformTests : public ::testing::Test {
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

/**
 * @test Map transformation
 * @brief Apply function to each element
 */
TEST_F(StreamTransformTests, MapTransform) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{1, 2, 3};
        auto             stream = async_stream<int>::from_vector(data).map([](int x) { return x * 2; });

        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{2, 4, 6}));
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test Filter transformation
 * @brief Keep only matching elements
 */
TEST_F(StreamTransformTests, FilterTransform) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{1, 2, 3, 4, 5};
        auto             stream = async_stream<int>::from_vector(data).filter([](int x) { return x % 2 == 0; });

        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{2, 4}));
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test Take transformation
 * @brief Limit number of elements
 */
TEST_F(StreamTransformTests, TakeTransform) {
    auto coro_fn = []() -> task<void> {
        auto stream = range_stream(0, 100).take(5);

        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{0, 1, 2, 3, 4}));
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test Skip transformation
 * @brief Skip first N elements
 */
TEST_F(StreamTransformTests, SkipTransform) {
    auto coro_fn = []() -> task<void> {
        auto stream = range_stream(0, 10).skip(7);

        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{7, 8, 9}));
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test Chained transformations
 * @brief Multiple transformations
 */
TEST_F(StreamTransformTests, ChainedTransforms) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        auto             stream = async_stream<int>::from_vector(data)
                                      .filter([](int x) { return x % 2 == 0; }) // 2, 4, 6, 8, 10
                                      .map([](int x) { return x * x; })         // 4, 16, 36, 64, 100
                                      .take(3);                                 // 4, 16, 36

        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{4, 16, 36}));
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

// =============================================================================
// TEST SUITE: Stream Operations
// =============================================================================

class StreamOperationTests : public ::testing::Test {
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

/**
 * @test Count operation
 * @brief Count elements in stream
 */
TEST_F(StreamOperationTests, CountOperation) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{1, 2, 3, 4, 5};
        auto             stream = async_stream<int>::from_vector(data);

        size_t count = co_await stream.count();

        EXPECT_EQ(count, 5);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test First operation
 * @brief Get first element
 */
TEST_F(StreamOperationTests, FirstOperation) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{42, 2, 3};
        auto             stream = async_stream<int>::from_vector(data);

        auto first = co_await stream.first();

        EXPECT_TRUE(first.has_value());
        EXPECT_EQ(*first, 42);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test First on empty stream
 * @brief Returns empty optional
 */
TEST_F(StreamOperationTests, FirstEmpty) {
    auto coro_fn = []() -> task<void> {
        auto stream = async_stream<int>::empty();

        auto first = co_await stream.first();

        EXPECT_FALSE(first.has_value());
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test For each operation
 * @brief Process each element
 */
TEST_F(StreamOperationTests, ForEachOperation) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{1, 2, 3};
        auto             stream = async_stream<int>::from_vector(data);

        int sum = 0;
        co_await stream.for_each([&sum](int x) { sum += x; });

        EXPECT_EQ(sum, 6);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test Any predicate
 * @brief Check if any element matches
 */
TEST_F(StreamOperationTests, AnyPredicate) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{1, 2, 3, 4, 5};
        auto             stream = async_stream<int>::from_vector(data);

        bool has_even     = co_await stream.any([](int x) { return x % 2 == 0; });
        bool has_negative = co_await stream.any([](int x) { return x < 0; });

        EXPECT_TRUE(has_even);
        EXPECT_FALSE(has_negative);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test All predicate
 * @brief Check if all elements match
 */
TEST_F(StreamOperationTests, AllPredicate) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{2, 4, 6, 8};
        auto             stream = async_stream<int>::from_vector(data);

        bool all_even     = co_await stream.all([](int x) { return x % 2 == 0; });
        bool all_positive = co_await stream.all([](int x) { return x > 0; });

        EXPECT_TRUE(all_even);
        EXPECT_TRUE(all_positive);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test Find operation
 * @brief Find first matching element
 */
TEST_F(StreamOperationTests, FindOperation) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{1, 2, 3, 4, 5};
        auto             stream = async_stream<int>::from_vector(data);

        auto found     = co_await stream.find([](int x) { return x > 3; });
        auto not_found = co_await async_stream<int>::from_vector(data).find([](int x) { return x > 10; });

        EXPECT_TRUE(found.has_value());
        EXPECT_EQ(*found, 4);
        EXPECT_FALSE(not_found.has_value());
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test Reduce operation
 * @brief Aggregate with function
 */
TEST_F(StreamOperationTests, ReduceOperation) {
    auto coro_fn = []() -> task<void> {
        std::vector<int> data{1, 2, 3, 4, 5};
        auto             stream = async_stream<int>::from_vector(data);

        int sum = co_await stream.reduce([](int a, int b) { return a + b; }, 0);

        EXPECT_EQ(sum, 15);
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

// =============================================================================
// TEST SUITE: Stream Utilities
// =============================================================================

class StreamUtilityTests : public ::testing::Test {
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

/**
 * @test Range stream
 * @brief Generate range
 */
TEST_F(StreamUtilityTests, RangeStream) {
    auto coro_fn = []() -> task<void> {
        auto stream = range_stream(5, 10);

        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{5, 6, 7, 8, 9}));
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

/**
 * @test Repeat value stream
 * @brief Infinite repetition
 */
TEST_F(StreamUtilityTests, RepeatValue) {
    auto coro_fn = []() -> task<void> {
        auto stream = repeat_value(7).take(3);

        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{7, 7, 7}));
    };

    auto t = coro_fn();
    coro_scheduler().spawn(std::move(t));
    run_for(50ms);
}

// =============================================================================
// TEST SUITE: Stream Lifetime & Source Safety
// Exercises patterns that involve lambdas captured in std::function (stored by
// value on the heap) — the safe pattern confirmed by audit — and tests
// from_channel's reference-lifetime contract.
// =============================================================================

class StreamLifetimeTests : public ::testing::Test {
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

/**
 * @test from_channel stream reads all values
 * @brief Verifies that a stream backed by a live channel drains correctly.
 *        The channel is kept alive in the enclosing coroutine frame while the
 *        stream reads from it, exercising the reference-capture lifetime rule.
 */
TEST_F(StreamLifetimeTests, FromChannelDrainsCorrectly) {
    std::atomic<bool> done{false};

    auto coro_fn = [&done]() -> task<void> {
        channel<int> ch(10);
        for (int i = 1; i <= 5; ++i)
            ch.try_send(i);
        ch.close();

        auto stream = async_stream<int>::from_channel(ch);
        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{1, 2, 3, 4, 5}));
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(200ms);
    EXPECT_TRUE(done);
}

/**
 * @test deep chained transforms
 * @brief Chains map→filter→take→skip to verify that each transform stores its
 *        source lambda safely in std::function (no dangling on the heap).
 */
TEST_F(StreamLifetimeTests, DeepChainedTransforms) {
    std::atomic<bool> done{false};

    auto coro_fn = [&done]() -> task<void> {
        std::vector<int> data{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        auto             stream = async_stream<int>::from_vector(data)
                                      .map([](int v) { return v * 2; })         // 2,4,6,8,10,12,14,16,18,20
                                      .filter([](int v) { return v % 4 == 0; }) // 4,8,12,16,20
                                      .skip(1)                                  // 8,12,16,20
                                      .take(3);                                 // 8,12,16

        auto result = co_await stream.collect();

        EXPECT_EQ(result, (std::vector<int>{8, 12, 16}));
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(200ms);
    EXPECT_TRUE(done);
}

/**
 * @test stream chain operator
 * @brief Two streams concatenated; verifies that the second source lambda in
 *        the chain() std::function closure is kept alive correctly.
 */
TEST_F(StreamLifetimeTests, ChainTwoStreams) {
    std::atomic<bool> done{false};

    auto coro_fn = [&done]() -> task<void> {
        auto s1      = async_stream<int>::from_vector({1, 2, 3});
        auto s2      = async_stream<int>::from_vector({4, 5, 6});
        auto chained = s1.chain(std::move(s2));
        auto result  = co_await chained.collect();

        EXPECT_EQ(result, (std::vector<int>{1, 2, 3, 4, 5, 6}));
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(200ms);
    EXPECT_TRUE(done);
}

/**
 * @test stream buffer batching
 * @brief Verifies that the buffer() transform groups elements correctly using
 *        its shared_ptr<vector> state stored in the std::function closure.
 */
TEST_F(StreamLifetimeTests, BufferBatching) {
    std::atomic<bool> done{false};

    auto coro_fn = [&done]() -> task<void> {
        std::vector<int> data{1, 2, 3, 4, 5, 6, 7};
        auto             stream = async_stream<int>::from_vector(data).buffer(3);

        auto batches = co_await stream.collect();

        // Expected: {1,2,3}, {4,5,6}, {7}
        EXPECT_EQ(batches.size(), 3u);
        EXPECT_EQ(batches[0], (std::vector<int>{1, 2, 3}));
        EXPECT_EQ(batches[1], (std::vector<int>{4, 5, 6}));
        EXPECT_EQ(batches[2], (std::vector<int>{7}));
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(200ms);
    EXPECT_TRUE(done);
}

/**
 * @test stream from_channel with async producer
 * @brief A separate producer coroutine sends values while a consumer reads via
 *        the stream. Tests that the channel reference held in from_channel's
 *        lambda remains valid while both coroutines are active.
 */
TEST_F(StreamLifetimeTests, FromChannelWithAsyncProducer) {
    std::atomic<bool> done{false};

    auto coro_fn = [&done]() -> task<void> {
        channel<int> ch(4);

        auto producer = [&ch]() -> task<void> {
            for (int i = 1; i <= 4; ++i) {
                co_await sleep(10ms);
                co_await ch.send(i);
            }
            ch.close();
        };
        coro_scheduler().spawn(producer());

        auto stream = async_stream<int>::from_channel(ch);
        int  sum    = 0;
        co_await stream.for_each([&sum](int v) { sum += v; });

        EXPECT_EQ(sum, 10); // 1+2+3+4
        done = true;
    };

    coro_scheduler().spawn(coro_fn());
    run_for(500ms);
    EXPECT_TRUE(done);
}

// =============================================================================
// TEST SUITE: Stream Advanced Factories and Operators
// =============================================================================

class StreamAdvancedTests : public ::testing::Test {
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

TEST_F(StreamAdvancedTests, MergeStreamsInterleavesRoundRobin) {
    auto coro_fn = []() -> task<void> {
        auto s1 = async_stream<int>::from_vector({1, 3, 5});
        auto s2 = async_stream<int>::from_vector({2, 4, 6});

        std::vector<async_stream<int>> vec;
        vec.push_back(std::move(s1));
        vec.push_back(std::move(s2));

        auto merged = merge_streams(std::move(vec));
        auto result = co_await merged.collect();

        EXPECT_EQ(result.size(), 6u);
        EXPECT_EQ(result[0], 1);
        EXPECT_EQ(result[1], 2);
        EXPECT_EQ(result[2], 3);
        EXPECT_EQ(result[3], 4);
    };

    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        co_await coro_fn();
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, MergeEmptyStreams) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<async_stream<int>> vec;
        auto                           merged = merge_streams(std::move(vec));
        auto                           result = co_await merged.collect();
        EXPECT_TRUE(result.empty());
        done = true;
    });
    run_for(200ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, ZipPairsElements) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto s1 = async_stream<int>::from_vector({1, 2, 3});
        auto s2 = async_stream<std::string>::from_vector({"a", "b", "c"});

        auto zipped = zip(std::move(s1), std::move(s2));
        auto result = co_await zipped.collect();

        EXPECT_EQ(result.size(), 3u);
        EXPECT_EQ(result[0].first, 1);
        EXPECT_EQ(result[0].second, "a");
        EXPECT_EQ(result[2].first, 3);
        EXPECT_EQ(result[2].second, "c");
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, ZipStopsAtShorterStream) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto s1 = async_stream<int>::from_vector({1, 2, 3, 4, 5});
        auto s2 = async_stream<int>::from_vector({10, 20});

        auto zipped = zip(std::move(s1), std::move(s2));
        auto result = co_await zipped.collect();

        EXPECT_EQ(result.size(), 2u);
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, DrainToChannel) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        channel<int> ch(10);
        auto         stream = async_stream<int>::from_vector({10, 20, 30});
        co_await stream.drain_to(ch);
        ch.close();

        std::vector<int> collected;
        while (auto v = co_await ch.recv()) {
            collected.push_back(*v);
        }
        EXPECT_EQ(collected.size(), 3u);
        EXPECT_EQ(collected[0], 10);
        EXPECT_EQ(collected[2], 30);
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, FromGeneratorSync) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        int  counter = 0;
        auto stream  = from_generator([&counter]() -> int { return counter++; });
        auto result  = co_await stream.take(5).collect();
        EXPECT_EQ(result.size(), 5u);
        EXPECT_EQ(result[0], 0);
        EXPECT_EQ(result[4], 4);
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, TimerStreamEmitsOnceAfterDelay) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto stream = timer(42, 50ms);
        auto result = co_await stream.collect();
        EXPECT_EQ(result.size(), 1u);
        EXPECT_EQ(result[0], 42);
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, IntervalStreamEmitsTicks) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto stream = interval(30ms, true);
        auto result = co_await stream.take(3).collect();
        EXPECT_EQ(result.size(), 3u);
        EXPECT_EQ(result[0], 0u);
        EXPECT_EQ(result[1], 1u);
        EXPECT_EQ(result[2], 2u);
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, BackpressureRespectsBound) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto source = async_stream<int>::from_vector({1, 2, 3, 4, 5});
        auto bp     = source.backpressure(2);
        auto result = co_await bp.collect();
        EXPECT_EQ(result.size(), 5u);
        EXPECT_EQ(result[0], 1);
        EXPECT_EQ(result[4], 5);
        done = true;
    });
    run_for(1000ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, ThrottleRateLimits) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto stream    = async_stream<int>::from_vector({1, 2, 3});
        auto throttled = stream.throttle(30ms);
        auto start     = std::chrono::steady_clock::now();
        auto result    = co_await throttled.collect();
        auto elapsed   = std::chrono::steady_clock::now() - start;
        EXPECT_EQ(result.size(), 3u);
        EXPECT_GE(elapsed, 50ms);
        done = true;
    });
    run_for(1000ms);
    EXPECT_TRUE(done);
}

TEST_F(StreamAdvancedTests, FromChannelSharedMultipleConsumers) {
    bool done = false;
    coro_scheduler().spawn([&]() -> task<void> {
        auto ch_ptr = std::make_shared<channel<int>>(10);
        co_await ch_ptr->send(1);
        co_await ch_ptr->send(2);
        co_await ch_ptr->send(3);
        ch_ptr->close();

        auto stream = async_stream<int>::from_channel_shared(ch_ptr);
        auto result = co_await stream.collect();
        EXPECT_EQ(result.size(), 3u);
        done = true;
    });
    run_for(500ms);
    EXPECT_TRUE(done);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    qb::io::async::init();
    return RUN_ALL_TESTS();
}
