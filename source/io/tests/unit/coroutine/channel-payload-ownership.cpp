/**
 * @file qb/io/tests/coroutine/test-coroutine-channel-payload.cpp
 * @brief Coroutine channel payload ownership tests
 *
 * This file contains tests for non-trivial payload handling through coroutine channels
 * and streams, including move-only values, large payload movement, and shared channel
 * ownership across consumer coroutines.
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
#include <string>
#include <vector>

#include <qb/io/async/coroutine.h>

using namespace qb::io::async;
using namespace std::chrono_literals;

namespace {

class CoroutineClientCompatibilityTest : public ::testing::Test {
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
pump_until(Predicate done, qb::duration timeout = 500ms) {
    const auto deadline = qb::mono_now() + timeout;
    while (!done() && qb::mono_now() < deadline) {
        qb::io::async::run_for(1ms);
    }
}

} // namespace

TEST_F(CoroutineClientCompatibilityTest, MoveOnlyPayloadRoundTripsThroughChannel) {
    channel<std::unique_ptr<int>> messages{1};
    ASSERT_TRUE(messages.try_send(std::make_unique<int>(42)));

    std::atomic<bool> done{false};
    int               value = 0;

    coro_scheduler().spawn([&]() -> task<void> {
        auto msg = co_await messages.recv();
        if (msg && *msg) {
            value = **msg;
        }
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); });

    ASSERT_TRUE(done.load());
    EXPECT_EQ(value, 42);
}

TEST_F(CoroutineClientCompatibilityTest, LargePayloadMovesThroughChannelWithoutTruncation) {
    channel<std::vector<char>> messages{1};
    std::vector<char>          payload(128 * 1024, 'x');

    ASSERT_TRUE(messages.try_send(std::move(payload)));

    std::atomic<bool> done{false};
    std::size_t       received_size = 0;
    bool              content_ok    = false;

    coro_scheduler().spawn([&]() -> task<void> {
        auto msg = co_await messages.recv();
        if (msg) {
            received_size = msg->size();
            content_ok    = msg->front() == 'x' && msg->back() == 'x';
        }
        done.store(true);
        co_return;
    });

    pump_until([&] { return done.load(); });

    ASSERT_TRUE(done.load());
    EXPECT_EQ(received_size, 128u * 1024u);
    EXPECT_TRUE(content_ok);
}

TEST_F(CoroutineClientCompatibilityTest, SharedChannelKeepsSourceAliveAcrossConsumerCoroutine) {
    auto                     messages = std::make_shared<channel<std::string>>(2);
    std::atomic<bool>        done{false};
    std::vector<std::string> result;

    coro_scheduler().spawn([messages, &done, &result]() -> task<void> {
        result = co_await async_stream<std::string>::from_channel_shared(messages).collect();
        done.store(true);
        co_return;
    });

    ASSERT_TRUE(messages->try_send(std::string{"first"}));
    ASSERT_TRUE(messages->try_send(std::string{"second"}));
    messages->close();

    pump_until([&] { return done.load(); });

    ASSERT_TRUE(done.load());
    EXPECT_EQ(result, (std::vector<std::string>{"first", "second"}));
}
