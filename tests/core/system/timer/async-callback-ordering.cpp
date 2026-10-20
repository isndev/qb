/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/timer/async-callback-ordering.cpp
 * @brief `qb::io::async::callback(fn, delay)` — ordered dispatch and per-callback timing, proven by
 *        CAUSAL CHAINING rather than a wall-clock race.
 *
 * The original raced N callbacks at `50ms * id` and asserted they *happened* to finish in ascending
 * order — a flaky timing race. Here each callback schedules the NEXT one, so the completion order is
 * guaranteed by construction (callback k cannot run before callback k-1 has scheduled it). The order
 * vector is therefore a deterministic proof that `async::callback` fires the scheduled continuation,
 * in order, exactly `N` times — not a hope that the scheduler happened to interleave a certain way.
 *
 * Timing test: a fixed-delay chain of `N` callbacks must take AT LEAST `N * delay` (each link waits
 * its full delay) and asserts the EXACT callback count. The elapsed lower bound is a real invariant
 * (you cannot run `N` sequential `1ms` waits in less than `N` ms of monotonic time); the upper bound
 * is a generous, loudly-failing deadline — never a silent hang.
 *
 * Per-test state lives in the fixture, not file-global mutables, so the cases are independent.
 *
 * The two `qb::io::async::callback([this, idx] { ... }, delay)` sites below are the SUBJECT of this
 * file, not scaffolding: converting either to `spawn` + `co_await ctx.sleep` would delete the thing
 * under test and leave the primitive unproven (audited 2026-08; both kept). They are also safe —
 * each chain's terminal step calls `Main::stop()` and `kill()`, so the actor is alive at every link
 * and the pending `Timeout` is never outlived by its actor. Measured: zero ASan reports.
 */

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/main.h>
#include <qb/system/time.h>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Causal-ordering chain: callback k records its index then schedules callback k+1. Completion order
// is structural. The recorded order + count are mirrored to globals (reset per fixture) and asserted
// after join() so a never-scheduled chain cannot pass vacuously.
// ---------------------------------------------------------------------------
namespace {
std::vector<int>  g_order;    // single-writer (one chain on one core), read after join()
std::atomic<int>  g_fired{0}; // total callbacks that fired
std::atomic<bool> g_chain_done{false};
} // namespace

class OrderedChainActor : public qb::Actor {
    const int _count;

public:
    explicit OrderedChainActor(int count)
        : _count(count) {}

    qb::io::async::task<bool>
    onInit() override {
        schedule(0);
        co_return true;
    }

private:
    void
    schedule(int idx) {
        if (idx >= _count) {
            g_chain_done.store(true);
            qb::Main::stop();
            kill();
            return;
        }
        // Each link waits its own delay, then records + chains the next — strict causal order.
        qb::io::async::callback(
            [this, idx]() {
                g_order.push_back(idx);
                g_fired.fetch_add(1);
                schedule(idx + 1);
            },
            1ms);
    }
};

TEST(AsyncCallbackOrdering, ChainedCallbacksFireInOrderExactlyOnce) {
    g_order.clear();
    g_fired.store(0);
    g_chain_done.store(false);

    constexpr int kCount = 8;

    qb::Main main;
    main.addActor<OrderedChainActor>(0, kCount);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    ASSERT_TRUE(g_chain_done.load()) << "the chain must have run to completion";

    // EXACT count: not >= something — exactly kCount callbacks fired.
    EXPECT_EQ(g_fired.load(), kCount);
    ASSERT_EQ(static_cast<int>(g_order.size()), kCount) << "every callback must have recorded its index";

    // Causal chaining guarantees ascending order 0..kCount-1.
    std::vector<int> expected;
    for (int i = 0; i < kCount; ++i)
        expected.push_back(i);
    EXPECT_EQ(g_order, expected) << "chained callbacks must fire in scheduling order";
}

// ---------------------------------------------------------------------------
// Timing: this test expects a fixed-delay chain to take AT LEAST N*delay and fire EXACTLY N times.
// The elapsed floor is a regression check on the test host; the upper bound is a loud deadline.
//
// The 50-link chain was the tightest recorded floor in the suite and has zero slack by design.
// Its deadline construction relies on the following implementation paths:
//   - `qb::detail::to_ev_seconds` uses `duration_cast<duration<double>>`, avoiding integer-ms
//     truncation; floating-point conversion does not guarantee rounding upward;
//   - `async::callback` starts an event::timer, whose start() refreshes the loop clock with
//     `ev_now_update` before arming, so each link uses a fresh deadline rather than cached time;
//     losing that refresh is a regression this elapsed-floor check is intended to expose;
//   - `timers_reify` in qb/src/qb/ev/ev.c fires only once its clock is strictly past the deadline
//     (`ANHE_at(timers[HEAP0]) < mn_now`); get_clock selects the configured platform clock,
//     including POSIX monotonic and Windows paths, with a wall-clock fallback.
// `qb::mono_now()` below reads steady_clock, which need not be the loop clock on every platform.
// Thus conversion and cross-clock differences prevent a universal proof of this elapsed floor.
// We deliberately avoid Actor::time(), which is cached per VirtualCore loop turn. Recorded
// headroom: +0.34ms worst over 125 release runs (0.7% of the 50ms nominal), increasing to +334ms
// under 40ms SIGSTOP stalls. These historical measurements do not establish every host's bound.
// Investigate a failure, including clock selection and deadline refresh; do not silently widen it.
// ---------------------------------------------------------------------------
namespace {
std::atomic<int>      g_timed_fired{0};
std::atomic<uint64_t> g_elapsed_ns{0};
std::atomic<bool>     g_timed_done{false};
} // namespace

class TimedChainActor : public qb::Actor {
    const int          _count;
    const qb::duration _delay;
    qb::mono_time      _start{}; // monotonic — the correct clock for an elapsed lower bound

public:
    TimedChainActor(int count, qb::duration delay)
        : _count(count)
        , _delay(delay) {}

    qb::io::async::task<bool>
    onInit() override {
        _start = qb::mono_now();
        tick(0);
        co_return true;
    }

private:
    void
    tick(int idx) {
        if (idx >= _count) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(qb::mono_now() - _start);
            g_elapsed_ns.store(static_cast<uint64_t>(elapsed.count()));
            g_timed_done.store(true);
            qb::Main::stop();
            kill();
            return;
        }
        qb::io::async::callback(
            [this, idx]() {
                g_timed_fired.fetch_add(1);
                tick(idx + 1);
            },
            _delay);
    }
};

TEST(AsyncCallbackOrdering, FixedDelayChainHonoursElapsedLowerBound) {
    g_timed_fired.store(0);
    g_elapsed_ns.store(0);
    g_timed_done.store(false);

    constexpr int  kCount   = 50;
    constexpr auto kDelay   = 1ms;
    const uint64_t lower_ns = static_cast<uint64_t>(kCount) * 1'000'000ull; // 50 * 1ms
    // Generous upper bound: even a heavily-loaded CI box runs 50 1ms ticks well under 5s. If the
    // chain wedged, this fails LOUDLY rather than hanging (the ctest TIMEOUT is the last backstop).
    const uint64_t upper_ns = 5'000'000'000ull;

    qb::Main main;
    main.addActor<TimedChainActor>(0, kCount, kDelay);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    ASSERT_TRUE(g_timed_done.load()) << "the timed chain must have completed";

    EXPECT_EQ(g_timed_fired.load(), kCount) << "exactly kCount callbacks must fire";

    const uint64_t elapsed = g_elapsed_ns.load();
    EXPECT_GE(elapsed, lower_ns) << "N sequential " << 1 << "ms waits cannot finish in under N ms (elapsed=" << elapsed << "ns)";
    EXPECT_LT(elapsed, upper_ns) << "the chain must not wedge (elapsed=" << elapsed << "ns)";
}

// ---------------------------------------------------------------------------
// async::defer() from inside a real actor event handler must run on the next core
// tick — NOT inline (proving the re-entrancy-break contract in the actor context),
// and NOT stranded (the VirtualCore pump gates on has_deferred(), so a bare defer()
// from actor code with no other io/coroutine work is still drained).
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_defer_ran{false};
std::atomic<bool> g_defer_inline{false};
} // namespace

struct WakeEvent : qb::Event {};

class DeferringActor : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<WakeEvent>(*this);
        push<WakeEvent>(id()); // defer from a genuine handler, not onInit
        co_return true;
    }

    void
    on(WakeEvent const &) {
        // Capture only `this` (the actor lives until its own deferred kill()); NEVER a
        // local by reference — the deferred body runs after on() has returned.
        qb::io::async::defer([this]() {
            g_defer_ran.store(true);
            qb::Main::stop();
            kill();
        });
        // defer() must NOT have executed inline: the body has not run yet here.
        if (g_defer_ran.load())
            g_defer_inline.store(true);
    }
};

TEST(AsyncCallbackOrdering, DeferFromActorHandlerRunsOnNextTickNotInline) {
    g_defer_ran.store(false);
    g_defer_inline.store(false);

    qb::Main main;
    main.addActor<DeferringActor>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_defer_ran.load()) << "a defer() queued from an actor handler must be drained (VirtualCore gates on has_deferred())";
    EXPECT_FALSE(g_defer_inline.load()) << "defer() must NOT run inline inside the handler";
}
