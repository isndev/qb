/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/coroutine/channel-sync-ops.cpp
 * @brief `qb::io::async::channel<T>` synchronous surface — no coroutine, no event loop.
 *
 * The unit half of the former monolithic test-coroutine-channel.cpp split (the async half
 * lives in system/coroutine/channel-async.cpp; the UAF/lifetime regressions in
 * system/coroutine/channel-lifetime.cpp). Everything here exercises the channel's
 * *spawn-free synchronous methods* — the paths that complete entirely on the calling
 * thread without ever suspending a coroutine: `try_send` (copy + move overloads),
 * `try_recv`, `size`/`capacity`/`empty`, `close`/`is_closed`, the non-blocking
 * `channel_range` drain iterator, the `make_channel` factory, and `register_select_waiter`
 * (the synchronous select-registration path that resolves a buffered-or-closed channel
 * immediately). These never touch a timer or park on the scheduler, so they are
 * deterministic pure logic: no `pump_until`, no `run_for`, no sleeps. We still call
 * `reset_async_context()` in SetUp because `try_send`/`try_recv` route a freed buffer slot
 * through `schedule_via_current` when a (here always-absent) receiver is queued — the TLS
 * scheduler must exist for that call to resolve.
 *
 * Assertion bar: every capacity/close/select contract is pinned to exact values
 * (the resolved winner index, the `std::any_cast` value, `closed`, `empty()`), not a
 * size-only smoke check.
 */

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;

namespace {

class ChannelSyncOps : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// try_send — capacity, full, closed, copy + move overloads
// ---------------------------------------------------------------------------

TEST_F(ChannelSyncOps, TrySendBufferedRespectsCapacity) {
    channel<int> ch(2);

    EXPECT_TRUE(ch.try_send(1));
    EXPECT_TRUE(ch.try_send(2));
    EXPECT_FALSE(ch.try_send(3)) << "buffer full -> try_send must fail";

    EXPECT_EQ(ch.size(), 2u);
}

TEST_F(ChannelSyncOps, TrySendReportsClosedAndFullForCopyAndMove) {
    channel<std::string> ch(1);
    const std::string    first  = "first";
    const std::string    second = "second";

    // Copy overload: first fits, second overflows the cap-1 buffer.
    EXPECT_TRUE(ch.try_send(first));
    EXPECT_FALSE(ch.try_send(second));
    // Move overload: also rejected while full.
    EXPECT_FALSE(ch.try_send(std::string{"third"}));

    ch.close();

    // Both overloads reject on a closed channel.
    EXPECT_FALSE(ch.try_send(second));
    EXPECT_FALSE(ch.try_send(std::string{"closed-move"}));
}

// ---------------------------------------------------------------------------
// try_recv — value then empty
// ---------------------------------------------------------------------------

TEST_F(ChannelSyncOps, TryReceiveReturnsValueThenEmpty) {
    channel<int> ch(2);

    ASSERT_TRUE(ch.try_send(42));

    auto val = ch.try_recv();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);

    auto empty = ch.try_recv();
    EXPECT_FALSE(empty.has_value());
}

// ---------------------------------------------------------------------------
// capacity / size / empty bookkeeping
// ---------------------------------------------------------------------------

TEST_F(ChannelSyncOps, CapacitySizeEmptyBookkeeping) {
    channel<int> ch(3);

    EXPECT_EQ(ch.capacity(), 3u);
    EXPECT_EQ(ch.size(), 0u);
    EXPECT_TRUE(ch.empty());

    ASSERT_TRUE(ch.try_send(1));
    ASSERT_TRUE(ch.try_send(2));
    ASSERT_TRUE(ch.try_send(3));

    EXPECT_EQ(ch.size(), 3u);
    EXPECT_FALSE(ch.empty());
}

// ---------------------------------------------------------------------------
// close — idempotent flag flip; receive/send on closed
// ---------------------------------------------------------------------------

TEST_F(ChannelSyncOps, CloseFlipsIsClosedAndIsIdempotent) {
    channel<int> ch(5);

    EXPECT_FALSE(ch.is_closed());
    ch.close();
    EXPECT_TRUE(ch.is_closed());
    ch.close(); // idempotent — must not assert/throw
    EXPECT_TRUE(ch.is_closed());
}

TEST_F(ChannelSyncOps, ReceiveFromClosedEmptyReturnsNullopt) {
    channel<int> ch(5);
    ch.close();

    auto result = ch.try_recv();
    EXPECT_FALSE(result.has_value());
}

TEST_F(ChannelSyncOps, TrySendToClosedReturnsFalse) {
    channel<int> ch(5);
    ASSERT_TRUE(ch.try_send(1));
    ch.close();

    EXPECT_FALSE(ch.try_send(2));
    // The value buffered before close stays drainable.
    EXPECT_EQ(ch.size(), 1u);
}

// ---------------------------------------------------------------------------
// channel_range — non-blocking drain of buffered items only
// ---------------------------------------------------------------------------

TEST_F(ChannelSyncOps, ChannelRangeIteratesBufferedItemsInOrder) {
    channel<int> ch(10);
    ASSERT_TRUE(ch.try_send(1));
    ASSERT_TRUE(ch.try_send(2));
    ASSERT_TRUE(ch.try_send(3));

    std::vector<int> collected;
    for (auto val : channel_range(ch))
        collected.push_back(val);

    EXPECT_EQ(collected, (std::vector<int>{1, 2, 3}));
    EXPECT_TRUE(ch.empty()) << "channel_range must drain every buffered item";
}

TEST_F(ChannelSyncOps, ChannelRangeOverEmptyChannelYieldsNothing) {
    channel<int>     ch(10);
    std::vector<int> collected;
    for (auto val : channel_range(ch))
        collected.push_back(val);

    EXPECT_TRUE(collected.empty());
}

// ---------------------------------------------------------------------------
// make_channel factory
// ---------------------------------------------------------------------------

TEST_F(ChannelSyncOps, MakeChannelFactoryProducesUsableChannel) {
    auto ch = make_channel<std::string>(5);
    ASSERT_NE(ch, nullptr);
    EXPECT_EQ(ch->capacity(), 5u);

    ASSERT_TRUE(ch->try_send("hello"));
    auto val = ch->try_recv();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "hello");
}

// ---------------------------------------------------------------------------
// register_select_waiter — synchronous resolution of buffered / closed channels
// ---------------------------------------------------------------------------

TEST_F(ChannelSyncOps, RegisterSelectWaiterResolvesBufferedChannelImmediately) {
    channel<int> buffered(2);
    auto         state = std::make_shared<channel_select_state>();

    ASSERT_TRUE(buffered.try_send(42));
    buffered.register_select_waiter(state, /*idx=*/3);

    EXPECT_TRUE(state->resolved);
    EXPECT_FALSE(state->closed);
    EXPECT_EQ(state->winner, 3u);
    ASSERT_TRUE(state->value.has_value());
    EXPECT_EQ(std::any_cast<int>(state->value), 42);
    EXPECT_TRUE(buffered.empty()) << "the buffered value must be consumed into the select state";
}

TEST_F(ChannelSyncOps, RegisterSelectWaiterResolvesClosedChannelAsClosed) {
    channel<int> closed(1);
    auto         state = std::make_shared<channel_select_state>();

    closed.close();
    closed.register_select_waiter(state, /*idx=*/1);

    EXPECT_TRUE(state->resolved);
    EXPECT_TRUE(state->closed);
    EXPECT_EQ(state->winner, 1u);
    EXPECT_FALSE(state->value.has_value());
}
