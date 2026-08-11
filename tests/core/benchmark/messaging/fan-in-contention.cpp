/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/fan-in-contention.cpp
 * @brief Many producers → one consumer: measures single-mailbox write contention.
 *
 * Each of `producers` source actors pushes `msgs_per_producer` one-way `push<FanInMsg>` at the
 * same sink, so the whole run hammers one mailbox; the sink ends the engine with
 * `broadcast<KillEvent>()` the instant it has counted exactly `producers * msgs_per_producer`.
 * The contention axis is producer placement: when `producers_same_core_as_sink` is false and
 * `cappedBenchmarkCores() > 1`, producers are round-robined strictly onto cores *other than*
 * `consumer_core` (real cross-core MPSC traffic); otherwise everything shares one core.
 *
 * Benchmark methodology (this is a perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - per-iteration engine construction is hoisted out of the timed region with
 *     `PauseTiming()`/`ResumeTiming()`, so only `start(true)` + `join()` (the actual delivery
 *     work) is measured under `UseRealTime()`;
 *   - `messages_per_s` is the iteration-invariant total-work rate; `bytes_per_s` is published via
 *     `SetBytesProcessed` so the payload-volume column is comparable across benches;
 *   - the per-iteration counter assignments happen once *after* the timed loop (the old bench
 *     re-wrote them every iteration and also exported a `remote_producers` configuration
 *     self-counter — both dropped here);
 *   - a one-shot, out-of-loop correctness assert (`benchmark::DoNotOptimize` on a single counted
 *     run) catches a structurally broken bench (e.g. a sink that never reaches its quota) without
 *     becoming a timing gate.
 *
 * Drops the old `remote_producers` self-counter (it merely echoed the placement configuration).
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <memory>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkActors.h"
#include "../../shared/BenchmarkCores.h"

namespace {

struct FanInMsg final : qb::Event {};

// Producer core for index `i`: strictly off `consumer_core` (round-robin) when distributed and
// there is more than one core; otherwise the sink core (single-core fallback).
[[nodiscard]] std::uint32_t
producer_core_for_index(std::uint32_t const i, std::uint32_t const consumer_core, std::uint32_t const cap, bool const same_core) {
    if (same_core || cap <= 1u)
        return consumer_core;
    std::uint32_t walk = i % (cap - 1u);
    for (std::uint32_t c = 0; c < cap; ++c) {
        if (c == consumer_core)
            continue;
        if (walk == 0u)
            return c;
        --walk;
    }
    return consumer_core; // unreachable (loop above always finds a slot)
}

// Build the fan-in topology into `main`; returns the total messages the sink must observe.
[[nodiscard]] std::uint64_t
build_fan_in(qb::Main &main, std::uint32_t const producers, std::uint64_t const msgs_per, std::uint32_t const consumer_core,
             std::uint32_t const cap, bool const same_core, std::shared_ptr<std::atomic<std::uint64_t>> const &tally = nullptr) {
    const std::uint64_t total = static_cast<std::uint64_t>(producers) * msgs_per;
    auto const          sink  = main.addActor<qb::bench::CountAndKillSinkActor<FanInMsg>>(consumer_core, total, tally);
    for (std::uint32_t i = 0; i < producers; ++i) {
        const auto pc = producer_core_for_index(i, consumer_core, cap, same_core);
        main.addActor<qb::bench::LoopAndKillSourceActor<FanInMsg>>(pc, sink, msgs_per);
    }
    return total;
}

void
BM_FanIn_OneWayPush(benchmark::State &state) {
    const auto producers     = static_cast<std::uint32_t>(state.range(0));
    const auto msgs_per      = static_cast<std::uint64_t>(state.range(1));
    const auto consumer_core = static_cast<std::uint32_t>(state.range(2));
    const auto same_core     = state.range(3) != 0;

    const auto          cap   = qb::bench::cappedBenchmarkCores();
    const std::uint64_t total = static_cast<std::uint64_t>(producers) * msgs_per;

    // One-shot out-of-loop correctness probe: assert the single sink actually counted every
    // message across all producers (a dropped/mis-routed push is caught by a positive check,
    // not merely by a join() hang on a sink that never reaches quota).
    {
        auto     tally = std::make_shared<std::atomic<std::uint64_t>>(0);
        qb::Main probe;
        auto     expect = build_fan_in(probe, producers, msgs_per, consumer_core, cap, same_core, tally);
        probe.start(true);
        probe.join();
        if (tally->load(std::memory_order_relaxed) != expect) {
            state.SkipWithError("fan-in dropped messages: sink count != producers * msgs_per_producer");
            return;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        (void) build_fan_in(main, producers, msgs_per, consumer_core, cap, same_core);
        state.ResumeTiming();

        main.start(true);
        main.join();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * total));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * total * sizeof(FanInMsg)));
    state.counters["messages_per_s"] = benchmark::Counter(static_cast<double>(total), benchmark::Counter::kIsIterationInvariantRate);
}

void
ArgsFanIn(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    for (std::int64_t same : {0, 1}) {
        const std::uint32_t pmax = std::min<std::uint32_t>(cap, 8u);
        for (std::uint32_t p = 1; p <= pmax; ++p) {
            b->Args({static_cast<std::int64_t>(p), 50000, 0, same});
            if (cap > 1u)
                b->Args({static_cast<std::int64_t>(p), 50000, static_cast<std::int64_t>(cap - 1), same});
        }
    }
}

} // namespace

BENCHMARK(BM_FanIn_OneWayPush)
    ->Apply(ArgsFanIn)
    ->ArgNames({"producers", "msgs_per_producer", "consumer_core", "producers_same_core_as_sink"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
