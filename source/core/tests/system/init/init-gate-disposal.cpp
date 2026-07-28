/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/init/init-gate-disposal.cpp
 * @brief The activation gate must DISPOSE an event it hands to a pending continuation.
 *
 * `VirtualCore::__receive_events__` has four terminal paths for an inbound event, and every one of
 * them ends in a destructor call: the normal route disposes after the handler returns, a stash
 * overflow disposes on the spot, a failed/killed init disposes the whole stash, and a successful
 * activation replays the stash *through* the router (which disposes). The fifth path — the ask-reply
 * fast lane, where `ask_try_deliver_reply` hands the event straight to a suspended coroutine so an
 * in-`onInit()` ask does not deadlock on its own reply — used to `continue` without disposing.
 *
 * That is invisible for a well-behaved exchange event: the awaiter *moves* the event into its
 * `result`, so the un-destroyed original is only a moved-from husk. It stops being invisible the
 * moment the move degrades to a copy — a `const` member, or (as here) a user-declared copy
 * constructor that suppresses the implicit move. Then the original still owns its heap when the
 * gate drops it on the floor, and the leak is permanent and per-reply.
 *
 * The oracle is a payload type that counts its own live instances (ctor/copy +1, dtor -1) and holds
 * a real heap allocation, so the assertion is on OBJECT lifetime rather than on an allocator
 * statistic: after `Main::join()` every instance the test created must have been destroyed.
 * `KeepsHeapOnMove` deliberately declares a copy constructor and NO move constructor, which is the
 * ordinary shape of a hand-written RAII payload — not an exotic construction.
 *
 * The second case scales it: the leak is one payload PER REPLY, so a fan-out of in-init asks must
 * still land on zero live payloads. That is what makes the bug unbounded rather than a one-off — a
 * service that asks for its configuration during `onInit()` leaks once per actor it spawns.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <atomic>
#include <chrono>
#include <string>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns/request.h>
#include <qb/main.h>

using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// A payload that owns heap and counts live instances. Copy ctor is user-declared, which
// SUPPRESSES the implicit move ctor — so `std::move(payload)` selects the copy and the source
// keeps its own allocation. Ordinary hand-written-RAII shape, not a contrivance.
// ---------------------------------------------------------------------------
std::atomic<int> g_live{0};

struct KeepsHeapOnMove {
    std::string *heap = nullptr;

    KeepsHeapOnMove()
        : heap(new std::string(128, 'x')) {
        g_live.fetch_add(1, std::memory_order_relaxed);
    }
    KeepsHeapOnMove(KeepsHeapOnMove const &other)
        : heap(new std::string(*other.heap)) {
        g_live.fetch_add(1, std::memory_order_relaxed);
    }
    KeepsHeapOnMove &
    operator=(KeepsHeapOnMove const &other) {
        if (this != &other)
            *heap = *other.heap;
        return *this;
    }
    // No move ctor / move assignment: declaring the copy ctor already suppressed them, so every
    // `std::move(*this)` in the framework selects the COPY — which is the whole point here.
    ~KeepsHeapOnMove() {
        delete heap;
        g_live.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct Exchange : qb::AskEvent {
    KeepsHeapOnMove payload{};
    int             answer = 0;
};

static_assert(!std::is_trivially_destructible_v<Exchange>, "the test only means something for a payload-owning event");

// ---------------------------------------------------------------------------
// 1. In-onInit ask: the reply is consumed by the gate's fast lane and must still be destroyed.
// ---------------------------------------------------------------------------
std::atomic<bool> g_ask_ok{false};
std::atomic<int>  g_ask_answer{-1};

class Responder : public qb::Actor {
    int _remaining;

public:
    explicit Responder(int replies_before_exit)
        : _remaining(replies_before_exit) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Exchange>(*this);
        co_return true; // sync — active before anyone asks
    }
    void
    on(Exchange &e) {
        e.answer = 4242;
        reply(e); // preserves correlation_id — routes back to the still-Activating asker
        if (--_remaining <= 0)
            kill();
    }
};

class AsksDuringInit : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AsksDuringInit(qb::ActorId peer)
        : _peer(peer) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Exchange>(*this);
        auto reply = co_await qb::ask(context(), _peer, Exchange{}, 2s);
        g_ask_answer.store(reply.answer, std::memory_order_relaxed);
        g_ask_ok.store(true, std::memory_order_relaxed);
        kill();
        co_return true;
    }
    void
    on(Exchange &) {} // present so a stashed+replayed reply would have somewhere to land
};

TEST(ActivationGateDisposal, AskReplyConsumedByTheGateIsDestroyed) {
    g_live.store(0, std::memory_order_relaxed);
    {
        qb::Main   main;
        const auto responder = main.addActor<Responder>(0, 1);
        main.addActor<AsksDuringInit>(0, responder);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_TRUE(g_ask_ok.load(std::memory_order_relaxed)) << "the in-init ask must have resolved";
    EXPECT_EQ(g_ask_answer.load(std::memory_order_relaxed), 4242);
    EXPECT_EQ(g_live.load(std::memory_order_relaxed), 0)
        << "the reply the activation gate handed to the suspended coroutine was never destroyed: "
           "its payload is leaked once per in-init ask";
}

// ---------------------------------------------------------------------------
// 2. Many in-init asks: the leak is per-reply, so N asks must still end at zero live payloads.
//    This is what makes it unbounded in a real service that asks for its config on every restart.
// ---------------------------------------------------------------------------
constexpr int kFanout = 64;

std::atomic<int> g_fanout_done{0};

class AsksDuringInitN : public qb::Actor {
    qb::ActorId _peer;

public:
    explicit AsksDuringInitN(qb::ActorId peer)
        : _peer(peer) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Exchange>(*this);
        auto r = co_await qb::ask(context(), _peer, Exchange{}, 5s);
        if (r.answer == 4242)
            g_fanout_done.fetch_add(1, std::memory_order_relaxed);
        kill();
        co_return true;
    }
    void
    on(Exchange &) {}
};

TEST(ActivationGateDisposal, ManyInInitAsksLeaveNoLivePayloads) {
    g_live.store(0, std::memory_order_relaxed);
    g_fanout_done.store(0, std::memory_order_relaxed);
    {
        qb::Main   main;
        const auto responder = main.addActor<Responder>(0, kFanout);
        for (int i = 0; i < kFanout; ++i)
            main.addActor<AsksDuringInitN>(0, responder);
        main.start(false);
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_EQ(g_fanout_done.load(std::memory_order_relaxed), kFanout);
    EXPECT_EQ(g_live.load(std::memory_order_relaxed), 0) << "one payload leaked per in-init ask reply";
}

} // namespace
