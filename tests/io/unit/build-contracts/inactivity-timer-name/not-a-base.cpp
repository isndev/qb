/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/inactivity-timer-name/not-a-base.cpp
 * @brief A session whose `inactivity_timer_t` names a timer it only converts to -- a member reached through a
 *        conversion operator, no base -- does not compile where it is extracted (Huly QB-1248).
 *
 * Accepted, the extraction stopped that member in place of the session's inactivity timer -- a session that had one
 * kept it armed, and was timed out, held past its extraction, on a connection its next owner has (the twentieth
 * independent review; this unit's session has no inactivity timer: the refusal alone is read). The name must be a
 * public base, which a pointer to the session converts to. This unit is built on demand by the ctest entry
 * `qb-io-test-unit-inactivity-timer-not-a-base`, which passes when the build fails with the static_assert's message as
 * its only error. Built with `QB_CONTRACT_CONTROL`, the timer is a public base: that unit is the target of the entry's
 * name, built and linked with the suite.
 */

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>

namespace inactivity_timer_not_a_base_contract {
using namespace qb::io;

class Server;

/// The timer the session names.
struct Timer : async::with_timeout<Timer> {
    Timer()
        : async::with_timeout<Timer>(qb::duration::zero()) {}
    void
    on(async::event::timer const &) {}
};

class Session
    : public use<Session>::tcp::client<Server>
#ifdef QB_CONTRACT_CONTROL
    , public Timer // a public base: used
#endif
{

public:
    using Protocol           = qb::protocol::text::command<Session>;
    using inactivity_timer_t = Timer;
#ifndef QB_CONTRACT_CONTROL
    Timer timer; ///< a member reached through the conversion below, no base: refused
          operator Timer &() noexcept {
        return timer;
    }
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
} // namespace inactivity_timer_not_a_base_contract

int
main() {
    return 0;
}
