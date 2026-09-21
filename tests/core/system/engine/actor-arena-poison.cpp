/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */
/**
 * @file tests/core/system/engine/actor-arena-poison.cpp
 * @brief Under AddressSanitizer a reaped actor's block is poisoned on the arena's free list
 *        (Huly QB-217): a read through a dangling actor pointer is reported as a use-after-poison,
 *        where the recycled block used to be read in silence. Registered in an ASan build only
 *        (`tests/core/system/CMakeLists.txt`); a death test, re-executed.
 *
 * The shape is the one GUARDRAILS names first: a raw pointer to an actor kept past its life. The
 * doomed actor leaks `this` into a global from its constructor and kills itself as it initialises;
 * a second actor on the same core keeps the core alive, waits past the reap, and reads a member
 * through the leaked pointer. In an instrumented build the arena has poisoned the block, so the
 * read is the sanitizer's report and the process dies; without the poisoning it reads the free-list
 * word and the process ends normally -- which is what the death test refuses ("failed to die"),
 * and what the tree read before this file existed (proven by disabling the poisoning once).
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <chrono>
#include <qb/system/allocator/thread_arena.h>

static_assert(qb::allocator::thread_arena::poisons_free_blocks,
              "registered in AddressSanitizer builds only: the arena must see the same sanitizer this test does");

// A named namespace: these actors are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace actor_arena_poison_test {

class Doomed;
inline Doomed *g_doomed = nullptr; ///< the pointer a correct program never keeps

class Doomed final : public qb::Actor {
    int _payload = 42;

public:
    Doomed() {
        g_doomed = this;
    }
    [[nodiscard]] int
    payload() const noexcept {
        return _payload; // a member read: the load the sanitizer must report once the block is free
    }
    qb::io::async::task<bool>
    onInit() override {
        kill(); // reaped by the core at the end of this pass
        co_return true;
    }
};

class Watcher final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() override {
        spawn([](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(std::chrono::milliseconds{50}); // well past the reap of the doomed actor
            volatile int const v = g_doomed->payload();        // the use-after-free
            (void) v;
            ctx.push<qb::KillEvent>(); // not reached under ASan; ends the engine when nothing reported the read
        });
        co_return true;
    }
};

/// The whole engine, in the child process the death test forks: the doomed actor dies at once, the
/// watcher touches it 50 ms later.
void
read_a_reaped_actor() {
    qb::Main main;
    main.core(0).addActor<Doomed>();
    main.core(0).addActor<Watcher>();
    main.start();
    main.join();
}

} // namespace actor_arena_poison_test

using namespace actor_arena_poison_test;

TEST(ActorArenaPoison, AReadThroughAReapedActorIsReportedAsUseAfterPoison) {
    // The statement starts a whole engine, so the child must re-exec rather than fork a process
    // that already holds core threads.
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH(read_a_reaped_actor(), "use-after-poison")
        << "the arena handed the reaped actor's block back unpoisoned: a read through a dangling actor "
           "pointer went unreported under AddressSanitizer";
}
