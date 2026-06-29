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
        if (v1.has_value())
            EXPECT_EQ(*v1, 10);
        EXPECT_TRUE(v2.has_value());
        if (v2.has_value())
            EXPECT_EQ(*v2, 20);
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

TEST_F(ChannelAsync, SelectStaleWaiterIsSkippedByLaterSend) {
    channel<int>       ch_a(0), ch_b(0);
    select_result      result;
    std::atomic<bool>  select_done{false};
    std::atomic<bool>  stale_sender_done{false};
    std::atomic<bool>  receiver_done{false};
    std::optional<int> received;

    // A select() parks on both channels; ch_b's send resolves it. ch_a keeps a now-stale
    // select waiter entry, which a LATER ch_a.send must skip (resolve() is a no-op once
    // resolved) and instead hand its value to a real receiver.
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await select(ch_a, ch_b);
        select_done.store(true);
    });
    EXPECT_TRUE(pump_until([&] { return ch_b.empty(); })); // let select park on both

    coro_scheduler().spawn([&]() -> task<void> { co_await ch_b.send(99); });
    EXPECT_TRUE(pump_until([&] { return select_done.load(); })) << "select never resolved on ch_b";
    EXPECT_EQ(result.index, 1u);
    EXPECT_EQ(result.get<int>(), 99);

    // This sender must NOT be satisfied by the stale ch_a select entry — it parks.
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
        if (v.has_value())
            EXPECT_EQ(*v, 42);
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

TEST_F(ChannelAsync, SendForTimeoutLeavesStaleWaiterThatLaterSendSkips) {
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

    auto first = ch.try_recv();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 1);

    // A fresh send must be served by the freed slot, NOT by the timed-out sender's stale
    // waiter entry (which is lazily discarded via its guard).
    coro_scheduler().spawn([&]() -> task<void> { second_sender_done.store(co_await ch.send_for(3, 1000ms)); });
    EXPECT_TRUE(pump_until([&] { return second_sender_done.load(); })) << "second send never resolved";

    auto second = ch.try_recv();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 3) << "the stale waiter must be skipped; only the live sender's value remains";
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

// recv_for() parked and reclaimed while UNRESOLVED (no sender, no timeout yet): the timed_recv_awaiter
// dtor must mark the shared select-state resolved + drop the outer handle, so the still-parked
// channel_timer's later fire is a no-op and nothing schedules the freed frame. Complements the
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
        co_await sleep(20ms); // the recv_for frame was reclaimed unresolved; its channel_timer must no-op
    });
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
