/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/actor/actor-callback.cpp
 * @brief `ICallback` periodic ticking — register / fire-exact-quota / unregister-stops-firing, and
 *        the `LoopEvent` payload contract.
 *
 * `qb::ICallback` lets an actor receive a `LoopEvent` once per event-loop pass while it is
 * registered (`registerCallback`). This suite pins, on a real `qb::Main`:
 *   - a never-registered actor receives ZERO ticks (and the engine still drains);
 *   - a registered actor receives EXACTLY its quota of ticks before self-killing (its tick count is
 *     mirrored to a post-`join()` atom AND its destructor asserts the same — dual oracle);
 *   - `unregisterCallback()` actually STOPS the ticks: after the actor unregisters mid-run, NOT ONE
 *     further `LoopEvent` fires. The removal is async (it routes an `UnregisterCallbackEvent` to
 *     self) so we drive the proof event-deterministically: the unregistering tick records the count
 *     and self-`push`es a `StopProbe`. The worker loop processes mailbox events (the
 *     UnregisterCallbackEvent → callback removed) BEFORE the per-pass callback dispatch, so by the
 *     time `StopProbe` lands the callback is gone — the final count therefore EQUALS the
 *     count-at-unregister, with no slack. (See VirtualCore.cpp __workflow__: __receive__ precedes
 *     the callback snapshot dispatch.)
 *   - `LoopEvent` carries a coherent context: `now == Actor::time()` and non-zero, `iteration`
 *     strictly monotonic across passes.
 *
 * No wall-clock oracle: every actor self-terminates on a counter / control event, the ctest TIMEOUT
 * is the only backstop. Every in-actor outcome is mirrored to a process-global atom asserted after
 * `join()` so a never-scheduled actor cannot let a case pass vacuously.
 */

#include <atomic>
#include <cstdint>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

namespace {

// ---------------------------------------------------------------------------
// Case 1+2: quota ticking + never-registered. Atoms mirror the in-actor counts.
// ---------------------------------------------------------------------------
std::atomic<std::uint64_t> g_quota_ticks{0};   // ticks observed by the quota actor
std::atomic<bool>          g_quota_dtor_ok{false}; // dtor saw count == quota
std::atomic<std::uint64_t> g_never_ticks{0};   // ticks observed by the never-registered actor

class QuotaCallbackActor final
    : public qb::Actor
    , public qb::ICallback {
    const std::uint64_t _max_loop;
    std::uint64_t       _count_loop = 0;

public:
    QuotaCallbackActor() = delete;
    explicit QuotaCallbackActor(std::uint64_t const max_loop)
        : _max_loop(max_loop) {
        if (_max_loop)
            registerCallback(*this);
        else
            kill(); // never registered → must never tick
    }

    ~QuotaCallbackActor() final {
        if (_max_loop) {
            EXPECT_EQ(_count_loop, _max_loop);
            if (_count_loop == _max_loop)
                g_quota_dtor_ok.store(true);
        }
    }

    void
    on(qb::LoopEvent const &) final {
        if (_max_loop)
            g_quota_ticks.store(++_count_loop, std::memory_order_relaxed);
        else
            g_never_ticks.fetch_add(1, std::memory_order_relaxed); // must stay 0
        if (_count_loop >= _max_loop)
            kill();
    }
};

TEST(CallbackActor, UnregisteredActorNeverFires) {
    g_never_ticks.store(0);
    qb::Main main;
    main.addActor<QuotaCallbackActor>(0, 0); // kills in ctor, never registers
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_never_ticks.load(), 0u) << "a never-registered actor must receive no LoopEvent";
}

TEST(CallbackActor, RegisteredCallbackFiresExactQuota) {
    constexpr std::uint64_t kQuota = 1000;
    g_quota_ticks.store(0);
    g_quota_dtor_ok.store(false);

    qb::Main main;
    main.addActor<QuotaCallbackActor>(0, kQuota);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_quota_ticks.load(), kQuota) << "the callback must fire exactly its quota of ticks";
    EXPECT_TRUE(g_quota_dtor_ok.load()) << "the destructor oracle must have confirmed the same count";
}

// ---------------------------------------------------------------------------
// Case 3: unregisterCallback() actually stops the ticks (no further LoopEvent).
// ---------------------------------------------------------------------------
namespace {
std::atomic<std::uint64_t> g_unreg_total{0};         // total ticks the actor ever saw
std::atomic<std::uint64_t> g_unreg_at_unregister{0}; // tick count captured at unregister
std::atomic<bool>          g_unreg_stop_seen{false}; // the post-unregister control event landed
} // namespace

class UnregisterActor final
    : public qb::Actor
    , public qb::ICallback {
    static constexpr std::uint64_t kUnregisterAt = 50;
    std::uint64_t                  _ticks         = 0;

    struct StopProbe : public qb::Event {};

public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<StopProbe>(*this);
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        ++_ticks;
        g_unreg_total.store(_ticks, std::memory_order_relaxed);
        if (_ticks == kUnregisterAt) {
            g_unreg_at_unregister.store(_ticks, std::memory_order_relaxed);
            unregisterCallback();     // async: routes UnregisterCallbackEvent to self
            push<StopProbe>(id());    // processed AFTER the unregister event (FIFO mailbox)
        }
    }

    void
    on(StopProbe const &) {
        // By now the UnregisterCallbackEvent has been processed (it preceded us in the mailbox and
        // mailbox events are drained before the callback dispatch), so the callback is gone.
        g_unreg_stop_seen.store(true);
        kill();
    }
};

TEST(CallbackActor, UnregisterStopsCallbackFiring) {
    g_unreg_total.store(0);
    g_unreg_at_unregister.store(0);
    g_unreg_stop_seen.store(false);

    qb::Main main;
    main.addActor<UnregisterActor>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_unreg_stop_seen.load()) << "the post-unregister control event must have landed";
    EXPECT_EQ(g_unreg_at_unregister.load(), 50u) << "the actor must have unregistered at tick 50";
    // The load-bearing assertion: NO tick fired after unregister — the total equals the count we
    // captured the instant we unregistered.
    EXPECT_EQ(g_unreg_total.load(), g_unreg_at_unregister.load())
        << "no LoopEvent may fire after unregisterCallback() takes effect";
}

// ---------------------------------------------------------------------------
// Case 4: LoopEvent payload — now == Actor::time() (non-zero), iteration strictly monotonic.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_now_matches_time{true};
std::atomic<bool> g_now_nonzero{true};
std::atomic<bool> g_iteration_monotonic{true};
std::atomic<int>  g_loop_ticks{0};
} // namespace

class LoopEventActor final
    : public qb::Actor
    , public qb::ICallback {
    std::uint64_t _prev_iter = 0;
    bool          _first     = true;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &loop) final {
        if (loop.now != time()) // same cached timestamp as Actor::time()
            g_now_matches_time.store(false);
        if (loop.now == 0)
            g_now_nonzero.store(false);
        if (!_first && loop.iteration <= _prev_iter)
            g_iteration_monotonic.store(false);
        _first     = false;
        _prev_iter = loop.iteration;
        if (g_loop_ticks.fetch_add(1) + 1 >= 25) {
            unregisterCallback();
            kill();
        }
    }
};

TEST(CallbackActor, LoopEventCarriesLoopContext) {
    g_now_matches_time.store(true);
    g_now_nonzero.store(true);
    g_iteration_monotonic.store(true);
    g_loop_ticks.store(0);

    qb::Main main;
    main.addActor<LoopEventActor>(0);
    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_GE(g_loop_ticks.load(), 25);
    EXPECT_TRUE(g_now_matches_time.load());    // LoopEvent.now == time()
    EXPECT_TRUE(g_now_nonzero.load());         // a real wall-clock timestamp
    EXPECT_TRUE(g_iteration_monotonic.load()); // iteration strictly increases per pass
}

} // namespace
