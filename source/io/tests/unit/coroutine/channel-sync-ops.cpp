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

#include <atomic>
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

// ===========================================================================
// Event-loop-driven channel paths
//
// The cases above exercise the channel's purely-synchronous surface. The few
// remaining channel.h branches require a coroutine to actually *park* on the
// scheduler (a recv/send/select that suspends) and then be woken by a second
// actor: the `try_send(const T&)` lvalue recv-handoff, the `send_for`
// resume-on-close / resume-into-pending-receiver paths, the variadic `select()`
// suspend-then-resolve path, and the awaiter frame-destruction de-registration
// guards (reached via a `when_any` loser whose parked awaiter frame is torn down
// when the race resolves). Every case gates on a real completion flag through
// `qb::io::test::pump_until` — never a blind `run_for`/sleep — so a wedged
// coroutine fails LOUD instead of hanging the runner.
//
// These tests use a dedicated fixture whose TearDown drains and resets the coro
// scheduler: some of them intentionally leave a coroutine parked (a never-served
// recv, a when_any loser), and those suspended frames must be destroyed between
// tests rather than leaking into the next one.
// ===========================================================================

namespace {

using namespace std::chrono_literals;
using qb::io::test::pump_until;

class ChannelLoopOps : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        // Drain ready coroutines, then destroy any still-suspended frames so a
        // parked recv / when_any loser does not leak into the next test.
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// try_send(const T&) — lvalue copy overload hands a value to a parked receiver
// (channel.h:413-418, the _recv_waiters direct-handoff branch of the *copy*
// overload; the existing sync tests only drive the buffer-full / closed paths).
// ---------------------------------------------------------------------------

TEST_F(ChannelLoopOps, TrySendCopyOverloadHandsValueToParkedReceiver) {
    channel<std::string> ch(0); // unbuffered: a recv with no value MUST park
    std::atomic<bool>    parked{false};
    std::atomic<bool>    received{false};
    std::string          got;

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);            // set just before the suspend point
        auto val = co_await ch.recv(); // parks: buffer empty, not closed
        if (val)
            got = std::move(*val);
        received.store(true);
    });

    // Pump until the receiver has run up to (and parked at) co_await recv().
    EXPECT_TRUE(pump_until([&] { return parked.load(); }, 200ms)) << "receiver never reached recv()";
    EXPECT_FALSE(received.load()) << "the receiver must still be parked (no value sent yet)";

    // lvalue (copy) overload: must take the _recv_waiters direct-handoff branch.
    const std::string payload = "handoff";
    EXPECT_TRUE(ch.try_send(payload)) << "try_send(const T&) to a parked receiver must succeed";

    EXPECT_TRUE(pump_until([&] { return received.load(); })) << "parked receiver was never woken by try_send(const T&)";
    EXPECT_EQ(got, "handoff");
    EXPECT_TRUE(ch.empty()) << "the value went straight to the receiver, not the buffer";
}

// ---------------------------------------------------------------------------
// send_for — a parked sender woken by close() reports failure
// (channel.h:679-680, `if (ch._closed) return false` in timed_send_awaiter::
// await_resume).
// ---------------------------------------------------------------------------

TEST_F(ChannelLoopOps, SendForParkedThenClosedReportsFailure) {
    channel<int>      ch(1);
    std::atomic<bool> parked{false};
    std::atomic<bool> done{false};
    std::atomic<bool> result{true};

    ASSERT_TRUE(ch.try_send(1)); // fill the cap-1 buffer so send_for must park

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        bool ok = co_await ch.send_for(2, 5s); // long timeout: only close() can wake us
        result.store(ok);
        done.store(true);
    });

    // Pump until the sender has entered send_for and parked on the full buffer.
    EXPECT_TRUE(pump_until([&] { return parked.load(); }, 200ms)) << "sender never reached send_for()";
    EXPECT_FALSE(done.load()) << "send_for must still be parked before close()";

    ch.close(); // wakes the parked sender -> await_resume sees _closed
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "close() never woke the parked send_for";
    EXPECT_FALSE(result.load()) << "send_for on a channel closed while parked must return false";
}

// ---------------------------------------------------------------------------
// send_for — a parked sender woken by a newly-arrived receiver hands the value
// directly (channel.h:682-686, the `!ch._recv_waiters.empty()` deliver-direct
// branch of timed_send_awaiter::await_resume). Requires an unbuffered channel
// so the sender parks first and a later receiver parks behind it; recv()'s
// wake_one_sender() then resumes the sender while the receiver is still queued.
// ---------------------------------------------------------------------------

TEST_F(ChannelLoopOps, SendForParkedDeliversDirectlyToLaterReceiver) {
    channel<int>      ch(0); // unbuffered rendezvous
    std::atomic<bool> sender_parked{false};
    std::atomic<bool> sent{false};
    std::atomic<bool> got_value{false};
    std::atomic<int>  received{-1};
    std::atomic<bool> sender_result{false};

    // Sender parks first: no receiver yet, capacity 0 -> slow path, parks.
    coro_scheduler().spawn([&]() -> task<void> {
        sender_parked.store(true);
        bool ok = co_await ch.send_for(77, 5s);
        sender_result.store(ok);
        sent.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return sender_parked.load(); }, 200ms)) << "sender never reached send_for()";
    EXPECT_FALSE(sent.load()) << "the sender must park before any receiver arrives";

    // Receiver parks behind it and immediately wakes one sender; the resumed
    // send_for sees a pending receiver and hands the value over directly.
    coro_scheduler().spawn([&]() -> task<void> {
        auto val = co_await ch.recv();
        if (val) {
            received.store(*val);
            got_value.store(true);
        }
    });

    EXPECT_TRUE(pump_until([&] { return sent.load() && got_value.load(); }))
        << "the parked send_for never delivered to the later receiver";
    EXPECT_TRUE(sender_result.load()) << "the direct hand-off must report success";
    EXPECT_EQ(received.load(), 77);
    EXPECT_TRUE(ch.empty()) << "a direct hand-off must not touch the buffer";
}

// ---------------------------------------------------------------------------
// select() (variadic) — suspends when no channel has data nor is closed, then
// resolves when a sender delivers (channel.h:1022/1036 fall-through of
// try_data/try_closed, await_suspend register_all, await_resume on a real win).
// ---------------------------------------------------------------------------

TEST_F(ChannelLoopOps, SelectSuspendsOnEmptyOpenChannelsThenResolvesOnSend) {
    channel<int>         ch_a(1);
    channel<std::string> ch_b(1);
    std::atomic<bool>    parked{false};
    std::atomic<bool>    done{false};
    std::atomic<size_t>  winner{99};
    std::atomic<bool>    closed{true};
    std::string          value;

    // Both channels empty + open -> try_data and try_closed both fall through to
    // `return false` -> await_ready false -> select suspends and registers.
    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        auto res = co_await select(ch_a, ch_b);
        winner.store(res.index);
        closed.store(res.closed);
        if (res.index == 1 && !res.closed)
            value = res.template get<std::string>();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return parked.load(); }, 200ms)) << "select coroutine never started";
    EXPECT_FALSE(done.load()) << "select must park while both channels are empty and open";

    // Deliver on the second channel via co_await send(): the send awaiter is the
    // path that satisfies a registered _select_waiters entry (try_send only ever
    // wakes _recv_waiters / buffers, never a parked select). This resolves the
    // select with ch_b as the winner.
    coro_scheduler().spawn([&]() -> task<void> {
        co_await ch_b.send(std::string{"picked"});
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "select never resolved after a send";
    EXPECT_EQ(winner.load(), 1u) << "the channel that received the value must win";
    EXPECT_FALSE(closed.load());
    EXPECT_EQ(value, "picked");
}

// ---------------------------------------------------------------------------
// recv_awaiter de-registration on frame destruction (channel.h:337-341): a recv
// parked in _recv_waiters whose coroutine frame is torn down must erase its
// queue entry so a later send cannot write through the dangling &_result. Driven
// deterministically as a `when_any` loser: the recv branch parks, the other
// branch wins, and resolving the race destroys the still-parked recv frame.
// ---------------------------------------------------------------------------

TEST_F(ChannelLoopOps, ParkedRecvDeregistersWhenFrameDestroyedAsWhenAnyLoser) {
    channel<int>       ch(0); // unbuffered: recv parks
    cancellation_token token;
    std::atomic<bool>  parked{false};
    std::atomic<bool>  done{false};
    std::atomic<size_t> winner{99};

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        // recv() parks forever (nobody sends); check_cancelled wins on cancel,
        // and the race teardown destroys the parked recv frame -> dtor de-registers.
        auto res = co_await when_any(
            [&ch]() -> task<void> { (void) co_await ch.recv(); }(),
            [token]() -> task<void> { co_await check_cancelled(token); }());
        winner.store(res.index);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return parked.load(); }, 200ms)) << "when_any coroutine never started";
    EXPECT_FALSE(done.load()) << "the recv branch must keep the race parked until cancellation";

    token.cancel(); // check_cancelled branch wins; recv loser frame is destroyed
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "cancel never resolved the when_any over a parked recv";
    EXPECT_EQ(winner.load(), 1u) << "the cancellation branch must win the race";

    // The de-registration is proven safe: a post-teardown send must NOT crash
    // (no dangling _recv_waiters entry survived) and simply buffers.
    EXPECT_FALSE(ch.try_send(5)) << "unbuffered channel with no live receiver must reject try_send";
}

// ---------------------------------------------------------------------------
// send_awaiter de-registration on frame destruction (channel.h:180-182): a
// parked sender (buffer full) whose frame is torn down must erase its
// _send_waiters entry. Driven as a `when_any` loser: the send branch parks on a
// full buffer, the cancellation branch wins, and the teardown destroys the
// parked send frame -> dtor de-registers.
// ---------------------------------------------------------------------------

TEST_F(ChannelLoopOps, ParkedSendDeregistersWhenFrameDestroyedAsWhenAnyLoser) {
    channel<int>       ch(1);
    cancellation_token token;
    std::atomic<bool>  parked{false};
    std::atomic<bool>  done{false};
    std::atomic<size_t> winner{99};

    ASSERT_TRUE(ch.try_send(1)); // fill cap-1 buffer so the send parks

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        auto res = co_await when_any(
            [&ch]() -> task<void> {
                try {
                    co_await ch.send(2); // parks on the full buffer
                } catch (const channel_closed &) {
                }
            }(),
            [token]() -> task<void> { co_await check_cancelled(token); }());
        winner.store(res.index);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return parked.load(); }, 200ms)) << "when_any coroutine never started";
    EXPECT_FALSE(done.load()) << "the send branch must keep the race parked on the full buffer";

    token.cancel(); // cancellation wins; the parked send loser frame is destroyed
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "cancel never resolved the when_any over a parked send";
    EXPECT_EQ(winner.load(), 1u);

    // The sender de-registered: draining the buffered value must wake NO ghost
    // sender (the queue is empty), and the channel stays usable.
    auto drained = ch.try_recv();
    ASSERT_TRUE(drained.has_value());
    EXPECT_EQ(*drained, 1);
    EXPECT_TRUE(ch.empty());
}
