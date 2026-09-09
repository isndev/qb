/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-ready-async.cpp
 * @brief `ActorHandle::ready_async` is event-driven: resumed by the pass that ends the child's
 *        activation, whichever way it ends (Huly QB-62).
 *
 * Until 3.2 `ready_async` polled `ready()` through a 1 ms cancellable sleep: a timer armed and
 * disarmed a thousand times a second for a wait the core could answer on the spot, and a child
 * whose init FAILED was only ever reported through the waiter's own timeout. The awaiter behind it
 * now links a waiter node into the core's Activating entry and is fired -- once, unlinked first --
 * by whichever path ends the activation. This file pins that contract, edge by edge:
 *
 *   - ResumedByThePassThatActivates     -- the parent resumes within two core passes of the child's
 *                                          init completing (a 1 ms poll is hundreds of passes);
 *   - FailedInitReportedAtOnce           -- `co_return false` in the child: `false` in milliseconds,
 *                                          not after the waiter's 10 s timeout;
 *   - ThrowingInitReportedAtOnce         -- an init that throws after suspending: the same;
 *   - ActivationDeadlineReportedAtOnce   -- the core's activation deadline failing the child is
 *                                          reported the pass it fires, not at the waiter's timeout;
 *   - ChildKilledWhileActivating         -- a `KillEvent` reaching an Activating child (the gate lets
 *                                          it through) ends the wait with `false`;
 *   - WaiterTimeoutLeavesNothingBehind   -- a waiter that times out before the child activates is
 *                                          unlinked: the child then activates normally and `ready()`
 *                                          reads true, with nothing left in the core to fire;
 *   - AlreadyActiveDoesNotSuspend        -- a sync-init child answers `true` without a suspension;
 *   - InvalidHandleDoesNotSuspend        -- a default handle answers `false` without a suspension;
 *   - TwoWaitersOnOneChild               -- three coroutines awaiting the same child all resume `true`.
 *
 * tier=system. Every in-actor observation is mirrored to a post-`join()` atomic so a never-scheduled
 * parent cannot pass the test vacuously; every timing assertion is a generous bound on a mechanism
 * that is deterministic (a pass count, a wall-clock ceiling an order of magnitude above the event),
 * never a measurement of speed.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace init_ready_async_test {

using clock = std::chrono::steady_clock;

// ===========================================================================
// 1. Resumed by the pass that activates: a pass counter on the parent, read by the child when its
//    init body completes and by the parent when it resumes.
// ===========================================================================
std::atomic<std::uint64_t> g_passes{0};
std::atomic<std::uint64_t> g_pass_at_init_done{0};
std::atomic<std::uint64_t> g_pass_at_resume{0};
std::atomic<bool>          g_p1_ready{false};
std::atomic<bool>          g_p1_ran{false};

class SlowChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms); // the Activating window
        g_pass_at_init_done.store(g_passes.load());
        co_return true;
    }
};

class CountingParent
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        auto       child = addRefActor<SlowChild>();
        const bool ok    = co_await child.ready_async(context(), 5s);
        g_pass_at_resume.store(g_passes.load());
        g_p1_ready.store(ok && child.ready());
        if (child.ready())
            child->kill();
        g_p1_ran.store(true);
        kill();
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        g_passes.fetch_add(1);
    }
};

TEST(InitReadyAsync, ResumedByThePassThatActivates) {
    g_passes.store(0);
    g_pass_at_init_done.store(0);
    g_pass_at_resume.store(0);
    g_p1_ready.store(false);
    g_p1_ran.store(false);
    qb::Main main;
    main.addActor<CountingParent>(0);
    main.start(false);
    main.join();
    ASSERT_TRUE(g_p1_ran.load());
    EXPECT_TRUE(g_p1_ready.load());
    // The child's `co_return` runs in the io phase of pass P; the pump of THAT pass completes the
    // activation and schedules the parent, whose frame runs in the io phase of pass P + 1 -- before
    // that pass's callbacks tick the counter. Two is the ceiling with room; a 1 ms poll on a core
    // that passes every few microseconds reads in the hundreds.
    EXPECT_LE(g_pass_at_resume.load() - g_pass_at_init_done.load(), 2u)
        << "init done at pass " << g_pass_at_init_done.load() << ", parent resumed at pass " << g_pass_at_resume.load();
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 2. A failed init (`co_return false`) is reported the pass it fails, not at the waiter's timeout.
// ===========================================================================
std::atomic<bool>         g_p2_ran{false};
std::atomic<bool>         g_p2_result{true};
std::atomic<std::int64_t> g_p2_wait_ms{-1};

class FailingChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms);
        co_return false; // a clean init failure
    }
};

class FailWaitingParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto       child = addRefActor<FailingChild>();
        const auto t0    = clock::now();
        const bool ok    = co_await child.ready_async(context(), 10s); // the poll would wait these 10 s
        g_p2_wait_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        g_p2_result.store(ok);
        g_p2_ran.store(true);
        kill();
        co_return true;
    }
};

TEST(InitReadyAsync, FailedInitReportedAtOnce) {
    g_p2_ran.store(false);
    g_p2_result.store(true);
    g_p2_wait_ms.store(-1);
    qb::Main main;
    main.addActor<FailWaitingParent>(0);
    main.start(false);
    main.join();
    ASSERT_TRUE(g_p2_ran.load());
    EXPECT_FALSE(g_p2_result.load()) << "a failed init must resolve ready_async to false";
    EXPECT_LT(g_p2_wait_ms.load(), 3000) << "reported through the waiter's 10 s timeout, not when the init failed";
    EXPECT_GE(g_p2_wait_ms.load(), 15) << "the child's 20 ms Activating window was not waited for";
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 3. An init that THROWS after suspending: the same, `false` at once.
// ===========================================================================
std::atomic<bool>         g_p3_ran{false};
std::atomic<bool>         g_p3_result{true};
std::atomic<std::int64_t> g_p3_wait_ms{-1};

class ThrowingChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms);
        throw std::runtime_error("init failed loudly");
    }
};

class ThrowWaitingParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto       child = addRefActor<ThrowingChild>();
        const auto t0    = clock::now();
        const bool ok    = co_await child.ready_async(context(), 10s);
        g_p3_wait_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        g_p3_result.store(ok);
        g_p3_ran.store(true);
        kill();
        co_return true;
    }
};

TEST(InitReadyAsync, ThrowingInitReportedAtOnce) {
    g_p3_ran.store(false);
    g_p3_result.store(true);
    g_p3_wait_ms.store(-1);
    qb::Main main;
    main.addActor<ThrowWaitingParent>(0);
    main.start(false);
    main.join();
    ASSERT_TRUE(g_p3_ran.load());
    EXPECT_FALSE(g_p3_result.load());
    EXPECT_LT(g_p3_wait_ms.load(), 3000);
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 4. The core's activation deadline failing the child is reported the pass it fires.
// ===========================================================================
std::atomic<bool>         g_p4_ran{false};
std::atomic<bool>         g_p4_result{true};
std::atomic<std::int64_t> g_p4_wait_ms{-1};

class StuckChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(30s); // never completes on its own; the deadline cancels it
        co_return true;
    }
};

/// The waiter is a coroutine spawned by an already-active parent: a 50 ms activation deadline
/// would otherwise cancel the PARENT's own suspended `onInit()` along with the child's (the
/// deadline bounds every Activating actor), and the observation would never be recorded.
class DeadlineWaitingParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto child = addRefActor<StuckChild>();
        spawn([child](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto t0 = clock::now();
            const bool ok = co_await child.ready_async(ctx, 10s); // longer than the deadline
            g_p4_wait_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
            g_p4_result.store(ok);
            g_p4_ran.store(true);
            ctx.push<qb::KillEvent>(); // to self: the parent dies once the observation is recorded
        });
        co_return true; // the parent is active before it waits
    }
};

TEST(InitReadyAsync, ActivationDeadlineReportedAtOnce) {
    g_p4_ran.store(false);
    g_p4_result.store(true);
    g_p4_wait_ms.store(-1);
    const auto saved                        = qb::VirtualCore::activation_deadline_ns;
    qb::VirtualCore::activation_deadline_ns = 50ull * 1000u * 1000u; // 50 ms
    {
        qb::Main main;
        main.addActor<DeadlineWaitingParent>(0);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    qb::VirtualCore::activation_deadline_ns = saved;
    ASSERT_TRUE(g_p4_ran.load());
    EXPECT_FALSE(g_p4_result.load()) << "a child failed by the activation deadline must resolve to false";
    EXPECT_LT(g_p4_wait_ms.load(), 3000) << "reported at the waiter's 10 s timeout instead of the 50 ms deadline";
    EXPECT_GE(g_p4_wait_ms.load(), 40) << "resolved before the deadline could have fired";
}

// ===========================================================================
// 5. A KillEvent reaching an Activating child (the gate lets kills through) ends the wait: false.
// ===========================================================================
std::atomic<bool>         g_p5_ran{false};
std::atomic<bool>         g_p5_result{true};
std::atomic<std::int64_t> g_p5_wait_ms{-1};

class KilledChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(5s); // the kill lands long before this
        co_return true;
    }
};

class KillingParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto child = addRefActor<KilledChild>();
        push<qb::KillEvent>(child.id()); // delivered through the gate, on this core's next pass
        const auto t0 = clock::now();
        const bool ok = co_await child.ready_async(context(), 10s);
        g_p5_wait_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        g_p5_result.store(ok);
        g_p5_ran.store(true);
        kill();
        co_return true;
    }
};

TEST(InitReadyAsync, ChildKilledWhileActivating) {
    g_p5_ran.store(false);
    g_p5_result.store(true);
    g_p5_wait_ms.store(-1);
    qb::Main main;
    main.addActor<KillingParent>(0);
    main.start(false);
    main.join();
    ASSERT_TRUE(g_p5_ran.load());
    EXPECT_FALSE(g_p5_result.load()) << "a child killed while Activating must resolve to false";
    EXPECT_LT(g_p5_wait_ms.load(), 3000) << "the kill was not reported to the waiter";
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 6. A waiter that times out first leaves nothing behind: the child activates later, `ready()`
//    reads true, and the core fires nothing into a wait that is over.
// ===========================================================================
std::atomic<bool> g_p6_ran{false};
std::atomic<bool> g_p6_timed_out{false};
std::atomic<bool> g_p6_ready_later{false};

class LaterChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(120ms);
        co_return true;
    }
};

class ImpatientParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto       child = addRefActor<LaterChild>();
        const bool ok    = co_await child.ready_async(context(), 30ms); // times out first
        g_p6_timed_out.store(!ok && !child.ready());
        co_await context().sleep(300ms); // the child activates meanwhile; the old waiter is gone
        g_p6_ready_later.store(child.ready());
        if (child.ready())
            child->kill();
        g_p6_ran.store(true);
        kill();
        co_return true;
    }
};

TEST(InitReadyAsync, WaiterTimeoutLeavesNothingBehind) {
    g_p6_ran.store(false);
    g_p6_timed_out.store(false);
    g_p6_ready_later.store(false);
    qb::Main main;
    main.addActor<ImpatientParent>(0);
    main.start(false);
    main.join();
    ASSERT_TRUE(g_p6_ran.load());
    EXPECT_TRUE(g_p6_timed_out.load()) << "a 30 ms wait on a 120 ms init must time out with false";
    EXPECT_TRUE(g_p6_ready_later.load()) << "the child must still activate after the waiter gave up";
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 7 + 8. No suspension when the answer is known: an active child (true), an invalid handle (false).
// ===========================================================================
std::atomic<bool>          g_p7_ran{false};
std::atomic<bool>          g_p7_active_true{false};
std::atomic<bool>          g_p7_invalid_false{false};
std::atomic<std::uint64_t> g_p7_passes{0};
std::atomic<std::uint64_t> g_p7_passes_spent{99};

class SyncChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true; // no Activating phase
    }
};

class NoSuspendParent
    : public qb::Actor
    , public qb::ICallback {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerCallback(*this);
        auto                       child  = addRefActor<SyncChild>();
        const auto                 before = g_p7_passes.load();
        const bool                 a      = co_await child.ready_async(context(), 5s); // already active
        qb::ActorHandle<SyncChild> none;
        const bool                 b = co_await none.ready_async(context(), 5s); // invalid: never
        g_p7_passes_spent.store(g_p7_passes.load() - before);
        g_p7_active_true.store(a);
        g_p7_invalid_false.store(!b);
        if (child.ready())
            child->kill();
        g_p7_ran.store(true);
        kill();
        co_return true;
    }
    void
    on(qb::LoopEvent const &) final {
        g_p7_passes.fetch_add(1);
    }
};

TEST(InitReadyAsync, KnownAnswersDoNotSuspend) {
    g_p7_ran.store(false);
    g_p7_active_true.store(false);
    g_p7_invalid_false.store(false);
    g_p7_passes.store(0);
    g_p7_passes_spent.store(99);
    qb::Main main;
    main.addActor<NoSuspendParent>(0);
    main.start(false);
    main.join();
    ASSERT_TRUE(g_p7_ran.load());
    EXPECT_TRUE(g_p7_active_true.load()) << "an already-active child answers true";
    EXPECT_TRUE(g_p7_invalid_false.load()) << "an invalid handle answers false";
    EXPECT_EQ(g_p7_passes_spent.load(), 0u) << "neither answer may suspend the coroutine (no pass elapsed)";
    EXPECT_FALSE(main.hasError());
}

// ===========================================================================
// 9. Three waiters on one activation: all fired, all true.
// ===========================================================================
std::atomic<int>  g_p9_true{0};
std::atomic<int>  g_p9_done{0};
std::atomic<bool> g_p9_ran{false};

class SharedChild : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(20ms);
        co_return true;
    }
};

class TwoWaiterParent : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        auto child = addRefActor<SharedChild>();
        // Two spawned waiters, capturing the handle BY VALUE (never `this`), plus the parent's own
        // await on the same child: three nodes on one activation.
        for (int i = 0; i < 2; ++i) {
            spawn([child](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                if (co_await child.ready_async(ctx, 5s))
                    g_p9_true.fetch_add(1);
                g_p9_done.fetch_add(1);
            });
        }
        if (co_await child.ready_async(context(), 5s))
            g_p9_true.fetch_add(1);
        co_await context().sleep(50ms); // the two spawned waiters resume on the passes after ours
        if (child.ready())
            child->kill();
        g_p9_ran.store(true);
        kill();
        co_return true;
    }
};

TEST(InitReadyAsync, TwoWaitersOnOneChild) {
    g_p9_true.store(0);
    g_p9_done.store(0);
    g_p9_ran.store(false);
    qb::Main main;
    main.addActor<TwoWaiterParent>(0);
    main.start(false);
    main.join();
    ASSERT_TRUE(g_p9_ran.load());
    EXPECT_EQ(g_p9_done.load(), 2) << "both spawned waiters must have resumed";
    EXPECT_EQ(g_p9_true.load(), 3) << "all three waiters on the same activation must resume true";
    EXPECT_FALSE(main.hasError());
}

} // namespace init_ready_async_test
