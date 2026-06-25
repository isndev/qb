/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/idempotency-answer.cpp
 * @brief Responder-side exactly-once effects: `qb::answer_idempotent` + a `dedup_map` keyed store.
 *
 * Because `ask_retry` re-sends a request with a fresh correlation id per attempt, a reply lost to a
 * timeout would make the responder run its side effect twice. `answer_idempotent` de-duplicates by a
 * STABLE `idempotency_key` carried on the request: the first request runs the effect and caches the
 * response; a repeat with the same key REPLAYS the cached response WITHOUT re-running the effect.
 * Proven here against the running engine, counting the effect executions inside the responder (not a
 * value the asker set):
 *   - SAME KEY twice → both replies are the cached response (50), but the effect ran EXACTLY ONCE;
 *   - DEFAULT (zero) KEY → never de-duplicated → the effect ran each time (twice);
 *   - DISTINCT KEYS → independent → each runs its own effect, each gets its own value.
 *
 * The effect counter and both reply values are mirrored to atomics behind a "ran" flag asserted
 * after join(), so a never-scheduled asker cannot pass vacuously. The asker stops the engine the
 * instant both round-trips complete (event-driven, no wall-clock offset).
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
#include <cstdint>

using namespace qb;
using namespace std::chrono_literals;

namespace {
std::atomic<int>  g_idem_processed{0}; // counts responder effect executions
std::atomic<int>  g_idem_r1{-1};
std::atomic<int>  g_idem_r2{-1};
std::atomic<bool> g_idem_ran{false};

struct Charge : qb::Request<int> {
    std::uint64_t idempotency_key{0};
    int           amount{0};
};
} // namespace

class IdemBank : public qb::Actor {
    qb::dedup_map<std::uint64_t, int> _seen{16};

public:
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Charge>(*this);
        co_return true;
    }
    void
    on(Charge &e) {
        qb::answer_idempotent(*this, e, _seen, [](Charge const &r) {
            g_idem_processed.fetch_add(1); // the effect — must run at most once per non-zero key
            return r.amount * 10;
        });
    }
};

class IdemAsker : public qb::Actor {
    qb::ActorId   _bank;
    std::uint64_t _k1, _k2;
    int           _a1, _a2;

public:
    IdemAsker(qb::ActorId bank, std::uint64_t k1, int a1, std::uint64_t k2, int a2)
        : _bank(bank)
        , _k1(k1)
        , _k2(k2)
        , _a1(a1)
        , _a2(a2) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Charge>(*this);
        auto bank = _bank;
        auto k1 = _k1, k2 = _k2;
        auto a1 = _a1, a2 = _a2;
        spawn([bank, k1, k2, a1, a2](qb::ScopedCoroContext c) -> qb::io::async::task<void> {
            Charge c1;
            c1.idempotency_key = k1;
            c1.amount          = a1;
            auto r1            = co_await qb::ask(c, bank, c1, 1s);
            g_idem_r1.store(r1.response);
            Charge c2;
            c2.idempotency_key = k2;
            c2.amount          = a2;
            auto r2            = co_await qb::ask(c, bank, c2, 1s);
            g_idem_r2.store(r2.response);
            g_idem_ran.store(true);
            qb::Main::stop(); // event-driven: stop the instant both round-trips complete
        });
        co_return true;
    }
    void
    on(Charge &e) {
        resolve_ask(e);
    }
};

static void
run_idem(std::uint64_t k1, int a1, std::uint64_t k2, int a2) {
    g_idem_processed.store(0);
    g_idem_r1.store(-1);
    g_idem_r2.store(-1);
    g_idem_ran.store(false);
    qb::Main   main;
    const auto bank = main.addActor<IdemBank>(0);
    main.addActor<IdemAsker>(0, bank, k1, a1, k2, a2);
    main.start(false);
    main.join();
    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_idem_ran.load()) << "the idempotent asker coroutine must have run both round-trips";
}

TEST(Idempotency, SameKeyProcessedOnce) {
    run_idem(42, 5, 42, 5); // same key twice (a retry / duplicate)
    EXPECT_EQ(g_idem_r1.load(), 50);
    EXPECT_EQ(g_idem_r2.load(), 50) << "the second reply is the cached response";
    EXPECT_EQ(g_idem_processed.load(), 1) << "…but the effect ran exactly once";
}

TEST(Idempotency, ZeroKeyNotCached) {
    run_idem(0, 5, 0, 5); // default key → never de-duplicated
    EXPECT_EQ(g_idem_r1.load(), 50);
    EXPECT_EQ(g_idem_r2.load(), 50);
    EXPECT_EQ(g_idem_processed.load(), 2) << "a default key bypasses the cache → effect ran each time";
}

TEST(Idempotency, DistinctKeysBothProcessed) {
    run_idem(1, 1, 2, 2); // different keys → independent
    EXPECT_EQ(g_idem_r1.load(), 10);
    EXPECT_EQ(g_idem_r2.load(), 20);
    EXPECT_EQ(g_idem_processed.load(), 2) << "distinct keys are independent → each runs its effect";
}
