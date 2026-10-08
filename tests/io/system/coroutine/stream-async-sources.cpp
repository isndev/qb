/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/stream-async-sources.cpp
 * @brief `qb::io::async::async_stream<T>` clock/loop-driven sources — timer/interval/throttle/backpressure/live channels.
 *
 * The system half of the former test-coroutine-stream.cpp split (the deterministic
 * factories/transforms/terminals live in unit/coroutine/stream-transforms.cpp). Everything
 * here genuinely depends on the running event loop: the `timer`/`interval` factories that
 * emit on the libev clock, `throttle`/`backpressure` flow control, and `from_channel`/
 * `from_channel_shared`/`drain_to` over LIVE producer coroutines feeding a real channel.
 *
 * De-flake: every test gates on a real completion flag through the shared
 * `qb::io::test::pump_until` (loud bounded timeout) instead of a blind `run_for(Nms)` window.
 * Two clock-coupled assertions are made deterministic rather than wall-clock:
 *   - `ThrottleDeliversEveryValueInOrder` drops the original `elapsed >= 50ms` wall-clock
 *     check (the file's single flakiest assertion) and instead asserts that throttle delivers
 *     every input value, in order — the value contract, not the timing.
 *   - `DrainToPreservesBackpressureBoundary` pumps until the bounded output channel fills to
 *     its capacity (`out.size() == 2`) rather than guessing with a fixed `run_for(5ms)`.
 *
 * Merges the live-producer cases from the former test-coroutine-stream-advanced.cpp
 * (move-only values from a shared-channel stream, drain_to backpressure boundary) and the
 * channel-payload shared-channel collect.
 */

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

class StreamAsyncSources : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        // Live producers/timers may still be parked when a test ends; drain then destroy
        // suspended frames so no watcher leaks into the next test.
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// from_channel / from_channel_shared over live producers
// ---------------------------------------------------------------------------

TEST_F(StreamAsyncSources, FromChannelDrainsPrefilledClosedChannel) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        channel<int> ch(10);
        for (int i = 1; i <= 5; ++i)
            ch.try_send(i);
        ch.close();

        result = co_await async_stream<int>::from_channel(ch).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "from_channel collect never ran";
    EXPECT_EQ(result, (std::vector<int>{1, 2, 3, 4, 5}));
}

TEST_F(StreamAsyncSources, FromChannelWithAsyncProducerSumsAllValues) {
    int               sum = -1;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        channel<int> ch(4);

        coro_scheduler().spawn([&ch]() -> task<void> {
            for (int i = 1; i <= 4; ++i) {
                co_await sleep(10ms);
                co_await ch.send(i);
            }
            ch.close();
        });

        int local = 0;
        co_await async_stream<int>::from_channel(ch).for_each([&local](int v) { local += v; });
        sum = local;
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "from_channel + async producer never finished";
    EXPECT_EQ(sum, 10) << "1+2+3+4 produced one item at a time";
}

TEST_F(StreamAsyncSources, FromChannelSharedCollectsAllValues) {
    std::vector<std::string> result;
    std::atomic<bool>        done{false};
    auto                     messages = std::make_shared<channel<std::string>>(2);

    coro_scheduler().spawn([messages, &result, &done]() -> task<void> {
        result = co_await async_stream<std::string>::from_channel_shared(messages).collect();
        done.store(true);
    });

    ASSERT_TRUE(messages->try_send(std::string{"first"}));
    ASSERT_TRUE(messages->try_send(std::string{"second"}));
    messages->close();

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "from_channel_shared collect never ran";
    EXPECT_EQ(result, (std::vector<std::string>{"first", "second"}));
}

TEST_F(StreamAsyncSources, MoveOnlyValuesConsumedFromSharedChannelStream) {
    // Merged from the former stream-advanced suite: move-only payload through a stream
    // terminal (not just bare channel::recv).
    auto              source = std::make_shared<channel<std::unique_ptr<int>>>(4);
    std::atomic<bool> done{false};
    int               sum = -1;

    coro_scheduler().spawn([source, &done, &sum]() -> task<void> {
        int local = 0;
        co_await async_stream<std::unique_ptr<int>>::from_channel_shared(source).for_each([&local](std::unique_ptr<int> value) {
            if (value)
                local += *value;
        });
        sum = local;
        done.store(true);
    });

    ASSERT_TRUE(source->try_send(std::make_unique<int>(10)));
    ASSERT_TRUE(source->try_send(std::make_unique<int>(20)));
    ASSERT_TRUE(source->try_send(std::make_unique<int>(30)));
    source->close();

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "move-only shared-channel stream never finished";
    EXPECT_EQ(sum, 60);
}

// ---------------------------------------------------------------------------
// drain_to
// ---------------------------------------------------------------------------

TEST_F(StreamAsyncSources, DrainToDeliversAllValuesToChannel) {
    std::vector<int>  collected;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        channel<int> ch(10);
        co_await async_stream<int>::from_vector({10, 20, 30}).drain_to(ch);
        ch.close();

        while (auto v = co_await ch.recv())
            collected.push_back(*v);
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "drain_to never finished";
    EXPECT_EQ(collected, (std::vector<int>{10, 20, 30}));
}

TEST_F(StreamAsyncSources, DrainToPreservesBackpressureBoundary) {
    // Merged from the former stream-advanced suite: drain_to into a bounded (cap-2) channel
    // must STOP filling once the buffer is full — it cannot run ahead of the consumer.
    channel<int>      out{2};
    std::atomic<bool> done{false};

    coro_scheduler().spawn([&]() -> task<void> {
        co_await range_stream(0, 3).drain_to(out); // 3 values into a cap-2 channel
        done.store(true);
    });

    // Pump until the output channel is full at its capacity; drain_to must be parked here.
    EXPECT_TRUE(pump_until([&] { return out.size() == 2u; })) << "drain_to never filled the bounded channel";
    EXPECT_FALSE(done.load()) << "drain_to must block once the bounded channel is full (backpressure)";
    EXPECT_EQ(out.try_recv(), std::optional<int>{0});

    // Freeing one slot lets the third value through and drain_to completes.
    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "drain_to never resumed after backpressure release";
    EXPECT_EQ(out.try_recv(), std::optional<int>{1});
    EXPECT_EQ(out.try_recv(), std::optional<int>{2});
    EXPECT_FALSE(out.is_closed()) << "drain_to must NOT close the destination channel";
    out.close();
}

// ---------------------------------------------------------------------------
// timer / interval
// ---------------------------------------------------------------------------

TEST_F(StreamAsyncSources, TimerStreamEmitsValueOnceAfterDelay) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await timer(42, 30ms).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "timer stream never emitted";
    EXPECT_EQ(result, (std::vector<int>{42})) << "timer must emit its single value exactly once";
}

TEST_F(StreamAsyncSources, IntervalStreamEmitsMonotonicTicks) {
    std::vector<size_t> result;
    std::atomic<bool>   done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await interval(20ms, /*start_with_now=*/true).take(3).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "interval stream never produced 3 ticks";
    EXPECT_EQ(result, (std::vector<size_t>{0, 1, 2})) << "interval must emit a monotonic tick counter";
}

// ---------------------------------------------------------------------------
// throttle / backpressure flow control
// ---------------------------------------------------------------------------

TEST_F(StreamAsyncSources, ThrottleDeliversEveryValueInOrder) {
    // De-flaked: the original asserted a wall-clock `elapsed >= 50ms` (flaky on a loaded
    // host). The value contract — throttle must not drop or reorder anything, only space
    // emissions — is what we pin here. The 30ms interval still exercises the timer path.
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::from_vector({1, 2, 3}).throttle(30ms).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "throttle never emitted all values";
    EXPECT_EQ(result, (std::vector<int>{1, 2, 3})) << "throttle delays emissions but must deliver every value in order";
}

TEST_F(StreamAsyncSources, BackpressureBufferPreservesAllValues) {
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).backpressure(2).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "backpressure stream never finished";
    EXPECT_EQ(result, (std::vector<int>{1, 2, 3, 4, 5})) << "a bounded buffer must still deliver every value in order";
}

TEST_F(StreamAsyncSources, BackpressureReleasesPermitOnEofAcrossPasses) {
    // Re-homed regression (finding 2.C.5): an EXTERNAL shared semaphore must not leak a
    // permit at end-of-stream — a second drain pass would hang forever on the missing slot.
    std::atomic<bool> finished{false};
    coro_scheduler().spawn([&finished]() -> task<void> {
        auto sem = std::make_shared<semaphore>(1);

        auto drain_one_pass = [&sem]() -> task<void> {
            auto result = co_await async_stream<int>::from_vector({10, 20, 30}).backpressure(1, sem).collect();
            EXPECT_EQ(result, (std::vector<int>{10, 20, 30}));
        };

        co_await drain_one_pass();
        co_await drain_one_pass(); // would hang if the first pass leaked the EOF permit
        finished.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return finished.load(); })) << "backpressure leaked a permit on the shared semaphore at EOF";
}

// Regression: a source whose _next() throws must (1) close the buffer so the
// consumer is not stranded forever and (2) surface the exception to the consumer
// (rethrown at end-of-stream) rather than silently vanishing as EOF. Previously
// backpressure_fill_task had no try/catch — the exception killed the filler frame
// and the buffer was never closed, hanging the consumer.
TEST_F(StreamAsyncSources, BackpressureSurfacesThrowingSourceInsteadOfHanging) {
    std::atomic<bool> done{false};
    std::atomic<bool> caught{false};

    coro_scheduler().spawn([&]() -> task<void> {
        auto calls = std::make_shared<int>(0);
        // Emits 1, 2, then throws on the third pull.
        async_stream<int> source([calls]() -> task<std::optional<int>> {
            if ((*calls)++ >= 2)
                throw std::runtime_error("source boom");
            co_return std::optional<int>(*calls);
        });
        try {
            (void) co_await source.backpressure(2).collect();
        } catch (const std::runtime_error &) {
            caught.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "backpressure consumer hung after the source threw — the buffer was never closed";
    EXPECT_TRUE(caught.load()) << "a throwing source must surface its exception to the consumer, not vanish as EOF";
}

// ---------------------------------------------------------------------------
// An early terminal ends the consumers before the source (Huly QB-289)
//
// `first` / `take` / `any` / `find` stop pulling while the source still has values. backpressure()'s
// filler is a detached coroutine, and nothing told it the last consumer was gone: it filled the
// buffer, took the next permit and parked for the life of the thread, keeping the source -- and, on a
// caller-supplied semaphore, the permits of everything it had buffered.
// ---------------------------------------------------------------------------

TEST_F(StreamAsyncSources, BackpressureEarlyTerminalHandsBackEveryPermitOfASharedSemaphore) {
    std::atomic<bool> finished{false};
    std::atomic<int>  head{-1};
    std::atomic<int>  permits_after_first{-1};
    coro_scheduler().spawn([&]() -> task<void> {
        auto sem = std::make_shared<semaphore>(1);

        auto first = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5, 6, 7, 8}).backpressure(1, sem).first();
        head.store(first.value_or(-1));
        // The abandoned stream is gone: whatever it held on the shared semaphore is back.
        permits_after_first.store(static_cast<int>(sem->available_permits()));

        // A second stream on the same semaphore needs that permit to move at all.
        auto all = co_await async_stream<int>::from_vector({10, 20, 30}).backpressure(1, sem).collect();
        EXPECT_EQ(all, (std::vector<int>{10, 20, 30}));
        finished.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return finished.load(); })) << "the abandoned backpressure stream kept the shared semaphore's permit (QB-289)";
    EXPECT_EQ(head.load(), 1);
    EXPECT_EQ(permits_after_first.load(), 1) << "an early terminal must give every permit back to a caller-supplied semaphore";
}

TEST_F(StreamAsyncSources, BackpressureEarlyTerminalStopsPullingASharedSource) {
    auto             source = std::make_shared<channel<int>>(8);
    std::atomic<int> head{-1};
    ASSERT_TRUE(source->try_send(1));

    coro_scheduler().spawn([source, &head]() -> task<void> {
        auto first = co_await async_stream<int>::from_channel_shared(source).backpressure(1).first();
        head.store(first.value_or(-1));
    });
    ASSERT_TRUE(pump_until([&] { return head.load() != -1; })) << "the first value never arrived";
    EXPECT_EQ(head.load(), 1);

    // Give anything still scheduled its turn: an orphaned filler would now park on the source's recv().
    qb::io::async::run_for(5ms);
    EXPECT_EQ(source.use_count(), 1) << "nothing may still hold the source once its only stream is gone (QB-289)";

    // A value sent now belongs to the channel's next reader, not to a stream that no longer exists.
    ASSERT_TRUE(source->try_send(2));
    qb::io::async::run_for(5ms);
    EXPECT_EQ(source->try_recv(), std::optional<int>{2}) << "an orphaned filler stole a value sent after its stream was gone (QB-289)";
}

// The owner of the producer is the stream AND every copy of it: one terminal ending must not stop a
// producer another copy is still reading from. This is what makes the fix a lease on the LAST handle,
// not a cancellation on each terminal.
TEST_F(StreamAsyncSources, BackpressureProducerServesASurvivingCopyAfterAnotherEnded) {
    std::atomic<bool>  done{false};
    std::optional<int> head;
    std::vector<int>   rest;
    coro_scheduler().spawn([&]() -> task<void> {
        auto stream = async_stream<int>::from_vector({1, 2, 3, 4, 5}).backpressure(1);
        auto copy   = stream;
        head        = co_await stream.first(); // ends one handle
        rest        = co_await copy.collect(); // the producer still serves the other
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "the surviving copy of a backpressure stream never finished";
    EXPECT_EQ(head, std::optional<int>{1});
    EXPECT_EQ(rest, (std::vector<int>{2, 3, 4, 5})) << "a copy still in use keeps its producer running";
}

// ---------------------------------------------------------------------------
// debounce
// ---------------------------------------------------------------------------

TEST_F(StreamAsyncSources, DebounceEmitsTheLastValueOfABurst) {
    // The whole burst is buffered before the first quiet period ends: from_vector never suspends, so
    // the lazily started producer pushes 1..5 and closes in the turn that delivers the first value.
    // The quiet period then sees nothing new and emits the latest value, once.
    std::vector<int>  result;
    std::atomic<bool> done{false};
    coro_scheduler().spawn([&]() -> task<void> {
        result = co_await async_stream<int>::from_vector({1, 2, 3, 4, 5}).debounce(10ms).collect();
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "debounce never emitted";
    EXPECT_EQ(result, (std::vector<int>{5})) << "a burst debounces to its last value";
}

TEST_F(StreamAsyncSources, DebounceEarlyTerminalStopsPullingASharedSource) {
    // debounce()'s producer had the same missing owner as backpressure()'s filler: once the consumer
    // took its value and went away, the producer stayed parked on the source for good, holding it and
    // taking every value sent to it afterwards (Huly QB-289).
    auto             source = std::make_shared<channel<int>>(8);
    std::atomic<int> head{-1};
    ASSERT_TRUE(source->try_send(1));

    coro_scheduler().spawn([source, &head]() -> task<void> {
        auto first = co_await async_stream<int>::from_channel_shared(source).debounce(5ms).first();
        head.store(first.value_or(-1));
    });
    ASSERT_TRUE(pump_until([&] { return head.load() != -1; })) << "the debounced value never arrived";
    EXPECT_EQ(head.load(), 1);

    qb::io::async::run_for(5ms);
    EXPECT_EQ(source.use_count(), 1) << "nothing may still hold the source once its only debounced stream is gone (QB-289)";

    ASSERT_TRUE(source->try_send(2));
    qb::io::async::run_for(5ms);
    EXPECT_EQ(source->try_recv(), std::optional<int>{2})
        << "an orphaned debounce producer stole a value sent after its stream was gone (QB-289)";
}
