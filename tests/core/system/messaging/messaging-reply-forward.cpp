/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/messaging/messaging-reply-forward.cpp
 * @brief reply() and forward() round-trip semantics, same-core and cross-core.
 *
 * `reply(event)` returns the event to its source; `forward(dest, event)` re-routes it while
 * preserving the original source. A requester sends both a plain TestEvent (replied) and an
 * EventForward (forwarded back), then waits for both to come home. Oracles: the requester's
 * destructor asserts it received exactly its two answers; the responder asserts it handled the
 * expected number (2 same-core, 4 cross-core via the extra broadcast); and a post-join global
 * counter asserts the total round-trips so a lost answer fails loudly instead of hanging.
 *
 * The second half (RelayChain*) pins the OWNERSHIP contract behind reply()/forward() for a
 * non-trivially-destructible event: each call moves the event's bytes into a pipe, so the copy
 * — not the original — owns the payload and runs the destructor exactly once, at the end of the
 * handler that finally keeps it. Since 3.2 that is a structural property of the copy: an event
 * in a pipe is born with `alive == 0` (`qb::detail::event_wire::copy` clears the bit in the
 * copy), and reply()/forward() raise the ORIGINAL's flag only after the copy — nothing on the
 * receive path writes the header any more. The oracle is a constructed/destroyed pair of
 * counters over a chain of H hops: H handlers, 1 construction, 1 destruction, same-core, cross-core
 * (where the copy is the mailbox relocation instead) and through a forward()ing relay — and the
 * FAN-OUT, `reply(e); forward(other, e);` on one event, whose second copy is made from an
 * original that already says alive: two copies, two destructions, on both transports. That
 * last shape is the one the receive-side `alive = 0` store used to cover before 3.2, and the
 * reason the header rewrites clear the bit themselves (the cross-core mailbox is a raw memcpy).
 */

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>
#include <qb/actor.h>
#include <qb/main.h>
#include "../../shared/ChecksumEvent.h"

using qb::test::TestEvent;

// EventForward is a distinct type so the responder can route it through forward() (vs reply()).
struct EventForward : public TestEvent {};

static std::atomic<int> g_answers_home{0}; // replies + forwards observed back at the requester

// Sends one TestEvent (expects a reply) and one EventForward (expects a forward-back); on the
// cross-core variant it additionally broadcasts both to the peer core index first.
class RequesterActor final : public qb::Actor {
    const qb::ActorId _to;
    int               _received = 0;

    void
    finishIfDone() {
        if (_received < 2)
            return;
        // Both answers are home: tear the (possibly multi-actor) test down deterministically.
        push<qb::KillEvent>(qb::BroadcastId(_to.index()));
        kill();
    }

public:
    explicit RequesterActor(qb::ActorId const to)
        : _to(to) {}

    ~RequesterActor() final {
        EXPECT_EQ(_received, 2);
    }

    qb::io::async::task<bool>
    onInit() final {
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
        registerEvent<TestEvent>(*this);
        registerEvent<EventForward>(*this);

        if (_to.index()) { // cross-core: also exercise the broadcast-to-core path
            push<TestEvent>(qb::BroadcastId(_to.index()));
            push<EventForward>(qb::BroadcastId(_to.index()));
        }
        push<TestEvent>(_to);
        push<EventForward>(_to);
        co_return true;
    }

    void
    on(TestEvent &event) {
        EXPECT_TRUE(event.checkSum());
        ++_received;
        g_answers_home.fetch_add(1, std::memory_order_relaxed);
        finishIfDone();
    }
    void
    on(EventForward &event) {
        EXPECT_TRUE(event.checkSum());
        ++_received;
        g_answers_home.fetch_add(1, std::memory_order_relaxed);
        finishIfDone();
    }
};

// Replies TestEvents to their source and forwards EventForwards back to their source.
class ResponderActor final : public qb::Actor {
    int _handled = 0;

public:
    ~ResponderActor() final {
        // A cross-core responder (non-zero index) additionally receives the two broadcast copies.
        EXPECT_EQ(_handled, id().index() ? 4 : 2);
    }

    qb::io::async::task<bool>
    onInit() final {
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
        registerEvent<TestEvent>(*this);
        registerEvent<EventForward>(*this);
        co_return true;
    }

    void
    on(TestEvent &event) {
        EXPECT_TRUE(event.checkSum());
        reply(event); // return to source
        ++_handled;
    }
    void
    on(EventForward &event) {
        EXPECT_TRUE(event.checkSum());
        forward(event.getSource(), event); // re-route preserving source
        ++_handled;
    }
};

TEST(MessagingReplyForward, SameCore) {
    g_answers_home = 0;
    qb::Main main;
    main.addActor<RequesterActor>(0, main.addActor<ResponderActor>(0));
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_answers_home.load(), 2); // one reply + one forward came home
}

TEST(MessagingReplyForward, CrossCore) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: cross-core reply/forward needs a second core";
    g_answers_home = 0;
    qb::Main main;
    main.addActor<RequesterActor>(0, main.addActor<ResponderActor>(1));
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_answers_home.load(), 2); // direct reply + forward home (broadcasts are extra peer load)
}

// ---------------------------------------------------------------------------------------------
// RelayChain: one event, H hops, one destructor.
// ---------------------------------------------------------------------------------------------

static std::atomic<int> g_relay_constructed{0};
static std::atomic<int> g_relay_destroyed{0};
static std::atomic<int> g_relay_handled{0};

// Non-trivially destructible on purpose: this is the event the dispatcher must decide the fate
// of after every handler (`~_Event()` unless `is_alive()`). `push`-only — `send` refuses it.
struct RelayEvent : qb::Event {
    int           hops_left;
    std::uint32_t chain; // which chain this event belongs to (payload must survive every copy)

    RelayEvent(int hops, std::uint32_t chain_id) noexcept
        : hops_left(hops)
        , chain(chain_id) {
        g_relay_constructed.fetch_add(1, std::memory_order_relaxed);
    }
    ~RelayEvent() {
        g_relay_destroyed.fetch_add(1, std::memory_order_relaxed);
    }
};

// Bounces every RelayEvent straight back to its source, decrementing the hop count.
class RelayPeer final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<RelayEvent>(*this);
        co_return true;
    }
    void
    on(RelayEvent &event) {
        --event.hops_left;
        g_relay_handled.fetch_add(1, std::memory_order_relaxed);
        reply(event); // the bytes move on; this frame's copy must NOT be destructed
    }
};

// Forwards every RelayEvent to `_next` unchanged (source preserved) — the forward() leg.
class RelayForwarder final : public qb::Actor {
    const qb::ActorId _next;

public:
    explicit RelayForwarder(qb::ActorId const next)
        : _next(next) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<RelayEvent>(*this);
        co_return true;
    }
    void
    on(RelayEvent &event) {
        g_relay_handled.fetch_add(1, std::memory_order_relaxed);
        forward(_next, event);
    }
};

// Starts `chains` chains of `hops` bounces against `_peer` and keeps replying until each
// event's hop count reaches zero; the LAST handler keeps the event, and that is where the one
// destructor must run. Kills every core's actors once the last chain is home.
class RelayOrigin final : public qb::Actor {
    const qb::ActorId _peer;
    const qb::CoreId  _peer_core;
    const int         _chains;
    const int         _hops;
    int               _home = 0;

public:
    RelayOrigin(qb::ActorId const peer, qb::CoreId const peer_core, int const chains, int const hops)
        : _peer(peer)
        , _peer_core(peer_core)
        , _chains(chains)
        , _hops(hops) {}

    ~RelayOrigin() final {
        EXPECT_EQ(_home, _chains);
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<RelayEvent>(*this);
        for (int c = 0; c < _chains; ++c)
            push<RelayEvent>(_peer, _hops, static_cast<std::uint32_t>(c));
        co_return true;
    }

    void
    on(RelayEvent &event) {
        g_relay_handled.fetch_add(1, std::memory_order_relaxed);
        EXPECT_LT(event.chain, static_cast<std::uint32_t>(_chains)) << "payload corrupted across a copy";
        if (event.hops_left > 0) {
            reply(event); // bounce again: a reply of an event that arrived as a reply
            return;
        }
        // Chain complete: the event stays here and the dispatcher destroys it once.
        if (++_home == _chains) {
            if (_peer_core != id().index())
                push<qb::KillEvent>(qb::BroadcastId(_peer_core));
            push<qb::KillEvent>(qb::BroadcastId(id().index()));
        }
    }
};

namespace {
struct RelayTally {
    int constructed, destroyed, handled;
};
RelayTally
run_relay(qb::Main &main, int const expected_handled) {
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    RelayTally t{g_relay_constructed.load(), g_relay_destroyed.load(), g_relay_handled.load()};
    EXPECT_EQ(t.handled, expected_handled) << "every hop must be handled exactly once";
    return t;
}
void
reset_relay() {
    g_relay_constructed = 0;
    g_relay_destroyed   = 0;
    g_relay_handled     = 0;
}
} // namespace

// Same core: reply() is event_wire::copy into the core's own pipe; the original is raised
// alive after the copy and skipped by the dispatcher, the copy is born dead and destroyed at the
// end of the chain. 8 chains × 5 hops: 8 constructions, 8 destructions, 8 × (5 + 5) handlers
// (each bounce is one peer handler + one origin handler; the final origin handler keeps it).
TEST(RelayChain, SameCoreReplyChainDestroysEachEventExactlyOnce) {
    reset_relay();
    constexpr int chains = 8, hops = 5;
    qb::Main      main;
    const auto    peer = main.addActor<RelayPeer>(0);
    main.addActor<RelayOrigin>(0, peer, qb::CoreId{0}, chains, hops);
    const auto t = run_relay(main, chains * hops * 2);
    EXPECT_EQ(t.constructed, chains);
    EXPECT_EQ(t.destroyed, chains) << "an event replied H times must be destroyed once, at its final handler";
}

// Cross core: reply() copies through the peer's mailbox (SharedCoreCommunication::send) instead
// of a local pipe. The original's `alive` is raised only AFTER that copy, so the relocated bytes
// carry 0 whatever the original then says — the same ownership answer on the other transport.
TEST(RelayChain, CrossCoreReplyChainDestroysEachEventExactlyOnce) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: cross-core reply needs a second core";
    reset_relay();
    constexpr int chains = 8, hops = 5;
    qb::Main      main;
    const auto    peer = main.addActor<RelayPeer>(1);
    main.addActor<RelayOrigin>(0, peer, qb::CoreId{1}, chains, hops);
    const auto t = run_relay(main, chains * hops * 2);
    EXPECT_EQ(t.constructed, chains);
    EXPECT_EQ(t.destroyed, chains);
}

// forward() leg: origin → forwarder → peer (reply to the preserved source) → origin. Each hop
// adds one forwarder handler; forward()'s copy (event_wire::set_dest + copy) is held to the
// same once-only destruction as reply()'s.
TEST(RelayChain, SameCoreForwardChainDestroysEachEventExactlyOnce) {
    reset_relay();
    constexpr int chains = 4, hops = 3;
    qb::Main      main;
    const auto    peer      = main.addActor<RelayPeer>(0);
    const auto    forwarder = main.addActor<RelayForwarder>(0, peer);
    // First leg: origin → forwarder → peer, with forward() preserving the origin as source, so
    // the peer's reply lands at the origin; every later bounce is origin ⇄ peer directly. The
    // forwarder is therefore on one leg per chain: handled = chains × (1 + hops peer + hops origin).
    main.addActor<RelayOrigin>(0, forwarder, qb::CoreId{0}, chains, hops);
    const auto t = run_relay(main, chains * (1 + hops * 2));
    EXPECT_EQ(t.constructed, chains);
    EXPECT_EQ(t.destroyed, chains);
}

// ---------------------------------------------------------------------------------------------
// Fan-out: reply(e) then forward(sink, e) on ONE event. The second rewrite starts from an
// original that reply() just raised alive; the copy it makes — local pipe or cross-core
// mailbox — must still be born dead, or the sink's dispatcher skips its destructor and the
// payload leaks. RelayEvent's payload is two integers, so two byte-copies are sound.
// ---------------------------------------------------------------------------------------------

static std::atomic<int> g_fanout_kept{0};

// The origin and the sink both KEEP their copy; whoever keeps the last one tears down.
static void
fanout_maybe_finish(qb::Actor &self, int const expected, qb::CoreId const other_core) {
    if (g_fanout_kept.fetch_add(1, std::memory_order_relaxed) + 1 != expected)
        return;
    if (other_core != self.id().index())
        self.push<qb::KillEvent>(qb::BroadcastId(other_core));
    self.push<qb::KillEvent>(qb::BroadcastId(self.id().index()));
}

class FanOutActor final : public qb::Actor {
    const qb::ActorId _sink;

public:
    explicit FanOutActor(qb::ActorId const sink)
        : _sink(sink) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<RelayEvent>(*this);
        co_return true;
    }
    void
    on(RelayEvent &event) {
        g_relay_handled.fetch_add(1, std::memory_order_relaxed);
        reply(event);          // copy 1 → source
        forward(_sink, event); // copy 2 → sink, from an original that now says alive
    }
};

class FanOutSink final : public qb::Actor {
    const int        _expected;
    const qb::CoreId _other_core;

public:
    FanOutSink(int const expected, qb::CoreId const other_core)
        : _expected(expected)
        , _other_core(other_core) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<RelayEvent>(*this);
        co_return true;
    }
    void
    on(RelayEvent &event) {
        g_relay_handled.fetch_add(1, std::memory_order_relaxed);
        EXPECT_EQ(event.hops_left, 0);
        fanout_maybe_finish(*this, _expected, _other_core); // kept: destroyed when this returns
    }
};

class FanOutOrigin final : public qb::Actor {
    const qb::ActorId _fanout;
    const int         _events;
    const int         _expected_kept;
    const qb::CoreId  _other_core;

public:
    FanOutOrigin(qb::ActorId const fanout, int const events, qb::CoreId const other_core)
        : _fanout(fanout)
        , _events(events)
        , _expected_kept(2 * events)
        , _other_core(other_core) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<RelayEvent>(*this);
        for (int i = 0; i < _events; ++i)
            push<RelayEvent>(_fanout, 0, static_cast<std::uint32_t>(i));
        co_return true;
    }
    void
    on(RelayEvent &event) {
        g_relay_handled.fetch_add(1, std::memory_order_relaxed);
        EXPECT_EQ(event.hops_left, 0);
        fanout_maybe_finish(*this, _expected_kept, _other_core);
    }
};

TEST(RelayChain, SameCoreReplyThenForwardFanOutDestroysBothCopies) {
    reset_relay();
    g_fanout_kept        = 0;
    constexpr int events = 6;
    qb::Main      main;
    const auto    sink   = main.addActor<FanOutSink>(0, 2 * events, qb::CoreId{0});
    const auto    fanout = main.addActor<FanOutActor>(0, sink);
    main.addActor<FanOutOrigin>(0, fanout, events, qb::CoreId{0});
    const auto t = run_relay(main, 3 * events);
    EXPECT_EQ(t.constructed, events);
    EXPECT_EQ(t.destroyed, 2 * events) << "reply + forward of one event are two owning copies";
}

// The fan-out actor sits on the other core, so BOTH copies travel through core 0's mailbox —
// the raw memcpy transport. The forward()'s copy is taken from an original reply() just raised
// alive; set_dest's rewrite must have cleared it, or the sink never destroys its copy.
TEST(RelayChain, CrossCoreReplyThenForwardFanOutDestroysBothCopies) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the fan-out must cross a mailbox";
    reset_relay();
    g_fanout_kept        = 0;
    constexpr int events = 6;
    qb::Main      main;
    const auto    sink   = main.addActor<FanOutSink>(0, 2 * events, qb::CoreId{1});
    const auto    fanout = main.addActor<FanOutActor>(1, sink);
    main.addActor<FanOutOrigin>(0, fanout, events, qb::CoreId{1});
    const auto t = run_relay(main, 3 * events);
    EXPECT_EQ(t.constructed, events);
    EXPECT_EQ(t.destroyed, 2 * events);
}
