/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-ask-gate.cpp
 * @brief The ask-reply gate: an `ask` issued from inside `onInit()` must have its reply
 *        DELIVERED to the suspended coroutine, never stashed.
 *
 * The flagship async-init use case is an actor that `co_await qb::ask(...)`s a peer to fetch its
 * configuration during initialization. While that init is suspended the actor is *Activating*, so
 * the activation gate (`VirtualCore::__receive_events__`) would normally stash an inbound unicast
 * event for FIFO replay once active. But the ask's reply lands while the asker is still Activating:
 * if it were stashed, the init would DEADLOCK on its own reply. The gate therefore tries
 * `ask_try_deliver_reply` first and only stashes on failure (see VirtualCore.cpp).
 *
 * This file proves the reply is NOT stashed with a dedicated **stash counter**: the asker registers
 * the exchange event and installs an `on(Cfg&)` handler that increments `g_*_stash_hits`. A stashed
 * event is replayed *through the normal router*, which would invoke that handler; the coroutine ask
 * path delivers straight to the awaiter and bypasses `on()`. So the handler firing zero times — while
 * the coroutine still resolves with the correct value — is the observable proof the reply skipped the
 * stash. Covered both same-core and cross-core (requires-multicore), plus the case where the responder
 * is ITSELF Activating (request stashed remotely, reply delivered once both activate). A final case
 * pins the frame-reclamation invariant: an in-init ask must reclaim the onInit frame, the ask awaiter,
 * and its timer (the worker's thread_local `live_frames` returns to its captured baseline).
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/InitFixtures.h"

using namespace std::chrono_literals;
using qb::test::Cfg;
using qb::test::CfgService;

namespace {

// ---------------------------------------------------------------------------
// 1. Same-core: onInit asks a peer; the reply must be delivered to the coroutine,
//    NOT routed through the asker's on(Cfg&) handler (stash counter stays 0).
// ---------------------------------------------------------------------------
std::atomic<bool> g_sc_ok{false};
std::atomic<int>  g_sc_value{-1};
std::atomic<int>  g_sc_stash_hits{0}; // on(Cfg&) firings = how often the reply hit the router/stash path

class AsksConfigInInit : public qb::Actor {
    qb::ActorId _svc;

public:
    explicit AsksConfigInInit(qb::ActorId svc)
        : _svc(svc) {}
    qb::io::async::task<bool>
    onInit() override {
        // Register Cfg so the stash/replay path HAS a handler to hit — if the reply were stashed
        // and replayed, on(Cfg&) below would fire. It must not.
        registerEvent<Cfg>(*this);
        auto reply = co_await qb::ask(context(), _svc, Cfg{7}, 2s); // reply lands while Activating
        g_sc_value.store(reply.response);
        g_sc_ok.store(true);
        kill();
        co_return true;
    }
    void
    on(Cfg &e) {
        // Reached only if the reply was stashed+replayed (the bug) or for an unsolicited Cfg.
        g_sc_stash_hits.fetch_add(1, std::memory_order_relaxed);
        (void) resolve_ask(e);
    }
};

TEST(InitAskGate, ReplyDeliveredNotStashedSameCore) {
    g_sc_ok.store(false);
    g_sc_value.store(-1);
    g_sc_stash_hits.store(0);
    qb::Main   main;
    const auto svc = main.addActor<CfgService>(0);
    main.addActor<AsksConfigInInit>(0, svc);
    main.start(false);
    main.join();
    EXPECT_TRUE(g_sc_ok.load()) << "the in-init ask must resolve";
    EXPECT_EQ(g_sc_value.load(), 70); // 7 * 10, computed by the responder
    EXPECT_EQ(g_sc_stash_hits.load(), 0)
        << "the ask reply was routed through on(Cfg&) — it was stashed instead of delivered to the awaiter";
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 2. Cross-core: same gate, responder on a different core (requires-multicore).
// ---------------------------------------------------------------------------
TEST(InitAskGate, ReplyDeliveredNotStashedCrossCore) {
    if (std::thread::hardware_concurrency() < 2)
        GTEST_SKIP() << "requires-multicore: single-core runner cannot place asker and responder on distinct cores";
    g_sc_ok.store(false);
    g_sc_value.store(-1);
    g_sc_stash_hits.store(0);
    qb::Main   main;
    const auto svc = main.addActor<CfgService>(1); // responder on core 1
    main.addActor<AsksConfigInInit>(0, svc);       // asker (Activating) on core 0
    main.start(false);
    main.join();
    EXPECT_TRUE(g_sc_ok.load());
    EXPECT_EQ(g_sc_value.load(), 70);
    EXPECT_EQ(g_sc_stash_hits.load(), 0)
        << "cross-core ask reply was stashed instead of delivered to the awaiter";
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 3. The load-bearing case: the RESPONDER is itself Activating when the request
//    arrives (request stashed remotely at the responder), the reply is then
//    delivered to the still-Activating asker once both activate. Stash counter
//    on the asker still proves the reply bypassed its own stash.
// ---------------------------------------------------------------------------
std::atomic<int> g_both_value{-1};
std::atomic<int> g_both_stash_hits{0};

class AsyncResponder : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_await context().sleep(25ms); // Activating — the asker's request stashes HERE
        co_return true;
    }
    void
    on(Cfg &e) {
        qb::answer(*this, e, [](Cfg const &r) { return r.key * 10; });
        kill();
    }
};

class AsyncAsker : public qb::Actor {
    qb::ActorId _r;

public:
    explicit AsyncAsker(qb::ActorId r)
        : _r(r) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_await context().sleep(5ms); // be Activating on the asker side too
        auto reply = co_await qb::ask(context(), _r, Cfg{4}, 2s);
        g_both_value.store(reply.response);
        kill();
        co_return true;
    }
    void
    on(Cfg &e) {
        g_both_stash_hits.fetch_add(1, std::memory_order_relaxed);
        (void) resolve_ask(e);
    }
};

TEST(InitAskGate, ReplyFromActivatingResponderDeliveredNotStashed) {
    g_both_value.store(-1);
    g_both_stash_hits.store(0);
    qb::Main   main;
    const auto r = main.addActor<AsyncResponder>(0);
    main.addActor<AsyncAsker>(0, r);
    main.start(false);
    main.join();
    EXPECT_EQ(g_both_value.load(), 40); // request stashed at the Activating responder, reply delivered
    EXPECT_EQ(g_both_stash_hits.load(), 0)
        << "reply from an Activating responder was stashed at the asker instead of delivered to the awaiter";
    EXPECT_FALSE(main.hasError());
}

// ---------------------------------------------------------------------------
// 4. Frame-reclamation invariant on the in-init ask path: the onInit frame, the
//    ask awaiter, and its timer must ALL be reclaimed. start(false) runs the
//    (single) core on THIS thread, so the worker's thread_local live_frames is
//    observable here against a captured baseline.
// ---------------------------------------------------------------------------
class EchoPeer : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Cfg>(*this);
        co_return true;
    }
    void
    on(Cfg &e) {
        qb::answer(*this, e, [](Cfg const &r) { return r.key + 1; });
    }
};

std::atomic<int> g_frames_value{-1};

class AsksThenStops : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AsksThenStops(qb::ActorId p)
        : _peer(p) {}
    qb::io::async::task<bool>
    onInit() override {
        auto r = co_await qb::ask(context(), _peer, Cfg{41}, 500ms);
        g_frames_value.store(r.response);
        qb::Main::stop();
        co_return true;
    }
};

TEST(InitAskGate, InInitAskReclaimsAllCoroutineFrames) {
    g_frames_value.store(-1);
    const long baseline = qb::io::async::detail::CoroutineFrameAllocator::live_frames;
    {
        qb::Main   main;
        const auto peer = main.addActor<EchoPeer>(0);
        main.addActor<AsksThenStops>(0, peer);
        main.start(false); // last (only) core runs on this thread → live_frames is ours
        main.join();
    }
    EXPECT_EQ(g_frames_value.load(), 42); // 41 + 1, computed by the peer
    EXPECT_EQ(qb::io::async::detail::CoroutineFrameAllocator::live_frames, baseline)
        << "in-init ask (onInit frame + ask awaiter + timer) leaked coroutine frames";
}

} // namespace
