/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/request-operation.cpp
 * @brief `qb::ask` as a frame-free awaitable (`qb::ask_operation`, Huly QB-214): every shape a
 *        caller can give it, against the running engine.
 *
 * Since QB-214 `qb::ask` returns an awaitable that lives in the awaiting coroutine's own frame
 * instead of a `task<E>` coroutine of its own. The exchange contract is unchanged, so the
 * existing ask suites cover the reply, the timeout and the cancel paths; what THIS suite pins is
 * the surface the new type must keep, in one coroutine pass with one atomic per oracle behind a
 * "ran" guard (a never-scheduled coroutine cannot pass vacuously):
 *   - DIRECT: `co_await qb::ask(...)` yields the responder's value (seq * 2);
 *   - LAZY: an operation that is stored sends NOTHING until it is awaited — a counting responder
 *     on the same core sees zero requests across a `sleep` (the passes ran), one after the await;
 *     and an lvalue operation can be awaited;
 *   - TASK: it converts to `task<E>` (`task<E> t = qb::ask(...)`), the shape `ask_all` uses
 *     through `emplace_back` into a `std::vector<task<E>>` for `when_all`;
 *   - COMBINATORS: the variadic `when_all` and `when_any` and `coro_with_timeout` accept the
 *     operation directly — `when_any` proves the reclaim path: the silent branch is torn down while
 *     parked (its registry entry and 5 s deadline released) the instant the echo branch wins;
 *   - EMPLACE: the emplace form, awaited directly and as a task;
 *   - TIMEOUT: a silent responder still surfaces `timeout_error`.
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
#include <utility>
#include <vector>
#include "../../shared/AskResponders.h"

using namespace std::chrono_literals;
using qb::test::Echoer;
using qb::test::Ping;
using qb::test::SilentMarket;

namespace request_operation_test {

std::atomic<long> g_requests{0}; // requests the counting responder received
std::atomic<int>  g_direct{-1};
std::atomic<long> g_lazy_before{-1};
std::atomic<long> g_lazy_after{-1};
std::atomic<int>  g_lazy_val{-1};
std::atomic<int>  g_task{-1};
std::atomic<int>  g_all{-1};
std::atomic<int>  g_vec{-1};
std::atomic<int>  g_emplace{-1};
std::atomic<int>  g_emplace_task{-1};
std::atomic<int>  g_cwt{-1};
std::atomic<int>  g_any_index{-1};
std::atomic<int>  g_any_val{-1};
std::atomic<bool> g_timeout{false};
std::atomic<bool> g_threw{false};
std::atomic<bool> g_ran{false};

/// An `Echoer` that counts what it receives: the oracle of the LAZY case.
class CountingEchoer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        co_return true;
    }
    void
    on(Ping &p) {
        g_requests.fetch_add(1, std::memory_order_relaxed);
        qb::answer(*this, p, [](Ping const &r) { return r.seq * 2; });
    }
};

class OperationAsker : public qb::Actor {
    qb::ActorId _echo, _silent, _counter;

public:
    OperationAsker(qb::ActorId echo, qb::ActorId silent, qb::ActorId counter)
        : _echo(echo)
        , _silent(silent)
        , _counter(counter) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        const auto echo = _echo, silent = _silent, counter = _counter;
        spawn([echo, silent, counter](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            try {
                { // DIRECT: the frame-free form
                    auto r = co_await qb::ask<Ping>(c, echo, Ping{5}, 500ms);
                    g_direct.store(r.response);
                }
                { // LAZY: stored, nothing sent until awaited (as an lvalue)
                    const long before = g_requests.load();
                    auto       op     = qb::ask<Ping>(c, counter, Ping{7}, 500ms);
                    co_await c.sleep(20ms); // passes run: a sent request would have been served by now
                    g_lazy_before.store(g_requests.load() - before);
                    auto r = co_await op;
                    g_lazy_after.store(g_requests.load() - before);
                    g_lazy_val.store(r.response);
                }
                { // TASK: the conversion, and the ask_all shape (emplace_back into vector<task<E>>)
                    qb::io::async::task<Ping> t = qb::ask<Ping>(c, echo, Ping{6}, 500ms);
                    auto                      r = co_await std::move(t);
                    g_task.store(r.response);
                    std::vector<qb::io::async::task<Ping>> calls;
                    for (int i = 1; i <= 3; ++i)
                        calls.emplace_back(qb::ask<Ping>(c, echo, Ping{i}, 500ms));
                    auto rs  = co_await qb::io::async::when_all(std::move(calls));
                    int  sum = 0;
                    for (auto &x : rs)
                        sum += x.response;
                    g_vec.store(sum); // 2 + 4 + 6
                }
                { // COMBINATORS: variadic when_all, coro_with_timeout, when_any with a reclaimed loser
                    auto [a, b] =
                        co_await qb::io::async::when_all(qb::ask<Ping>(c, echo, Ping{1}, 500ms), qb::ask<Ping>(c, echo, Ping{2}, 500ms));
                    g_all.store(a.response * 100 + b.response);
                    auto r = co_await qb::io::async::coro_with_timeout(qb::ask<Ping>(c, echo, Ping{10}, 500ms), 500ms);
                    g_cwt.store(r.response);
                    auto w = co_await qb::io::async::when_any(qb::ask<Ping>(c, silent, Ping{11}, 5s), qb::ask<Ping>(c, echo, Ping{11}, 500ms));
                    g_any_index.store(static_cast<int>(w.index));
                    g_any_val.store(w.template get<Ping>().response);
                }
                { // EMPLACE: direct and as a task
                    auto r = co_await qb::ask<Ping>(c, echo, 500ms, 8);
                    g_emplace.store(r.response);
                    qb::io::async::task<Ping> t  = qb::ask<Ping>(c, echo, 500ms, 9);
                    auto                      r2 = co_await std::move(t);
                    g_emplace_task.store(r2.response);
                }
                try { // TIMEOUT
                    (void) co_await qb::ask<Ping>(c, silent, Ping{12}, 30ms);
                } catch (const qb::io::async::timeout_error &) {
                    g_timeout.store(true);
                }
            } catch (...) {
                g_threw.store(true);
            }
            g_ran.store(true);
            qb::Main::stop(); // event-driven: stop the instant the coroutine finishes
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
};

} // namespace request_operation_test

using namespace request_operation_test;

TEST(AskOperation, EveryShapeOfTheFrameFreeAsk) {
    g_requests.store(0);
    g_direct.store(-1);
    g_lazy_before.store(-1);
    g_lazy_after.store(-1);
    g_lazy_val.store(-1);
    g_task.store(-1);
    g_all.store(-1);
    g_vec.store(-1);
    g_emplace.store(-1);
    g_emplace_task.store(-1);
    g_cwt.store(-1);
    g_any_index.store(-1);
    g_any_val.store(-1);
    g_timeout.store(false);
    g_threw.store(false);
    g_ran.store(false);

    qb::Main   main;
    const auto echo    = main.addActor<Echoer>(0);
    const auto silent  = main.addActor<SilentMarket>(0);
    const auto counter = main.addActor<CountingEchoer>(0);
    main.addActor<OperationAsker>(0, echo, silent, counter);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    ASSERT_TRUE(g_ran.load()) << "the asker coroutine must have run";
    EXPECT_FALSE(g_threw.load()) << "no shape may throw";
    EXPECT_EQ(g_direct.load(), 10) << "direct: seq 5 * 2";
    EXPECT_EQ(g_lazy_before.load(), 0L) << "a stored operation sends nothing until awaited";
    EXPECT_EQ(g_lazy_after.load(), 1L) << "exactly one request once awaited";
    EXPECT_EQ(g_lazy_val.load(), 14) << "lvalue await: seq 7 * 2";
    EXPECT_EQ(g_task.load(), 12) << "task<E> conversion: seq 6 * 2";
    EXPECT_EQ(g_vec.load(), 12) << "vector<task<E>> through emplace_back + when_all: 2 + 4 + 6";
    EXPECT_EQ(g_all.load(), 204) << "variadic when_all over two operations: 2 and 4";
    EXPECT_EQ(g_cwt.load(), 20) << "coro_with_timeout over an operation: seq 10 * 2";
    EXPECT_EQ(g_any_index.load(), 1) << "when_any: the echo branch wins, the silent one is reclaimed";
    EXPECT_EQ(g_any_val.load(), 22) << "when_any: seq 11 * 2";
    EXPECT_EQ(g_emplace.load(), 16) << "emplace form: seq 8 * 2";
    EXPECT_EQ(g_emplace_task.load(), 18) << "emplace form as a task: seq 9 * 2";
    EXPECT_TRUE(g_timeout.load()) << "a silent responder still surfaces timeout_error";
}
