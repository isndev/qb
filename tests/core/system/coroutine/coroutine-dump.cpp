/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/coroutine/coroutine-dump.cpp
 * @brief An actor's named coroutines in its core's `CoroutineScheduler::dump()` (Huly QB-71).
 *
 * An actor turns suspension tracking on for its core, spawns a coroutine under a name -- `spawn(name, fn)` -- that
 * asks an actor which never answers, and a detached one -- `spawn_detached(name, fn)` -- that sleeps, and reads its
 * core's dump from a handler: both named roots are there, and at the end of their chains one waits on an ask, the
 * other on a sleep. The qb-io half (every kind, chains, ageing, destruction, names) is
 * qb-io-test-system-suspension-tracking. Every effect is mirrored to a file-scope atomic asserted after `join()`.
 */

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/main.h>

#include "../../shared/AskResponders.h"

using namespace std::chrono_literals;

// A named namespace: these types are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace coroutine_dump_test {

std::atomic<bool> g_checked{false};    ///< the handler read the dump
std::atomic<bool> g_named_root{false}; ///< the named coroutine is a root of the dump
std::atomic<bool> g_on_ask{false};     ///< at the end of its chain, it waits on an ask
std::atomic<bool> g_in_text{false};    ///< the written dump names it and the ask
std::atomic<bool> g_detached{false};   ///< the detached coroutine is a named root, waiting on a sleep

struct Check : public qb::Event {};

class Asker : public qb::Actor {
    qb::ActorId _silent;
    int         _tries = 0;

public:
    explicit Asker(qb::ActorId silent)
        : _silent(silent) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Check>(*this);
        qb::io::async::listener::current.coro_scheduler().set_suspension_tracking(true);
        spawn("fetch-price", [silent = _silent](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                (void) co_await qb::ask(ctx, silent, qb::test::Ping{1}, 30s);
            } catch (...) {
                // cancelled with the actor at the end of the case
            }
        });
        // detached: it outlives the actor, and the core's teardown reclaims it
        spawn_detached("audit-trail", [](qb::CoroContext) -> qb::io::async::task<void> { co_await qb::io::async::sleep(5s); });
        push<Check>(id());
        co_return true;
    }

    /// The root spawned under `name`, and what the coroutine at the end of its chain waits on.
    static std::pair<qb::io::async::parked_coroutine const *, std::string_view>
    root_and_leaf(std::vector<qb::io::async::parked_coroutine> const &dump, std::string_view name) {
        qb::io::async::parked_coroutine const *root = nullptr;
        for (auto const &p : dump)
            if (p.name == name)
                root = &p;
        auto const *leaf = root;
        for (std::size_t depth = 0; leaf && leaf->waits_on && depth < dump.size(); ++depth) {
            qb::io::async::parked_coroutine const *next = nullptr;
            for (auto const &p : dump)
                if (p.frame == leaf->waits_on)
                    next = &p;
            if (!next)
                break;
            leaf = next;
        }
        return {root, leaf && leaf->kind ? std::string_view{leaf->kind} : std::string_view{}};
    }

    void
    on(Check const &) {
        auto      &sched          = qb::io::async::listener::current.coro_scheduler();
        const auto dump           = sched.dump();
        const auto [root, leaf]   = root_and_leaf(dump, "fetch-price");
        const auto [audit, nap]   = root_and_leaf(dump, "audit-trail");
        const bool on_ask         = leaf == "ask";
        const bool detached_sleep = audit && audit->root && nap == "sleep";
        if (!(on_ask && detached_sleep) && ++_tries < 50) {
            push<Check>(id()); // a coroutine has not parked yet: look again on a later pass
            return;
        }
        g_named_root = root && root->root;
        g_on_ask     = on_ask;
        g_detached   = detached_sleep;
        std::ostringstream text;
        sched.dump(text);
        g_in_text = text.str().find("\"fetch-price\"") != std::string::npos && text.str().find(" ask") != std::string::npos;
        sched.set_suspension_tracking(false);
        g_checked = true;
        kill(); // the scoped coroutine is cancelled with its actor
    }
};

} // namespace coroutine_dump_test

TEST(CoroutineDump, AnActorsNamedCoroutinesAreInItsCoresDumpWithWhatTheyWaitOn) {
    qb::Main   main;
    const auto silent = main.addActor<qb::test::SilentMarket>(0);
    main.addActor<coroutine_dump_test::Asker>(0, silent);
    main.start();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!coroutine_dump_test::g_checked && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    main.stop();
    main.join();
    EXPECT_FALSE(main.hasError());
    ASSERT_TRUE(coroutine_dump_test::g_checked) << "the handler never read the dump";
    EXPECT_TRUE(coroutine_dump_test::g_named_root) << "the coroutine spawned under a name is not a named root of the dump";
    EXPECT_TRUE(coroutine_dump_test::g_on_ask) << "the dump does not say the coroutine waits on an ask";
    EXPECT_TRUE(coroutine_dump_test::g_in_text) << "the written dump does not name the coroutine and its ask";
    EXPECT_TRUE(coroutine_dump_test::g_detached) << "the coroutine spawned detached under a name is not a named root on a sleep";
}
