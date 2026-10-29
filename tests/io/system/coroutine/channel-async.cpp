/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/channel-async.cpp
 * @brief `qb::io::async::channel<T>` suspending surface — send/recv await, select, timeouts.
 *
 * The system half of the former monolithic test-coroutine-channel.cpp split (the spawn-free
 * synchronous methods live in unit/coroutine/channel-sync-ops.cpp; the destroy-while-parked
 * UAF trio + the make_pipeline raw-pointer regression live in
 * system/coroutine/channel-lifetime.cpp, which MUST run under ASan). Everything here drives
 * the real `coro_scheduler()` on a live libev loop: a coroutine parks on `co_await send`/
 * `recv`, on the `(capacity==0)` rendezvous handshake, on `select(...)`, or on the timed
 * `recv_for`/`send_for` awaiters, and the test advances the loop until it resumes.
 *
 * De-flake: every test gates on a real completion flag through the shared
 * `qb::io::test::pump_until` (loud bounded timeout) — NOT a blind `run_for(Nms)` window
 * racing a `sleep`. A coroutine that never completes fails the `EXPECT_TRUE(pump_until(...))`
 * with a greppable message rather than wedging the runner or passing vacuously.
 *
 * Strengthened over the original suite:
 *   - `MPSCMultipleSenders` now spawns N *concurrent* producer coroutines into a cap-0
 *     channel with a single draining consumer (real multi-producer fan-in), and asserts the
 *     exact received multiset {0..N-1} — the original serialised the sends on one coroutine
 *     and recv'd them on the same coroutine, exercising no concurrency at all.
 *   - `SelectInLoop` asserts the exact multiset {1,2,3}, not just `size()==3`.
 *   - The three near-identical `send_for` backpressure-release variants are collapsed into
 *     one parametrised case.
 *   - Re-homes the unique channel regression cases (value-not-lost-on-buffer-full,
 *     send_for-pushes-on-wake, send_for-timer-guard, close-wakes-pending-senders,
 *     send-on-closed-throws, recv_for/send_for timeout) from the dissolved regression file.
 */

#include <algorithm>
#include <atomic>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"
#include "../../shared/coroutine_reclaim_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;
using qb::io::test::reclaim_fast_winner;
using qb::io::test::run_reclaim_driver;

namespace {

class ChannelAsync : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        // Drain then destroy any frame still parked on a channel awaiter (the unbuffered
        // rendezvous / timed-send tests deliberately leave senders/receivers suspended on
        // a now-out-of-scope local channel). destroy_all_suspended() runs their awaiter
        // destructors so the next test starts from a clean loop with no dangling watchers.
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Buffered + fast-path send/recv
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, BufferedProducerConsumerSumsAllValues) {
    channel<int>      ch(5);
    std::atomic<int>  received{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&ch]() -> task<void> {
        for (int i = 1; i <= 5; ++i)
            co_await ch.send(i);
        ch.close();
    });
    coro_scheduler().spawn([&ch, &received, &done]() -> task<void> {
        while (true) {
            auto val = co_await ch.recv();
            if (!val)
                break;
            received += *val;
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "consumer never drained the channel";
    EXPECT_EQ(received.load(), 15) << "1+2+3+4+5";
}

TEST_F(ChannelAsync, SendFastPathCompletesWithoutSuspending) {
    channel<int>      ch(20);
    std::atomic<int>  sent{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&ch, &sent, &done]() -> task<void> {
        for (int i = 0; i < 10; ++i) {
            co_await ch.send(i); // buffer has space -> await_ready short-circuit
            sent.fetch_add(1);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "producer never finished fast-path sends";
    EXPECT_EQ(sent.load(), 10);
    EXPECT_EQ(ch.size(), 10u);
}

TEST_F(ChannelAsync, RecvFastPathDrainsPrefilledBuffer) {
    channel<int> ch(10);
    for (int i = 0; i < 5; ++i)
        ASSERT_TRUE(ch.try_send(i + 1));

    std::atomic<int>  sum{0};
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&ch, &sum, &done]() -> task<void> {
        for (int i = 0; i < 5; ++i) {
            auto v = co_await ch.recv(); // data present -> await_ready short-circuit
            if (v)
                sum += *v;
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "consumer never drained the prefilled buffer";
    EXPECT_EQ(sum.load(), 15) << "1+2+3+4+5";
}

TEST_F(ChannelAsync, TrySendCopyThenMoveWakesParkedReceiverInOrder) {
    channel<std::string>     ch(0);
    std::vector<std::string> received;
    std::atomic<bool>        done{false};

    coro_scheduler().spawn([&ch, &received, &done]() -> task<void> {
        auto first = co_await ch.recv();
        if (first)
            received.push_back(*first);
        auto second = co_await ch.recv();
        if (second)
            received.push_back(*second);
        done.store(true);
    });

    // Let the receiver park on the empty unbuffered channel.
    EXPECT_TRUE(pump_until([&] { return !received.empty() || ch.try_send(std::string{"copy-path"}); }))
        << "copy try_send never reached a parked receiver";

    std::string moved = "move-path";
    EXPECT_TRUE(pump_until([&] { return received.size() >= 1 && ch.try_send(std::move(moved)); }))
        << "move try_send never reached the parked receiver";

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "receiver never completed both recvs";
    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0], "copy-path");
    EXPECT_EQ(received[1], "move-path");
}

// Regression: on a rendezvous (cap-0) channel, send_for() must deliver to a
// recv_for()/select() waiter that is ALREADY parked. recv_for parks in the
// channel's select list (not _recv_waiters); send_for's fast path used to consult only
// _recv_waiters/buffer, so it took the timed slow path and BOTH sides stayed
// parked until their timeouts elapsed instead of matching immediately. The op
// timeouts (10s) far exceed pump_until's window (2s), so with the bug neither
// side completes in-window and pump_until fails loudly; with the fix they match
// synchronously.
TEST_F(ChannelAsync, SendForMatchesAlreadyParkedRecvForOnRendezvous) {
    channel<int>      ch(0);
    std::atomic<bool> recv_done{false};
    std::atomic<bool> send_done{false};
    std::atomic<bool> send_ok{false};
    std::atomic<int>  received{-1};

    // Receiver parks FIRST (recv_for registers in the channel's select list).
    coro_scheduler().spawn([&]() -> task<void> {
        auto v = co_await ch.recv_for(10s);
        if (v)
            received.store(*v);
        recv_done.store(true);
    });
    // Sender's fast path must see the parked recv_for and hand off synchronously.
    coro_scheduler().spawn([&]() -> task<void> {
        send_ok.store(co_await ch.send_for(42, 10s));
        send_done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return recv_done.load() && send_done.load(); }))
        << "send_for ignored the parked recv_for on a rendezvous channel — both parked (hang)";
    EXPECT_TRUE(send_ok.load()) << "send_for should report success on a rendezvous match";
    EXPECT_EQ(received.load(), 42) << "recv_for should receive the rendezvous value, not time out";
}

// ---------------------------------------------------------------------------
// Awaited send/recv close semantics
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, AwaitedSendToClosedChannelThrows) {
    channel<int> ch(1);
    ch.close();

    std::atomic<bool> done{false};
    std::atomic<bool> threw_closed{false};

    coro_scheduler().spawn([&ch, &done, &threw_closed]() -> task<void> {
        try {
            co_await ch.send(42);
        } catch (channel_closed const &) {
            threw_closed.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "send-to-closed coroutine never finished";
    EXPECT_TRUE(threw_closed.load());
    EXPECT_TRUE(ch.empty());
}

TEST_F(ChannelAsync, CloseWakesPendingUnbufferedSenderWithChannelClosed) {
    channel<int>      ch(0); // unbuffered: send parks until a receiver or close
    std::atomic<bool> caught{false};
    std::atomic<bool> sender_parked{false};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&ch, &caught, &sender_parked, &done]() -> task<void> {
        sender_parked.store(true);
        try {
            co_await ch.send(1); // blocks: no receiver
        } catch (const channel_closed &) {
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return sender_parked.load(); })) << "sender never started";
    EXPECT_FALSE(done.load()) << "sender must still be parked before close()";

    ch.close();

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "close() never woke the parked sender";
    EXPECT_TRUE(caught.load()) << "a parked sender woken by close() must observe channel_closed";
}

TEST_F(ChannelAsync, RecvDrainsBufferedValuesThenReportsCloseAsNullopt) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&done]() -> task<void> {
        channel<int> ch(5);
        co_await ch.send(10);
        co_await ch.send(20);
        ch.close();

        auto v1 = co_await ch.recv();
        auto v2 = co_await ch.recv();
        auto v3 = co_await ch.recv();

        EXPECT_TRUE(v1.has_value());
        if (v1.has_value()) {
            EXPECT_EQ(*v1, 10);
        }
        EXPECT_TRUE(v2.has_value());
        if (v2.has_value()) {
            EXPECT_EQ(*v2, 20);
        }
        EXPECT_FALSE(v3.has_value()) << "after the buffer drains, a closed channel yields nullopt";
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "recv-drains-on-close coroutine never finished";
}

// ---------------------------------------------------------------------------
// Regression: value-not-lost / send_for slow path (re-homed from regression monolith)
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, SendDoesNotLoseValueWhenWokenAfterBufferFull) {
    std::atomic<bool> done{false};
    std::vector<int>  received;

    coro_scheduler().spawn([&]() -> task<void> {
        channel<int> ch(1); // capacity 1 forces the producer to park twice

        coro_scheduler().spawn([](channel<int> *ch) -> task<void> {
            co_await ch->send(10);
            co_await ch->send(20); // parks: buffer full
            co_await ch->send(30); // parks again
            ch->close();
        }(&ch));

        while (true) {
            auto val = co_await ch.recv();
            if (!val)
                break;
            received.push_back(*val);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "producer/consumer never completed";
    EXPECT_EQ(received, (std::vector<int>{10, 20, 30})) << "a sender woken by freed space must still deliver its value";
}

TEST_F(ChannelAsync, SendForPushesValueWhenWokenBeforeTimeout) {
    std::atomic<bool> done{false};
    std::vector<int>  received;

    coro_scheduler().spawn([&]() -> task<void> {
        channel<int> ch(1);
        co_await ch.send(1); // fill the single slot

        coro_scheduler().spawn([](channel<int> *ch) -> task<void> {
            bool ok = co_await ch->send_for(2, 1000ms); // parks until the consumer drains
            EXPECT_TRUE(ok);
            ch->close();
        }(&ch));

        // Drain so the timed sender is woken (well before its timeout).
        auto first = co_await ch.recv();
        if (first)
            received.push_back(*first);
        while (true) {
            auto val = co_await ch.recv();
            if (!val)
                break;
            received.push_back(*val);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "send_for slow-path coroutine never completed";
    EXPECT_EQ(received, (std::vector<int>{1, 2})) << "send_for woken before timeout must push its value";
}

TEST_F(ChannelAsync, SendForTimerGuardPreventsDoubleScheduleCrash) {
    // Both the recv-wake and the timer can race to resume the parked sender; the shared
    // guard flag must let exactly one win. The contract is "no crash, sender resolves once".
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&done]() -> task<void> {
        channel<int> ch(1);
        co_await ch.send(99); // fill buffer
        bool ok = co_await ch.send_for(42, 5ms);
        (void) ok; // timeout OR success — either is fine; the test is that it resolves once
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "send_for never resolved (double-schedule would crash)";
}

// ---------------------------------------------------------------------------
// Unbuffered rendezvous (capacity 0)
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, UnbufferedSenderFirstRendezvous) {
    channel<int>      ch(0);
    std::atomic<bool> sender_done{false};
    std::atomic<bool> receiver_done{false};
    std::atomic<int>  received{-1};
    std::atomic<bool> sender_parked{false};

    coro_scheduler().spawn([&]() -> task<void> {
        sender_parked.store(true);
        co_await ch.send(7); // parks: no receiver yet
        sender_done.store(true);
    });

    // Deterministically force the sender to park BEFORE the receiver arrives.
    EXPECT_TRUE(pump_until([&] { return sender_parked.load(); })) << "sender never started";
    EXPECT_FALSE(sender_done.load()) << "sender must park on the unbuffered channel before a receiver exists";

    coro_scheduler().spawn([&]() -> task<void> {
        auto v = co_await ch.recv();
        if (v)
            received.store(*v);
        receiver_done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return sender_done.load() && receiver_done.load(); })) << "rendezvous never completed";
    EXPECT_EQ(received.load(), 7);
}

TEST_F(ChannelAsync, UnbufferedRecvParkedThenCloseResolvesNullopt) {
    // Receiver parks first on an empty unbuffered channel; the sender closes it instead of
    // sending. The parked recv must resolve to nullopt (channel-closed), not hang.
    channel<int>       ch(0);
    std::atomic<bool>  receiver_parked{false};
    std::atomic<bool>  done{false};
    std::optional<int> got{42};

    coro_scheduler().spawn([&]() -> task<void> {
        receiver_parked.store(true);
        got = co_await ch.recv();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return receiver_parked.load(); })) << "receiver never started";
    EXPECT_FALSE(done.load()) << "receiver must park before close()";

    ch.close();

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "close() never woke the parked receiver";
    EXPECT_FALSE(got.has_value()) << "a recv parked when the channel is closed must resolve to nullopt";
}

// ---------------------------------------------------------------------------
// Real multi-producer fan-in (MPSC)
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, MPSCMultipleConcurrentSendersFanInToOneConsumer) {
    constexpr int     N = 8;
    channel<int>      ch(0); // unbuffered: every producer must rendezvous with the consumer
    std::vector<int>  received;
    std::atomic<bool> consumer_done{false};

    // N concurrent producer coroutines, each sending exactly one distinct value.
    for (int i = 0; i < N; ++i) {
        coro_scheduler().spawn([&ch, i]() -> task<void> { co_await ch.send(i); });
    }
    // Single consumer drains exactly N values, then the channel is closed to end the loop.
    coro_scheduler().spawn([&]() -> task<void> {
        for (int i = 0; i < N; ++i) {
            auto v = co_await ch.recv();
            if (v)
                received.push_back(*v);
        }
        consumer_done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return consumer_done.load(); })) << "consumer never received all N producer values";

    ASSERT_EQ(received.size(), static_cast<size_t>(N));
    std::sort(received.begin(), received.end());
    std::vector<int> expected(N);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(received, expected) << "every concurrent producer's value must arrive exactly once";
}

// ---------------------------------------------------------------------------
// make_pipeline / transform / filter / collect composition
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, MakePipelineTransformsAndPropagatesClose) {
    auto [in, out] = make_pipeline<int, int>([](int v) { return v * 10; }, 8);

    std::atomic<int>  sum{0};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([in = in.get()]() -> task<void> {
        for (int i = 1; i <= 5; ++i)
            co_await in->send(i);
        in->close();
    });
    coro_scheduler().spawn([out = out.get(), &sum, &done]() -> task<void> {
        while (true) {
            auto v = co_await out->recv();
            if (!v)
                break;
            sum += *v;
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "pipeline consumer never finished";
    EXPECT_EQ(sum.load(), (1 + 2 + 3 + 4 + 5) * 10) << "pipeline must transform every value";
    EXPECT_TRUE(out->is_closed()) << "closing the input must propagate close to the output";
}

// Regression: a make_pipeline transform that throws must still close `out` so the
// consumer wakes (end-of-stream) instead of hanging forever. Previously the
// exception propagated out of pipeline_worker and out->close() was skipped.
TEST_F(ChannelAsync, MakePipelineThrowingTransformClosesOutputInsteadOfHanging) {
    auto [in, out] = make_pipeline<int, int>(
        [](int v) -> int {
            if (v == 3)
                throw std::runtime_error("transform boom");
            return v * 10;
        },
        8);

    std::atomic<bool> done{false};

    coro_scheduler().spawn([in = in.get()]() -> task<void> {
        for (int i = 1; i <= 5; ++i)
            co_await in->send(i);
        in->close();
    });
    coro_scheduler().spawn([out = out.get(), &done]() -> task<void> {
        while (auto v = co_await out->recv()) {
            // drain whatever the transform produced before it threw
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "pipeline consumer hung after the transform threw — out was never closed";
    EXPECT_TRUE(out->is_closed()) << "a throwing transform must still close the output channel";
}

TEST_F(ChannelAsync, MakePipelineTypeChangingTransform) {
    auto [in, out] = make_pipeline<int, std::string>([](int v) { return "item" + std::to_string(v); }, 4);

    std::vector<std::string> received;
    std::atomic<bool>        done{false};

    coro_scheduler().spawn([in = in.get()]() -> task<void> {
        for (int i = 0; i < 3; ++i)
            co_await in->send(i);
        in->close();
    });
    coro_scheduler().spawn([out = out.get(), &received, &done]() -> task<void> {
        while (true) {
            auto v = co_await out->recv();
            if (!v)
                break;
            received.push_back(*v);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "type-changing pipeline never finished";
    EXPECT_EQ(received, (std::vector<std::string>{"item0", "item1", "item2"}));
}

TEST_F(ChannelAsync, TransformFilterCollectComposeAcrossChannels) {
    channel<int> source(4);
    channel<int> doubled(4);
    channel<int> filtered(4);

    std::vector<int>  collected;
    std::atomic<bool> done{false};

    coro_scheduler().spawn(transform<int, int>(source, doubled, [](int v) { return v * 2; }));
    coro_scheduler().spawn(filter<int>(doubled, filtered, [](int v) { return v >= 6; }));
    coro_scheduler().spawn([&]() -> task<void> {
        collected = co_await collect(filtered);
        done.store(true);
    });
    coro_scheduler().spawn([&]() -> task<void> {
        for (int v : {1, 2, 3, 4})
            co_await source.send(v);
        source.close();
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "transform/filter/collect pipeline never finished";
    EXPECT_EQ(collected, (std::vector<int>{6, 8})) << "filter(>=6) of {2,4,6,8} keeps {6,8}";
    EXPECT_TRUE(doubled.is_closed());
    EXPECT_TRUE(filtered.is_closed());
}

TEST_F(ChannelAsync, CollectReturnsBufferedValuesOnlyAfterClose) {
    channel<std::string>     ch(4);
    std::vector<std::string> collected;
    std::atomic<bool>        done{false};

    ASSERT_TRUE(ch.try_send("alpha"));
    ASSERT_TRUE(ch.try_send("beta"));

    coro_scheduler().spawn([&]() -> task<void> {
        collected = co_await collect(ch);
        done.store(true);
    });

    // collect must NOT return while the channel is still open with buffered data.
    qb::io::async::run_for(10ms);
    EXPECT_FALSE(done.load()) << "collect must block until the channel is closed";

    ch.close();
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "collect never returned after close";
    EXPECT_EQ(collected, (std::vector<std::string>{"alpha", "beta"}));
}

// ---------------------------------------------------------------------------
// select()
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, SelectFastPathReturnsBufferedValue) {
    channel<int>      ch_a(4), ch_b(4);
    select_result     result;
    std::atomic<bool> done{false};

    ASSERT_TRUE(ch_a.try_send(42));

    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await select(ch_a, ch_b);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "select never resolved the buffered value";
    EXPECT_EQ(result.index, 0u);
    EXPECT_EQ(result.get<int>(), 42);
}

TEST_F(ChannelAsync, SelectSlowPathFirstSenderWins) {
    channel<int>      ch_a(1), ch_b(1);
    select_result     result;
    std::atomic<bool> done{false};

    const qb::io::test::coroutine_scope_cleanup cleanup;
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await select(ch_a, ch_b);
        done.store(true);
    });
    // ch_b's sender is given a head start so it is the deterministic first to deliver.
    coro_scheduler().spawn([&ch_b]() -> task<void> {
        co_await sleep(5ms);
        co_await ch_b.send(99);
    });
    coro_scheduler().spawn([&ch_a]() -> task<void> {
        co_await sleep(60ms);
        co_await ch_a.send(77);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "select never resolved a sender";
    EXPECT_EQ(result.index, 1u) << "ch_b delivered first";
    EXPECT_EQ(result.get<int>(), 99);
}

TEST_F(ChannelAsync, SelectResolvesClosedChannelAsClosed) {
    channel<int>      ch_a(1), ch_b(1);
    select_result     result;
    std::atomic<bool> done{false};

    ch_a.close();

    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await select(ch_a, ch_b);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "select never resolved the closed channel";
    EXPECT_EQ(result.index, 0u);
    EXPECT_TRUE(result.closed);
}

TEST_F(ChannelAsync, SelectResolvedOnOneChannelRetractsItsEntryFromTheOther) {
    channel<int>       ch_a(0), ch_b(0);
    select_result      result;
    std::atomic<bool>  select_done{false};
    std::atomic<bool>  stale_sender_done{false};
    std::atomic<bool>  receiver_done{false};
    std::optional<int> received;

    // A select() parks on both channels; ch_b's send resolves it. Since Huly QB-273 the resumed select
    // retracts its entry from ch_a at once (before, a stale entry stayed there for a LATER ch_a.send to
    // skip): either way that later send must park and hand its value to a real receiver, never to the
    // select that is gone.
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await select(ch_a, ch_b);
        select_done.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return ch_b.empty(); })); // let select park on both

    coro_scheduler().spawn([&]() -> task<void> { co_await ch_b.send(99); });
    EXPECT_TRUE(pump_until([&] { return select_done.load(); })) << "select never resolved on ch_b";
    EXPECT_EQ(result.index, 1u);
    EXPECT_EQ(result.get<int>(), 99);
    EXPECT_EQ(ch_a.waiter_census().selects, 0u) << "the losing channel must not keep the resolved select's entry (Huly QB-273)";

    // This sender has no select to be satisfied by — it parks.
    coro_scheduler().spawn([&]() -> task<void> {
        co_await ch_a.send(77);
        stale_sender_done.store(true);
    });
    qb::io::async::run_for(20ms);
    EXPECT_FALSE(stale_sender_done.load()) << "the stale select waiter must NOT consume the later send";

    // A real receiver finally takes the value.
    coro_scheduler().spawn([&]() -> task<void> {
        received = co_await ch_a.recv();
        receiver_done.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return stale_sender_done.load() && receiver_done.load(); })) << "the later send never reached a real receiver";
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, 77);
}

// A select resolved by one channel keeps its node on the other until its awaiter goes (Huly QB-273): a send on that
// other channel before the select resumes must find no taker. The skip of a resolved node in the hand-off is what
// refuses it -- without it the send "delivered" its value to the resolved state, where resolve() is a no-op, and
// reported success: the value was dropped. A NAMED awaiter holds the node past the turn, as long as it lives.
TEST_F(ChannelAsync, SelectResolvedByOneChannelRefusesASendOnTheOtherBeforeItsAwaiterEnds) {
    channel<int>       ch_a(0), ch_b(0);
    select_result      result;
    std::atomic<bool>  select_done{false};
    std::atomic<bool>  sends_done{false};
    bool               b_took = false, a_took = true;
    std::size_t        a_nodes_between = 99;
    std::optional<int> received;
    std::atomic<bool>  receiver_done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await select(ch_a, ch_b);
        select_done.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return ch_a.waiter_census().selects == 1 && ch_b.waiter_census().selects == 1; }))
        << "the select never parked on both channels";

    // One coroutine body, no yield between the two sends: the select is resolved and queued, not yet resumed.
    coro_scheduler().spawn([&]() -> task<void> {
        b_took          = ch_b.try_send(1); // resolves the select through ch_b
        a_nodes_between = ch_a.waiter_census().selects;
        a_took          = ch_a.try_send(2); // ch_a still holds the resolved node: it must not take the value
        sends_done.store(true);
        co_return;
    });
    EXPECT_TRUE(pump_until([&] { return sends_done.load() && select_done.load(); })) << "the sends or the select never ran";
    EXPECT_TRUE(b_took);
    EXPECT_EQ(a_nodes_between, 1u) << "the transient the census documents: the losing channel holds the resolved node";
    EXPECT_FALSE(a_took) << "a send on the losing channel was reported delivered to a select already resolved: dropped";
    EXPECT_EQ(result.index, 1u);
    EXPECT_EQ(result.get<int>(), 1);
    EXPECT_EQ(ch_a.waiter_census().selects, 0u) << "the node goes with the awaiter";

    // The value the refused send kept reaches a real receiver.
    coro_scheduler().spawn([&]() -> task<void> {
        received = co_await ch_a.recv();
        receiver_done.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return ch_a.waiter_census().receivers == 1; })) << "the receiver never parked";
    EXPECT_TRUE(ch_a.try_send(2));
    EXPECT_TRUE(pump_until([&] { return receiver_done.load(); })) << "the receiver never resumed";
    EXPECT_EQ(received.value_or(-1), 2);

    // A named awaiter: the resolved node stays on the losing channel while the awaiter lives, past its turn.
    std::atomic<bool> named_resumed{false}, named_released{false};
    std::size_t       a_nodes_after_resume = 99;
    bool              a_took_after_resume  = true;
    coro_scheduler().spawn([&]() -> task<void> {
        {
            auto s = select(ch_a, ch_b);
            auto r = co_await s;
            named_resumed.store(r.index == 1u && r.get<int>() == 3);
            co_await sleep(5ms); // a later turn, the awaiter still alive
            a_nodes_after_resume = ch_a.waiter_census().selects;
            a_took_after_resume  = ch_a.try_send(4);
        }
        named_released.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return ch_b.waiter_census().selects == 1; })) << "the named select never parked";
    EXPECT_TRUE(ch_b.try_send(3));
    EXPECT_TRUE(pump_until([&] { return named_released.load(); })) << "the named select never ended";
    EXPECT_TRUE(named_resumed.load());
    EXPECT_EQ(a_nodes_after_resume, 1u) << "a named awaiter keeps its resolved node on the losing channel while it lives";
    EXPECT_FALSE(a_took_after_resume) << "and that node must still refuse a send";
    EXPECT_EQ(ch_a.waiter_census().selects, 0u) << "the awaiter's end retracts it";
}

TEST_F(ChannelAsync, VectorSelectPicksFirstReady) {
    channel<int>                ch0(1), ch1(1), ch2(1);
    std::vector<channel<int> *> channels = {&ch0, &ch1, &ch2};
    select_result               result;
    std::atomic<bool>           done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await select(channels);
        done.store(true);
    });
    coro_scheduler().spawn([&ch2]() -> task<void> {
        co_await sleep(5ms);
        co_await ch2.send(7);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "vector select never resolved";
    EXPECT_EQ(result.index, 2u);
    EXPECT_EQ(result.get<int>(), 7);
}

TEST_F(ChannelAsync, VectorSelectFastPathPrefersBufferedDataOverClosedChannel) {
    channel<int> ch0(1), ch1(1), ch2(1);
    ch0.close();
    ASSERT_TRUE(ch1.try_send(77));

    std::vector<channel<int> *> channels = {&ch0, &ch1, &ch2};
    select_result               result;
    std::atomic<bool>           done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await select(channels);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "vector select never resolved";
    EXPECT_FALSE(result.closed) << "buffered data must win over a closed channel (avoid starvation)";
    EXPECT_EQ(result.index, 1u);
    EXPECT_EQ(result.get<int>(), 77);
}

TEST_F(ChannelAsync, VectorSelectFastPathReportsClosedEmptyChannel) {
    channel<int> ch0(1), ch1(1);
    ch1.close();

    std::vector<channel<int> *> channels = {&ch0, &ch1};
    select_result               result;
    std::atomic<bool>           done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await select(channels);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "vector select never resolved";
    EXPECT_TRUE(result.closed);
    EXPECT_EQ(result.index, 1u);
    EXPECT_FALSE(result.value.has_value());
}

TEST_F(ChannelAsync, SelectFirstReadyHeterogeneousChannels) {
    // Re-homed regression: select over channels of different value types; the fast int
    // channel wins, then the slow producer is drained so its raw-pointer-captured channel
    // is not used-after-free (the producer outlives the select).
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&done]() -> task<void> {
        channel<int>         ch_fast(1);
        channel<std::string> ch_slow(1);

        coro_scheduler().spawn([](channel<int> *ch) -> task<void> {
            co_await sleep(5ms);
            co_await ch->send(42);
        }(&ch_fast));
        coro_scheduler().spawn([](channel<std::string> *ch) -> task<void> {
            co_await sleep(100ms);
            co_await ch->send(std::string("slow"));
        }(&ch_slow));

        auto result = co_await select(ch_fast, ch_slow);
        EXPECT_EQ(result.index, 0u);
        EXPECT_EQ(result.get<int>(), 42);

        // Keep ch_slow alive until its producer finishes (otherwise its later send would
        // dereference this frame's freed local — caught under ASan).
        auto slow = co_await ch_slow.recv();
        EXPECT_EQ(slow, "slow");
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "heterogeneous select never finished";
}

TEST_F(ChannelAsync, SelectInLoopDrainsExactMultiset) {
    channel<int>      ch_a(4), ch_b(4);
    std::vector<int>  received;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&ch_a, &ch_b]() -> task<void> {
        co_await ch_a.send(1);
        co_await ch_b.send(2);
        co_await ch_a.send(3);
        ch_a.close();
        ch_b.close();
    });

    // select() while both are open; once one closes, drain the other directly. This avoids
    // the deterministic left-to-right bias of select repeatedly picking the same closed
    // channel.
    coro_scheduler().spawn([&]() -> task<void> {
        bool a_open = true, b_open = true;
        while (a_open || b_open) {
            if (a_open && b_open) {
                auto res = co_await select(ch_a, ch_b);
                if (res.closed)
                    (res.index == 0 ? a_open : b_open) = false;
                else
                    received.push_back(res.get<int>());
            } else if (a_open) {
                auto v = co_await ch_a.recv();
                if (!v)
                    a_open = false;
                else
                    received.push_back(*v);
            } else {
                auto v = co_await ch_b.recv();
                if (!v)
                    b_open = false;
                else
                    received.push_back(*v);
            }
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "select-in-loop never drained both channels";
    std::sort(received.begin(), received.end());
    EXPECT_EQ(received, (std::vector<int>{1, 2, 3})) << "every produced value must be received exactly once";
}

// ---------------------------------------------------------------------------
// recv_for / send_for timeouts
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, RecvForReturnsValueArrivingBeforeTimeout) {
    channel<int>       ch(1);
    std::optional<int> result;
    std::atomic<bool>  done{false};

    coro_scheduler().spawn([&ch]() -> task<void> {
        co_await sleep(10ms);
        co_await ch.send(42);
    });
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await ch.recv_for(1000ms);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "recv_for never resolved";
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST_F(ChannelAsync, RecvForTimesOutWhenNoValueArrives) {
    channel<int>       ch(1);
    std::optional<int> result{999};
    std::atomic<bool>  done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await ch.recv_for(20ms);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "recv_for never timed out";
    EXPECT_FALSE(result.has_value()) << "a recv_for with no sender must resolve to nullopt on timeout";
}

TEST_F(ChannelAsync, RecvForBufferedFastPath) {
    channel<int> ch(4);
    ASSERT_TRUE(ch.try_send(55));
    std::optional<int> result;
    std::atomic<bool>  done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await ch.recv_for(50ms);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "recv_for fast path never resolved";
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 55);
}

TEST_F(ChannelAsync, RecvForClosedChannelReturnsNullopt) {
    channel<int> ch(1);
    ch.close();
    std::optional<int> result{999};
    std::atomic<bool>  done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await ch.recv_for(50ms);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "recv_for on closed channel never resolved";
    EXPECT_FALSE(result.has_value());
}

TEST_F(ChannelAsync, SendForClosedChannelReturnsFalseWithoutParking) {
    channel<int> ch(1);
    ch.close();
    std::atomic<bool> done{false};
    std::atomic<bool> sent{true};

    coro_scheduler().spawn([&]() -> task<void> {
        sent.store(co_await ch.send_for(1, 100ms));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "send_for on closed channel never resolved";
    EXPECT_FALSE(sent.load());
    EXPECT_TRUE(ch.empty());
}

TEST_F(ChannelAsync, SendForSucceedsImmediatelyWhenSpaceAvailable) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&done]() -> task<void> {
        channel<int> ch(1);
        bool         sent = co_await ch.send_for(42, 100ms);
        EXPECT_TRUE(sent);
        auto v = ch.try_recv();
        EXPECT_TRUE(v.has_value());
        if (v.has_value()) {
            EXPECT_EQ(*v, 42);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "send_for success path never resolved";
}

TEST_F(ChannelAsync, SendForTimesOutWhenBufferStaysFull) {
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&done]() -> task<void> {
        channel<int> ch(1);
        co_await ch.send(1); // fill — no consumer ever drains
        bool ok = co_await ch.send_for(2, 30ms);
        EXPECT_FALSE(ok);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "send_for never timed out";
}

TEST_F(ChannelAsync, SendForResumesWhenBackpressureReleased) {
    // Collapses the three near-identical backpressure-release variants from the original
    // file into one: a timed sender parks on a full cap-1 channel, a drain frees the slot,
    // and the sender must resume and deliver its value (before timeout).
    channel<int> ch(1);
    ASSERT_TRUE(ch.try_send(1));

    std::atomic<bool> sender_done{false};
    std::atomic<bool> sent{false};

    coro_scheduler().spawn([&]() -> task<void> {
        sent.store(co_await ch.send_for(2, 1000ms));
        sender_done.store(true);
    });

    qb::io::async::run_for(10ms);
    EXPECT_FALSE(sender_done.load()) << "sender must park while the buffer is full";

    auto first = ch.try_recv(); // release backpressure
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 1);

    EXPECT_TRUE(pump_until([&] { return sender_done.load(); })) << "send_for never resumed after backpressure release";
    EXPECT_TRUE(sent.load());
    auto second = ch.try_recv();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2);
}

TEST_F(ChannelAsync, SendForTimeoutRetractsItsEntryAndALaterSendTakesTheSlot) {
    channel<int> ch(1);
    ASSERT_TRUE(ch.try_send(1));

    std::atomic<bool> timed_sender_done{false};
    std::atomic<bool> timed_sender_sent{true};
    std::atomic<bool> second_sender_done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        timed_sender_sent.store(co_await ch.send_for(2, 20ms));
        timed_sender_done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return timed_sender_done.load(); })) << "timed send never resolved";
    EXPECT_FALSE(timed_sender_sent.load()) << "send_for must time out while the buffer stays full";
    EXPECT_EQ(ch.waiter_census().senders, 0u) << "a timed-out send_for retracts its entry as its watchdog fires (Huly QB-273)";

    auto first = ch.try_recv();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 1);

    // A fresh send must be served by the freed slot. Since Huly QB-273 the timed-out sender took its
    // entry with it (before, the entry stayed, its guard set, for a later wake to discard).
    coro_scheduler().spawn([&]() -> task<void> { second_sender_done.store(co_await ch.send_for(3, 1000ms)); });
    EXPECT_TRUE(pump_until([&] { return second_sender_done.load(); })) << "second send never resolved";

    auto second = ch.try_recv();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 3) << "only the live sender's value remains";
}

// ---------------------------------------------------------------------------
// Rendezvous (capacity 0): a select()/recv_for() that registers AFTER a sender has
// already parked must wake that sender and observe its value — recv()/try_recv() did,
// but select()/recv_for() previously never saw the pending rendezvous value.
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, RendezvousRecvForWakesParkedSender) {
    channel<int>      ch(0); // rendezvous
    int               got = -777;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await ch.send(42); // parks: no receiver yet
    });
    coro_scheduler().spawn([&]() -> task<void> {
        co_await sleep(10ms); // let the sender park first
        auto v = co_await ch.recv_for(200ms);
        got    = v ? *v : -1;
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "recv_for never completed";
    EXPECT_EQ(got, 42) << "recv_for did not receive the parked rendezvous sender's value";
}

TEST_F(ChannelAsync, RendezvousSelectWakesParkedSender) {
    channel<int>      ch(0); // rendezvous
    int               got = -777;
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await ch.send(7); // parks: no receiver yet
    });
    coro_scheduler().spawn([&]() -> task<void> {
        co_await sleep(10ms);
        auto result = co_await select(ch); // single-channel select over the rendezvous channel
        got         = std::any_cast<int>(result.value);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "select never completed";
    EXPECT_EQ(got, 7) << "select did not receive the parked rendezvous sender's value";
}

// ---------------------------------------------------------------------------
// Same rendezvous wake-up, but the parked sender is a TIMED send_for(). recv_for()/select()
// wakes it via wake_one_sender(); timed_send_awaiter::await_resume must hand the value off to
// the select/recv_for waiter (not silently buffer it). Pre-fix the value was buffered, the
// sender reported success, and the receiver timed out → a lost rendezvous message.
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, RendezvousRecvForWakesParkedTimedSender) {
    channel<int>      ch(0); // rendezvous
    int               got = -777;
    std::atomic<bool> sent{false};
    std::atomic<bool> sender_done{false};
    std::atomic<bool> recv_done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        sent.store(co_await ch.send_for(42, 1000ms)); // parks: no receiver yet
        sender_done.store(true);
    });
    coro_scheduler().spawn([&]() -> task<void> {
        co_await sleep(10ms); // let the timed sender park first
        auto v = co_await ch.recv_for(1000ms);
        got    = v ? *v : -1;
        recv_done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return recv_done.load() && sender_done.load(); })) << "recv_for/send_for never completed";
    EXPECT_EQ(got, 42) << "recv_for did not receive the parked timed sender's value (lost message)";
    EXPECT_TRUE(sent.load()) << "send_for must report success after the value was delivered";
    EXPECT_TRUE(ch.empty()) << "the rendezvous value must not be left in the buffer";
}

TEST_F(ChannelAsync, RendezvousSelectWakesParkedTimedSender) {
    channel<int>      ch(0); // rendezvous
    int               got = -777;
    std::atomic<bool> sent{false};
    std::atomic<bool> sender_done{false};
    std::atomic<bool> recv_done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        sent.store(co_await ch.send_for(7, 1000ms)); // parks: no receiver yet
        sender_done.store(true);
    });
    coro_scheduler().spawn([&]() -> task<void> {
        co_await sleep(10ms);
        auto result = co_await select(ch); // single-channel select over the rendezvous channel
        got         = std::any_cast<int>(result.value);
        recv_done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return recv_done.load() && sender_done.load(); })) << "select/send_for never completed";
    EXPECT_EQ(got, 7) << "select did not receive the parked timed sender's value (lost message)";
    EXPECT_TRUE(sent.load()) << "send_for must report success after the value was delivered";
    EXPECT_TRUE(ch.empty()) << "the rendezvous value must not be left in the buffer";
}

// ---------------------------------------------------------------------------
// Destroy-while-parked, DEPTH-2: a recv() waiter nested one level below a when_any branch, WOKEN
// (by a sender) in the SAME drain its subtree is reclaimed. The winner sends then completes in one
// resume, so the depth-2 recv waiter is queued (schedule_via_current) exactly when the loser branch
// is reclaimed. The deeper waiter is freed via the task<T> dtor cascade, which must forget() it from
// the scheduler (task.h forget_frame_if_current) — else its queued handle dangles in ready_queue_ →
// heap-UAF in run_ready(). Frame >4 KiB so it escapes the coroutine frame pool and ASan sees it.
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, RecvDepth2WaiterWokenThenReclaimedNoUAF) {
    run_reclaim_driver([]() -> task<void> {
        auto ch    = std::make_shared<channel<int>>(0); // rendezvous: recv parks with no sender
        auto inner = [](std::shared_ptr<channel<int>> c) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            auto v = co_await c->recv(); // parks; woken by the winner's try_send()
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return v ? *v : -1;
        };
        auto outer = [inner](std::shared_ptr<channel<int>> c) -> task<int> {
            co_return co_await inner(c); // depth-2: outer -> inner -> channel.recv
        };
        auto winner = [](std::shared_ptr<channel<int>> c) -> task<int> {
            co_await sleep(5ms);    // let the depth-2 receiver park first
            (void) c->try_send(42); // delivers to the parked receiver -> schedules it (queued)
            co_return 99;           // -> when_any reclaim destroys the (queued) receiver -> would dangle
        };
        auto r = co_await when_any(outer(ch), winner(ch));
        EXPECT_EQ(r.index, 1u) << "the sending winner must win the race";
        co_await sleep(20ms);
    });
}

// ---------------------------------------------------------------------------
// Wake-token-loss for channel recv(): a sender hands its value into the parked receiver's _result
// and pops it from _recv_waiters (try_send / send_awaiter). If that receiver is reclaimed before it
// resumes (a when_any/race loser, woken-then-reclaimed), the value is destroyed with the frame — a
// LOST MESSAGE, even though the sender reported success. The recv_awaiter dtor must re-buffer the
// unconsumed value so the next receiver still gets it (mirrors the sync-primitive wake-token return).
// Deterministic: the winner sends then completes in one resume, so the value is handed to the
// depth-2 receiver exactly when its branch is reclaimed.
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, RecvWokenThenReclaimedDoesNotLoseValue) {
    int  recovered = -1;
    bool had_value = false;
    run_reclaim_driver([&recovered, &had_value]() -> task<void> {
        auto ch    = std::make_shared<channel<int>>(1); // buffered: try_send hands directly to a waiter
        auto inner = [](std::shared_ptr<channel<int>> c) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            auto v = co_await c->recv(); // value handed by the winner's try_send(), then reclaimed
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return v ? *v : -1;
        };
        auto outer = [inner](std::shared_ptr<channel<int>> c) -> task<int> {
            co_return co_await inner(c);
        };
        auto winner = [](std::shared_ptr<channel<int>> c) -> task<int> {
            co_await sleep(5ms);
            (void) c->try_send(42); // hands 42 into the parked receiver's _result + pops it
            co_return 99;           // -> when_any reclaim destroys the receiver holding 42
        };
        auto r = co_await when_any(outer(ch), winner(ch));
        EXPECT_EQ(r.index, 1u);
        co_await sleep(20ms);
        // 42 was handed to the reclaimed receiver. Without re-buffering it is lost; with the fix the
        // dtor returns it to the channel and the next receiver gets it.
        auto again = co_await ch->recv_for(100ms);
        had_value  = again.has_value();
        recovered  = again.value_or(-1);
    });
    EXPECT_TRUE(had_value) << "value handed to a reclaimed receiver was LOST (wake token lost)";
    EXPECT_EQ(recovered, 42);
}

// recv_for() variant of the above: a sender RESOLVES the timed receiver's shared select-state with a
// value; if that recv_for is reclaimed before consuming it, timed_recv_awaiter must re-buffer the
// value (mirrors recv_awaiter) so the message is not lost.
TEST_F(ChannelAsync, RecvForWokenThenReclaimedDoesNotLoseValue) {
    int  recovered = -1;
    bool had_value = false;
    run_reclaim_driver([&recovered, &had_value]() -> task<void> {
        auto ch    = std::make_shared<channel<int>>(0); // rendezvous
        auto inner = [](std::shared_ptr<channel<int>> c) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            auto v = co_await c->recv_for(1000ms); // resolved by the parked sender, then reclaimed
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return v ? *v : -1;
        };
        auto outer = [inner](std::shared_ptr<channel<int>> c) -> task<int> {
            co_return co_await inner(c);
        };
        auto winner = [](std::shared_ptr<channel<int>> c) -> task<int> {
            co_await sleep(5ms);  // let recv_for register its select-waiter first
            co_await c->send(42); // send_awaiter delivers to the recv_for select-waiter (resolve), synchronously
            co_return 99;         // -> when_any reclaim destroys the resolved-but-unconsumed recv_for
        };
        auto r = co_await when_any(outer(ch), winner(ch));
        EXPECT_EQ(r.index, 1u);
        co_await sleep(20ms);
        auto again = co_await ch->recv_for(100ms);
        had_value  = again.has_value();
        recovered  = again.value_or(-1);
    });
    EXPECT_TRUE(had_value) << "value resolved into a reclaimed recv_for was LOST (wake token lost)";
    EXPECT_EQ(recovered, 42);
}

// A value given back by a reclaimed receiver goes through the one hand-off (Huly QB-273): R1 is handed 42, R2 parks
// behind it, R1's frame is reclaimed before it resumes -- R2 receives 42, and a capacity-0 channel's buffer stays
// empty. Before, the destructor pushed the value straight into the buffer: R2 stayed parked (a lost wake) and a
// rendezvous channel held a value. The recv() form, then the recv_for() form.
TEST_F(ChannelAsync, ReclaimedRecvHandsItsValueToTheReceiverParkedBehindIt) {
    std::optional<int> r2_got;
    std::size_t        buffered = 99;
    run_reclaim_driver([&r2_got, &buffered]() -> task<void> {
        auto ch    = std::make_shared<channel<int>>(0);
        auto inner = [](std::shared_ptr<channel<int>> c) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            auto v = co_await c->recv(); // R1: first in line, handed 42, then reclaimed
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return v ? *v : -1;
        };
        auto winner = [&r2_got](std::shared_ptr<channel<int>> c) -> task<int> {
            co_await sleep(5ms);                                                                   // R1 parks first
            coro_scheduler().spawn([c, &r2_got]() -> task<void> { r2_got = co_await c->recv(); }); // R2, behind R1
            co_await sleep(0ms);                                                                   // R2 parks
            (void) c->try_send(42);                                                                // hands 42 to R1 (FIFO), R2 stays parked
            co_return 99; // -> when_any reclaims R1, which gives 42 back
        };
        auto r = co_await when_any(inner(ch), winner(ch));
        EXPECT_EQ(r.index, 1u);
        co_await sleep(20ms);
        buffered = ch->size();
        ch->close(); // ends R2 if it is still parked (the defect)
    });
    EXPECT_EQ(r2_got.value_or(-1), 42) << "the value a reclaimed receiver gave back skipped the receiver parked behind it";
    EXPECT_EQ(buffered, 0u) << "a capacity-0 channel never buffers";
}

TEST_F(ChannelAsync, ReclaimedRecvForHandsItsValueToTheTimedReceiverParkedBehindIt) {
    std::optional<int> r2_got;
    std::size_t        buffered = 99;
    run_reclaim_driver([&r2_got, &buffered]() -> task<void> {
        auto ch    = std::make_shared<channel<int>>(0);
        auto inner = [](std::shared_ptr<channel<int>> c) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            auto v = co_await c->recv_for(1000ms); // R1: first select registration, resolved with 42, then reclaimed
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return v ? *v : -1;
        };
        auto winner = [&r2_got](std::shared_ptr<channel<int>> c) -> task<int> {
            co_await sleep(5ms);                                                                             // R1 parks first
            coro_scheduler().spawn([c, &r2_got]() -> task<void> { r2_got = co_await c->recv_for(1000ms); }); // R2
            co_await sleep(0ms);                                                                             // R2 registers behind R1
            (void) c->try_send(42); // resolves R1 (FIFO), R2 stays parked
            co_return 99;
        };
        auto r = co_await when_any(inner(ch), winner(ch));
        EXPECT_EQ(r.index, 1u);
        co_await sleep(20ms);
        buffered = ch->size();
        ch->close(); // ends R2 now rather than at its deadline if it is still parked (the defect)
    });
    EXPECT_EQ(r2_got.value_or(-1), 42) << "the value a reclaimed recv_for gave back skipped the recv_for parked behind it";
    EXPECT_EQ(buffered, 0u) << "a capacity-0 channel never buffers";
}

// recv_for() parked and reclaimed while UNRESOLVED (no sender, no timeout yet): the timed_recv_awaiter
// dtor must stop its watchdog and retract its registration (Huly QB-273; it used to mark a shared state
// resolved and leave a detached timer to no-op), so nothing schedules the freed frame. Complements the
// resolved-then-reclaimed test above (which drives the re-buffer branch); this drives the not-resolved
// teardown branch.
TEST_F(ChannelAsync, RecvForReclaimedWhileParkedUnresolved) {
    run_reclaim_driver([]() -> task<void> {
        auto ch   = std::make_shared<channel<int>>(0); // rendezvous, no sender → recv_for parks unresolved
        auto park = [](std::shared_ptr<channel<int>> c) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            auto v = co_await c->recv_for(1000ms); // parks; never resolved (reclaimed first)
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return v ? *v : -1;
        };
        auto r = co_await when_any(park(ch), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        co_await sleep(20ms); // the recv_for frame was reclaimed unresolved; its watchdog went with it
        EXPECT_EQ(ch->waiter_census().selects, 0u) << "the reclaimed recv_for must retract its registration (Huly QB-273)";
    });
    // The recv_for frame is a small pooled one: ASan cannot see a watchdog left in it, the loop can.
    qb::io::async::run_for(5ms);
    EXPECT_FALSE(qb::io::async::listener::current.has_work()) << "the reclaimed recv_for's 1000 ms watchdog outlived its frame";
}

// A sender parked on a FULL buffer, woken when a recv frees space with no other receiver/select
// waiting, must buffer its value in await_resume (the woken-then-buffer branch) so the next recv
// gets it. Drives send_awaiter::await_resume's buffer hand-off path.
TEST_F(ChannelAsync, SendWokenBuffersValueWhenSpaceFreed) {
    std::atomic<bool> done{false};
    std::atomic<int>  combined{-1};

    coro_scheduler().spawn([&]() -> task<void> {
        auto ch = std::make_shared<channel<int>>(1); // capacity 1
        co_await ch->send(10);                       // buffers 10 → buffer full

        // A second sender parks (buffer full, no receiver waiting).
        auto sender = [](std::shared_ptr<channel<int>> c) -> task<void> {
            co_await c->send(20); // parks; woken when space frees → await_resume buffers 20
        };
        coro_scheduler().spawn(sender(ch));
        co_await sleep(10ms); // let the second sender park

        auto first = co_await ch->recv();  // drains 10 → frees space → wakes the parked sender
        co_await sleep(10ms);              // let the woken sender's await_resume buffer 20
        auto second = co_await ch->recv(); // gets 20 (buffered by the woken sender)

        combined.store(first.value_or(-1) * 100 + second.value_or(-1));
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "send-woken-buffers coordinator never finished";
    EXPECT_EQ(combined.load(), 10 * 100 + 20) << "a woken sender must buffer its value (await_resume buffer hand-off)";
}

// ---------------------------------------------------------------------------
// Huly QB-273: an entry never outlives its waiter, a watchdog never outlives its wait.
//
// Before: a timed-out recv_for / send_for, a select another channel won and a wait reclaimed while
// parked left their entry in the channel's ring until a later send met and skipped it -- an unbounded
// history on a live, quiet channel, scanned by every send_for's fast-path decision -- and the detached
// timer coroutine of a wait won early lived to its deadline with its ev_timer armed. Now the awaiter owns
// its state and its watchdog, retracts its entry on timeout and in its destructor, and stops the watchdog
// when the wait ends. Two observables: `waiter_census()` (the live population of a channel's rings) and
// `listener::current.has_work()` (a watcher the loop would still deliver).
// ---------------------------------------------------------------------------

TEST_F(ChannelAsync, RecvForTimeoutsLeaveNoRegistrationBehind) {
    channel<int>      ch(0); // rendezvous, nobody sends: every recv_for times out
    std::atomic<bool> done{false};
    std::atomic<int>  values{0};
    std::atomic<int>  worst_selects{0};

    coro_scheduler().spawn([&]() -> task<void> {
        int worst = 0;
        for (int i = 0; i < 256; ++i) {
            auto v = co_await ch.recv_for(0ms); // one cooperative turn each
            if (v)
                ++values;
            worst = std::max(worst, static_cast<int>(ch.waiter_census().selects));
        }
        for (int i = 0; i < 4; ++i) {
            auto v = co_await ch.recv_for(1ms); // the watchdog path
            if (v)
                ++values;
            worst = std::max(worst, static_cast<int>(ch.waiter_census().selects));
        }
        worst_selects.store(worst);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the timed receiver never finished";
    EXPECT_EQ(values.load(), 0) << "nothing was ever sent";
    EXPECT_EQ(worst_selects.load(), 0) << "a timed-out recv_for must take its registration with it (pre-QB-273: one per timeout)";
    EXPECT_EQ(ch.waiter_census().selects, 0u);
    EXPECT_EQ(ch.waiter_census().receivers, 0u);
}

TEST_F(ChannelAsync, SendForTimeoutsLeaveNoEntryBehind) {
    channel<int> ch(1);
    ASSERT_TRUE(ch.try_send(1)); // full: every send_for parks, then times out
    std::atomic<bool> done{false};
    std::atomic<int>  sent{0};
    std::atomic<int>  worst_senders{0};
    std::atomic<int>  late_ok{-1};
    std::atomic<int>  late_value{-1};

    coro_scheduler().spawn([&]() -> task<void> {
        int worst = 0;
        for (int i = 0; i < 256; ++i) {
            if (co_await ch.send_for(2, 0ms)) // one cooperative turn each
                ++sent;
            worst = std::max(worst, static_cast<int>(ch.waiter_census().senders));
        }
        for (int i = 0; i < 4; ++i) {
            if (co_await ch.send_for(2, 1ms)) // the watchdog path
                ++sent;
            worst = std::max(worst, static_cast<int>(ch.waiter_census().senders));
        }
        worst_senders.store(worst);
        // The ring is clean AND functional: free the slot, a later send_for takes it, nothing else does.
        auto first = ch.try_recv();
        late_ok.store(co_await ch.send_for(3, 100ms) ? 1 : 0);
        late_value.store((first.value_or(-1) == 1) ? ch.try_recv().value_or(-1) : -2);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the timed sender never finished";
    EXPECT_EQ(sent.load(), 0) << "the buffer stayed full";
    EXPECT_EQ(worst_senders.load(), 0) << "a timed-out send_for must take its entry with it (pre-QB-273: one per timeout, its guard set)";
    EXPECT_EQ(late_ok.load(), 1) << "the freed slot must serve a later send_for";
    EXPECT_EQ(late_value.load(), 3);
    EXPECT_EQ(ch.waiter_census().senders, 0u);
}

TEST_F(ChannelAsync, SelectWonElsewhereLeavesNoEntryInTheLosingChannel) {
    channel<int>      ch_a(0), ch_b(0);
    std::atomic<bool> done{false};
    std::atomic<int>  bad{0};
    std::atomic<int>  sum{0};

    coro_scheduler().spawn([&]() -> task<void> {
        for (int i = 0; i < 64; ++i) {
            // ch_b's sender runs once the select is parked on both channels, and resolves it.
            coro_scheduler().spawn([&ch_b, i]() -> task<void> {
                co_await sleep(0ms);
                co_await ch_b.send(i);
            });
            auto r = co_await select(ch_a, ch_b);
            if (r.index != 1u || r.closed)
                ++bad;
            else
                sum += r.get<int>();
            if (ch_a.waiter_census().selects != 0) // pre-QB-273: i + 1 resolved entries sat here
                ++bad;
        }
        // The vector form of select, the same way.
        for (int i = 0; i < 16; ++i) {
            coro_scheduler().spawn([&ch_b, i]() -> task<void> {
                co_await sleep(0ms);
                co_await ch_b.send(1000 + i);
            });
            auto r = co_await select(std::vector<channel<int> *>{&ch_a, &ch_b});
            if (r.index != 1u || r.closed)
                ++bad;
            else
                sum += r.get<int>();
            if (ch_a.waiter_census().selects != 0)
                ++bad;
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the selects never finished";
    EXPECT_EQ(bad.load(), 0) << "a select resolved by ch_b must retract its entry from ch_a as it resumes";
    EXPECT_EQ(sum.load(), 63 * 64 / 2 + 16 * 1000 + 15 * 16 / 2);
    EXPECT_EQ(ch_a.waiter_census().selects, 0u);
    EXPECT_EQ(ch_b.waiter_census().selects, 0u);
}

TEST_F(ChannelAsync, EarlyWonTimedWaitsLeaveNoWatchdogArmed) {
    channel<int>      ch(0);
    std::atomic<bool> done{false};
    std::atomic<int>  received{0};
    std::atomic<int>  delivered{0};

    coro_scheduler().spawn([&]() -> task<void> {
        // 64 recv_for with a far deadline, each won at once by a sender.
        for (int i = 0; i < 64; ++i) {
            coro_scheduler().spawn([&ch, i]() -> task<void> {
                co_await sleep(0ms);
                co_await ch.send(i);
            });
            auto v = co_await ch.recv_for(30s);
            if (v && *v == i)
                ++received;
        }
        done.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the timed receivers never finished";
    EXPECT_EQ(received.load(), 64);

    // 64 send_for with a far deadline on a full buffer, each served by a receive that frees the slot.
    channel<int> full(1);
    ASSERT_TRUE(full.try_send(-1));
    done.store(false);
    coro_scheduler().spawn([&]() -> task<void> {
        for (int i = 0; i < 64; ++i) {
            coro_scheduler().spawn([&full]() -> task<void> {
                co_await sleep(0ms);
                (void) co_await full.recv(); // frees the slot: the parked send_for is served by the wake
            });
            if (co_await full.send_for(i, 30s))
                ++delivered;
        }
        done.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the timed senders never finished";
    EXPECT_EQ(delivered.load(), 64);

    qb::io::async::run_for(5ms); // let the last frames end
    EXPECT_FALSE(qb::io::async::listener::current.has_work())
        << "a watchdog outlived the wait it guarded (pre-QB-273: 128 timers armed for 30 s)";
    EXPECT_EQ(ch.waiter_census().selects, 0u);
    EXPECT_EQ(full.waiter_census().senders, 0u);
}

// A parked timed wait is parked on a watcher, and the scheduler sees it as it sees a sleep (Huly QB-273): in its
// suspended set while the watchdog is armed, so active_count() counts it and dump() lists it -- as they counted the
// detached timer coroutine that guarded the wait before the watchdog moved into the awaiter -- and out of it once
// the wait ends. Sync primitives keep their waiters to themselves; watcher-backed awaiters register.
TEST_F(ChannelAsync, ParkedTimedWaitsAreInTheSchedulersSuspendedSet) {
    (void) coro_scheduler().set_suspension_tracking(false); // the default: the scheduler's own set is what dump() reads
    channel<int> ch(0);
    channel<int> full(1);
    ASSERT_TRUE(full.try_send(-1));
    std::atomic<int>   ended{0};
    std::optional<int> got;
    bool               sent = true;

    coro_scheduler().spawn([&]() -> task<void> {
        got = co_await ch.recv_for(30s);
        ++ended;
    });
    coro_scheduler().spawn([&]() -> task<void> {
        sent = co_await full.send_for(1, 30s);
        ++ended;
    });
    EXPECT_TRUE(pump_until([&] { return ch.waiter_census().selects == 1 && full.waiter_census().senders == 1; })) << "the waits never parked";
    EXPECT_EQ(coro_scheduler().active_count(), 2u) << "the two parked timed waits, each on its armed watchdog";
    std::size_t watchers = 0;
    for (auto const &p : coro_scheduler().dump())
        if (p.kind != nullptr && std::string(p.kind) == "watcher")
            ++watchers;
    EXPECT_EQ(watchers, 2u) << "dump() with tracking off lists a parked timed wait as a watcher wait";

    ch.close(); // the receiver resumes empty, the sender false
    full.close();
    EXPECT_TRUE(pump_until([&] { return ended.load() == 2; })) << "the closes never ended the waits";
    EXPECT_FALSE(got.has_value());
    EXPECT_FALSE(sent);
    EXPECT_EQ(coro_scheduler().active_count(), 0u) << "an ended wait leaves the suspended set";
}

TEST_F(ChannelAsync, ZeroTimeoutIsOneCooperativeTurn) {
    channel<int>       ch(0);
    std::atomic<bool>  done{false};
    std::optional<int> alone, served;
    bool               alone_sent = true, full_sent = true;

    coro_scheduler().spawn([&]() -> task<void> {
        alone = co_await ch.recv_for(0ms); // nobody: one cooperative turn, then timed out, nothing left registered
        coro_scheduler().spawn([&ch]() -> task<void> { co_await ch.send(7); });
        co_await sleep(0ms);                       // let the sender park on the rendezvous
        served     = co_await ch.recv_for(0ms);    // the wake hands 7 over at registration (a resume, no watchdog)
        alone_sent = co_await ch.send_for(8, 0ms); // nobody receives: one turn, then false
        channel<int> full(1);
        (void) full.try_send(1);
        full_sent = co_await full.send_for(2, 0ms); // the buffer stays full: one turn, then false
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the zero-timeout coordinator never finished";
    EXPECT_FALSE(alone.has_value());
    EXPECT_EQ(served.value_or(-1), 7);
    EXPECT_FALSE(alone_sent);
    EXPECT_FALSE(full_sent);
    EXPECT_EQ(ch.waiter_census().selects, 0u);
    EXPECT_EQ(ch.waiter_census().senders, 0u);
}

// A delivery and the watchdog due in ONE loop iteration, in each order (Huly QB-273). The order is forced, not left to
// chance: a coroutine queued behind the waiter keeps the thread out of the loop past both deadlines, so the next
// iteration dispatches the two expired timers together -- libev invokes them in the order of their expiry, the earlier
// deadline first -- and the delivery records whether the wait was still registered, so each case proves it ran the
// order it names. Whichever comes first wins: the value is handed to the wait, or buffered for the next one; never
// lost, never delivered twice, and the wait resumes once.
namespace channel_async_test {

struct same_turn_race {
    std::optional<int> first, second;
    int                registered_at_delivery = -1; ///< ch.waiter_census().selects as the delivery runs
};

inline same_turn_race
race_a_delivery_against_the_watchdog(qb::duration delivery_after, qb::duration timeout) {
    channel<int>      ch(1);
    std::atomic<bool> done{false};
    same_turn_race    out;
    coro_scheduler().spawn([&]() -> task<void> {
        coro_scheduler().spawn([]() -> task<void> {                      // runs right after the wait parks, in the same drain
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // past both deadlines
            co_return;
        });
        qb::io::async::callback(
            [&] {
                out.registered_at_delivery = static_cast<int>(ch.waiter_census().selects);
                (void) ch.try_send(5);
            },
            delivery_after);
        out.first = co_await ch.recv_for(timeout);
        if (!out.first)
            out.second = co_await ch.recv_for(200ms);
        done.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the racing receiver never finished";
    EXPECT_EQ(ch.waiter_census().selects, 0u);
    EXPECT_EQ(ch.size(), 0u) << "the value was delivered once, and nothing else is left";
    return out;
}

} // namespace channel_async_test

TEST_F(ChannelAsync, DeliveryDueBeforeTheWatchdogInOneTurnIsHandedToTheWait) {
    auto const r = channel_async_test::race_a_delivery_against_the_watchdog(1ms, 50ms);
    EXPECT_EQ(r.registered_at_delivery, 1) << "the order was not forced: the delivery must run first, the wait registered";
    EXPECT_EQ(r.first.value_or(-1), 5) << "the delivery that runs first is handed to the wait";
    EXPECT_FALSE(r.second.has_value());
}

TEST_F(ChannelAsync, WatchdogDueBeforeTheDeliveryInOneTurnLeavesTheValueForTheNextWait) {
    auto const r = channel_async_test::race_a_delivery_against_the_watchdog(50ms, 1ms);
    EXPECT_EQ(r.registered_at_delivery, 0) << "the order was not forced: the watchdog must run first and take the registration";
    EXPECT_FALSE(r.first.has_value()) << "the watchdog that runs first times the wait out";
    EXPECT_EQ(r.second.value_or(-1), 5) << "the delivery after it is buffered for the next wait, not lost";
}

TEST_F(ChannelAsync, SendForReclaimedWhileParkedRetractsItsEntry) {
    std::size_t senders_after = 99;
    bool        later_ok      = false;
    int         later_value   = -1;
    run_reclaim_driver([&senders_after, &later_ok, &later_value]() -> task<void> {
        auto ch = std::make_shared<channel<int>>(1);
        EXPECT_TRUE(ch->try_send(1)); // full: send_for must park
        auto park = [](std::shared_ptr<channel<int>> c) -> task<int> {
            volatile char big[8192];
            big[0]  = 7;
            bool ok = co_await c->send_for(2, 1000ms); // parks; reclaimed first
            big[1]  = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return ok ? 1 : 0;
        };
        auto r = co_await when_any(park(ch), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        senders_after = ch->waiter_census().senders; // pre-QB-273: 1, the guard set and the entry left for a later wake
        auto first    = ch->try_recv();              // frees the slot
        EXPECT_EQ(first.value_or(-1), 1);
        later_ok    = co_await ch->send_for(3, 100ms); // the freed slot serves a live sender
        later_value = ch->try_recv().value_or(-1);
    });
    EXPECT_EQ(senders_after, 0u) << "a send_for reclaimed while parked must retract its entry";
    EXPECT_TRUE(later_ok);
    EXPECT_EQ(later_value, 3);
    qb::io::async::run_for(5ms); // as RecvForReclaimedWhileParkedUnresolved: the loop sees what ASan cannot
    EXPECT_FALSE(qb::io::async::listener::current.has_work()) << "the reclaimed send_for's 1000 ms watchdog outlived its frame";
}

TEST_F(ChannelAsync, SelectReclaimedWhileParkedRetractsEveryEntry) {
    std::size_t a_after = 99, b_after = 99;
    run_reclaim_driver([&a_after, &b_after]() -> task<void> {
        auto ch_a = std::make_shared<channel<int>>(0);
        auto ch_b = std::make_shared<channel<int>>(0);
        auto park = [](std::shared_ptr<channel<int>> a, std::shared_ptr<channel<int>> b) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            auto r = co_await select(*a, *b); // parks on both; reclaimed first
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return static_cast<int>(r.index);
        };
        auto r = co_await when_any(park(ch_a, ch_b), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        a_after = ch_a->waiter_census().selects; // pre-QB-273: 1 each, resolved by the dtor, left for a later send
        b_after = ch_b->waiter_census().selects;
        EXPECT_FALSE(ch_a->try_send(1)) << "no live select may take the value";
        co_return;
    });
    EXPECT_EQ(a_after, 0u) << "a select reclaimed while parked must retract its entries";
    EXPECT_EQ(b_after, 0u);
}

// The vector form's destructor walks its own slots (Huly QB-273): reclaimed while parked, it retracts its node from
// every channel, and no later send takes a value for the select that is gone.
TEST_F(ChannelAsync, VectorSelectReclaimedWhileParkedRetractsEveryEntry) {
    std::size_t a_after = 99, b_after = 99;
    bool        a_took = true;
    run_reclaim_driver([&a_after, &b_after, &a_took]() -> task<void> {
        auto ch_a = std::make_shared<channel<int>>(0);
        auto ch_b = std::make_shared<channel<int>>(0);
        auto park = [](std::shared_ptr<channel<int>> a, std::shared_ptr<channel<int>> b) -> task<int> {
            volatile char big[8192];
            big[0] = 7;
            auto r = co_await select(std::vector<channel<int> *>{a.get(), b.get()}); // parks on both; reclaimed first
            big[1] = big[0];
            qb::io::test::g_resumed_after_reclaim.store(true, std::memory_order_relaxed);
            co_return static_cast<int>(r.index);
        };
        auto r = co_await when_any(park(ch_a, ch_b), reclaim_fast_winner());
        EXPECT_EQ(r.index, 1u);
        a_after = ch_a->waiter_census().selects;
        b_after = ch_b->waiter_census().selects;
        a_took  = ch_a->try_send(1);
        co_return;
    });
    EXPECT_EQ(a_after, 0u) << "a vector select reclaimed while parked must retract its entries";
    EXPECT_EQ(b_after, 0u);
    EXPECT_FALSE(a_took) << "no live select may take the value";
}
