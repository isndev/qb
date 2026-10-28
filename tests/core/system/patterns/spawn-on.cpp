/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/patterns/spawn-on.cpp
 * @brief `qb::spawn_on` / `qb::Spawner` (Huly QB-54): an actor created on another core while the engine runs.
 *
 * Each case is a `qb::test::TestKit` scenario (`run<qb::SpawnRequest>`: the driver resolves the answers). Covered:
 * the spawn on another core and on the caller's own, the new actor RUNNING there (it reports its core) and reachable
 * by its id; arguments moved across (a `std::string`, a `std::vector`); an async `onInit()` waited for (a lower
 * bound only); an init that fails before it suspends (`refused`: the core rolls its admission back), one that fails
 * after and one that outlasts the timeout (`not_ready`, the id valid); a service tag
 * already there (`refused`); a core with no spawner and a core that does not exist (`unreachable`, no crash); many
 * spawns at once; and the caller that does not register `qb::SpawnRequest`, whose answer never resolves.
 */

#include <gtest/gtest.h>
#include <qb/core/patterns.h>
#include <qb/string.h>
#include <qb/testkit.h>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace spawn_on_test {

/// What a spawned actor reports from its `onInit()`: where it runs and what it was built with.
struct Hello : qb::Event {
    qb::CoreId     core = 0;
    qb::string<32> name;
    std::size_t    items = 0;
    Hello(qb::CoreId c, std::string const &n, std::size_t i)
        : core(c)
        , name(n.c_str())
        , items(i) {}
};

struct Ping : qb::Event {};
struct Pong : qb::Event {};

class Worker : public qb::Actor {
    std::string      _name;
    std::vector<int> _items;
    qb::ActorId      _report;

public:
    Worker(std::string name, std::vector<int> items, qb::ActorId report)
        : _name(std::move(name))
        , _items(std::move(items))
        , _report(report) {}
    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Ping>(*this);
        push<Hello>(_report, getIndex(), _name, _items.size());
        co_return true;
    }
    void
    on(Ping const &) {
        push<Pong>(_report);
    }
};

class SlowInit : public qb::Actor {
    qb::duration _wait;

public:
    explicit SlowInit(qb::duration wait)
        : _wait(wait) {}
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(_wait);
        co_return true;
    }
};

class FailingInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return false; // before any suspension: the core rolls the admission back
    }
};

class FailingAsyncInit : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_await context().sleep(std::chrono::milliseconds(1));
        co_return false; // after a suspension: the actor exists, Activating, then fails
    }
};

struct Registry : qb::ServiceActor<Registry> {
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

class Idle : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        co_return true;
    }
};

using steady = std::chrono::steady_clock;

} // namespace spawn_on_test

using namespace spawn_on_test;

TEST(SpawnOn, CreatesAnActiveActorOnAnotherCoreThatRunsThere) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello, Pong>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto w = co_await qb::spawn_on<Worker>(ctx, 1, 5s, std::string("shard-7"), std::vector<int>{1, 2, 3}, probe);
            EXPECT_TRUE(w) << static_cast<int>(w.status);
            EXPECT_EQ(w.status, qb::spawn_status::spawned);
            EXPECT_EQ(w.id.index(), 1) << "created by the spawner of core 1, on core 1";
            const auto hello = co_await qb::test::expect<Hello>(ctx, probe, 5s);
            EXPECT_TRUE(hello.has_value());
            if (hello) {
                EXPECT_EQ(hello->core, 1) << "its onInit ran on core 1";
                EXPECT_EQ(std::string(hello->name.c_str()), "shard-7") << "the string crossed by value";
                EXPECT_EQ(hello->items, 3u);
                EXPECT_EQ(hello->getSource(), w.id);
            }
            ctx.push_to<Ping>(w.id);
            EXPECT_TRUE((co_await qb::test::expect<Pong>(ctx, probe, 5s)).has_value()) << "reachable by its id";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, TheCallersOwnCoreWorksToo) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(0);
    const auto run = kit.run<qb::SpawnRequest>([probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
        const auto w = co_await qb::spawn_on<Worker>(ctx, 0, 5s, std::string("local"), std::vector<int>{}, probe);
        EXPECT_EQ(w.status, qb::spawn_status::spawned);
        EXPECT_EQ(w.id.index(), 0);
        const auto hello = co_await qb::test::expect<Hello>(ctx, probe, 5s);
        EXPECT_EQ(hello ? hello->core : qb::CoreId{99}, 0);
    });
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, AnAsyncInitIsWaitedForBeforeTheAnswer) {
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<Idle>(0);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto start = steady::now();
            const auto w     = co_await qb::spawn_on<SlowInit>(ctx, 1, 5s, qb::duration{std::chrono::milliseconds(50)});
            EXPECT_EQ(w.status, qb::spawn_status::spawned);
            EXPECT_GE(steady::now() - start, 50ms) << "answered once onInit() completed, not at its construction";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, AnInitThatFailsIsRefusedOrNotReadyByWhenItFails) {
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto sync_failed = co_await qb::spawn_on<FailingInit>(ctx, 1, 5s);
            EXPECT_EQ(sync_failed.status, qb::spawn_status::refused) << "failed before suspending: never admitted";
            EXPECT_FALSE(sync_failed.id.is_valid());
            const auto async_failed = co_await qb::spawn_on<FailingAsyncInit>(ctx, 1, 5s);
            EXPECT_EQ(async_failed.status, qb::spawn_status::not_ready) << "failed after suspending: it existed";
            EXPECT_TRUE(async_failed.id.is_valid());
            const auto slow = co_await qb::spawn_on<SlowInit>(ctx, 1, 100ms, qb::duration{std::chrono::seconds(30)});
            EXPECT_EQ(slow.status, qb::spawn_status::not_ready) << "still activating when the timeout passed";
            EXPECT_TRUE(slow.id.is_valid());
        },
        30s);
    // The slow actor is still Activating when the run ends: the engine's stop ends it, and a failed init is
    // reported by hasError() -- what the kit's engine_error says. The scenario itself completed.
    EXPECT_TRUE(run.completed) << run.describe();
    EXPECT_FALSE(run.timed_out);
}

TEST(SpawnOn, AServiceAlreadyOnTheCoreIsRefused) {
    qb::test::TestKit kit;
    (void) kit.add<qb::Spawner>(1);
    (void) kit.add<Registry>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto again = co_await qb::spawn_on<Registry>(ctx, 1, 2s);
            EXPECT_EQ(again.status, qb::spawn_status::refused);
            EXPECT_FALSE(again.id.is_valid());
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ACoreWithoutSpawnerOrThatDoesNotExistIsUnreachable) {
    qb::test::TestKit kit;
    (void) kit.add<Idle>(1); // core 1 runs, but no spawner there
    const auto run = kit.run<qb::SpawnRequest>(
        [](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto start = steady::now();
            const auto none  = co_await qb::spawn_on<Idle>(ctx, 1, 100ms);
            EXPECT_EQ(none.status, qb::spawn_status::unreachable);
            EXPECT_GE(steady::now() - start, 100ms);
            const auto absent = co_await qb::spawn_on<Idle>(ctx, 7, 100ms);
            EXPECT_EQ(absent.status, qb::spawn_status::unreachable) << "a core the engine does not run";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ManySpawnsInFlightEachGetTheirOwnActor) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run<qb::SpawnRequest>(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            std::vector<qb::io::async::task<qb::spawn_result>> spawns;
            for (int i = 0; i < 8; ++i)
                spawns.push_back(qb::spawn_on<Worker>(ctx, 1, 5s, "w" + std::to_string(i), std::vector<int>(i), probe));
            const auto              results = co_await qb::io::async::when_all(std::move(spawns));
            std::set<std::uint64_t> ids;
            for (auto const &r : results) {
                EXPECT_EQ(r.status, qb::spawn_status::spawned);
                ids.insert(r.id.raw64());
            }
            EXPECT_EQ(ids.size(), 8u) << "eight distinct actors";
            std::set<std::size_t> sizes;
            for (int i = 0; i < 8; ++i) {
                const auto hello = co_await qb::test::expect<Hello>(ctx, probe, 5s);
                if (hello)
                    sizes.insert(hello->items);
            }
            EXPECT_EQ(sizes.size(), 8u) << "each built with its own arguments";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}

TEST(SpawnOn, ACallerThatDoesNotRegisterSpawnRequestNeverSeesTheAnswer) {
    qb::test::TestKit kit;
    const auto        probe = kit.probe<Hello>(0);
    (void) kit.add<qb::Spawner>(1);
    const auto run = kit.run(
        [probe](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            const auto w = co_await qb::spawn_on<Worker>(ctx, 1, 100ms, std::string("orphan"), std::vector<int>{}, probe);
            EXPECT_EQ(w.status, qb::spawn_status::unreachable) << "the answer is a dead letter at the driver";
            EXPECT_TRUE((co_await qb::test::expect<Hello>(ctx, probe, 5s)).has_value()) << "the actor exists all the same";
        },
        30s);
    EXPECT_TRUE(run) << run.describe();
}
