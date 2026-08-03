/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file system/lifecycle/actor-resource-cleanup.cpp
 * @brief An actor owning heap resources releases ALL of them when it terminates — whether via a
 *        graceful shutdown or an abrupt (failure-style) kill — proven by a strict alloc/free balance
 *        counter and event-driven completion (no `200/300ms` phase timers).
 *
 * `ManagedResource` bumps a global on construction and on destruction, so a non-zero
 * `allocated - freed` after the engine drains means a resource leaked. The two tests prove the two
 * teardown paths:
 *   - GRACEFUL: the actor clears its resource vector on a shutdown event, then kills itself;
 *   - FAILURE:  the actor is killed abruptly with resources STILL held — the destructor (RAII via
 *               `unique_ptr`) must free them anyway.
 *
 * Strengthened over the original (which left test #2 empty and ignored every Status/Report):
 *   - the Status → Report round-trip is asserted: the live count the actor reports back matches the
 *     allocations the test made (and proves the actor is reachable mid-run);
 *   - NOT-all-freed-before-shutdown is asserted: while the actor is alive and holding resources,
 *     `freed < allocated` — so the final all-freed assertion is meaningful (it is the SHUTDOWN that
 *     freed them, not an early release);
 *   - completion is driven by a Report ack + a kill ack, never a wall clock.
 */

#include <atomic>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/main.h>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Alloc/free balance oracle (single-core; read after join()).
// ---------------------------------------------------------------------------
namespace {
std::atomic<int>    g_alloc{0};
std::atomic<int>    g_freed{0};
std::atomic<size_t> g_bytes_alloc{0};
std::atomic<size_t> g_bytes_freed{0};
std::atomic<int>    g_reported_live{-1}; // live count the actor reported via Status (−1 = never)
std::atomic<bool>   g_done{false};

void
reset_globals() {
    g_alloc.store(0);
    g_freed.store(0);
    g_bytes_alloc.store(0);
    g_bytes_freed.store(0);
    g_reported_live.store(-1);
    g_done.store(false);
}
} // namespace

class ManagedResource {
    std::vector<char> _data;

public:
    explicit ManagedResource(size_t size)
        : _data(size) {
        g_alloc.fetch_add(1);
        g_bytes_alloc.fetch_add(size);
    }
    ~ManagedResource() {
        g_freed.fetch_add(1);
        g_bytes_freed.fetch_add(_data.size());
    }
    [[nodiscard]] size_t
    size() const {
        return _data.size();
    }
};

// ---------------------------------------------------------------------------
// Protocol.
// ---------------------------------------------------------------------------
struct Allocate : public qb::Event {
    size_t size;
    explicit Allocate(size_t s)
        : size(s) {}
};
struct Status : public qb::Event {
    qb::ActorId reply_to;
    explicit Status(qb::ActorId r)
        : reply_to(r) {}
};
struct Report : public qb::Event {
    int    live_count;
    size_t live_bytes;
    Report(int c, size_t b)
        : live_count(c)
        , live_bytes(b) {}
};
struct GracefulShutdown : public qb::Event {}; // release everything, then kill
struct ForceShutdown : public qb::Event {};    // kill abruptly, resources still held
struct Killed : public qb::Event {             // actor → coordinator: I am about to die
    qb::ActorId who;
    explicit Killed(qb::ActorId w)
        : who(w) {}
};

class ResourceActor : public qb::Actor {
    std::vector<std::unique_ptr<ManagedResource>> _resources;
    qb::ActorId                                   _coord;

public:
    explicit ResourceActor(qb::ActorId coord)
        : _coord(coord) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Allocate>(*this);
        registerEvent<Status>(*this);
        registerEvent<GracefulShutdown>(*this);
        registerEvent<ForceShutdown>(*this);
        co_return true;
    }

    void
    on(const Allocate &e) {
        _resources.push_back(std::make_unique<ManagedResource>(e.size));
    }

    void
    on(const Status &e) {
        size_t bytes = 0;
        int    count = 0;
        for (const auto &r : _resources)
            if (r) {
                ++count;
                bytes += r->size();
            }
        to(e.reply_to).push<Report>(count, bytes);
    }

    void
    on(const GracefulShutdown &) {
        _resources.clear(); // explicit release; the destructor would do this anyway
        push<Killed>(_coord, id());
        kill();
    }

    void
    on(const ForceShutdown &) {
        // Resources are STILL held here: only the destructor (RAII) will free them.
        push<Killed>(_coord, id());
        kill();
    }
    // ~ResourceActor default: the unique_ptr vector frees every ManagedResource (RAII).
};

// ---------------------------------------------------------------------------
// Coordinator: allocate → status round-trip → (graceful | force) shutdown → stop on kill ack.
// `force_path` selects which teardown the single resource actor takes.
// ---------------------------------------------------------------------------
class ResourceCoordinator : public qb::Actor {
    const int    _n_alloc;
    const size_t _bytes;
    const bool   _force_path;
    qb::ActorId  _actor;

public:
    ResourceCoordinator(int n_alloc, size_t bytes, bool force_path)
        : _n_alloc(n_alloc)
        , _bytes(bytes)
        , _force_path(force_path) {}

    qb::io::async::task<bool>
    onInit() override {
        registerEvent<Report>(*this);
        registerEvent<Killed>(*this);

        _actor = addRefActor<ResourceActor>(id()).id();
        for (int i = 0; i < _n_alloc; ++i)
            to(_actor).push<Allocate>(_bytes);
        // Query status AFTER the allocations (mailbox-ordered): the report drives the shutdown.
        to(_actor).push<Status>(id());
        co_return true;
    }

    void
    on(const Report &e) {
        g_reported_live.store(e.live_count);
        // The actor reports exactly what we allocated, and is still holding them: not-all-freed.
        EXPECT_EQ(e.live_count, _n_alloc) << "the actor must report every live resource";
        EXPECT_EQ(e.live_bytes, static_cast<size_t>(_n_alloc) * _bytes);
        // Direct not-all-freed-before-shutdown oracle: while the actor is alive and holding its
        // resources, the global free count must lag the alloc count.
        EXPECT_LT(g_freed.load(), g_alloc.load()) << "resources must still be live before shutdown (none released early)";

        if (_force_path)
            to(_actor).push<ForceShutdown>();
        else
            to(_actor).push<GracefulShutdown>();
    }

    void
    on(const Killed &) {
        g_done.store(true);
        qb::Main::stop();
        kill();
    }
};

static void
run_cleanup(bool force_path) {
    reset_globals();
    constexpr int    n_alloc = 50;
    constexpr size_t bytes   = 1024;

    qb::Main main;
    main.core(0).addActor<ResourceCoordinator>(n_alloc, bytes, force_path);

    main.start(false);
    main.join();

    EXPECT_FALSE(main.hasError());
    EXPECT_TRUE(g_done.load()) << "the coordinator must reach its kill ack";

    // The status round-trip happened and saw every resource still live (none freed early).
    EXPECT_EQ(g_reported_live.load(), n_alloc) << "Status/Report round-trip must report all resources";

    // All resources allocated, and all freed by the time the engine drained — the SHUTDOWN freed
    // them (the mid-run report proved they were still live before shutdown).
    EXPECT_EQ(g_alloc.load(), n_alloc);
    EXPECT_EQ(g_freed.load(), g_alloc.load()) << "every resource must be freed at actor teardown";
    EXPECT_EQ(g_bytes_freed.load(), g_bytes_alloc.load());
}

// Test #1: graceful shutdown releases all resources.
TEST(ResourceCleanup, ReleasesAllResourcesOnGracefulShutdown) {
    run_cleanup(/*force_path*/ false);
}

// Test #2 (was empty: SUCCEED-by-omission): an abrupt failure-style kill with resources STILL held
// must also free everything via the destructor (RAII), not only the graceful path.
TEST(ResourceCleanup, ReleasesResourcesEvenAfterAbruptKill) {
    run_cleanup(/*force_path*/ true);
}
