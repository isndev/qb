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
 *
 * The third part pins the ORDER contracts (Huly QB-976): `reply()`/`forward()` are eager and
 * unordered -- cross-core the answer precedes the handler's earlier pushes to the same actor
 * (ReplyIsImmediate*) -- and `push_reply()`/`push_forward()` are the push-family answers,
 * FIFO with those pushes, same core and cross-core (PushReply*).
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/system/time.h>
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
    const bool        _allocated_tail;

    void
    finishIfDone() {
        if (_received < 2)
            return;
        // Both answers are home: tear the (possibly multi-actor) test down deterministically.
        push<qb::KillEvent>(qb::BroadcastId(_to.index()));
        kill();
    }

public:
    explicit RequesterActor(qb::ActorId const to, bool allocated_tail = false)
        : _to(to)
        , _allocated_tail(allocated_tail) {}

    ~RequesterActor() final {
        EXPECT_EQ(_received, 2);
    }

    qb::io::async::task<bool>
    onInit() final {
        EXPECT_NE(id().raw64(), 0u);
        registerEvent<TestEvent>(*this);
        registerEvent<EventForward>(*this);

        if (_to.index()) { // cross-core: also exercise the broadcast-to-core path
            push<TestEvent>(qb::BroadcastId(_to.index()));
            push<EventForward>(qb::BroadcastId(_to.index()));
        }
        if (_allocated_tail) {
            auto &event = getPipe(_to).allocated_push<TestEvent>(32);
            std::fill(std::begin(event._data), std::end(event._data), std::uint8_t{0x5a});
            event._sum           = 32 * 0x5a;
            event.has_extra_data = true;
            qb::test::copyAllocatedPayload(event);
        } else {
            push<TestEvent>(_to);
        }
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
        EXPECT_NE(id().raw64(), 0u);
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
// of after every handler (`~_Event()` unless `is_alive()`). Its integer payload owns no resource.
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
    const bool        _raw_copy;

public:
    explicit FanOutActor(qb::ActorId const sink, bool raw_copy = false)
        : _sink(sink)
        , _raw_copy(raw_copy) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<RelayEvent>(*this);
        co_return true;
    }
    void
    on(RelayEvent &event) {
        g_relay_handled.fetch_add(1, std::memory_order_relaxed);
        reply(event); // copy 1 → source
        if (_raw_copy) {
            EXPECT_TRUE(event.is_alive());
            send(event); // same destination, without a rewrite that would clear the original flag
        } else {
            forward(_sink, event); // copy 2 → sink, from an original that now says alive
        }
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

// Raw send preserves the destination already written by reply(). Both copies return
// to the origin; the second must clear the inherited alive flag on both transports.
TEST(RelayChain, SameCoreReplyThenRawSendDestroysBothCopies) {
    reset_relay();
    g_fanout_kept        = 0;
    constexpr int events = 6;
    qb::Main      main;
    const auto    fanout = main.addActor<FanOutActor>(0, qb::ActorId{}, true);
    main.addActor<FanOutOrigin>(0, fanout, events, qb::CoreId{0});
    const auto t = run_relay(main, 3 * events);
    EXPECT_EQ(t.constructed, events);
    EXPECT_EQ(t.destroyed, 2 * events);
}

TEST(RelayChain, CrossCoreReplyThenRawSendDestroysBothCopies) {
    if (std::thread::hardware_concurrency() < 2u) {
        GTEST_SKIP() << "requires-multicore: raw send must cross a mailbox";
    }
    reset_relay();
    g_fanout_kept        = 0;
    constexpr int events = 6;
    qb::Main      main;
    const auto    fanout = main.addActor<FanOutActor>(1, qb::ActorId{}, true);
    main.addActor<FanOutOrigin>(0, fanout, events, qb::CoreId{1});
    const auto t = run_relay(main, 3 * events);
    EXPECT_EQ(t.constructed, events);
    EXPECT_EQ(t.destroyed, 2 * events);
}

// ---------------------------------------------------------------------------------------------
// The ORDER contracts of the four answers (Huly QB-976).
// ---------------------------------------------------------------------------------------------
//
// `reply()` and `forward()` are eager: across cores `VirtualCore::send(Event const&)` publishes the
// answer straight into the peer's ring, ahead of everything the outbound pipe to that core still
// holds for the pass-end flush -- an answer can reach its destination BEFORE an event the same
// handler pushed to it earlier. That is the documented, unordered contract (send-family), and the
// property a peer waiting on nothing else is served by. `push_reply()` and `push_forward()`
// are the push-family answers: the copy goes into the pipe behind everything this actor pushed to
// the destination, and the flush delivers them in order.
//
// Witnesses, none of which depends on timing:
//  * ORDERED: the responder pushes `markers` events to the recorder, then hands the probe on with
//    the ordered spelling (`push_reply()`, or `push_forward()` to the recorder as the sink);
//    the recorder must see 1..markers, then the probe -- same core and cross-core, and through a
//    flood wider than one flush run (300 events, runs of at most 256 buckets).
//  * UNORDERED, BY CONTRACT: the same shape with `reply()` cross-core: the probe arrives FIRST,
//    deterministically -- it is in the recorder's ring before the responder's handler returns, the
//    markers enter that ring only at the flush, behind it (one producer, one FIFO ring).
//  * EAGER: the responder pushes two markers to one actor, `reply()`s the probe to another and spins
//    in its handler until the prober acknowledges -- which only a direct publish allows.

namespace messaging_reply_forward_test {

enum class Answer : int { reply = 0, forward = 1, push_reply = 2, push_forward = 3 };

struct OrderProbe : qb::Event {
    int         seq;     ///< the probe's rank in the sequence the recorder expects
    int         markers; ///< how many markers the responder pushes before the probe
    qb::ActorId sink;    ///< the recorder: where the markers go, and the forward's destination
    Answer      answer;  ///< the spelling the responder hands the probe on with

    OrderProbe(int const s, int const m, qb::ActorId const k, Answer const a) noexcept
        : seq(s)
        , markers(m)
        , sink(k)
        , answer(a) {}
};

struct OrderMarker : qb::Event {
    int seq;

    explicit OrderMarker(int const s) noexcept
        : seq(s) {}
};

// Pushes the markers to the probe's sink, then hands the probe on: what the sink sees first is the
// contract under test.
class OrderResponder final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<OrderProbe>(*this);
        co_return true;
    }

    void
    on(OrderProbe &event) {
        for (int i = 1; i <= event.markers; ++i)
            push<OrderMarker>(event.sink, i);
        switch (event.answer) {
            case Answer::reply:
                reply(event);
                break;
            case Answer::forward:
                forward(event.sink, event); // the sink is the probe's source: a forward() back to it
                break;
            case Answer::push_reply:
                push_reply(event);
                break;
            case Answer::push_forward:
                push_forward(event.sink, event);
                break;
        }
    }
};

// Sends the probe, records the order everything arrives in, tears down once all is home.
class OrderRecorder final : public qb::Actor {
    const qb::ActorId      _responder;
    const qb::CoreId       _responder_core;
    const int              _markers;
    const Answer           _answer;
    const std::vector<int> _expected;
    std::vector<int>       _order;

    void
    note(int const seq) {
        _order.push_back(seq);
        if (_order.size() < _expected.size())
            return;
        EXPECT_EQ(_order, _expected);
        if (_responder_core != id().index())
            push<qb::KillEvent>(qb::BroadcastId(_responder_core));
        push<qb::KillEvent>(qb::BroadcastId(id().index()));
    }

public:
    OrderRecorder(qb::ActorId const responder, qb::CoreId const responder_core, int const markers, Answer const answer,
                  std::vector<int> expected)
        : _responder(responder)
        , _responder_core(responder_core)
        , _markers(markers)
        , _answer(answer)
        , _expected(std::move(expected)) {}

    ~OrderRecorder() final {
        EXPECT_EQ(_order.size(), _expected.size()) << "an event never arrived";
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<OrderMarker>(*this);
        registerEvent<OrderProbe>(*this);
        push<OrderProbe>(_responder, _markers + 1, _markers, id(), _answer);
        co_return true;
    }

    void
    on(OrderMarker &event) {
        note(event.seq);
    }
    void
    on(OrderProbe &event) {
        note(event.seq);
    }
};

// 1..markers then the probe (`in_order`), or the probe first then 1..markers (`probe_first`).
std::vector<int>
in_order(int const markers) {
    std::vector<int> v(static_cast<std::size_t>(markers) + 1u);
    std::iota(v.begin(), v.end(), 1);
    return v;
}
std::vector<int>
probe_first(int const markers) {
    std::vector<int> v{markers + 1};
    for (int i = 1; i <= markers; ++i)
        v.push_back(i);
    return v;
}

void
run_order(qb::CoreId const responder_core, Answer const answer, int const markers, std::vector<int> expected) {
    qb::Main   main;
    const auto responder = main.addActor<OrderResponder>(responder_core);
    main.addActor<OrderRecorder>(0, responder, responder_core, markers, answer, std::move(expected));
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
}

// The eagerness witness. `g_eager_answered` is set by the prober's handler the moment the answer
// arrives; `g_eager_direct` records whether the responder saw that while its own handler still ran.
static std::atomic<bool> g_eager_answered{false};
static std::atomic<bool> g_eager_direct{false};
static std::atomic<int>  g_eager_markers{0};

struct EagerProbe : qb::Event {
    qb::ActorId other; ///< the actor the markers go to, on the prober's core

    explicit EagerProbe(qb::ActorId const o) noexcept
        : other(o) {}
};

struct EagerMarker : qb::Event {};

// Pushes two markers to `other`, reply()s, then spins until the prober acknowledges the answer.
class EagerResponder final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<EagerProbe>(*this);
        co_return true;
    }

    void
    on(EagerProbe &event) {
        push<EagerMarker>(event.other);
        push<EagerMarker>(event.other);
        reply(event);
        // The answer was published directly if, and only if, the prober sees it while this
        // handler still runs: the markers above leave at the next pass's flush, after it returns,
        // and so would an answer that took the pipe. The bound only ends a failing run.
        const auto deadline = qb::mono_now() + std::chrono::seconds{5};
        while (!g_eager_answered.load(std::memory_order_acquire) && qb::mono_now() < deadline)
            std::this_thread::yield();
        g_eager_direct.store(g_eager_answered.load(std::memory_order_acquire), std::memory_order_release);
    }
};

// The markers' destination: both arrive after the responder's handler returned; it tears down.
class EagerOther final : public qb::Actor {
    const qb::CoreId _responder_core;

public:
    explicit EagerOther(qb::CoreId const responder_core)
        : _responder_core(responder_core) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<EagerMarker>(*this);
        co_return true;
    }

    void
    on(EagerMarker &) {
        if (g_eager_markers.fetch_add(1, std::memory_order_acq_rel) + 1 != 2)
            return;
        push<qb::KillEvent>(qb::BroadcastId(_responder_core));
        push<qb::KillEvent>(qb::BroadcastId(id().index()));
    }
};

class EagerProber final : public qb::Actor {
    const qb::ActorId _responder;
    const qb::ActorId _other;

public:
    EagerProber(qb::ActorId const responder, qb::ActorId const other)
        : _responder(responder)
        , _other(other) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<EagerProbe>(*this);
        push<EagerProbe>(_responder, _other);
        co_return true;
    }

    void
    on(EagerProbe &) {
        g_eager_answered.store(true, std::memory_order_release);
    }
};

} // namespace messaging_reply_forward_test

using messaging_reply_forward_test::Answer;
using messaging_reply_forward_test::in_order;
using messaging_reply_forward_test::probe_first;
using messaging_reply_forward_test::run_order;

TEST(PushReply, SameCorePushReplyFollowsEarlierPushes) {
    run_order(qb::CoreId{0}, Answer::push_reply, 2, in_order(2));
}

TEST(PushReply, SameCorePushForwardFollowsEarlierPushes) {
    run_order(qb::CoreId{0}, Answer::push_forward, 2, in_order(2));
}

TEST(PushReply, CrossCorePushReplyFollowsEarlierPushesToTheSameActor) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the answer and the pushes must cross a mailbox";
    run_order(qb::CoreId{1}, Answer::push_reply, 2, in_order(2));
}

TEST(PushReply, CrossCorePushForwardFollowsEarlierPushesToTheSameActor) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the answer and the pushes must cross a mailbox";
    run_order(qb::CoreId{1}, Answer::push_forward, 2, in_order(2));
}

// Wider than one flush run (256 buckets): the order holds across runs too.
TEST(PushReply, CrossCorePushReplyBehindAFloodKeepsItsPlace) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the answer and the pushes must cross a mailbox";
    run_order(qb::CoreId{1}, Answer::push_reply, 300, in_order(300));
}

// The documented contract of reply(): eager, so cross-core it precedes the pushes made before it.
TEST(ReplyIsImmediate, CrossCoreReplyPrecedesEarlierPushesByContract) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the answer must cross a mailbox to precede the pushes";
    run_order(qb::CoreId{1}, Answer::reply, 2, probe_first(2));
}

TEST(ReplyIsImmediate, CrossCoreForwardPrecedesEarlierPushesByContract) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the answer must cross a mailbox to precede the pushes";
    run_order(qb::CoreId{1}, Answer::forward, 2, probe_first(2));
}

TEST(ReplyIsImmediate, CrossCoreReplyIsPublishedBeforeTheHandlerReturns) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the answer must cross a mailbox while its sender's core is busy";
    using namespace messaging_reply_forward_test;
    g_eager_answered = false;
    g_eager_direct   = false;
    g_eager_markers  = 0;
    qb::Main   main;
    const auto other     = main.addActor<EagerOther>(0, qb::CoreId{1});
    const auto responder = main.addActor<EagerResponder>(1);
    main.addActor<EagerProber>(0, responder, other);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_eager_direct.load()) << "reply() waited for the pass-end flush";
    EXPECT_EQ(g_eager_markers.load(), 2);
}

// Exercise reply past byte 64, including a tail checked after it comes home.
TEST(MessagingReplyForward, SameCoreAllocatedTail) {
    g_answers_home = 0;
    qb::Main main;
    main.addActor<RequesterActor>(0, main.addActor<ResponderActor>(0), true);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(g_answers_home.load(), 2);
}

namespace messaging_reply_wrap_test {

constexpr std::size_t kBucket = QB_LOCKFREE_EVENT_BUCKET_BYTES;
// SharedCoreCommunication uses max_deliverable_buckets as its SPSC capacity;
// spsc::ringbuffer<T, N> allocates N + 1 physical positions, including the empty slot.
constexpr std::size_t kPhysicalSlots = qb::detail::max_deliverable_buckets + 1u;
constexpr std::size_t kRounds        = kPhysicalSlots + 2u;
static_assert(kPhysicalSlots > 3u && kPhysicalSlots % 3u != 0u);

struct OwnedValue;
struct Probe {
    std::array<qb::ActorId, 2> ids{};
    std::array<std::size_t, 2> received{};
    std::array<std::size_t, 2> replied{};
    std::atomic<unsigned>      constructed{0};
    std::atomic<unsigned>      event_destroyed{0};
    std::atomic<unsigned>      owner_destroyed{0};
    OwnedValue                *owner_address = nullptr;
};

struct OwnedValue {
    Probe        &probe;
    std::uint64_t stamp = 0;

    explicit OwnedValue(Probe &p) noexcept
        : probe(p) {}
    ~OwnedValue() {
        probe.owner_destroyed.fetch_add(1, std::memory_order_relaxed);
    }
};

constexpr std::uint64_t
front_value(std::uint64_t const hop) noexcept {
    return 0xfedcba9876543210ULL ^ (hop * 0x9e3779b97f4a7c15ULL);
}

// The first member exercises the beginning of the derived payload, including ABIs
// that reuse Event's tail padding immediately after its occupied 24-byte header.
struct Prefix : qb::Event {
    std::uint64_t               front = 0;
    std::unique_ptr<OwnedValue> owner;
    std::uint64_t               hop = 0;

    explicit Prefix(Probe &p)
        : owner(std::make_unique<OwnedValue>(p)) {
        p.owner_address = owner.get();
        p.constructed.fetch_add(1, std::memory_order_relaxed);
    }
};
static_assert(sizeof(Prefix) <= 2u * kBucket);

struct ThreeBucketEvent : Prefix {
    // Alignment makes this tail start after the prefix's last occupied bucket on
    // either base-tail-padding ABI. It ends at the last byte of bucket three.
    alignas(kBucket) std::array<std::uint8_t, 3u * kBucket - sizeof(Prefix)> tail{};

    explicit ThreeBucketEvent(Probe &p)
        : Prefix(p) {
        set_hop(0);
    }
    ~ThreeBucketEvent() {
        owner->probe.event_destroyed.fetch_add(1, std::memory_order_relaxed);
    }

    static std::uint8_t
    tail_byte(std::uint64_t const step, std::size_t const i) noexcept {
        // Every complete eight-byte word carries the hop in its upper half:
        // stale ring bytes cannot pass merely because an eight-bit pattern repeats.
        const std::uint64_t word = (step << 32u) | (0xa5000000u + static_cast<std::uint32_t>(i / 8u));
        return static_cast<std::uint8_t>(word >> ((i % 8u) * 8u));
    }
    void
    set_hop(std::uint64_t const step) noexcept {
        hop          = step;
        front        = front_value(step);
        owner->stamp = front;
        for (std::size_t i = 0; i < tail.size(); ++i)
            tail[i] = tail_byte(step, i);
    }
};
static_assert(sizeof(ThreeBucketEvent) == 3u * kBucket);
static_assert(qb::allocator::getItemSize<ThreeBucketEvent, EventBucket>() == 3u);

class Endpoint final : public qb::Actor {
    Probe            &_probe;
    const std::size_t _side;

    void
    finish() {
        push<qb::KillEvent>(_probe.ids[1u - _side]);
        kill();
    }

public:
    Endpoint(Probe &probe, std::size_t side)
        : _probe(probe)
        , _side(side) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<ThreeBucketEvent>(*this);
        if (_side == 0u)
            push<ThreeBucketEvent>(_probe.ids[1], _probe);
        co_return true;
    }

    void
    on(ThreeBucketEvent &event) {
        // The actor-local receive count, not the transported hop, is the oracle.
        const std::uint64_t expected = 2u * _probe.received[_side] + (_side == 0u ? 1u : 0u);
        ++_probe.received[_side];
        EXPECT_FALSE(event.is_alive());
        EXPECT_EQ(event.getSize(), 3u * kBucket);
        EXPECT_EQ(event.getDestination().raw64(), _probe.ids[_side].raw64());
        EXPECT_EQ(event.getSource().raw64(), _probe.ids[1u - _side].raw64());
        EXPECT_EQ(event.hop, expected);
        EXPECT_EQ(event.front, front_value(expected));
        const bool owner_ok = event.owner.get() == _probe.owner_address && event.owner != nullptr;
        EXPECT_TRUE(owner_ok);
        if (owner_ok)
            EXPECT_EQ(event.owner->stamp, front_value(expected));

        std::size_t mismatch = event.tail.size();
        for (std::size_t i = 0; i < event.tail.size(); ++i) {
            if (event.tail[i] != ThreeBucketEvent::tail_byte(expected, i)) {
                mismatch = i;
                break;
            }
        }
        EXPECT_EQ(mismatch, event.tail.size()) << "tail byte " << mismatch << ", hop " << expected;
        if (!owner_ok || event.hop != expected || event.front != front_value(expected) || mismatch != event.tail.size()) {
            if (!owner_ok) {
                // An omitted second span can corrupt the pointer representation. This
                // is still the only token: restore its known owner for orderly failure
                // cleanup, without deleting or dereferencing the corrupted pointer.
                (void) event.owner.release();
                event.owner.reset(_probe.owner_address);
            }
            finish();
            return;
        }
        if (_side == 0u && _probe.received[0] == kRounds) {
            finish(); // retain this last relocation: one event and one owner destructor
            return;
        }

        event.set_hop(expected + 1u);
        const auto before = getCoreStats();
        reply(event); // both directions use the public reply path, including wrapped replies
        ++_probe.replied[_side];
        const auto after = getCoreStats();
        EXPECT_EQ(after.events_sent, before.events_sent + 1u);
        EXPECT_EQ(after.buckets_sent, before.buckets_sent + 3u);
        EXPECT_EQ(after.sends_blocked, before.sends_blocked) << "a queued fallback would not witness send_reply_large";
        // Publication transfers ownership: only local/core counters are read after reply.
    }
};

TEST(MessagingReplyForward, CrossCoreThreeBucketOwnedReplyWrapsBothRings) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the owned reply must cross two mailbox rings";
    Probe    probe;
    qb::Main main;
    probe.ids[0] = main.addActor<Endpoint>(0, std::ref(probe), std::size_t{0});
    probe.ids[1] = main.addActor<Endpoint>(1, std::ref(probe), std::size_t{1});
    ASSERT_NE(probe.ids[0].raw64() >> 32u, 0u);
    ASSERT_NE(probe.ids[1].raw64() >> 32u, 0u);
    ASSERT_NE(probe.ids[0].raw64(), probe.ids[1].raw64());

    // Bound a lost/corrupted reply without adding traffic to the ring phase.
    // The timeout is an escape hatch, not an elapsed-time assertion.
    std::promise<void> finished;
    auto               finished_future = finished.get_future();
    std::atomic<bool>  timed_out{false};
    std::thread        watchdog([&] {
        if (finished_future.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
            timed_out.store(true, std::memory_order_relaxed);
            qb::Main::stop();
        }
    });
    main.start(true);
    main.join();
    finished.set_value();
    watchdog.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_EQ(probe.received[0], kRounds) << "watchdog stopped the engine: " << timed_out.load(std::memory_order_relaxed);
    EXPECT_EQ(probe.received[1], kRounds);
    EXPECT_EQ(probe.replied[0], kRounds - 1u);
    EXPECT_EQ(probe.replied[1], kRounds);
    EXPECT_EQ(probe.constructed.load(), 1u);
    EXPECT_EQ(probe.event_destroyed.load(), 1u);
    EXPECT_EQ(probe.owner_destroyed.load(), 1u);
    // With only this token in flight, each ring advances by three. gcd(3, P) == 1
    // and > P replies per direction visit every start position, including P-1 and P-2,
    // irrespective of the initial phase. Removing send_reply_large's second memcpy
    // must corrupt the checked final bucket before either chain can finish.
}

} // namespace messaging_reply_wrap_test

namespace messaging_large_forward_test {

using messaging_reply_wrap_test::front_value;
using messaging_reply_wrap_test::kBucket;
using messaging_reply_wrap_test::ThreeBucketEvent;

struct Probe {
    messaging_reply_wrap_test::Probe wire;
    bool                             remote = true;
    std::array<qb::ActorId, 3>       ids{};
    std::array<unsigned, 3>          received{};
};

// A(core 0) pushes to B(core 0 or 1), B forwards to C(core 0), C replies locally to A.
// With remote B, its physical producer differs from the preserved source identity.
class Endpoint final : public qb::Actor {
    Probe            &_probe;
    const std::size_t _side;

    void
    finish() {
        for (auto const target : _probe.ids)
            if (target != id())
                push<qb::KillEvent>(target);
        kill();
    }

public:
    Endpoint(Probe &probe, std::size_t side)
        : _probe(probe)
        , _side(side) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<ThreeBucketEvent>(*this);
        if (_side == 0u)
            push<ThreeBucketEvent>(_probe.ids[1], _probe.wire);
        co_return true;
    }

    void
    on(ThreeBucketEvent &event) {
        constexpr std::array<std::uint64_t, 3> expected_hops{2, 0, 1};
        const auto                             expected = expected_hops[_side];
        ++_probe.received[_side];
        EXPECT_FALSE(event.is_alive());
        EXPECT_EQ(event.getSize(), 3u * kBucket);
        EXPECT_EQ(event.getDestination().raw64(), _probe.ids[_side].raw64());
        EXPECT_EQ(event.getSource().raw64(), _probe.ids[_side == 0u ? 2u : 0u].raw64());
        EXPECT_EQ(event.hop, expected);
        EXPECT_EQ(event.front, front_value(expected));
        const bool owner_ok = event.owner.get() == _probe.wire.owner_address && event.owner != nullptr;
        EXPECT_TRUE(owner_ok);
        if (owner_ok)
            EXPECT_EQ(event.owner->stamp, front_value(expected));
        std::size_t mismatch = event.tail.size();
        for (std::size_t i = 0; i < event.tail.size(); ++i) {
            if (event.tail[i] != ThreeBucketEvent::tail_byte(expected, i)) {
                mismatch = i;
                break;
            }
        }
        EXPECT_EQ(mismatch, event.tail.size()) << "tail byte " << mismatch << ", hop " << expected;
        if (!owner_ok || event.hop != expected || event.front != front_value(expected) || mismatch != event.tail.size()) {
            if (!owner_ok) {
                (void) event.owner.release();
                event.owner.reset(_probe.wire.owner_address);
            }
            finish();
            return;
        }
        if (_side == 0u) {
            finish(); // the sole event and its owner are destroyed after this final handler
        } else if (_side == 1u) {
            event.set_hop(1);
            const auto before = getCoreStats();
            forward(_probe.ids[2], event);
            const auto after = getCoreStats();
            EXPECT_EQ(after.events_sent, before.events_sent + (_probe.remote ? 1u : 0u));
            EXPECT_EQ(after.buckets_sent, before.buckets_sent + (_probe.remote ? 3u : 0u));
            EXPECT_EQ(after.sends_blocked, before.sends_blocked)
                << "local forwarding does not publish; remote forwarding must witness send_large";
            // No access to the transferred payload after publication.
        } else {
            event.set_hop(2);
            reply(event);
        }
    }
};

void
run_large_forward(bool const remote) {
    Probe probe;
    probe.remote = remote;
    qb::Main main;
    probe.ids[0] = main.addActor<Endpoint>(0, std::ref(probe), std::size_t{0});
    probe.ids[1] = main.addActor<Endpoint>(remote ? qb::CoreId{1} : qb::CoreId{0}, std::ref(probe), std::size_t{1});
    probe.ids[2] = main.addActor<Endpoint>(0, std::ref(probe), std::size_t{2});
    for (auto const actor : probe.ids)
        ASSERT_NE(actor.raw64() >> 32u, 0u);
    ASSERT_NE(probe.ids[0].raw64(), probe.ids[2].raw64());

    std::promise<void> finished;
    auto               done = finished.get_future();
    std::atomic<bool>  timed_out{false};
    std::thread        watchdog([&] {
        if (done.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
            timed_out.store(true, std::memory_order_relaxed);
            qb::Main::stop();
        }
    });
    main.start(true);
    main.join();
    finished.set_value();
    watchdog.join();

    EXPECT_FALSE(timed_out.load(std::memory_order_relaxed));
    EXPECT_FALSE(main.hasError());
    for (auto const received : probe.received)
        EXPECT_EQ(received, 1u);
    EXPECT_EQ(probe.wire.constructed.load(), 1u);
    EXPECT_EQ(probe.wire.event_destroyed.load(), 1u);
    EXPECT_EQ(probe.wire.owner_destroyed.load(), 1u);
}

TEST(MessagingReplyForward, SameCoreThreeBucketOwnedForwardPreservesSourceAndWholePayload) {
    run_large_forward(false);
}

TEST(MessagingReplyForward, CrossCoreThreeBucketOwnedForwardPreservesSourceAndWholePayload) {
    if (std::thread::hardware_concurrency() < 2u)
        GTEST_SKIP() << "requires-multicore: the large forward must cross a mailbox";
    run_large_forward(true);
}

} // namespace messaging_large_forward_test
