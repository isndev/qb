/**
 * @file qb/tests/core/system/coroutine/ask-deadlines.cpp
 * @brief Request timeouts without a libev timer (Huly QB-189): the per-core deadline list.
 *
 * A timed `ask` (and `ask_stream::next()`, `ping`, `require`) used to arm an `ev_timer`, which
 * kept the io loop's active count above zero for the whole wait, so every pass of the core ran
 * `ev_run` for the one request in flight. The deadline lives in the core's own clock now: an
 * intrusive node in a per-thread list, checked by the pass against a coarse clock first and
 * against the precise one only within a scheduler tick of the earliest deadline. These cases pin
 * the contract in both directions -- what fires, when, in which order, and that the loop is NOT
 * involved -- rather than only that a timeout still throws.
 *
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/main.h>

#include "../../shared/AskResponders.h"

using namespace qb;
using namespace std::chrono_literals;
using qb::test::Market;
using qb::test::Ping;
using qb::test::SilentMarket;

namespace ask_deadlines_test {

std::atomic<int>           g_timeouts{0};
std::atomic<int>           g_replies{0};
std::atomic<int>           g_cancels{0};
std::atomic<int>           g_max_active_watchers{0};  ///< the loop's referenced active count, max seen while waiting
std::atomic<int>           g_armed_while_waiting{-1}; ///< `deadlines_armed()` observed from the pass callback
std::atomic<int>           g_armed_after{-1};         ///< `deadlines_armed()` right after the awaited ask resumed
std::atomic<long long>     g_fire_delay_us{-1};       ///< arm -> timeout_error, microseconds
std::atomic<std::uint64_t> g_busy_passes{0};
std::atomic<std::uint64_t> g_deadline_at_ns{0};       ///< the busy ask's deadline, `mono_now()` epoch
std::atomic<int>           g_ticks_after_deadline{0}; ///< busy passes that ran after the deadline and before the timeout
std::atomic<int>           g_order[3]{{0}, {0}, {0}};
std::atomic<int>           g_order_n{0};

void
reset() {
    g_timeouts             = 0;
    g_replies              = 0;
    g_cancels              = 0;
    g_max_active_watchers  = 0;
    g_armed_while_waiting  = -1;
    g_armed_after          = -1;
    g_fire_delay_us        = -1;
    g_busy_passes          = 0;
    g_deadline_at_ns       = 0;
    g_ticks_after_deadline = 0;
    g_order_n              = 0;
}

struct Done : public qb::Event {};

/// Watches the loop from the pass: is any watcher referenced while a timed ask is pending?
class LoopWatcher
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(const qb::LoopEvent &) override {
        const int active = qb::io::async::listener::current.loop().active_count();
        int       seen   = g_max_active_watchers.load();
        while (active > seen && !g_max_active_watchers.compare_exchange_weak(seen, active)) {
        }
        if (qb::detail::deadlines_armed())
            g_armed_while_waiting = 1;
    }
};

// ---------------------------------------------------------------------------
// 1. A timeout fires with NO libev watcher active, and the list is empty again afterwards.
// ---------------------------------------------------------------------------
class TimeoutNoWatcher : public qb::Actor {
    qb::ActorId _market;

public:
    explicit TimeoutNoWatcher(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<Done>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto t0 = qb::mono_now();
            try {
                co_await qb::ask(ctx, mkt, Ping{1}, 30ms);
            } catch (const qb::io::async::timeout_error &) {
                ++g_timeouts;
                g_fire_delay_us = std::chrono::duration_cast<std::chrono::microseconds>(qb::mono_now() - t0).count();
            }
            g_armed_after = qb::detail::deadlines_armed() ? 1 : 0;
            ctx.push<Done>();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
    void
    on(const Done &) {
        push<qb::KillEvent>(_market);
        broadcast<qb::KillEvent>();
    }
};

TEST(AskDeadlines, TimeoutFiresWithNoWatcherAndLeavesTheListEmpty) {
    reset();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(0);
    main.addActor<LoopWatcher>(0);
    main.addActor<TimeoutNoWatcher>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_timeouts.load(), 1) << "a silent responder must drive the ask to timeout_error";
    EXPECT_EQ(g_armed_while_waiting.load(), 1) << "the pass must have seen the deadline armed while the ask waited";
    EXPECT_EQ(g_armed_after.load(), 0) << "a fired deadline is unlinked: nothing armed once the ask resumed";
    EXPECT_EQ(g_max_active_watchers.load(), 0) << "no libev watcher may back a request timeout: the loop is not run for it";
    EXPECT_GE(g_fire_delay_us.load(), 30'000) << "never early";
    EXPECT_LT(g_fire_delay_us.load(), 30'000 + 20'000) << "and not a scheduler tick late (a spinning core checks every pass)";
}

// ---------------------------------------------------------------------------
// 2. A reply beats the deadline: the value arrives, the deadline is disarmed on the spot.
// ---------------------------------------------------------------------------
class ReplyBeats : public qb::Actor {
    qb::ActorId _market;

public:
    explicit ReplyBeats(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<Done>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (int i = 0; i < 1000; ++i) {
                auto r = co_await qb::ask(ctx, mkt, Ping{i}, 1s);
                if (r.response == i * 2)
                    ++g_replies;
                if (qb::detail::deadlines_armed())
                    g_armed_after = 1; // a replied ask must leave nothing armed
            }
            if (g_armed_after.load() < 0)
                g_armed_after = 0;
            ctx.push<Done>();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
    void
    on(const Done &) {
        push<qb::KillEvent>(_market);
        broadcast<qb::KillEvent>();
    }
};

TEST(AskDeadlines, AThousandRepliedAsksArmAndDisarmWithoutTheLoop) {
    reset();
    qb::Main main;
    auto     mkt = main.addActor<Market>(0);
    main.addActor<LoopWatcher>(0);
    main.addActor<ReplyBeats>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_replies.load(), 1000);
    EXPECT_EQ(g_armed_after.load(), 0) << "each reply disarms its deadline before the asker resumes";
    EXPECT_EQ(g_max_active_watchers.load(), 0) << "a thousand timed asks, zero watchers";
}

// ---------------------------------------------------------------------------
// 3. Deadlines fire in deadline order, not in arming order.
// ---------------------------------------------------------------------------
class OutOfOrder : public qb::Actor {
    qb::ActorId _market;

public:
    explicit OutOfOrder(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<Done>(*this);
        auto mkt = _market;
        // Armed 30, 10, 20 ms -- the list must sort them, and a shorter deadline armed after a
        // longer one must fire first.
        auto one = [mkt](qb::ScopedCoroContext ctx, int tag, qb::duration t) -> qb::io::async::task<void> {
            try {
                co_await qb::ask(ctx, mkt, Ping{tag}, t);
            } catch (const qb::io::async::timeout_error &) {
                const int n = g_order_n.fetch_add(1);
                if (n < 3)
                    g_order[n] = tag;
                if (n == 2)
                    ctx.push<Done>();
            }
        };
        spawn([one](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await one(ctx, 30, 30ms); });
        spawn([one](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await one(ctx, 10, 10ms); });
        spawn([one](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await one(ctx, 20, 20ms); });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
    void
    on(const Done &) {
        push<qb::KillEvent>(_market);
        kill();
    }
};

TEST(AskDeadlines, FireInDeadlineOrderNotArmingOrder) {
    reset();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(0);
    main.addActor<OutOfOrder>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    ASSERT_EQ(g_order_n.load(), 3);
    EXPECT_EQ(g_order[0].load(), 10);
    EXPECT_EQ(g_order[1].load(), 20);
    EXPECT_EQ(g_order[2].load(), 30);
}

// ---------------------------------------------------------------------------
// 4. A BUSY core fires on time. Busy in the pass's own sense: an event moves on EVERY pass (the
//    actor re-sends itself a tick from the tick's handler), so the core never takes the idle
//    path and its free clock read -- only the busy-pass check, coarse clock first, can fire the
//    deadline. The tick chain stops itself after 2 s so a check that never fires ends as a
//    failed assertion, not a hang.
// ---------------------------------------------------------------------------
struct Tick : public qb::Event {};

class BusyAsker : public qb::Actor {
    qb::ActorId   _market;
    qb::mono_time _started{};
    bool          _stop_ticking = false;

public:
    explicit BusyAsker(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<Done>(*this);
        registerEvent<Tick>(*this);
        _started = qb::mono_now();
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto t0    = qb::mono_now();
            g_deadline_at_ns = static_cast<std::uint64_t>((t0 + 20ms).time_since_epoch().count());
            try {
                co_await qb::ask(ctx, mkt, Ping{1}, 20ms);
            } catch (const qb::io::async::timeout_error &) {
                ++g_timeouts;
                g_fire_delay_us = std::chrono::duration_cast<std::chrono::microseconds>(qb::mono_now() - t0).count();
            }
            ctx.push<Done>();
        });
        push<Tick>(id());
        co_return true;
    }
    void
    on(const Tick &) {
        // ~50 us of work, then the next tick: one event per pass, for the whole wait. Lateness is
        // counted in PASSES, not wall-clock time -- a loaded host can deschedule this thread for
        // milliseconds, and a pass that did not run cannot have fired anything.
        const auto now = qb::mono_now();
        if (g_timeouts.load() == 0 && g_deadline_at_ns.load() != 0
            && static_cast<std::uint64_t>(now.time_since_epoch().count()) >= g_deadline_at_ns.load())
            ++g_ticks_after_deadline;
        volatile std::uint64_t x = 0;
        for (int i = 0; i < 20000; ++i)
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        ++g_busy_passes;
        if (!_stop_ticking && now - _started < 2s)
            push<Tick>(id());
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
    void
    on(const Done &) {
        _stop_ticking = true;
        push<qb::KillEvent>(_market);
        kill();
    }
};

TEST(AskDeadlines, ABusyCoreFiresOnTimeThroughTheCoarsePreCheck) {
    reset();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(0);
    main.addActor<BusyAsker>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_timeouts.load(), 1) << "a deadline on a core that is never idle must still fire (the busy-pass check)";
    EXPECT_GE(g_busy_passes.load(), 50u) << "the core must actually have been busy for the whole wait";
    std::printf("        [busy] %llu ticks, timeout fired %lld us after the arm (deadline 20 ms), %d busy passes ran past the deadline first\n",
                static_cast<unsigned long long>(g_busy_passes.load()), static_cast<long long>(g_fire_delay_us.load()),
                g_ticks_after_deadline.load());
    EXPECT_GE(g_fire_delay_us.load(), 20'000) << "never early";
    EXPECT_LE(g_ticks_after_deadline.load(), 2)
        << "the pass that straddles the deadline and at most one more: the coarse pre-check must not add a scheduler tick";
    EXPECT_LT(g_fire_delay_us.load(), 20'000 + 200'000) << "sanity: a loaded host may deschedule the core, not stall it";
}

// ---------------------------------------------------------------------------
// 5. Re-arming from a fired deadline: a timeout of one nanosecond fires on the NEXT pass, never
//    inside the check that is running, so a chain of immediate timeouts terminates pass by pass.
// ---------------------------------------------------------------------------
class ChainOfZeroTimeouts : public qb::Actor {
    qb::ActorId _market;

public:
    explicit ChainOfZeroTimeouts(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<Done>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (int i = 0; i < 5; ++i) {
                try {
                    co_await qb::ask(ctx, mkt, Ping{i}, 1ns);
                } catch (const qb::io::async::timeout_error &) {
                    ++g_timeouts;
                }
            }
            ctx.push<Done>();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
    void
    on(const Done &) {
        push<qb::KillEvent>(_market);
        kill();
    }
};

TEST(AskDeadlines, AOneNanosecondTimeoutFiresOnTheNextPassAndChains) {
    reset();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(0);
    main.addActor<ChainOfZeroTimeouts>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_timeouts.load(), 5);
}

// ---------------------------------------------------------------------------
// 6. A kill mid-wait: the scope cancel wins, and the dying frame disarms its deadline (the
//    list must not keep a node into freed memory -- the pass would fire into it).
// ---------------------------------------------------------------------------
class CancelledMidWait : public qb::Actor {
    qb::ActorId _market;

public:
    explicit CancelledMidWait(qb::ActorId m)
        : _market(m) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<Done>(*this);
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::ask(ctx, mkt, Ping{1}, 1s);
            } catch (const qb::io::async::cancelled_error &) {
                ++g_cancels;
            } catch (const qb::io::async::timeout_error &) {
                ++g_timeouts;
            }
        });
        push<Done>(id()); // handled on the next pass: kill while the ask still waits
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
    void
    on(const Done &) {
        push<qb::KillEvent>(_market);
        kill();
    }
};

/// Keeps the core alive a little after the asker died, so a deadline left in the list would get
/// its chance to fire into freed memory (ASan / a crash) before the core stops.
class Lingerer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(60ms);
            if (qb::detail::deadlines_armed())
                g_armed_after = 1;
            else if (g_armed_after.load() < 0)
                g_armed_after = 0;
            ctx.push<qb::KillEvent>();
        });
        co_return true;
    }
};

TEST(AskDeadlines, AKillMidWaitCancelsAndDisarms) {
    reset();
    qb::Main main;
    auto     mkt = main.addActor<SilentMarket>(0);
    main.addActor<CancelledMidWait>(0, mkt);
    main.addActor<Lingerer>(0);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_cancels.load(), 1) << "the scope cancel must win over a 1 s deadline";
    EXPECT_EQ(g_timeouts.load(), 0);
    EXPECT_EQ(g_armed_after.load(), 0) << "the dead asker's deadline must have left the list";
}

// ---------------------------------------------------------------------------
// 7. A PARKING core wakes for the deadline: the loop holds no timer for the request any more, so
//    the park's cap is the smaller of the core's latency and the time to the earliest deadline --
//    in both park shapes, the condition variable (no watcher) and the loop (a watcher active).
// ---------------------------------------------------------------------------
class ParkedAsker : public qb::Actor {
    qb::ActorId _market;
    bool        _with_watcher;

public:
    ParkedAsker(qb::ActorId m, bool with_watcher)
        : _market(m)
        , _with_watcher(with_watcher) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<Done>(*this);
        if (_with_watcher) // a libev timer the actor's kill cancels: the core parks INSIDE its loop (Park::Loop)
            spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> { co_await ctx.sleep(10s); });
        auto mkt = _market;
        spawn([mkt](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto t0 = qb::mono_now();
            try {
                co_await qb::ask(ctx, mkt, Ping{1}, 30ms);
            } catch (const qb::io::async::timeout_error &) {
                ++g_timeouts;
                g_fire_delay_us = std::chrono::duration_cast<std::chrono::microseconds>(qb::mono_now() - t0).count();
            }
            ctx.push<Done>();
        });
        co_return true;
    }
    void
    on(Ping &e) {
        resolve_ask(e);
    }
    void
    on(const Done &) {
        push<qb::KillEvent>(_market);
        kill();
    }
};

static void
run_parked(bool with_watcher) {
    reset();
    qb::Main main;
    main.core(0).setLatency(200ms).setIdleSpin(0us); // park on the first idle pass, for up to 200 ms
    auto mkt = main.addActor<SilentMarket>(0);
    main.addActor<ParkedAsker>(0, mkt, with_watcher);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_timeouts.load(), 1);
    std::printf("        [parked, %s] timeout fired %lld us after the arm (deadline 30 ms, latency 200 ms)\n",
                with_watcher ? "loop park" : "cv park", static_cast<long long>(g_fire_delay_us.load()));
    EXPECT_GE(g_fire_delay_us.load(), 30'000) << "never early";
    EXPECT_LT(g_fire_delay_us.load(), 30'000 + 30'000)
        << "the park must be bounded by the deadline, not by the 200 ms latency (OS timer granularity aside)";
}

TEST(AskDeadlines, AParkingCoreWakesForTheDeadline_ConditionVariablePark) {
    run_parked(false);
}

TEST(AskDeadlines, AParkingCoreWakesForTheDeadline_LoopPark) {
    run_parked(true);
}

} // namespace ask_deadlines_test
