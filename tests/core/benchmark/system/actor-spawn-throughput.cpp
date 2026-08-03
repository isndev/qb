/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/system/actor-spawn-throughput.cpp
 * @brief Actor registration + first-frame throughput, and `KillEvent` broadcast-kill teardown.
 *
 * Two engine-level benches over actor lifecycle at scale, shaped from the qb-core system suite:
 *   - `BM_ActorSpawn_FirstFrame` — register `actors` trivial actors on one core (the
 *     `actor-dependency` 2048-spawn shape) and run the engine to completion. Each actor self-kills in
 *     `onInit()`, so the run measures bulk `addActor<>()` registration + first-frame init + the core
 *     draining its actor set to empty (the spawn/teardown round-trip), not steady-state messaging.
 *   - `BM_ActorSpawn_BroadcastKill` — register `actors` targets on core 1 plus one sender on core 0
 *     that, in `onInit()`, broadcasts a single `KillEvent` to `BroadcastId(1)` (the `actor-add`
 *     1024-kill shape). This isolates the cost of fanning ONE broadcast kill across N live actors and
 *     reaping them, vs. the per-actor self-kill of the first bench.
 *
 * Both keep the system suites' load-bearing oracle as a benchmark-time invariant via a live-instance
 * counter: every target actor `++`s on construction and `--`s on destruction, so after `join()` the
 * live count MUST be 0 (no survivors) and the built count MUST equal `actors`. A broadcast kill that
 * missed an actor, or a registration that silently dropped one, fails this OUT of the timed loop
 * (one-shot) — never an `EXPECT_LT(duration,…)` gate; this is a perf harness.
 *
 * Benchmark methodology:
 *   - per-iteration engine construction + the (large) `addActor<>()` registration loop is hoisted out
 *     of the timed region with `PauseTiming()`/`ResumeTiming()`; only `start(true)` + `join()` (the
 *     init/kill/reap work) is measured under `UseRealTime()`;
 *   - `SetItemsProcessed` is the total actors processed, assigned ONCE after the loop.
 */

#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"

namespace {

// Live-instance + ever-built counters: the spawn/kill correctness oracle (must return to 0 / equal N).
std::atomic<std::int64_t> g_alive{0};
std::atomic<std::int64_t> g_built{0};

void
reset_counters() {
    g_alive.store(0, std::memory_order_relaxed);
    g_built.store(0, std::memory_order_relaxed);
}

// Self-killing target: counts construct/destruct, self-kills in onInit (first-frame spawn shape).
class SelfKillTarget final : public qb::Actor {
public:
    SelfKillTarget() {
        g_alive.fetch_add(1, std::memory_order_relaxed);
        g_built.fetch_add(1, std::memory_order_relaxed);
    }
    ~SelfKillTarget() override {
        g_alive.fetch_sub(1, std::memory_order_relaxed);
    }

    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

// Broadcast-kill target: counts construct/destruct, stays alive until a KillEvent reaps it.
class KillTarget final : public qb::Actor {
public:
    KillTarget() {
        g_alive.fetch_add(1, std::memory_order_relaxed);
        g_built.fetch_add(1, std::memory_order_relaxed);
    }
    ~KillTarget() override {
        g_alive.fetch_sub(1, std::memory_order_relaxed);
    }

    qb::io::async::task<bool>
    onInit() final {
        co_return true;
    }
};

// Sender: broadcasts one KillEvent to every actor on core 1, then self-kills (actor-add 1024-kill).
class BroadcastKillSender final : public qb::Actor {
public:
    BroadcastKillSender() = default;

    qb::io::async::task<bool>
    onInit() final {
        push<qb::KillEvent>(id());               // self
        push<qb::KillEvent>(qb::BroadcastId(1)); // every actor on core 1
        co_return true;
    }
};

// ---------------------------------------------------------------------------
// Bench 1: registration + first-frame init + self-kill teardown.
// ---------------------------------------------------------------------------
void
BM_ActorSpawn_FirstFrame(benchmark::State &state) {
    const auto actors = static_cast<std::uint32_t>(state.range(0));

    // One-shot out-of-loop correctness probe: every spawned actor must be built and reaped.
    {
        reset_counters();
        qb::Main probe;
        for (std::uint32_t i = 0; i < actors; ++i)
            probe.addActor<SelfKillTarget>(0);
        probe.start(true);
        probe.join();
        auto       built = g_built.load(std::memory_order_relaxed);
        const auto alive = g_alive.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(built);
        if (probe.hasError() || built != static_cast<std::int64_t>(actors) || alive != 0) {
            state.SkipWithError("spawn topology did not build/reap every actor");
            return;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        reset_counters();
        qb::Main main;
        for (std::uint32_t i = 0; i < actors; ++i)
            main.addActor<SelfKillTarget>(0);
        state.ResumeTiming();

        main.start(true);
        main.join();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * actors));
    state.counters["actors"] = static_cast<double>(actors);
}

// ---------------------------------------------------------------------------
// Bench 2: one broadcast KillEvent reaps N live actors.
// ---------------------------------------------------------------------------
void
BM_ActorSpawn_BroadcastKill(benchmark::State &state) {
    const auto actors = static_cast<std::uint32_t>(state.range(0));

    auto build = [actors](qb::Main &main) {
        main.addActor<BroadcastKillSender>(0);
        auto builder = main.core(1).builder();
        for (std::uint32_t i = 0; i < actors; ++i)
            builder.addActor<KillTarget>();
    };

    // One-shot out-of-loop correctness probe: the single broadcast must leave ZERO survivors.
    {
        reset_counters();
        qb::Main probe;
        build(probe);
        probe.start(true);
        probe.join();
        auto       built = g_built.load(std::memory_order_relaxed);
        const auto alive = g_alive.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(built);
        if (probe.hasError() || built != static_cast<std::int64_t>(actors) || alive != 0) {
            state.SkipWithError("broadcast kill left survivors or dropped a target");
            return;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        reset_counters();
        qb::Main main;
        build(main);
        state.ResumeTiming();

        main.start(true);
        main.join();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * actors));
    state.counters["actors"] = static_cast<double>(actors);
}

void
ArgsActors(benchmark::internal::Benchmark *b) {
    // Shapes from the system suites: 1024 (actor-add kill) and 2048 (actor-dependency spawn),
    // plus smaller points for the registration-cost curve.
    for (std::int64_t n : {128, 512, 1024, 2048})
        b->Args({n});
}

} // namespace

BENCHMARK(BM_ActorSpawn_FirstFrame)->Apply(ArgsActors)->ArgNames({"actors"})->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_ActorSpawn_BroadcastKill)->Apply(ArgsActors)->ArgNames({"actors"})->Unit(benchmark::kMillisecond)->UseRealTime();

BENCHMARK_MAIN();
