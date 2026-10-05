/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/async/handler-detection.cpp
 * @brief What `qb::has_on` and `qb::has_own_on` can see -- the traits that gate every optional
 *        event the io bases, the acceptor and the modules dispatch (Huly QB-252).
 *
 * A gated dispatch (`if constexpr (qb::has_on<D, Evt>) d.on(...)`) skips a handler the trait
 * cannot see, with no diagnostic. Access is checked where the detection is WRITTEN -- inside the
 * `has_method_on` struct QB_DEFINE_METHOD_TRAIT generates -- so a private handler is seen only
 * when its class befriends that struct; befriending the CRTP base that dispatches is not enough.
 * `qb::has_own_on` used to run in a qb::detail function that no friend declaration reaches: a
 * private handler was reported absent even WITH the friend, and the acceptor threw "Acceptor has
 * been disconnected" instead of calling the qbm-http servers' handler. It now reads the same
 * struct, so one friend declaration serves both traits.
 *
 * Every case is a static_assert (a regression breaks the build, on every compiler) and is
 * re-stated as a gtest case for the report; the last two cases run a CRTP dispatch shaped like
 * the acceptor's.
 */

#include <gtest/gtest.h>
#include <qb/utility/type_traits.h>

namespace handler_detection_test {

struct Evt {};

// A CRTP base carrying its own fallback handler, as the acceptor and tcp::server do.
template <typename D>
struct PublicFallback {
    void
    on(Evt &&) {}
};

// The same with the fallback private, as the framework bases keep theirs.
template <typename D>
class PrivateFallback {
    void
    on(Evt &&) {}
};

struct PublicOwn : PublicFallback<PublicOwn> {
    void
    on(Evt &&) {}
};

struct Inherits : PublicFallback<Inherits> {};

struct ConstRefOwn : PublicFallback<ConstRefOwn> {
    void
    on(Evt const &) {}
};

class PrivateNoFriend : public PublicFallback<PrivateNoFriend> {
    void
    on(Evt &&) {}
};

class PrivateFriendDetector : public PrivateFallback<PrivateFriendDetector> {
    friend struct has_method_on<PrivateFriendDetector, void, Evt>;
    void
    on(Evt &&) {}
};

class PrivateFriendAll : public PrivateFallback<PrivateFriendAll> {
    template <typename, typename, typename...>
    friend struct ::has_method_on;
    void
    on(Evt &&) {}
};

class PrivateFriendBaseOnly : public PublicFallback<PrivateFriendBaseOnly> {
    friend struct PublicFallback<PrivateFriendBaseOnly>;
    void
    on(Evt &&) {}
};

struct LvalueOnly {
    void
    on(Evt &) {}
};

// --- qb::has_on: "can this event be delivered to D at all" ----------------------------------
static_assert(qb::has_on<PublicOwn, Evt>);
static_assert(qb::has_on<Inherits, Evt>, "has_on sees an inherited handler -- why has_own_on exists");
static_assert(qb::has_on<ConstRefOwn, Evt>);
static_assert(!qb::has_on<PrivateNoFriend, Evt>, "a private handler without the friend is invisible");
static_assert(qb::has_on<PrivateFriendDetector, Evt>);
static_assert(qb::has_on<PrivateFriendAll, Evt>);
static_assert(!qb::has_on<PrivateFriendBaseOnly, Evt>, "befriending the dispatching base is not enough");
static_assert(!qb::has_on<LvalueOnly, Evt>, "events are delivered as rvalues: on(Evt&) never binds");

// --- qb::has_own_on: "does D carry its own handler, distinct from Base's" -------------------
static_assert(qb::has_own_on<PublicOwn, PublicFallback<PublicOwn>, Evt>);
static_assert(!qb::has_own_on<Inherits, PublicFallback<Inherits>, Evt>, "an inherited fallback is not D's own");
static_assert(qb::has_own_on<ConstRefOwn, PublicFallback<ConstRefOwn>, Evt>);
static_assert(!qb::has_own_on<PrivateNoFriend, PublicFallback<PrivateNoFriend>, Evt>);
static_assert(qb::has_own_on<PrivateFriendDetector, PrivateFallback<PrivateFriendDetector>, Evt>,
              "the friend declaration that serves has_on serves has_own_on (Huly QB-252)");
static_assert(qb::has_own_on<PrivateFriendAll, PrivateFallback<PrivateFriendAll>, Evt>);
static_assert(!qb::has_own_on<PrivateFriendBaseOnly, PublicFallback<PrivateFriendBaseOnly>, Evt>);

// A CRTP dispatcher shaped like the acceptor's on(event::disconnected&&): the derived class's own
// handler when it has one, the fallback otherwise -- never a re-dispatch to the fallback itself.
template <typename D>
struct Dispatcher {
    int fallback_calls = 0;
    void
    dispatch() {
        if constexpr (qb::has_own_on<D, Dispatcher, Evt>)
            static_cast<D &>(*this).on(Evt{});
        else
            ++fallback_calls;
    }
    void
    on(Evt &&) {
        ++fallback_calls;
    }
};

class PrivateHandler : public Dispatcher<PrivateHandler> {
    friend struct has_method_on<PrivateHandler, void, Evt>;
    friend struct Dispatcher<PrivateHandler>;
    void
    on(Evt &&) {
        ++own_calls;
    }

public:
    int own_calls = 0;
};

struct NoHandler : Dispatcher<NoHandler> {};

// The runtime restatements below go through these, so each check is one call with no comma.
template <typename D>
constexpr bool
sees_on() {
    return qb::has_on<D, Evt>;
}
template <typename D, template <typename> class Base>
constexpr bool
sees_own() {
    return qb::has_own_on<D, Base<D>, Evt>;
}

} // namespace handler_detection_test

using namespace handler_detection_test;

TEST(HandlerDetection, HasOnSeesPublicInheritedAndBefriendedHandlers) {
    EXPECT_TRUE(sees_on<PublicOwn>());
    EXPECT_TRUE(sees_on<Inherits>());
    EXPECT_TRUE(sees_on<ConstRefOwn>());
    EXPECT_TRUE(sees_on<PrivateFriendDetector>());
    EXPECT_TRUE(sees_on<PrivateFriendAll>());
}

TEST(HandlerDetection, HasOnMissesPrivateUnbefriendedAndLvalueHandlers) {
    EXPECT_FALSE(sees_on<PrivateNoFriend>());
    EXPECT_FALSE(sees_on<PrivateFriendBaseOnly>()) << "befriending the dispatching base is not enough";
    EXPECT_FALSE(sees_on<LvalueOnly>());
}

TEST(HandlerDetection, HasOwnOnHonoursTheSameFriendDeclaration) {
    EXPECT_TRUE((sees_own<PrivateFriendDetector, PrivateFallback>()));
    EXPECT_TRUE((sees_own<PrivateFriendAll, PrivateFallback>()));
    EXPECT_TRUE((sees_own<PublicOwn, PublicFallback>()));
    EXPECT_TRUE((sees_own<ConstRefOwn, PublicFallback>()));
}

TEST(HandlerDetection, HasOwnOnRejectsInheritedAndInvisibleHandlers) {
    EXPECT_FALSE((sees_own<Inherits, PublicFallback>()));
    EXPECT_FALSE((sees_own<PrivateNoFriend, PublicFallback>()));
    EXPECT_FALSE((sees_own<PrivateFriendBaseOnly, PublicFallback>()));
}

TEST(HandlerDetection, CrtpDispatchReachesABefriendedPrivateHandler) {
    PrivateHandler h;
    h.dispatch();
    EXPECT_EQ(h.own_calls, 1) << "the derived class's private handler must be called";
    EXPECT_EQ(h.fallback_calls, 0);
}

TEST(HandlerDetection, CrtpDispatchFallsBackWithoutAnOwnHandler) {
    NoHandler n;
    n.dispatch();
    EXPECT_EQ(n.fallback_calls, 1) << "no own handler: the fallback branch, not a re-dispatch";
}
