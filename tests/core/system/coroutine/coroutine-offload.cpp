/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/coroutine-offload.cpp
 * @brief `ScopedCoroContext::offload` (Huly QB-69): an actor's offload, and what a kill does to it.
 *
 * The qb-io half -- where the call runs, what crosses, a destroyed frame, a thread that exits --
 * is `tests/io/system/coroutine/offload.cpp`; the wake of a parked core by a returning call is
 * `engine/core-park-wake.cpp`. This file pins the actor half: a call cannot be interrupted, so a
 * kill does not end the CALL, it ends the WAIT. The killed actor's coroutine unwinds with
 * `cancelled_error` while its call is still running -- nothing after its `co_await` runs, ever --
 * and when the call returns, its result is discarded on the core, counted in
 * `offload_stats::discarded`, and handed to no one.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

// A named namespace: these actors are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace coroutine_offload_test {

using namespace std::chrono_literals;

std::atomic<bool>          g_call_started{false};  ///< the call is running on a pool thread
std::atomic<bool>          g_call_returned{false}; ///< the call has returned
std::atomic<bool>          g_unwound{false};       ///< the wait threw `cancelled_error`
std::atomic<bool>          g_continued{false};     ///< the line after the `co_await` ran: must never be set
std::atomic<bool>          g_keeper_timed_out{false};
std::atomic<std::uint64_t> g_discarded_before{0};

/// Released by the test thread once the wait has unwound, so the call outlives the kill.
std::mutex              g_gate_m;
std::condition_variable g_gate_cv;
bool                    g_gate_open = false;

void
open_gate() {
    {
        std::lock_guard lk(g_gate_m);
        g_gate_open = true;
    }
    g_gate_cv.notify_all();
}

/// Offloads a call that waits on the gate, and kills itself once that call has started.
class Worker
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                (void) co_await ctx.offload([] {
                    g_call_started.store(true, std::memory_order_release);
                    std::unique_lock lk(g_gate_m);
                    (void) g_gate_cv.wait_for(lk, 10s, [] { return g_gate_open; });
                    g_call_returned.store(true, std::memory_order_release);
                    return 1;
                });
                g_continued.store(true, std::memory_order_release);
            } catch (qb::io::async::cancelled_error const &) {
                g_unwound.store(true, std::memory_order_release);
            }
        });
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (g_call_started.load(std::memory_order_acquire))
            kill();
    }
};

/// Keeps the core running until the abandoned call's result has been discarded ON it, then stops.
class Keeper
    : public qb::Actor
    , public qb::ICallback {
    std::chrono::steady_clock::time_point _deadline = std::chrono::steady_clock::now() + 10s;

public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        co_return true;
    }
    void
    on(qb::LoopEvent const &) override {
        if (qb::io::async::current_offload_stats().discarded > g_discarded_before.load(std::memory_order_acquire)) {
            kill();
        } else if (std::chrono::steady_clock::now() > _deadline) {
            g_keeper_timed_out.store(true, std::memory_order_release);
            kill();
        }
    }
};

TEST(CoroutineOffload, AKillEndsTheWaitAtOnceAndTheResultIsDiscardedOnTheCore) {
    // From a clean slate, so a second run in the same process (`--gtest_repeat`) asserts what the first one did.
    for (auto *flag : {&g_call_started, &g_call_returned, &g_unwound, &g_continued, &g_keeper_timed_out})
        flag->store(false, std::memory_order_release);
    {
        std::lock_guard lk(g_gate_m);
        g_gate_open = false;
    }
    g_discarded_before.store(qb::io::async::current_offload_stats().discarded, std::memory_order_release);

    qb::Main main;
    main.addActor<Worker>(0);
    main.addActor<Keeper>(0);
    main.start();

    // The kill must unwind the wait while the call is still running: the gate is still closed.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!g_unwound.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(g_unwound.load(std::memory_order_acquire)) << "the killed actor's wait never threw cancelled_error";
    EXPECT_FALSE(g_call_returned.load(std::memory_order_acquire)) << "the wait only ended when the call returned";
    open_gate();

    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_call_returned.load(std::memory_order_acquire));
    EXPECT_FALSE(g_keeper_timed_out.load(std::memory_order_acquire)) << "the abandoned call's result was never discarded";
    EXPECT_EQ(qb::io::async::current_offload_stats().discarded, g_discarded_before.load(std::memory_order_acquire) + 1);
    EXPECT_FALSE(g_continued.load(std::memory_order_acquire)) << "code after the co_await ran for a killed actor";
}

} // namespace coroutine_offload_test
