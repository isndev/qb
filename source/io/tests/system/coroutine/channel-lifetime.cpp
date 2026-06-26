/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/channel-lifetime.cpp
 * @brief `qb::io::async::channel<T>` destroy-while-parked UAF guards + pipeline raw-ptr fix.
 *
 * The ASan-critical third of the former test-coroutine-channel.cpp split (synchronous
 * methods → unit/coroutine/channel-sync-ops.cpp; suspending surface →
 * system/coroutine/channel-async.cpp). These are the *crown-jewel* lifetime regressions:
 * each parks a coroutine on a channel awaiter (`recv` / `send` / `send_for`), then frees the
 * channel WHILE the awaiter is still parked, then lets the deferred close()-resume run
 * against the now-freed channel. The fix (a `shared_ptr<bool>` liveness token set false in
 * `~channel`, captured by every awaiter and checked before any channel access) must make the
 * resume resolve cleanly — recv → nullopt, send → channel_closed, send_for → false — with
 * NO heap-use-after-free. Without the guard, AddressSanitizer reports a UAF on the resume;
 * these tests are therefore only meaningful when the binary is built with ASan, and they are
 * the canonical place that exercises that build.
 *
 * Also re-homed here: the `make_pipeline` raw-pointer regression — `make_pipeline` spawns its
 * worker holding *raw pointers* into the two heap channels it returns by `unique_ptr`; the
 * worker (`pipeline_worker`, a free function, not a dangling local lambda) must run correctly
 * after `make_pipeline` returns and the temporaries are gone.
 *
 * De-flake: parking is confirmed via `pump_until` on an explicit "parked / not-yet-done"
 * predicate rather than a blind fixed `run_for`, and the post-destruction resume is awaited
 * with `pump_until(done)` (loud bounded timeout). The destroy-while-parked SEQUENCING (park →
 * free → resume) is the contract under test, so the free happens between the two pumps.
 */

#include <atomic>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/coroutine_test_support.h"

using namespace qb::io::async;
using namespace std::chrono_literals;
using qb::io::test::pump_until;

namespace {

class ChannelLifetime : public ::testing::Test {
protected:
    void
    SetUp() override {
        qb::io::test::reset_async_context();
    }
    void
    TearDown() override {
        // Destroy any frame still parked on a channel awaiter before clearing the loop, so a
        // suspended waiter from one test never carries into the next.
        if (qb::io::async::listener::current.has_coro_scheduler()) {
            qb::io::async::run_for(5ms);
            qb::io::async::listener::current.reset_coro_scheduler();
        }
        qb::io::async::listener::current.clear();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Destroy-while-parked UAF trio (run under ASan to be meaningful)
// ---------------------------------------------------------------------------

TEST_F(ChannelLifetime, DestroyChannelWhileRecvParkedResolvesNulloptNoUAF) {
    auto              ch = std::make_unique<channel<int>>(4);
    std::atomic<bool> parked{false};
    std::atomic<bool> done{false};
    std::atomic<bool> got_value{true}; // must flip false (nullopt) once the channel is gone

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        auto r = co_await ch->recv(); // parks: buffer empty, channel open
        got_value.store(r.has_value());
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return parked.load(); })) << "receiver never started";
    EXPECT_FALSE(done.load()) << "receiver must still be parked before the channel is freed";

    ch.reset(); // destroy the channel while the receiver is parked

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "deferred resume never ran after channel destruction";
    EXPECT_FALSE(got_value.load()) << "a recv parked on a freed channel must resolve to nullopt (no UAF)";
}

TEST_F(ChannelLifetime, DestroyChannelWhileSendForParkedResolvesFalseNoUAF) {
    auto ch = std::make_unique<channel<int>>(1);
    ASSERT_TRUE(ch->try_send(1)); // fill the single slot so send_for must park

    std::atomic<bool> parked{false};
    std::atomic<bool> done{false};
    std::atomic<bool> sent{true}; // must flip false once the channel is gone

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        sent.store(co_await ch->send_for(2, 1000ms)); // parks: buffer full, channel open
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return parked.load(); })) << "sender never started";
    EXPECT_FALSE(done.load()) << "send_for must still be parked before the channel is freed";

    ch.reset();

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "deferred resume never ran after channel destruction";
    EXPECT_FALSE(sent.load()) << "a send_for parked on a freed channel must resolve to false (no UAF)";
}

TEST_F(ChannelLifetime, DestroyChannelWhileSendParkedThrowsChannelClosedNoUAF) {
    auto              ch = std::make_unique<channel<int>>(0); // unbuffered: send parks immediately
    std::atomic<bool> parked{false};
    std::atomic<bool> done{false};
    std::atomic<bool> threw_closed{false};

    coro_scheduler().spawn([&]() -> task<void> {
        parked.store(true);
        try {
            co_await ch->send(7); // parks: unbuffered channel with no receiver
        } catch (channel_closed const &) {
            threw_closed.store(true);
        }
        done.store(true);
    });

    EXPECT_TRUE(pump_until([&] { return parked.load(); })) << "sender never started";
    EXPECT_FALSE(done.load()) << "send must still be parked before the channel is freed";

    ch.reset();

    EXPECT_TRUE(pump_until([&] { return done.load(); })) << "deferred resume never ran after channel destruction";
    EXPECT_TRUE(threw_closed.load()) << "a send parked on a freed channel must throw channel_closed (no UAF)";
}

// ---------------------------------------------------------------------------
// make_pipeline raw-pointer regression
// ---------------------------------------------------------------------------

TEST_F(ChannelLifetime, MakePipelineWorkerRunsAfterFactoryReturnsNoDangling) {
    // make_pipeline spawns pipeline_worker holding raw pointers into the two heap channels it
    // returns by unique_ptr. The worker (a free function, not a local lambda) must run
    // correctly once make_pipeline has returned and its temporaries are gone — a dangling
    // capture here would corrupt or crash (caught under ASan).
    auto [in, out] = make_pipeline<int, int>([](int v) { return v; }, 4);

    std::atomic<int>  count{0};
    std::atomic<bool> consumer_done{false};

    coro_scheduler().spawn([out = out.get(), &count, &consumer_done]() -> task<void> {
        while (true) {
            auto v = co_await out->recv();
            if (!v)
                break;
            count.fetch_add(1);
        }
        consumer_done.store(true);
    });
    coro_scheduler().spawn([in = in.get()]() -> task<void> {
        co_await in->send(1);
        co_await in->send(2);
        in->close();
    });

    EXPECT_TRUE(pump_until([&] { return consumer_done.load(); })) << "pipeline worker never delivered through the heap channels";
    EXPECT_EQ(count.load(), 2) << "every value sent into the pipeline must arrive";
    EXPECT_TRUE(out->is_closed()) << "the worker must close the output channel on input close";
}
