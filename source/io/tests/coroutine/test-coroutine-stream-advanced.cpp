/**
 * @file qb/io/tests/coroutine/test-coroutine-stream-advanced.cpp
 * @brief Advanced coroutine stream tests
 *
 * This file contains advanced async_stream tests for suspending callbacks, structured
 * record pipelines, move-only values consumed from shared channel streams, and drain_to
 * backpressure behavior.
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
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <qb/io/async/coroutine.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class AsyncStreamAdvancedTest : public ::testing::Test {
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
pump_until(Predicate done, qb::duration timeout = 5s) {
    const auto deadline = qb::mono_now() + timeout;
    while (!done() && qb::mono_now() < deadline) {
        qb::io::async::run_for(1ms);
    }
}

struct Record {
    int         id{};
    std::string name;
};

} // namespace

TEST_F(AsyncStreamAdvancedTest, AsyncForEachCallbackMaySuspendBetweenItems) {
    std::atomic<bool> done{false};
    std::vector<int>  visited;

    coro_scheduler().spawn([&]() -> task<void> {
        co_await range_stream(0, 4).for_each([&](int value) -> task<void> {
            co_await sleep(0ms);
            visited.push_back(value);
            co_return;
        });
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); });

    ASSERT_TRUE(done.load());
    EXPECT_EQ(visited, (std::vector<int>{0, 1, 2, 3}));
}

TEST_F(AsyncStreamAdvancedTest, ComplexRecordsSurviveFilterMapReducePipeline) {
    std::atomic<bool> done{false};
    std::string       joined;

    coro_scheduler().spawn([&]() -> task<void> {
        std::vector<Record> records{{1, "skip"}, {2, "alpha"}, {3, "skip"}, {4, "beta"}};
        joined = co_await async_stream<Record>::from_vector(records)
                     .filter([](const Record &record) { return record.id % 2 == 0; })
                     .map([](Record record) { return record.name; })
                     .reduce(
                         [](std::string acc, std::string value) {
                             if (!acc.empty()) {
                                 acc += ",";
                             }
                             acc += value;
                             return acc;
                         },
                         std::string{});
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); });

    ASSERT_TRUE(done.load());
    EXPECT_EQ(joined, "alpha,beta");
}

TEST_F(AsyncStreamAdvancedTest, MoveOnlyValuesCanBeConsumedFromSharedChannelStream) {
    auto              source = std::make_shared<channel<std::unique_ptr<int>>>(4);
    std::atomic<bool> done{false};
    int               sum = 0;

    coro_scheduler().spawn([source, &done, &sum]() -> task<void> {
        co_await async_stream<std::unique_ptr<int>>::from_channel_shared(source).for_each([&](std::unique_ptr<int> value) {
            if (value) {
                sum += *value;
            }
        });
        done.store(true);
        co_return;
    });

    ASSERT_TRUE(source->try_send(std::make_unique<int>(10)));
    ASSERT_TRUE(source->try_send(std::make_unique<int>(20)));
    ASSERT_TRUE(source->try_send(std::make_unique<int>(30)));
    source->close();

    pump_until([&] { return done.load(); });

    ASSERT_TRUE(done.load());
    EXPECT_EQ(sum, 60);
}

TEST_F(AsyncStreamAdvancedTest, DrainToChannelPreservesBackpressureBoundary) {
    channel<int>      out{2};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await range_stream(0, 3).drain_to(out);
        done.store(true);
        co_return;
    });

    qb::io::async::run_for(5ms);
    EXPECT_EQ(out.size(), 2u);
    EXPECT_FALSE(done.load());

    EXPECT_EQ(out.try_recv(), std::optional<int>{0});
    pump_until([&] { return done.load(); });

    ASSERT_TRUE(done.load());
    EXPECT_EQ(out.try_recv(), std::optional<int>{1});
    EXPECT_EQ(out.try_recv(), std::optional<int>{2});
    EXPECT_FALSE(out.is_closed());
    out.close();
}
