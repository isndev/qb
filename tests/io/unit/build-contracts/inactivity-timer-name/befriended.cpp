/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file unit/build-contracts/inactivity-timer-name/befriended.cpp
 * @brief A session whose `inactivity_timer_t` is private does not compile where it is extracted, though it befriends
 *        the `io_handler` that extracts it (Huly QB-1248).
 *
 * The rule is a public name, on every compiler: read from the `io_handler` a session befriends, g++ and clang took the
 * private name for a usable one where MSVC refused it (the twenty-first independent review); the name's access is now
 * checked from a class template of qb-io's, never from the friend. This unit is built on demand by the ctest entry
 * `qb-io-test-unit-inactivity-timer-befriended`, which passes when the build fails with the static_assert's message as
 * its only error. Built with `QB_CONTRACT_CONTROL`, the name is public: that unit is the target of the entry's name,
 * built and linked with the suite.
 */

#include <qb/io/async.h>
#include <qb/io/protocol/text.h>

namespace inactivity_timer_befriended_contract {
using namespace qb::io;

class Server;

class Session
    : public use<Session>::tcp::client<Server>
    , public use<Session>::timeout {
    template <typename, typename>
    friend class qb::io::async::io_handler; // the class that extracts the session is a friend
#ifndef QB_CONTRACT_CONTROL
    using inactivity_timer_t = async::with_timeout<Session>; // private, the extraction a friend: refused all the same
#endif

public:
#ifdef QB_CONTRACT_CONTROL
    using inactivity_timer_t = async::with_timeout<Session>; // public: used
#endif
    using Protocol = qb::protocol::text::command<Session>;
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
} // namespace inactivity_timer_befriended_contract

int
main() {
    return 0;
}
