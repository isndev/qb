/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-clock.cpp
 * @brief `Actor::time()` / `Actor::now()` are a real instant inside `onInit()`, not zero.
 *
 * `Actor::time()` is documented as "nanoseconds since the epoch", cached once per loop pass. The
 * cache is `VirtualCore::_metrics._nanotimer`, refreshed at the top of `__workflow__`
 * (`VirtualCore.cpp:644`) — but `onInit()` runs BEFORE the first pass (`Main.cpp:348` drives
 * `__init__actors__`, `:355` enters the loop). Through 3.0.0 the field was value-initialised to 0,
 * so every `onInit()` in the tree read `time() == 0`: an elapsed-time subtraction there yielded
 * the entire UNIX epoch, and `now()` was the epoch itself. Nothing said so anywhere.
 *
 * The field is now seeded at `VirtualCore` construction (`VirtualCore.h:377`), which is the first
 * moment the core exists and is still before any actor is constructed. This pins the property that
 * matters to a user — the value is a plausible present instant, and it is the same one every actor
 * on that core sees — without pinning the mechanism.
 *
 * The lower bound is deliberately crude (2020-01-01) rather than "close to now": a tight window
 * would make this a flaky clock-skew test instead of a zero-value test.
 */

#include <atomic>
#include <chrono>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/patterns.h>

using namespace std::chrono_literals;

namespace init_clock_test {

/// 2020-01-01T00:00:00Z in nanoseconds. Any real epoch reading is above it; 0 is not.
constexpr std::uint64_t kPlausibleFloor = 1577836800ull * 1000000000ull;

std::atomic<std::uint64_t> g_sync_init_time{0};
std::atomic<std::uint64_t> g_async_before{0};
std::atomic<std::uint64_t> g_async_after{0};
std::atomic<std::uint64_t> g_peer_init_time{0};
std::atomic<std::uint64_t> g_child_init_time{0};
std::atomic<std::uint64_t> g_ctor_time{0};

/// Synchronous init: never suspends, so it runs entirely before the loop starts.
class SyncInit : public qb::Actor {
public:
    SyncInit() {
        // Even the CONSTRUCTOR runs after the core exists, so the clock is already usable here.
        g_ctor_time.store(time(), std::memory_order_relaxed);
    }
    qb::io::async::task<bool>
    onInit() override {
        g_sync_init_time.store(time(), std::memory_order_relaxed);
        EXPECT_EQ(qb::unix_nanos(now()), time()) << "now() must be the chrono view of the same cached instant";
        kill();
        co_return true;
    }
};

/// A second actor on the SAME core: must observe the identical pre-loop instant.
class PeerInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_peer_init_time.store(time(), std::memory_order_relaxed);
        kill();
        co_return true;
    }
};

/// Suspends, so it also observes the value after the loop has taken over the refresh.
class AsyncInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_async_before.store(time(), std::memory_order_relaxed);
        co_await context().sleep(5ms);
        g_async_after.store(time(), std::memory_order_relaxed);
        kill();
        co_return true;
    }
};

/// Created from a parent's own pre-loop `onInit()`: the child's init is pre-loop too, so before
/// the fix this path read 0 as well. It is the one an application actually hits.
class Child : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_child_init_time.store(time(), std::memory_order_relaxed);
        kill();
        co_return true;
    }
};
class Parent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        addRefActor<Child>();
        kill();
        co_return true;
    }
};

TEST(InitClock, TimeIsARealInstantBeforeTheFirstLoopPass) {
    {
        qb::Main main;
        main.addActor<SyncInit>(0);
        main.addActor<PeerInit>(0);
        main.addActor<AsyncInit>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }

    EXPECT_GT(g_ctor_time.load(), kPlausibleFloor) << "the clock must already be usable in an Actor constructor";
    EXPECT_GT(g_sync_init_time.load(), kPlausibleFloor) << "Actor::time() was 0 inside a synchronous onInit()";
    EXPECT_GT(g_async_before.load(), kPlausibleFloor) << "Actor::time() was 0 before the first co_await in onInit()";
    EXPECT_GT(g_async_after.load(), kPlausibleFloor);

    // Same core, same pre-loop instant — the per-pass caching contract, one pass earlier.
    EXPECT_EQ(g_sync_init_time.load(), g_peer_init_time.load()) << "two actors' pre-loop onInit() on one core must read the same instant";
    EXPECT_EQ(g_sync_init_time.load(), g_async_before.load());

    // The loop's own refresh still moves it forward across a suspension.
    EXPECT_GE(g_async_after.load(), g_async_before.load());
}

/// Why this is not a cosmetic fix. `qb::deadline_in(ctx, d)` is ABSOLUTE — `ctx.time() + d` — and
/// `qb::remaining(dl, ctx)` clamps to zero. Built from `onInit()` with a zero clock it evaluated to
/// `0 + d`, an instant in 1970, so the first `qb::remaining()` taken once the loop was running
/// returned ZERO and every `ask_by` on that chain failed `timeout_error` before sending anything.
/// A deadline chain started in `onInit()` was born expired, silently.
std::atomic<std::uint64_t> g_remaining_ns{0};

class DeadlineInInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        const auto dl = qb::deadline_in(context(), 500ms);
        co_await context().sleep(5ms); // resume inside the loop, where the clock is unambiguous
        g_remaining_ns.store(static_cast<std::uint64_t>(qb::remaining(dl, context()).count()), std::memory_order_relaxed);
        kill();
        co_return true;
    }
};

TEST(InitClock, ADeadlineBuiltInOnInitIsNotAlreadyExpired) {
    {
        qb::Main main;
        main.addActor<DeadlineInInit>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    const auto left = g_remaining_ns.load();
    EXPECT_GT(left, 0u) << "qb::deadline_in() from onInit() produced an already-spent budget";
    // ~495 ms should be left; assert a wide band so this is a zero-vs-nonzero test, not a timing one.
    EXPECT_GT(left, 100'000'000u);
    EXPECT_LE(left, 500'000'000u);
}

TEST(InitClock, TimeIsARealInstantInAChildCreatedFromAPreLoopOnInit) {
    {
        qb::Main main;
        main.addActor<Parent>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_GT(g_child_init_time.load(), kPlausibleFloor) << "a child created from a pre-loop onInit() also read 0";
}

} // namespace init_clock_test
