/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/saga-cancel.cpp
 * @brief Saga compensation aborts cleanly when the actor is killed mid-rollback (`qb::run_saga`).
 *
 * `run_saga` runs forward steps, each able to register a compensation, and on a (non-cancel) failure
 * runs the compensations in REVERSE order before re-throwing. Crucially, the rollback itself is
 * cancellation-aware: if a compensation throws `cancelled_error` (the actor is being killed mid-
 * rollback), `SagaScope::compensate()` STOPS rather than spinning through the remaining undos. This
 * test pins that abort precisely:
 *
 *   step 1 (ok) registers compA (asks the ok peer)        → would run LAST in rollback
 *   step 2 (ok) registers compB (asks a SILENT peer)      → runs FIRST in rollback, then parks
 *   step 3 TIMES OUT → rollback begins → compB starts, parks on the silent peer
 *   the actor is KILLED while compB is parked → compB's ask is cancelled → rollback aborts
 *
 * The teeth: compB was REACHED (`g_compB_started`), but compA's body was NEVER ENTERED
 * (`!g_compA_entered`) and therefore certainly never completed (`!g_compA_ran`) — the cancel-aware
 * rollback short-circuited exactly where documented. `g_compA_entered` is checked as the very first
 * line of compA's body, so it is a true "did we even start the next compensation" probe.
 *
 * De-flaked vs the monolith: the kill must land while compB is parked on the silent peer (after the
 * ~30ms step-3 timeout) — a generous window — and the killer arms only a BACKSTOP `stop()` so a
 * regression fails loudly via the flag assertions rather than hanging. The flags are the oracle, not
 * any wall-clock comparison.
 *
 * Run under ASAN_OPTIONS=detect_leaks=0 like the rest of the actor-coroutine suites.
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>
#include <qb/io/async.h>
#include <atomic>
#include <chrono>

using namespace qb;
using namespace std::chrono_literals;

namespace {
std::atomic<bool> g_compA_entered{false}; // compA's body — must NOT even be entered (abort skips it)
std::atomic<bool> g_compA_ran{false};     // compA completed — must be false on kill
std::atomic<bool> g_compB_started{false}; // runs FIRST in rollback — reached, then cancelled
std::atomic<bool> g_saga_failed{false};   // the saga surfaced a (non-cancel) failure
std::atomic<bool> g_saga_observed{false}; // the saga coroutine reached its catch block (ran-guard)
} // namespace

struct SagaQ : public qb::Request<int> {
    int v{0};
    SagaQ() = default;
    explicit SagaQ(int x)
        : v(x) {}
};

class OkPeer : public qb::Actor { // answers immediately
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SagaQ>(*this);
        co_return true;
    }
    void
    on(SagaQ &q) {
        qb::answer(*this, q, [](SagaQ const &r) { return r.v; });
    }
};

class SilentPeer : public qb::Actor { // never answers
public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SagaQ>(*this);
        co_return true;
    }
    void
    on(SagaQ &) {}
};

class SagaActor : public qb::Actor {
    qb::ActorId _ok;
    qb::ActorId _silent;

public:
    SagaActor(qb::ActorId ok, qb::ActorId silent)
        : _ok(ok)
        , _silent(silent) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<SagaQ>(*this);
        auto ok     = _ok;
        auto silent = _silent;
        spawn([ok, silent](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            try {
                co_await qb::run_saga(ctx, [ok, silent](qb::ScopedCoroContext c, qb::SagaScope &saga) -> qb::io::async::task<void> {
                    (void) co_await qb::ask(c, ok, SagaQ{1}, 500ms); // step 1 ok
                    saga.on_compensate([c, ok]() -> qb::io::async::task<void> {
                        g_compA_entered.store(true);                     // compA body entered?
                        (void) co_await qb::ask(c, ok, SagaQ{2}, 500ms); // compA — runs LAST
                        g_compA_ran.store(true);
                    });
                    (void) co_await qb::ask(c, ok, SagaQ{3}, 500ms); // step 2 ok
                    saga.on_compensate([c, silent]() -> qb::io::async::task<void> {
                        g_compB_started.store(true);                         // compB — runs FIRST
                        (void) co_await qb::ask(c, silent, SagaQ{4}, 500ms); // parks (silent peer)
                    });
                    (void) co_await qb::ask(c, silent, SagaQ{5}, 30ms); // step 3 TIMES OUT → rollback
                });
            } catch (const qb::io::async::timeout_error &) {
                g_saga_failed.store(true); // the saga failed (and rolled back as far as it could)
            } catch (const qb::io::async::cancelled_error &) {
                // possible if the kill races the rethrow — also acceptable
            }
            g_saga_observed.store(true);
        });
        co_return true;
    }
    void
    on(SagaQ &e) {
        resolve_ask(e); // route our own asks' replies back to the saga coroutine
    }
};

class SagaKiller : public qb::Actor {
    qb::ActorId _victim;

public:
    explicit SagaKiller(qb::ActorId v)
        : _victim(v) {}
    qb::io::async::task<bool>
    onInit() override {
        auto victim = _victim;
        // Compensation begins ~30ms (after the step-3 timeout); compB then parks on the silent peer.
        // Kill at 70ms → compB's ask is cancelled → rollback aborts before compA. Generous window.
        qb::io::async::callback([this, victim] { push<qb::KillEvent>(victim); }, 70ms);
        qb::io::async::callback([] { qb::Main::stop(); }, 2s); // backstop only — never the oracle
        co_return true;
    }
};

TEST(SagaCancel, CompensationAbortsOnKillMidRollback) {
    g_compA_entered.store(false);
    g_compA_ran.store(false);
    g_compB_started.store(false);
    g_saga_failed.store(false);
    g_saga_observed.store(false);
    qb::Main   main;
    const auto ok     = main.addActor<OkPeer>(0);
    const auto silent = main.addActor<SilentPeer>(0);
    const auto saga   = main.addActor<SagaActor>(0, ok, silent);
    main.addActor<SagaKiller>(0, saga);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_compB_started.load()) << "rollback must reach the first compensation";
    EXPECT_FALSE(g_compA_entered.load()) << "…and abort there: compA's body must never be entered";
    EXPECT_FALSE(g_compA_ran.load()) << "(so compA certainly did not complete either)";
}
