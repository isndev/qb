/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-deadline.cpp
 * @brief The activation deadline — the deadlock-proofing that bounds a suspended `onInit()`.
 *
 * `VirtualCore::activation_deadline_ns` bounds the *Activating* window: an `onInit()` that does not
 * complete within it is failed and removed (its coroutine scope is cancelled so the frame unwinds).
 * This makes mutual-init deadlocks impossible and clamps no-timeout in-init asks. This file proves
 * the deadline force-fails three flavours of stuck init:
 *
 *   - a raw `co_await context().until_cancelled()` that never completes on its own;
 *   - an in-init `qb::ask` to a peer that is ITSELF permanently Activating (the request is stashed
 *     forever) — the asker's own deadline cancels the ask;
 *   - an in-init `qb::ask_retry` loop whose total retry budget exceeds the deadline — the deadline
 *     cancels the WHOLE loop, not just one attempt.
 *
 * ORDERING, NOT ABSOLUTE TIME: each stuck actor exposes a "completed naturally" flag that is set
 * ONLY if its long/forever wait finished on its own, and a "destroyed" flag set in its destructor.
 * The deadline (here 200-250 ms) is configured far below the natural completion (5 s / never), so
 * the assertions check the *ordering* invariant — the deadline cancelled BEFORE natural completion
 * (`destroyed == true && completed_naturally == false`) — rather than a fragile wall-clock bound.
 * A generous ctest TIMEOUT remains the only backstop; there is no tight `EXPECT_LT(elapsed, ...)`.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <atomic>
#include <chrono>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/core/patterns.h>

#include "../../shared/InitFixtures.h"

using namespace std::chrono_literals;
using qb::test::Cfg;
using qb::test::ScopedDeadline;

namespace {

// ===========================================================================
// 1. A raw stuck init (parks forever) — only the deadline frees it.
// ===========================================================================
std::atomic<bool> g_stuck_started{false};
std::atomic<bool> g_stuck_completed_naturally{false};
std::atomic<bool> g_stuck_destroyed{false};

class StuckInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        g_stuck_started.store(true);
        co_await context().until_cancelled();   // parks forever — only the deadline cancels it
        g_stuck_completed_naturally.store(true); // must NOT happen (no natural completion)
        co_return true;
    }
    ~StuckInit() override {
        g_stuck_destroyed.store(true);
    }
};

TEST(InitDeadline, StuckInitForceFailedByDeadlineBeforeNaturalCompletion) {
    g_stuck_started.store(false);
    g_stuck_completed_naturally.store(false);
    g_stuck_destroyed.store(false);
    ScopedDeadline dl(200'000'000); // 200 ms — far below the (never) natural completion
    qb::Main       main;
    main.addActor<StuckInit>(0);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_stuck_started.load());              // the init body ran up to its park
    EXPECT_FALSE(g_stuck_completed_naturally.load()); // ordering: it never completed on its own
    EXPECT_TRUE(g_stuck_destroyed.load());            // ...the deadline cancelled + removed it first
}

// ===========================================================================
// 2. An in-init ask to a peer that is permanently Activating — the request is
//    stashed forever; the asker's OWN deadline cancels the stuck ask.
// ===========================================================================
class StuckPeer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(5s); // far longer than the test deadline → stays Activating
        co_return true;
    }
};

std::atomic<bool> g_asker_completed_naturally{false};
std::atomic<bool> g_asker_destroyed{false};

class AsksStuckPeer : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AsksStuckPeer(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        // The peer is Activating → this request is stashed and never answered. The asker's OWN
        // activation deadline cancels the scope; the ask throws cancelled_error, which we
        // deliberately let propagate so the init FAILS (catching + co_return true would wrongly
        // succeed). This is the deadlock-proofing the deadline guarantees.
        (void) co_await qb::ask(context(), _peer, Cfg{1}, 5s);
        g_asker_completed_naturally.store(true); // unreachable — the deadline cancels first
        co_return true;
    }
    ~AsksStuckPeer() override {
        g_asker_destroyed.store(true);
    }
};

TEST(InitDeadline, AskToStuckActivatingPeerBrokenByDeadline) {
    g_asker_completed_naturally.store(false);
    g_asker_destroyed.store(false);
    ScopedDeadline dl(200'000'000); // 200 ms
    qb::Main       main;
    const auto     peer = main.addActor<StuckPeer>(0);
    main.addActor<AsksStuckPeer>(0, peer);
    main.start(false);
    main.join();
    EXPECT_FALSE(g_asker_completed_naturally.load()); // ordering: the ask never resolved
    EXPECT_TRUE(g_asker_destroyed.load());            // the deadline cancelled the stuck ask → removed
}

// ===========================================================================
// 3. An in-init ask_retry loop whose total budget exceeds the deadline — the
//    deadline cancels the WHOLE loop, not just one attempt.
// ===========================================================================
std::atomic<bool> g_retry_completed_naturally{false};
std::atomic<bool> g_retry_destroyed{false};

class AskRetryDeadlineInInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AskRetryDeadlineInInit(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        // The peer (StuckPeer) never answers; each attempt times out and retries — but the per-actor
        // activation deadline cancels the WHOLE loop, not just one attempt.
        (void) co_await qb::ask_retry(context(), _peer, Cfg{1}, 80ms, qb::retry_policy{.max_attempts = 5});
        g_retry_completed_naturally.store(true); // unreachable — the deadline cancels first
        co_return true;
    }
    ~AskRetryDeadlineInInit() override {
        g_retry_destroyed.store(true);
    }
};

TEST(InitDeadline, AskRetryLoopBrokenByActivationDeadline) {
    g_retry_completed_naturally.store(false);
    g_retry_destroyed.store(false);
    ScopedDeadline dl(250'000'000); // 250 ms < 5 x 80 ms of retries → the loop cannot finish in time
    qb::Main       main;
    const auto     peer = main.addActor<StuckPeer>(0);
    main.addActor<AskRetryDeadlineInInit>(0, peer);
    main.start(false);
    main.join();
    EXPECT_FALSE(g_retry_completed_naturally.load()); // ordering: the retry loop never resolved
    EXPECT_TRUE(g_retry_destroyed.load());            // the deadline cancelled the retry loop → removed
}

} // namespace
