/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */
/**
 * @file tests/core/system/engine/actor-arena.cpp
 * @brief The actor object comes from its core thread's `thread_arena` (Huly QB-212): a burst of
 *        `addRefActor` lifetimes is a burst of arena blocks (the arena's live count follows the
 *        core's live actors, its chunks grow to slabs), the slabs go back to the process-wide
 *        cache when the worker thread exits, an over-aligned actor type stays aligned, a large
 *        actor falls through to the global allocator, and a derived class that declares its own
 *        `operator new` / `operator delete` keeps them (the arena never sees its objects).
 */

#include <gtest/gtest.h>
#include <qb/actor.h>
#include <qb/main.h>
#include <qb/system/allocator/slab.h>
#include <qb/system/allocator/thread_arena.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

using qb::allocator::slab_cache;
using qb::allocator::thread_arena;

// A named namespace: these actors are handed to coroutine-spawning framework templates
// (qb/scripts/check-coro-fixture-linkage.py).
namespace actor_arena_test {

// Written on the core thread by the probes below, read by the test after the engine stops.
// Handed to the actors by POINTER: `addActor` stores its constructor arguments by value.
struct Observed {
    std::size_t live_at_peak     = 0; ///< thread_arena::live() while the burst is alive
    std::size_t live_after_reap  = 0; ///< ... once the burst's actors are destroyed
    std::size_t chunks_at_peak   = 0;
    std::size_t live_actor_count = 0; ///< the test's own count of constructed-not-destroyed actors
    bool        aligned_ok       = false;
};

inline std::atomic<std::size_t> g_constructed{0};
inline std::atomic<std::size_t> g_destroyed{0};

/// Slabs the process holds outside the cache: what a leak would move.
[[nodiscard]] inline std::size_t
slabs_in_use() noexcept {
    return slab_cache::mapped() - slab_cache::cached();
}

struct Burst : qb::Event {};
struct Check : qb::Event {};

// A child that dies at once: one full lifetime per addRefActor.
class Leaf final : public qb::Actor {
public:
    Leaf() noexcept {
        g_constructed.fetch_add(1, std::memory_order_relaxed);
    }
    ~Leaf() noexcept override {
        g_destroyed.fetch_add(1, std::memory_order_relaxed);
    }
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

// A child that stays alive until it is told to die, so the burst's peak is observable.
class Holder final : public qb::Actor {
public:
    Holder() noexcept {
        g_constructed.fetch_add(1, std::memory_order_relaxed);
    }
    ~Holder() noexcept override {
        g_destroyed.fetch_add(1, std::memory_order_relaxed);
    }
    qb::io::async::task<bool>
    onInit() final {
        co_return true;
    }
};

class Parent final : public qb::Actor {
    Observed *const          _obs;
    const std::size_t        _n;
    std::vector<qb::ActorId> _children;
    std::size_t              _checks = 0;

public:
    Parent(Observed *obs, std::size_t n) noexcept
        : _obs(obs)
        , _n(n) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Burst>(*this);
        registerEvent<Check>(*this);
        push<Burst>(id());
        co_return true;
    }

    void
    on(Burst const &) {
        // Every child is constructed here, on this core's thread, through Actor::operator new.
        _children.reserve(_n);
        for (std::size_t i = 0; i < _n; ++i) {
            auto h = addRefActor<Holder>();
            ASSERT_TRUE(h.valid());
            _children.push_back(h.id());
        }
        _obs->live_at_peak     = thread_arena::live();
        _obs->chunks_at_peak   = thread_arena::chunks();
        _obs->live_actor_count = g_constructed.load(std::memory_order_relaxed) - g_destroyed.load(std::memory_order_relaxed);
        for (auto const child : _children)
            push<qb::KillEvent>(child); // each dies in the reap phase of the pass that delivers it
        push<Check>(id());
    }

    void
    on(Check const &) {
        // The reap runs after the pass's dispatch: poll until every child is destroyed
        // (bounded: a missing destructor call fails the count, never hangs the engine).
        if (g_destroyed.load(std::memory_order_relaxed) < _n && ++_checks < 100000) {
            push<Check>(id());
            return;
        }
        _obs->live_after_reap = thread_arena::live();
        kill();
    }
};

struct Spawner final : qb::Actor {
    Observed *const obs;
    explicit Spawner(Observed *o) noexcept
        : obs(o) {}
    qb::io::async::task<bool>
    onInit() final {
        const std::size_t before = thread_arena::live();
        for (int i = 0; i < 1000; ++i)
            EXPECT_TRUE(addRefActor<Leaf>().valid());      // dies in its own onInit: reaped at pass end
        obs->live_at_peak = thread_arena::live() - before; // still allocated until the reap
        kill();
        co_return true;
    }
};

struct alignas(64) OverAligned final : qb::Actor {
    Observed *const obs;
    char            pad[64];
    explicit OverAligned(Observed *o) noexcept
        : obs(o) {
        obs->aligned_ok = (reinterpret_cast<std::uintptr_t>(this) % 64u) == 0;
    }
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

// Larger than thread_arena::max_small: served by the global allocator, never counted.
struct Large final : qb::Actor {
    Observed *const obs;
    char            payload[2048];
    explicit Large(Observed *o) noexcept
        : obs(o) {}
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

// A class-level allocator of its own hides Actor's: the arena never sees these objects.
inline std::atomic<int> g_custom_new{0};
inline std::atomic<int> g_custom_delete{0};
struct OwnAllocator final : qb::Actor {
    Observed *const obs;
    explicit OwnAllocator(Observed *o) noexcept
        : obs(o) {}
    static void *
    operator new(std::size_t const size) {
        g_custom_new.fetch_add(1);
        return ::operator new(size);
    }
    static void
    operator delete(void *const p, std::size_t const size) noexcept {
        g_custom_delete.fetch_add(1);
        ::operator delete(p, size);
    }
    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

template <typename T>
struct Host final : qb::Actor {
    Observed *const obs;
    explicit Host(Observed *o) noexcept
        : obs(o) {}
    qb::io::async::task<bool>
    onInit() final {
        const std::size_t before = thread_arena::live();
        EXPECT_TRUE(addRefActor<T>(obs).valid());
        obs->live_after_reap = thread_arena::live() - before; // what the child's construction added
        kill();
        co_return true;
    }
};

// The placement form keeps compiling for a user constructing an actor into their own storage
// (never executed: an actor built outside a core has no core to live on).
[[maybe_unused]] inline void
placement_new_still_compiles(void *const mem, Observed *const obs) {
    [[maybe_unused]] auto *const a = new (mem) Large(obs);
}

} // namespace actor_arena_test

using namespace actor_arena_test;

TEST(ActorArena, ABurstOfActorLifetimesIsABurstOfArenaBlocks) {
    g_constructed.store(0);
    g_destroyed.store(0);
    constexpr std::size_t n             = 8000; // 8000 x ~100 B: past the 64 KiB first chunk, into a slab
    const std::size_t     in_use_before = slabs_in_use();
    Observed              obs;
    {
        qb::Main main;
        main.core(0).addActor<Parent>(&obs, n);
        main.start(); // a worker thread: its arena is torn down when the thread exits
        main.join();
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_EQ(obs.live_actor_count, n); // the children (the parent carries no counter)
    // The arena counts exactly the live actor objects of this core -- the children AND the
    // parent, which the factory built on the same thread; nothing else there is arena-allocated.
    EXPECT_EQ(obs.live_at_peak, n + 1u);
    EXPECT_EQ(obs.live_after_reap, 1u);
    EXPECT_GE(obs.chunks_at_peak, 2u); // the first chunk, then at least one slab
    EXPECT_EQ(g_destroyed.load(), n);
    // The worker exited with no actor alive: its slabs are back in the cache (none in use).
    EXPECT_EQ(slabs_in_use(), in_use_before);
    EXPECT_GE(slab_cache::cached(), obs.chunks_at_peak - 1);
}

TEST(ActorArena, AShortLivedActorIsOneLifetimeAndOneBlock) {
    g_constructed.store(0);
    g_destroyed.store(0);
    Observed obs;
    {
        qb::Main main;
        main.core(0).addActor<Spawner>(&obs);
        main.start(false);
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_EQ(obs.live_at_peak, 1000u);
    EXPECT_EQ(g_constructed.load(), 1000u);
    EXPECT_EQ(g_destroyed.load(), 1000u);
}

TEST(ActorArena, AnOverAlignedActorTypeIsAlignedAndNotPooled) {
    Observed obs;
    {
        qb::Main main;
        main.core(0).addActor<Host<OverAligned>>(&obs);
        main.start(false);
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_TRUE(obs.aligned_ok);
    EXPECT_EQ(obs.live_after_reap, 0u); // the aligned path is the global allocator's
}

TEST(ActorArena, ALargeActorFallsThroughToTheGlobalAllocator) {
    Observed obs;
    {
        qb::Main main;
        main.core(0).addActor<Host<Large>>(&obs);
        main.start(false);
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_EQ(obs.live_after_reap, 0u);
}

TEST(ActorArena, ADerivedClassWithItsOwnOperatorsKeepsThem) {
    g_custom_new.store(0);
    g_custom_delete.store(0);
    Observed obs;
    {
        qb::Main main;
        main.core(0).addActor<Host<OwnAllocator>>(&obs);
        main.start(false);
        EXPECT_FALSE(main.hasError());
    }
    EXPECT_EQ(g_custom_new.load(), 1);
    EXPECT_EQ(g_custom_delete.load(), 1);
    EXPECT_EQ(obs.live_after_reap, 0u);
}
