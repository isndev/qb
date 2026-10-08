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

// try_send must hand its value to a PARKED select waiter, mirroring the coroutine send() path.
// Regression for the cap-0 rendezvous gap: try_send only checked _recv_waiters + the buffer, so
// with a parked select()/recv_for() waiter and no usable buffer (cap 0) it returned false and the
// rendezvous deadlocked, even though a partner was ready.
TEST_F(ChannelSyncOps, TrySendSatisfiesParkedSelectWaiterOnRendezvousChannel) {
    channel<int> ch(0); // cap-0 rendezvous: no buffer — a sender must hand off directly
    auto         state = std::make_shared<channel_select_state>();

    ch.register_select_waiter(state, /*idx=*/7); // empty channel, no recv waiter -> parks
    ASSERT_FALSE(state->resolved) << "select waiter must park on an empty rendezvous channel";

    EXPECT_TRUE(ch.try_send(99)) << "try_send must satisfy a parked select waiter on a cap-0 channel (pre-fix returned false)";
    EXPECT_TRUE(state->resolved);
    EXPECT_FALSE(state->closed);
    EXPECT_EQ(state->winner, 7u);
    ASSERT_TRUE(state->value.has_value());
    EXPECT_EQ(std::any_cast<int>(state->value), 99);
    EXPECT_TRUE(ch.empty()) << "the value went to the select waiter, not the buffer";
}

// The move overload of try_send must satisfy a parked select waiter too.
TEST_F(ChannelSyncOps, TrySendMoveSatisfiesParkedSelectWaiter) {
    channel<std::string> ch(0);
    auto                 state = std::make_shared<channel_select_state>();

    ch.register_select_waiter(state, /*idx=*/2);
    ASSERT_FALSE(state->resolved);

    std::string v = "handoff";
    EXPECT_TRUE(ch.try_send(std::move(v)));
    EXPECT_TRUE(state->resolved);
    EXPECT_EQ(state->winner, 2u);
    ASSERT_TRUE(state->value.has_value());
    EXPECT_EQ(std::any_cast<std::string>(state->value), "handoff");
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
// (deliver()'s _recv_waiters direct-handoff branch, reached through the *copy*
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
// send_for — a parked sender woken by close() reports failure: close() resumes
// it without a hand-off, and timed_send_awaiter::await_resume reports whether
// a wake handed its value over.
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
// directly: an unbuffered channel, so the sender parks first and a later
// receiver parks behind it; recv()'s wake_one_sender() hands the sender's value
// straight to that receiver (deliver()'s _recv_waiters branch), then resumes it.
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

    // Receiver parks behind it and immediately wakes one sender, whose value the
    // wake hands to this receiver before resuming it.
    coro_scheduler().spawn([&]() -> task<void> {
        auto val = co_await ch.recv();
        if (val) {
            received.store(*val);
            got_value.store(true);
        }
    });

    EXPECT_TRUE(pump_until([&] { return sent.load() && got_value.load(); })) << "the parked send_for never delivered to the later receiver";
    EXPECT_TRUE(sender_result.load()) << "the direct hand-off must report success";
    EXPECT_EQ(received.load(), 77);
    EXPECT_TRUE(ch.empty()) << "a direct hand-off must not touch the buffer";
}

// ---------------------------------------------------------------------------
// select() (variadic) — suspends when no channel has data nor is closed, then
// resolves when a sender delivers (the try_data/try_closed fall-through,
// await_suspend's register_all, await_resume on a real win).
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
    coro_scheduler().spawn([&]() -> task<void> { co_await ch_b.send(std::string{"picked"}); });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "select never resolved after a send";
    EXPECT_EQ(winner.load(), 1u) << "the channel that received the value must win";
    EXPECT_FALSE(closed.load());
    EXPECT_EQ(value, "picked");
}

// ---------------------------------------------------------------------------
// recv_awaiter de-registration on frame destruction (~recv_awaiter): a recv
// parked in _recv_waiters whose coroutine frame is torn down must erase its
// queue entry so a later send cannot write through the dangling &_result. Driven
// deterministically as a `when_any` loser: the recv branch parks, the other
// branch wins, and resolving the race destroys the still-parked recv frame.
// ---------------------------------------------------------------------------

TEST_F(ChannelLoopOps, ParkedRecvDeregistersWhenFrameDestroyedAsWhenAnyLoser) {
    channel<int>        ch(0); // unbuffered: recv parks
    cancellation_token  token;
    std::atomic<bool>   parked{false};
    std::atomic<bool>   done{false};
    std::atomic<size_t> winner{99};

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        // recv() parks forever (nobody sends); check_cancelled wins on cancel,
        // and the race teardown destroys the parked recv frame -> dtor de-registers.
        // The lambdas are named locals of THIS coroutine frame, not temporaries of the
        // when_any(...) full-expression: `task`'s initial_suspend is suspend_always, so each body
        // starts on a LATER run_ready() — by which point an immediately-invoked temporary closure
        // is gone and every capture read is a dangling access.
        auto recv_op = [&ch]() -> task<void> {
            (void) co_await ch.recv();
        };
        auto cancel_op = [token]() -> task<void> {
            co_await check_cancelled(token);
        };
        auto res = co_await when_any(recv_op(), cancel_op());
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
// send_awaiter de-registration on frame destruction (~send_awaiter): a
// parked sender (buffer full) whose frame is torn down must erase its
// _send_waiters entry. Driven as a `when_any` loser: the send branch parks on a
// full buffer, the cancellation branch wins, and the teardown destroys the
// parked send frame -> dtor de-registers.
// ---------------------------------------------------------------------------

TEST_F(ChannelLoopOps, ParkedSendDeregistersWhenFrameDestroyedAsWhenAnyLoser) {
    channel<int>        ch(1);
    cancellation_token  token;
    std::atomic<bool>   parked{false};
    std::atomic<bool>   done{false};
    std::atomic<size_t> winner{99};

    ASSERT_TRUE(ch.try_send(1)); // fill cap-1 buffer so the send parks

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        // Named locals of this frame — see the note in the recv-side case above.
        auto send_op = [&ch]() -> task<void> {
            try {
                co_await ch.send(2); // parks on the full buffer
            } catch (const channel_closed &) {
            }
        };
        auto cancel_op = [token]() -> task<void> {
            co_await check_cancelled(token);
        };
        auto res = co_await when_any(send_op(), cancel_op());
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

// ---------------------------------------------------------------------------
// A parked sender's wake IS its delivery (Huly QB-272). The sender parked on a full buffer (or on a rendezvous with
// no receiver) is handed the room the moment it appears -- its value moves into the slot just freed, or straight to
// the receiver that just parked -- before anything else runs, and it resumes already done. Until 3.3 it was only
// WOKEN: it delivered on resume, so anything that ran in between saw the room as free and took it -- the sender then
// buffered past the capacity (and behind the newcomer), or buffered on a capacity-0 channel.
// ---------------------------------------------------------------------------

TEST_F(ChannelLoopOps, WokenSenderOwnsTheSlotItsWakeFreedSoANewcomerCannotTakeIt) {
    channel<int>      ch(1);
    std::atomic<bool> parked{false};
    std::atomic<bool> sent{false};
    ASSERT_TRUE(ch.try_send(1)); // the buffer is full: the next send parks

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        co_await ch.send(2); // parks on the full buffer
        sent.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return parked.load(); }, 200ms)) << "sender never reached send()";
    ASSERT_FALSE(sent.load()) << "the sender must be parked on the full buffer";

    // One synchronous block, no drain in between: the receive frees the slot, a newcomer tries to take it.
    auto first = ch.try_recv();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 1);
    EXPECT_FALSE(ch.try_send(3)) << "the slot try_recv freed belongs to the parked sender, not to a newcomer";
    EXPECT_EQ(ch.size(), 1u) << "the parked sender's value is in the buffer at its wake, not at its resume";
    EXPECT_LE(ch.size(), ch.capacity());

    ASSERT_TRUE(pump_until([&] { return sent.load(); })) << "the woken sender never completed";
    EXPECT_LE(ch.size(), ch.capacity()) << "the buffer outgrew its capacity";
    auto second = ch.try_recv();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2) << "the parked sender's value comes next, in FIFO order";
    EXPECT_FALSE(ch.try_recv().has_value()) << "nothing else was buffered";
}

TEST_F(ChannelLoopOps, TimedSenderWokenByAFreedSlotOwnsItToo) {
    channel<int>      ch(1);
    std::atomic<bool> parked{false};
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    ASSERT_TRUE(ch.try_send(1));

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        ok.store(co_await ch.send_for(2, 5s)); // parks: buffer full, long timeout
        done.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return parked.load(); }, 200ms)) << "sender never reached send_for()";
    ASSERT_FALSE(done.load());

    ASSERT_EQ(ch.try_recv(), std::optional<int>{1});
    EXPECT_FALSE(ch.try_send(3)) << "the freed slot belongs to the parked send_for";
    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "the woken send_for never completed";
    EXPECT_TRUE(ok.load()) << "a send_for handed its slot reports success";
    EXPECT_EQ(ch.try_recv(), std::optional<int>{2});
    EXPECT_TRUE(ch.empty());
}

TEST_F(ChannelLoopOps, RendezvousSenderWokenByAReceiverHandsItsValueToThatReceiver) {
    // Capacity 0: a parked receiver wakes the parked sender. A try_send running between that wake and the sender's
    // resume used to take the receiver, and the sender then buffered on a capacity-0 channel.
    channel<int>      ch(0);
    std::atomic<bool> sender_parked{false};
    std::atomic<bool> sent{false};
    std::atomic<int>  received{-1};
    std::atomic<int>  interposer_sent{-1}; // 1 = try_send accepted, 0 = refused

    coro_scheduler().spawn([&]() -> task<void> {
        sender_parked.store(true);
        co_await ch.send(7); // no receiver yet: parks
        sent.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return sender_parked.load(); }, 200ms));
    ASSERT_FALSE(sent.load());

    // Queued back to back: the receiver parks (waking the sender), then the interposer runs BEFORE the sender resumes.
    coro_scheduler().spawn([&]() -> task<void> {
        auto v = co_await ch.recv();
        received.store(v ? *v : -2);
    });
    coro_scheduler().spawn([&]() -> task<void> {
        interposer_sent.store(ch.try_send(8) ? 1 : 0);
        co_return;
    });
    ASSERT_TRUE(pump_until([&] { return sent.load() && received.load() != -1 && interposer_sent.load() != -1; }));
    EXPECT_EQ(received.load(), 7) << "the receiver that woke the sender gets the sender's value";
    EXPECT_EQ(interposer_sent.load(), 0) << "no receiver is left for the newcomer on a rendezvous channel";
    EXPECT_EQ(ch.size(), 0u) << "a capacity-0 channel buffered a value";
}

TEST_F(ChannelLoopOps, WokenSenderReclaimedBeforeItResumesHasDeliveredExactlyOnce) {
    // The wake hands the value over, so a sender whose branch is then reclaimed (a when_any it lost in the same
    // drain) has still sent: the value is in the channel once -- neither lost with its frame nor duplicated.
    channel<int>      ch(1);
    std::atomic<bool> done{false};
    ASSERT_TRUE(ch.try_send(1)); // full: the send below parks

    coro_scheduler().spawn([&]() -> task<void> {
        auto sender = [&ch]() -> task<int> {
            co_await ch.send(2); // parks on the full buffer
            co_return 1;
        };
        auto frees_the_slot_then_wins = [&ch]() -> task<int> {
            co_await sleep(0ms);                             // a yield: the sender parks first
            EXPECT_EQ(ch.try_recv(), std::optional<int>{1}); // frees the slot: the parked sender is handed it
            co_return 2;                                     // wins in the same resume: the sender's branch is reclaimed, queued
        };
        auto r = co_await when_any(sender(), frees_the_slot_then_wins());
        EXPECT_EQ(r.index, 1u);
        done.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return done.load(); })) << "the race never resolved";
    EXPECT_EQ(ch.try_recv(), std::optional<int>{2}) << "the value handed over at the wake was lost with the sender";
    EXPECT_FALSE(ch.try_recv().has_value()) << "the value was delivered twice";
}
