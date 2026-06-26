/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/channel-payload-ownership.cpp
 * @brief Non-trivial payload ownership through `channel<T>` and `async_stream<T>`.
 *
 * The distinct value of this file is the *payload-ownership* angle — the move-only,
 * large-buffer and shared-lifetime cases that the channel send/recv suites
 * (unit/coroutine/channel-sync-ops.cpp, system/coroutine/channel-async.cpp) do NOT cover.
 * It proves that the channel and the stream built on it move a payload through without
 * copying, truncating, or corrupting it:
 *
 *   - a move-only `unique_ptr<int>` round-trips through `channel::recv()` AND through the
 *     `async_stream::from_channel_shared(...).for_each(...)` terminal (the original only
 *     exercised bare `channel::recv()`);
 *   - a 128 KiB buffer moves through with FULL-CONTENT verification (a per-byte rolling
 *     hash, not just a `front()/back()/size()` spot-check, so a mid-buffer corruption fails);
 *   - a `shared_ptr<channel>` is kept alive across a consumer coroutine while
 *     `from_channel_shared(...).collect()` drains it.
 *
 * The original misleading `CoroutineClientCompatibilityTest` fixture (it tests no client and
 * no compatibility) and the file-local `pump_until` clone are replaced by a clear
 * `ChannelPayloadOwnership` fixture and the shared `qb::io::test::pump_until` de-flake helper.
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

class ChannelPayloadOwnership : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

// Deterministic per-byte fill + a cheap order-sensitive rolling hash over the whole buffer,
// so a single flipped, dropped, or reordered byte changes the digest (full-content check, not
// just endpoints).
std::uint64_t
rolling_hash(const std::vector<char> &bytes) {
    std::uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    for (char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull; // FNV-1a prime
    }
    return h;
}

} // namespace

TEST_F(ChannelPayloadOwnership, MoveOnlyPayloadRoundTripsThroughChannelRecv) {
    channel<std::unique_ptr<int>> messages{1};
    ASSERT_TRUE(messages.try_send(std::make_unique<int>(42)));

    std::atomic<bool> done{false};
    int               value = 0;

    coro_scheduler().spawn([&]() -> task<void> {
        auto msg = co_await messages.recv();
        if (msg && *msg)
            value = **msg;
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "move-only recv coroutine never completed";
    EXPECT_EQ(value, 42);
}

TEST_F(ChannelPayloadOwnership, MoveOnlyPayloadRoundTripsThroughAsyncStream) {
    // The move-only case through the async_stream terminal, not just bare channel::recv —
    // ensures the stream's _next() chain preserves move-only ownership.
    auto              source = std::make_shared<channel<std::unique_ptr<int>>>(4);
    std::atomic<bool> done{false};
    std::vector<int>  values;

    coro_scheduler().spawn([source, &values, &done]() -> task<void> {
        co_await async_stream<std::unique_ptr<int>>::from_channel_shared(source).for_each([&values](std::unique_ptr<int> v) {
            if (v)
                values.push_back(*v);
        });
        done.store(true);
    });

    ASSERT_TRUE(source->try_send(std::make_unique<int>(1)));
    ASSERT_TRUE(source->try_send(std::make_unique<int>(2)));
    ASSERT_TRUE(source->try_send(std::make_unique<int>(3)));
    source->close();

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "move-only stream coroutine never completed";
    EXPECT_EQ(values, (std::vector<int>{1, 2, 3})) << "every move-only value must survive the stream terminal, in order";
}

TEST_F(ChannelPayloadOwnership, LargePayloadMovesThroughChannelWithoutCorruption) {
    constexpr std::size_t kSize = 128u * 1024u;

    // Deterministic, position-dependent fill so reordering/truncation changes the hash.
    std::vector<char> payload(kSize);
    for (std::size_t i = 0; i < kSize; ++i)
        payload[i] = static_cast<char>((i * 31u + 7u) & 0xFFu);
    const std::uint64_t expected_hash = rolling_hash(payload);

    channel<std::vector<char>> messages{1};
    ASSERT_TRUE(messages.try_send(std::move(payload)));

    std::atomic<bool> done{false};
    std::size_t       received_size = 0;
    std::uint64_t     received_hash = 0;

    coro_scheduler().spawn([&]() -> task<void> {
        auto msg = co_await messages.recv();
        if (msg) {
            received_size = msg->size();
            received_hash = rolling_hash(*msg);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "large-payload coroutine never completed";
    EXPECT_EQ(received_size, kSize);
    EXPECT_EQ(received_hash, expected_hash) << "full-content hash mismatch — the 128 KiB payload was corrupted in transit";
}

TEST_F(ChannelPayloadOwnership, SharedChannelKeepsSourceAliveAcrossConsumerCoroutine) {
    auto                     messages = std::make_shared<channel<std::string>>(2);
    std::atomic<bool>        done{false};
    std::vector<std::string> result;

    coro_scheduler().spawn([messages, &done, &result]() -> task<void> {
        result = co_await async_stream<std::string>::from_channel_shared(messages).collect();
        done.store(true);
    });

    ASSERT_TRUE(messages->try_send(std::string{"first"}));
    ASSERT_TRUE(messages->try_send(std::string{"second"}));
    messages->close();

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "shared-channel consumer never completed";
    EXPECT_EQ(result, (std::vector<std::string>{"first", "second"}));
}
