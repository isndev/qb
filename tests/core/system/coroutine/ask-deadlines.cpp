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
 * Wall-clock bounds are LOOSE on purpose. `ctest -j` runs these beside a dozen other engines whose
 * core 0 is pinned to CPU 0 by default (`CoreInitializer` affinity), so a thread woken from a park
 * can wait tens of milliseconds for that CPU: the Windows gate measured a 30 ms deadline fire at
 * 97 and 187 ms under that contention, and 30.3 in isolation. Every engine here therefore runs
 * unpinned, a bound is set where the MECHANISM failing would land (a park that outlives the
 * deadline runs for the whole latency, so the latency is seconds and the bound a fraction of it),
 * and the exact figure is printed for the eye. Precision is asserted where the host cannot
 * interfere: in PASSES, on the busy core.
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

#include <algorithm>
#include <array>
#include <utility>
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
std::atomic<int>           g_max_active_watchers{0};    ///< the loop's referenced active count, max seen while waiting
std::atomic<int>           g_armed_while_waiting{-1};   ///< `deadlines_armed()` observed from the pass callback
std::atomic<int>           g_armed_after{-1};           ///< `deadlines_armed()` right after the awaited ask resumed
std::atomic<long long>     g_fire_delay_us{-1};         ///< arm -> timeout_error, microseconds
std::atomic<std::uint64_t> g_deadline_at_ns{0};         ///< no earlier than the busy ask's deadline, `mono_now()` epoch
std::atomic<int>           g_ticks_after_deadline{0};   ///< busy passes that ran after the deadline and before the timeout
std::atomic<std::uint64_t> g_wait_ticks{0};             ///< busy passes from the arm to the timeout
std::atomic<int>           g_idle_passes{0};            ///< passes with no tick, from the arm to the first tick after the timeout
std::atomic<bool>          g_tick_after_timeout{false}; ///< the chain was still ticking when the deadline fired
std::atomic<std::uint64_t> g_arm_t0_ns{0};              ///< the busy ask's `t0`, `mono_now()` epoch
std::atomic<std::uint64_t> g_first_tick_ns{0};          ///< the first tick after the arm, `mono_now()` epoch
std::atomic<int>           g_order[3]{{0}, {0}, {0}};
std::atomic<int>           g_order_n{0};
std::array<int, 3>         g_expected_order{};

void
reset() {
    g_timeouts             = 0;
    g_replies              = 0;
    g_cancels              = 0;
    g_max_active_watchers  = 0;
    g_armed_while_waiting  = -1;
    g_armed_after          = -1;
    g_fire_delay_us        = -1;
    g_deadline_at_ns       = 0;
    g_ticks_after_deadline = 0;
    g_wait_ticks           = 0;
    g_idle_passes          = 0;
    g_tick_after_timeout   = false;
    g_arm_t0_ns            = 0;
    g_first_tick_ns        = 0;
    g_order_n              = 0;
}

struct Done : public qb::Event {};

/// Every engine here runs its core unpinned (see the file header).
static void
unpin(qb::Main &main) {
    main.core(0).setAffinity(qb::CoreIdSet{qb::NoAffinity});
}

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
    unpin(main);
    auto mkt = main.addActor<SilentMarket>(0);
    main.addActor<LoopWatcher>(0);
    main.addActor<TimeoutNoWatcher>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_timeouts.load(), 1) << "a silent responder must drive the ask to timeout_error";
    EXPECT_EQ(g_armed_while_waiting.load(), 1) << "the pass must have seen the deadline armed while the ask waited";
    EXPECT_EQ(g_armed_after.load(), 0) << "a fired deadline is unlinked: nothing armed once the ask resumed";
    EXPECT_EQ(g_max_active_watchers.load(), 0) << "no libev watcher may back a request timeout: the loop is not run for it";
    std::printf("        [spin] timeout fired %lld us after the arm (deadline 30 ms)\n", static_cast<long long>(g_fire_delay_us.load()));
    EXPECT_GE(g_fire_delay_us.load(), 30'000) << "never early";
    EXPECT_LT(g_fire_delay_us.load(), 30'000 + 500'000) << "a spinning core checks every pass; the slack is for a loaded host";
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
    unpin(main);
    auto mkt = main.addActor<Market>(0);
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
    struct Pending {
        qb::detail::request_deadline deadline;
        OutOfOrder                  *owner;
        int                          tag;
    };
    qb::ActorId            _market;
    std::array<Pending, 3> _pending{};

    static void
    fired(void *arg) noexcept {
        auto     &pending = *static_cast<Pending *>(arg);
        const int n       = g_order_n.fetch_add(1);
        if (n < 3)
            g_order[n] = pending.tag;
        if (n == 2)
            pending.owner->push<Done>(pending.owner->id());
    }

public:
    explicit OutOfOrder(qb::ActorId market)
        : _market(market) {}
    ~OutOfOrder() override {
        for (auto &pending : _pending)
            qb::detail::deadline_disarm(pending.deadline);
    }
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Done>(*this);
        const std::array<int, 3>                     tags{30, 10, 20};
        std::array<std::pair<std::uint64_t, int>, 3> expected;
        for (std::size_t i = 0; i < tags.size(); ++i) {
            auto &pending = _pending[i];
            pending.owner = this;
            pending.tag   = tags[i];
            qb::detail::deadline_arm(pending.deadline, std::chrono::milliseconds(tags[i]), &fired, &pending);
            expected[i] = {pending.deadline.at, pending.tag};
        }
        std::stable_sort(expected.begin(), expected.end(), [](auto a, auto b) { return a.first < b.first; });
        for (std::size_t i = 0; i < expected.size(); ++i)
            g_expected_order[i] = expected[i].second;
        co_return true;
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
    unpin(main);
    auto mkt = main.addActor<SilentMarket>(0);
    main.addActor<OutOfOrder>(0, mkt);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    ASSERT_EQ(g_order_n.load(), 3);
    for (std::size_t i = 0; i < g_expected_order.size(); ++i)
        EXPECT_EQ(g_order[i].load(), g_expected_order[i]);
}

// ---------------------------------------------------------------------------
// 4. A BUSY core fires on time. Busy in the pass's own sense: an event moves on EVERY pass (the
//    actor re-sends itself a tick from the tick's handler), so the deadline can only be fired by
//    the check at the top of a busy pass -- the coarse clock first, the precise one only once the
//    coarse reading says the earliest deadline may be due. The wait is 150 ms, longer than the
//    coarse tick the pre-check allows for on every platform (20 ms on Linux and Windows, 50 ms on
//    Apple), so for most of it the pre-check returns on the coarse reading alone and must still let
//    the precise read through in time. The tick chain stops itself after 2 s and, if the deadline
//    has not fired by then, ends the run, so a check that never fires is a failed assertion, not a
//    hang.
//
//    "Busy for the whole wait" is MEASURED, in passes (Huly QB-1236): the chain runs before the
//    ask is armed, no pass between the arm and the timeout goes without a tick (the pass index of
//    consecutive ticks never skips one: a chain held back by anything but its own handler), and the
//    chain is still ticking in the pass after the timeout -- the deadline fired on a busy core. A
//    loaded host that deschedules the core's thread runs fewer passes in the wait, not idle ones,
//    so the premise holds however few there are; a floor on the passes counted in a wall-clock
//    window failed under load (5 for a floor of 10 in a 20 ms wait). The other two cases block the
//    core's thread, which is what a deschedule looks like from inside -- for the whole wait, and
//    between the reading the case takes at the arm and the ask's own -- and the verdicts must not
//    move.
// ---------------------------------------------------------------------------
struct Tick : public qb::Event {};

constexpr qb::duration kBusyWait = 150ms; ///< longer than every platform's coarse tick: the pre-check skips for most of it

class BusyAsker : public qb::Actor {
    static constexpr std::uint64_t kTicksBeforeArm = 10; ///< the chain is running before the deadline is armed

    qb::ActorId   _market;
    qb::duration  _stall_in_wait; ///< blocks the core's thread right after the arm, as a deschedule would
    qb::duration  _stall_at_arm;  ///< blocks it between the case's `t0` and the ask's own reading
    qb::mono_time _started{};
    std::uint64_t _ticks        = 0;
    std::uint64_t _last_pass    = 0;
    bool          _tracking     = false; ///< from the arm to the first tick after the timeout
    bool          _stalled      = false;
    bool          _stop_ticking = false;

public:
    BusyAsker(qb::ActorId m, qb::duration stall_in_wait, qb::duration stall_at_arm)
        : _market(m)
        , _stall_in_wait(stall_in_wait)
        , _stall_at_arm(stall_at_arm) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        registerEvent<Done>(*this);
        registerEvent<Tick>(*this);
        _started = qb::mono_now();
        push<Tick>(id());
        co_return true;
    }
    void
    on(const Tick &) {
        // ~50 us of work, then the next tick: one event per pass, for the whole wait. Lateness is
        // counted in PASSES, not wall-clock time -- a loaded host can deschedule this thread for
        // milliseconds, and a pass that did not run cannot have fired anything.
        const auto now    = qb::mono_now();
        const auto now_ns = static_cast<std::uint64_t>(now.time_since_epoch().count());
        const auto pass   = getCoreStats().loop_passes;
        if (_tracking) {
            if (pass > _last_pass + 1)
                ++g_idle_passes; // a pass ran without a tick
            if (g_timeouts.load() != 0) {
                g_tick_after_timeout = true;
                _tracking            = false;
            } else {
                ++g_wait_ticks;
                // The first pass after the arm: the ask armed its deadline on its own reading, taken after the
                // case's `t0` and before this one -- `kBusyWait` from now is no earlier than it, so a thread
                // descheduled between the two can only shrink the count below, never inflate it.
                if (g_deadline_at_ns.load() == 0) {
                    g_first_tick_ns  = now_ns;
                    g_deadline_at_ns = now_ns + static_cast<std::uint64_t>(std::chrono::nanoseconds(kBusyWait).count());
                } else if (now_ns >= g_deadline_at_ns.load()) {
                    ++g_ticks_after_deadline;
                }
            }
        }
        _last_pass = pass;

        volatile std::uint64_t x = 0;
        for (int i = 0; i < 20000; ++i)
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        if (++_ticks == kTicksBeforeArm) {
            _tracking = true;
            arm();
        } else if (_tracking && _stall_in_wait > qb::duration::zero() && !_stalled) {
            _stalled = true;
            std::this_thread::sleep_for(_stall_in_wait); // the core's thread off the CPU for the whole wait
        }
        if (!_stop_ticking && now - _started < 2s)
            push<Tick>(id());
        else if (!_stop_ticking && g_timeouts.load() == 0)
            push<Done>(id()); // 2 s and the deadline never fired: end the run, the assertions name it
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

private:
    void
    arm() {
        auto mkt = _market;
        spawn([mkt, stall = _stall_at_arm](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto t0 = qb::mono_now();
            g_arm_t0_ns   = static_cast<std::uint64_t>(t0.time_since_epoch().count());
            if (stall > qb::duration::zero())
                std::this_thread::sleep_for(stall); // the core's thread off the CPU before the ask reads its clock
            try {
                co_await qb::ask(ctx, mkt, Ping{1}, kBusyWait);
            } catch (const qb::io::async::timeout_error &) {
                ++g_timeouts;
                g_fire_delay_us = std::chrono::duration_cast<std::chrono::microseconds>(qb::mono_now() - t0).count();
            }
            ctx.push<Done>();
        });
    }
};

/// The verdicts of the busy-core cases: what fires, when, and the measured premise that the core was busy.
void
expect_busy_core_fired_on_time(const char *label) {
    EXPECT_EQ(g_timeouts.load(), 1) << "a deadline on a core that is never idle must still fire (the busy-pass check)";
    EXPECT_EQ(g_idle_passes.load(), 0) << "a pass without a tick ran between the arm and the timeout: the chain was held back";
    EXPECT_TRUE(g_tick_after_timeout.load()) << "the tick chain stopped before the deadline fired: it did not fire on a busy core";
    std::printf("        [%s] %llu ticks during the wait, first one %lld us after t0, timeout fired %lld us after t0 (deadline %lld ms), "
                "%d busy passes ran past the deadline first\n",
                label, static_cast<unsigned long long>(g_wait_ticks.load()),
                static_cast<long long>(g_first_tick_ns.load() - g_arm_t0_ns.load()) / 1000, static_cast<long long>(g_fire_delay_us.load()),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(kBusyWait).count()),
                g_ticks_after_deadline.load());
    EXPECT_GE(g_fire_delay_us.load(), std::chrono::duration_cast<std::chrono::microseconds>(kBusyWait).count()) << "never early";
    // The precise witness is the FIRE DELAY, read for the eye: tens to hundreds of microseconds past the deadline on a
    // quiet host. It is not bounded above: one wall-clock sample on a loaded host says how long the thread was off the
    // CPU, not how late the check was (Huly QB-1236). Lateness is asserted in PASSES: the deadline counted here is no
    // earlier than the true one, and once the pre-check's coarse reading reaches within a coarse tick of it, every pass
    // reads the precise clock, so the first pass past the deadline fires it before its tick runs -- at most one tick
    // lands past it, on any host and in any build. A pre-check that withheld the precise read until the coarse clock
    // itself crossed the deadline fires up to a coarse step late (a jiffy on Linux, 15.6 ms on Windows: tens to
    // hundreds of passes; Apple's approximate clock steps every few microseconds, where it cannot show), and a deadline
    // fired 5 ms late lands ~150 passes past it.
    EXPECT_LE(g_ticks_after_deadline.load(), 1) << "a busy pass ran past the deadline without the pre-check firing it";
}

void
run_busy_core(qb::duration stall_in_wait, qb::duration stall_at_arm) {
    reset();
    qb::Main main;
    unpin(main);
    auto mkt = main.addActor<SilentMarket>(0);
    main.addActor<BusyAsker>(0, mkt, stall_in_wait, stall_at_arm);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

TEST(AskDeadlines, ABusyCoreFiresOnTimeThroughTheCoarsePreCheck) {
    run_busy_core(qb::duration::zero(), qb::duration::zero());
    expect_busy_core_fired_on_time("busy");
}

// The same case with the core's thread off the CPU for the whole wait -- what a loaded host does to it, and what made the
// pass floor this case had fail (Huly QB-1236): a block right after the arm, past the deadline, leaves one pass in the wait.
TEST(AskDeadlines, ABusyCoreDescheduledForTheWholeWaitStillFiresThroughTheCoarsePreCheck) {
    run_busy_core(kBusyWait + 5ms, qb::duration::zero());
    expect_busy_core_fired_on_time("descheduled in the wait");
    EXPECT_GE(g_fire_delay_us.load(), std::chrono::duration_cast<std::chrono::microseconds>(kBusyWait + 5ms).count())
        << "the block lasted past the deadline: it cannot have fired before the core ran again";
}

// The thread off the CPU between the case's reading at the arm and the ask's own: the deadline the ask armed is later
// than `t0` plus the wait, and the passes the case counts past "the deadline" must be counted past the true one.
TEST(AskDeadlines, ABusyCoreDescheduledAtTheArmCountsPassesFromTheTrueDeadline) {
    run_busy_core(qb::duration::zero(), 25ms);
    expect_busy_core_fired_on_time("descheduled at the arm");
    EXPECT_GE(g_fire_delay_us.load(), std::chrono::duration_cast<std::chrono::microseconds>(kBusyWait + 25ms).count())
        << "the ask armed its deadline after the block";
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
    unpin(main);
    auto mkt = main.addActor<SilentMarket>(0);
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
    unpin(main);
    auto mkt = main.addActor<SilentMarket>(0);
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
    unpin(main);
    // Park on the first idle pass, for up to 3 s: a park the deadline does not bound would show
    // as ~3 s, which no scheduling delay reaches.
    main.core(0).setLatency(3s).setIdleSpin(0us);
    auto mkt = main.addActor<SilentMarket>(0);
    main.addActor<ParkedAsker>(0, mkt, with_watcher);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_timeouts.load(), 1);
    std::printf("        [parked, %s] timeout fired %lld us after the arm (deadline 30 ms, latency 3 s)\n",
                with_watcher ? "loop park" : "cv park", static_cast<long long>(g_fire_delay_us.load()));
    EXPECT_GE(g_fire_delay_us.load(), 30'000) << "never early";
    EXPECT_LT(g_fire_delay_us.load(), 1'000'000)
        << "the park must be bounded by the deadline (~30 ms; 30.3 measured in isolation), not by the 3 s latency";
}

TEST(AskDeadlines, AParkingCoreWakesForTheDeadline_ConditionVariablePark) {
    run_parked(false);
}

TEST(AskDeadlines, AParkingCoreWakesForTheDeadline_LoopPark) {
    run_parked(true);
}

} // namespace ask_deadlines_test
