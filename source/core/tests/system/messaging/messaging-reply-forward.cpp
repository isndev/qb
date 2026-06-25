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
 */

#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <qb/actor.h>
#include <qb/main.h>
#include "../../shared/ChecksumEvent.h"

using qb::test::TestEvent;

// EventForward is a distinct type so the responder can route it through forward() (vs reply()).
struct EventForward : public TestEvent {};

static std::atomic<int> g_answers_home{0};   // replies + forwards observed back at the requester

// Sends one TestEvent (expects a reply) and one EventForward (expects a forward-back); on the
// cross-core variant it additionally broadcasts both to the peer core index first.
class RequesterActor final : public qb::Actor {
    const qb::ActorId _to;
    int               _received = 0;

    void finishIfDone() {
        if (_received < 2)
            return;
        // Both answers are home: tear the (possibly multi-actor) test down deterministically.
        push<qb::KillEvent>(qb::BroadcastId(_to.index()));
        kill();
    }

public:
    explicit RequesterActor(qb::ActorId const to) : _to(to) {}

    ~RequesterActor() final { EXPECT_EQ(_received, 2); }

    qb::io::async::task<bool> onInit() final {
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
        registerEvent<TestEvent>(*this);
        registerEvent<EventForward>(*this);

        if (_to.index()) {  // cross-core: also exercise the broadcast-to-core path
            push<TestEvent>(qb::BroadcastId(_to.index()));
            push<EventForward>(qb::BroadcastId(_to.index()));
        }
        push<TestEvent>(_to);
        push<EventForward>(_to);
        co_return true;
    }

    void on(TestEvent &event) {
        EXPECT_TRUE(event.checkSum());
        ++_received;
        g_answers_home.fetch_add(1, std::memory_order_relaxed);
        finishIfDone();
    }
    void on(EventForward &event) {
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

    qb::io::async::task<bool> onInit() final {
        EXPECT_NE(static_cast<std::uint32_t>(id()), 0u);
        registerEvent<TestEvent>(*this);
        registerEvent<EventForward>(*this);
        co_return true;
    }

    void on(TestEvent &event) {
        EXPECT_TRUE(event.checkSum());
        reply(event);                       // return to source
        ++_handled;
    }
    void on(EventForward &event) {
        EXPECT_TRUE(event.checkSum());
        forward(event.getSource(), event);  // re-route preserving source
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
    EXPECT_EQ(g_answers_home.load(), 2);   // one reply + one forward came home
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
    EXPECT_EQ(g_answers_home.load(), 2);   // direct reply + forward home (broadcasts are extra peer load)
}
