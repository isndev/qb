/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/inactivity-timer-name/not-a-timer.cpp
 * @brief A session whose `inactivity_timer_t` names a public base that is no `with_timeout` and has none -- a base
 *        whose own `setTimeout()` forwards to a timer it holds -- does not compile where it is extracted (Huly QB-1248).
 *
 * Accepted, the extraction called that `setTimeout()`, which stopped the member it holds and left the session's
 * inactivity timer armed: the session, held past its extraction, was timed out on a connection its next owner has (the
 * twenty-first independent review). The named base must be a `with_timeout`, or have one, stopped through it. This
 * unit is built on demand by the ctest entry `qb-io-test-unit-inactivity-timer-not-a-timer`, which passes when the
 * build fails with the static_assert's message as its only error. Built with `QB_CONTRACT_CONTROL`, the name is the
 * session's `use<T>::timeout` base: that unit is the target of the entry's name, built and linked with the suite.
 */

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>

namespace inactivity_timer_not_a_timer_contract {
using namespace qb::io;

class Server;

/// A timer of its own.
struct Held : async::with_timeout<Held> {
    Held()
        : async::with_timeout<Held>(qb::duration::zero()) {}
    void
    on(async::event::timer const &) {}
};

/// A public base with a `setTimeout()` of its own, which reaches the timer it holds -- not the session's.
struct Forwarder {
    Held held;
    void
    setTimeout(qb::duration timeout) noexcept {
        held.setTimeout(timeout);
    }
};

class Session
    : public use<Session>::tcp::client<Server>
    , public use<Session>::timeout
    , public Forwarder {
public:
    using Protocol = qb::protocol::text::command<Session>;
#ifdef QB_CONTRACT_CONTROL
    using inactivity_timer_t = async::with_timeout<Session>; // a with_timeout base: used
#else
    using inactivity_timer_t = Forwarder; // a base that is no with_timeout and has none: refused
#endif
    explicit Session(Server &server);
    void
    on(Protocol::message &&) {}
    void
    on(async::event::timer const &) {}
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
} // namespace inactivity_timer_not_a_timer_contract

int
main() {
    return 0;
}
