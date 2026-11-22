/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/inactivity-timer-name/itself.cpp
 * @brief A session whose `inactivity_timer_t` names the session itself -- here through an alias of a reference to it
 *        -- does not compile where it is extracted (Huly QB-1248).
 *
 * The name must be a public base of the session; the session itself is none (the twentieth independent review), with
 * its reference and its cv-qualifiers removed. This unit is built on demand by the ctest entry
 * `qb-io-test-unit-inactivity-timer-itself`, which passes when the build fails with the static_assert's message as its
 * only error. Built with `QB_CONTRACT_CONTROL`, the name is an alias of a reference to the session's
 * `use<T>::timeout` base, which counts as the base itself (the twenty-first independent review): that unit is the
 * target of the entry's name, built and linked with the suite.
 */

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>

namespace inactivity_timer_itself_contract {
using namespace qb::io;

class Server;

class Session
    : public use<Session>::tcp::client<Server>
    , public use<Session>::timeout {
public:
    using Protocol = qb::protocol::text::command<Session>;
#ifdef QB_CONTRACT_CONTROL
    using inactivity_timer_t = async::with_timeout<Session> &; // a reference to a public with_timeout base: used
#else
    using inactivity_timer_t = Session &; // a reference to the session itself, no base of its own: refused
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
} // namespace inactivity_timer_itself_contract

int
main() {
    return 0;
}
