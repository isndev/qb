/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/streaming-ask-stream.cpp
 * @brief Multi-reply streaming over the mailbox: `qb::ask_stream` / `yield_answer` / `end_stream`.
 *
 * The `ask` pattern is single-reply; `ask_stream` lets a responder push MANY chunks for one request,
 * drained one at a time via `co_await stream.next()` until an end marker (or a terminal condition).
 * Chunks are `AskEvent`s routed through the same per-core continuation registry as `ask`. Proven
 * here against the running engine:
 *   - HAPPY PATH: 5 chunks drain in FIFO order — `next()` yields each in turn then `std::nullopt` at
 *     end-of-stream; the gathered sum (0+10+20+30+40 = 100) proves order AND that none is dropped;
 *   - TIMEOUT: a producer that emits one chunk then never ends → `next()` times out after the first
 *     chunk (got exactly 1, then `timeout_error`);
 *   - CANCEL: killed while parked on `next()` → `cancelled_error` (no hang);
 *   - OVERFLOW: a producer floods past a tiny buffer while the consumer stalls → `next()` throws
 *     `stream_overflow_error` (a LOUD failure, not a silent drop), having drained at most the buffer.
 *
 * Outcomes are mirrored to atomics behind a "ran" flag asserted after join(); shutdown is
 * event-driven from the consumer the instant the stream resolves / is cancelled. The kill test arms
 * only a generous backstop stop so a cancel-path regression fails loudly rather than hanging.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <atomic>
#include <chrono>

using namespace qb;
using namespace std::chrono_literals;

namespace {
std::atomic<int>  g_stream_n{0};
std::atomic<int>  g_stream_sum{0};
std::atomic<bool> g_stream_timeout{false};
std::atomic<bool> g_stream_cancelled{false};
std::atomic<bool> g_stream_ran{false}; // the consumer coroutine reached a terminal state
} // namespace

// Streaming exchange event: the responder fills `chunk` per reply; `count` is a request field.
struct Feed : qb::StreamRequest<int> {
    int count{0};
};

class StreamProducer : public qb::Actor {
    bool _end;

public:
    explicit StreamProducer(bool end = true)
        : _end(end) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Feed>(*this);
        co_return true;
    }
    void
    on(Feed &e) {
        for (int i = 0; i < e.count; ++i)
            qb::yield_answer(*this, e, i * 10); // chunks 0,10,20,…
        if (_end)
            qb::end_stream(*this, e);
    }
};

class StreamConsumer : public qb::Actor {
    qb::ActorId  _prod;
    int          _count;
    qb::duration _to;

public:
    StreamConsumer(qb::ActorId prod, int count, qb::duration to)
        : _prod(prod)
        , _count(count)
        , _to(to) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Feed>(*this);
        auto prod  = _prod;
        auto count = _count;
        auto to    = _to;
        spawn([prod, count, to](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            Feed f;
            f.count = count;
            auto s  = qb::ask_stream(c, prod, f, to);
            int  n = 0, sum = 0;
            try {
                while (auto chunk = co_await s.next()) {
                    ++n;
                    sum += chunk->chunk;
                }
            } catch (const qb::io::async::timeout_error &) {
                g_stream_timeout.store(true);
            } catch (const qb::io::async::cancelled_error &) {
                g_stream_cancelled.store(true);
            }
            g_stream_n.store(n);
            g_stream_sum.store(sum);
            g_stream_ran.store(true);
            qb::Main::stop(); // event-driven: stop the instant the stream resolves
        });
        co_return true;
    }
    void
    on(Feed &e) {
        (void) resolve_ask(e); // chunks are AskEvents → routed via the continuation registry
    }
};

TEST(AskStream, StreamsAllChunksInOrder) {
    g_stream_n.store(0);
    g_stream_sum.store(0);
    g_stream_timeout.store(false);
    g_stream_ran.store(false);
    qb::Main   main;
    const auto prod = main.addActor<StreamProducer>(0, /*end=*/true);
    main.addActor<StreamConsumer>(0, prod, 5, qb::duration{1s});
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_stream_ran.load()) << "the consumer coroutine must have run";
    EXPECT_EQ(g_stream_n.load(), 5) << "all five chunks drained";
    EXPECT_EQ(g_stream_sum.load(), 100) << "0+10+20+30+40 — FIFO order preserved";
    EXPECT_FALSE(g_stream_timeout.load());
}

TEST(AskStream, TimeoutBetweenChunks) {
    g_stream_n.store(0);
    g_stream_sum.store(0);
    g_stream_timeout.store(false);
    g_stream_ran.store(false);
    qb::Main   main;
    const auto prod = main.addActor<StreamProducer>(0, /*end=*/false); // emits, never ends
    main.addActor<StreamConsumer>(0, prod, 1, qb::duration{100ms});
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_stream_ran.load());
    EXPECT_EQ(g_stream_n.load(), 1) << "got the one chunk";
    EXPECT_TRUE(g_stream_timeout.load()) << "…then next() timed out (no end marker)";
}

// Kills `_victim` while it is parked on next(), then arms a generous backstop stop so a regression
// of the cancel path fails loudly via the assertion rather than hanging.
class StreamKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit StreamKiller(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto v = _victim;
        qb::io::async::callback([this, v] { push<qb::KillEvent>(v); }, 40ms); // kill while parked
        qb::io::async::callback([] { qb::Main::stop(); }, 2s);                 // backstop only
        co_return true;
    }
};

TEST(AskStream, CancelledOnKill) {
    g_stream_cancelled.store(false);
    g_stream_ran.store(false);
    qb::Main   main;
    const auto prod   = main.addActor<StreamProducer>(0, /*end=*/false);
    const auto victim = main.addActor<StreamConsumer>(0, prod, 1, qb::duration{5s});
    main.addActor<StreamKiller>(0, victim);
    main.start(false);
    main.join(); // must not hang — kill wakes the parked next(), which throws cancelled
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_stream_ran.load());
    EXPECT_TRUE(g_stream_cancelled.load()) << "a kill while parked on next() must surface as cancelled";
}

// ===========================================================================
// Overflow: a producer outruns a tiny buffer while the consumer stalls → next() throws loudly.
// ===========================================================================
namespace {
std::atomic<bool> g_stream_overflow{false};
std::atomic<int>  g_stream_overflow_n{-1};
std::atomic<bool> g_overflow_ran{false};
} // namespace

class OverflowConsumer : public qb::Actor {
    qb::ActorId _prod;

public:
    explicit OverflowConsumer(qb::ActorId prod)
        : _prod(prod) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Feed>(*this);
        auto prod = _prod;
        spawn([prod](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            Feed f;
            f.count = 20;                                            // flood
            auto s  = qb::ask_stream(c, prod, f, 1s, /*capacity=*/2); // tiny buffer
            co_await c.sleep(60ms);                                  // stall: let the producer overrun the buffer
            int n = 0;
            try {
                while (auto chunk = co_await s.next())
                    ++n;
            } catch (const qb::stream_overflow_error &) {
                g_stream_overflow.store(true); // loud failure, not a silent drop
            }
            g_stream_overflow_n.store(n);
            g_overflow_ran.store(true);
            qb::Main::stop(); // event-driven: stop when the stream surfaces the overflow
        });
        co_return true;
    }
    void
    on(Feed &e) {
        (void) resolve_ask(e);
    }
};

TEST(AskStream, OverflowThrows) {
    g_stream_overflow.store(false);
    g_stream_overflow_n.store(-1);
    g_overflow_ran.store(false);
    qb::Main   main;
    const auto prod = main.addActor<StreamProducer>(0, /*end=*/false); // emits 20, never ends
    main.addActor<OverflowConsumer>(0, prod);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_overflow_ran.load());
    EXPECT_TRUE(g_stream_overflow.load()) << "overflow must surface as a stream_overflow_error";
    EXPECT_LE(g_stream_overflow_n.load(), 2) << "at most the buffered chunks drained before it threw";
}
