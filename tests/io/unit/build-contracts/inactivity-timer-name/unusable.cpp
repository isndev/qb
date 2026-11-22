/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/inactivity-timer-name/unusable.cpp
 * @brief A session with a member `inactivity_timer_t` that `io_handler::extractSession()` cannot use does not compile
 *        where it is extracted (Huly QB-1248).
 *
 * Ignored, such a name let the extraction stop the session's own timer, or none: the session, held past its
 * extraction, was timed out on a connection its next owner has. `take_transport_()` refuses it with a static_assert,
 * after a probe that finds a member of that name whatever its access or kind -- here a data member, which a plain
 * `typename _Session::inactivity_timer_t` does not see.
 * This unit is built on demand by the ctest entry `qb-io-test-unit-inactivity-timer-name`, which passes when the
 * build fails with the static_assert's message as its only error -- a unit that compiles, or one refused for another
 * reason too, fails it. Built with `QB_CONTRACT_CONTROL`, the name is a usable public alias: that unit is the target of
 * the entry's name, built and linked with the suite, so the contract's unit is otherwise sound.
 */

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>

namespace inactivity_timer_name_contract {
using namespace qb::io;

class Server;

/// The session's inactivity timer, a base of its own type.
struct Timer : async::with_timeout<Timer> {
    Timer()
        : async::with_timeout<Timer>(qb::duration::zero()) {}
    void
    on(async::event::timer const &) {}
};

class Session
    : public use<Session>::tcp::client<Server>
    , public Timer {
public:
    using Protocol = qb::protocol::text::command<Session>;
#ifdef QB_CONTRACT_CONTROL
    using inactivity_timer_t = Timer; // a public alias of a public base with setTimeout(qb::duration): used
#else
    int inactivity_timer_t = 0; // a member of that name that is no alias: refused, by every compiler with the message
#endif
    explicit Session(Server &server);
    void
    on(Protocol::message &&) {}
};

class Server : public use<Server>::tcp::server<Session> {};

Session::Session(Server &server)
    : client(server) {}

/// What instantiates the extraction; never called.
[[maybe_unused]] inline void
extract(Server &server, qb::uuid const &id) {
    auto taken = server.extractSession(id);
    (void) taken;
}
} // namespace inactivity_timer_name_contract

int
main() {
    return 0;
}
