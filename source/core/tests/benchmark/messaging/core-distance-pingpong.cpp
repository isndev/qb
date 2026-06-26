/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/core-distance-pingpong.cpp
 * @brief Ping-pong throughput vs (ping_core, pong_core) — isolate cross-core delivery cost.
 *
 * The `send`/`reply` path and payload are fixed; only the (ping_core, pong_core) placement changes,
 * so the sweep prices "core distance": same-core (both on `p`), far (pong on the last core), and
 * adjacent (pong on `p+1`). A ping bounces a TTL-decrementing `CdTinyEvent` off the pong actor
 * `initial_ttl` times, then both actors `kill()`.
 *
 * Measured round-trip tally: the ping actor counts the round-trips it actually completed and
 * publishes that into a per-run `shared_ptr<std::atomic<std::uint64_t>>`. A one-shot, out-of-loop
 * probe run asserts the tally equals `initial_ttl`, so a chain that stalls early (e.g. a dropped
 * cross-core reply) is caught BEFORE any timing — never as a `EXPECT_LT(duration,…)` ctest gate
 * (this is a perf harness).
 *
 * Benchmark methodology: construction hoisted out of the timed region (`PauseTiming()`); counters
 * (`round_trips_per_s` + `bytes_per_s`) assigned once after the loop under `UseRealTime()`. The
 * cross-core grid points self-skip when `cappedBenchmarkCores() < 2`.
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <memory>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"

namespace {

struct CdTinyEvent final : qb::Event {
    std::uint64_t _ttl = 0;
    explicit CdTinyEvent(std::uint64_t const ttl)
        : _ttl(ttl) {}
};

class CdPongActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<CdTinyEvent>(*this);
        co_return true;
    }

    void
    on(CdTinyEvent &event) {
        --event._ttl;
        reply(event);
    }
};

class CdPingActor final : public qb::Actor {
    const std::uint64_t                         _max;
    const qb::ActorId                           _peer;
    std::uint64_t                               _round_trips = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> _tally;

public:
    CdPingActor(std::uint64_t const max, qb::ActorId const peer, std::shared_ptr<std::atomic<std::uint64_t>> tally)
        : _max(max)
        , _peer(peer)
        , _tally(std::move(tally)) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<CdTinyEvent>(*this);
        send<CdTinyEvent>(_peer, _max);
        co_return true;
    }

    void
    on(CdTinyEvent &event) {
        ++_round_trips;
        if (event._ttl)
            reply(event);
        else {
            _tally->store(_round_trips, std::memory_order_release);
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

void
build_core_distance(qb::Main &main, std::uint32_t const ping_core, std::uint32_t const pong_core, std::uint64_t const ttl,
                    std::shared_ptr<std::atomic<std::uint64_t>> const &tally) {
    auto const pong = main.addActor<CdPongActor>(pong_core);
    main.addActor<CdPingActor>(ping_core, ttl, pong, tally);
}

void
BM_CoreDistance_PingPong(benchmark::State &state) {
    const auto ping_core = static_cast<std::uint32_t>(state.range(0));
    const auto pong_core = static_cast<std::uint32_t>(state.range(1));
    const auto ttl       = static_cast<std::uint64_t>(state.range(2));

    if (ping_core != pong_core && qb::bench::cappedBenchmarkCores() < 2u) {
        state.SkipWithError("requires-multicore: a distinct (ping_core, pong_core) needs >= 2 benchmark cores");
        return;
    }

    // One-shot out-of-loop round-trip tally guard: the chain must complete exactly `ttl` round-trips.
    {
        qb::Main   probe;
        auto const tally = std::make_shared<std::atomic<std::uint64_t>>(0);
        build_core_distance(probe, ping_core, pong_core, ttl, tally);
        probe.start(true);
        probe.join();
        if (tally->load(std::memory_order_acquire) != ttl)
            state.SkipWithError("core-distance round-trip tally failed: chain did not complete every round-trip");
    }

    auto const tally = std::make_shared<std::atomic<std::uint64_t>>(0);
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        build_core_distance(main, ping_core, pong_core, ttl, tally);
        state.ResumeTiming();
        main.start(true);
        main.join();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * ttl));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * 2ull * ttl * sizeof(CdTinyEvent)));
    state.counters["round_trips_per_s"] = benchmark::Counter(static_cast<double>(ttl), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["messages_per_s"] =
        benchmark::Counter(static_cast<double>(2ull * ttl + 1ull), benchmark::Counter::kIsIterationInvariantRate);
}

void
ArgsCoreDistanceGrid(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    const auto ttl = 1ull << 14;
    for (std::uint32_t p = 0; p < cap; ++p) {
        b->Args({static_cast<std::int64_t>(p), static_cast<std::int64_t>(p), static_cast<std::int64_t>(ttl)});
        if (cap > 1u) {
            // Not named "far": Windows headers may #define far (16-bit legacy ABI).
            const std::uint32_t far_core = cap - 1u;
            if (p != far_core)
                b->Args({static_cast<std::int64_t>(p), static_cast<std::int64_t>(far_core), static_cast<std::int64_t>(ttl)});
            const std::uint32_t adj = (p + 1u) % cap;
            if (adj != p && adj != far_core)
                b->Args({static_cast<std::int64_t>(p), static_cast<std::int64_t>(adj), static_cast<std::int64_t>(ttl)});
        }
    }
}

} // namespace

BENCHMARK(BM_CoreDistance_PingPong)
    ->Apply(ArgsCoreDistanceGrid)
    ->ArgNames({"ping_core", "pong_core", "initial_ttl"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
