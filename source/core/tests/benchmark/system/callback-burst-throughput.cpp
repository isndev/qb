/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/system/callback-burst-throughput.cpp
 * @brief One-way throughput: an `ICallback` producer sends in bursts vs one message per tick.
 *
 * Same total `messages` and the same `push<BurstMsg>` API; only the burst size (pushes per
 * `on(qb::LoopEvent)` tick) changes, isolating scheduler/callback interaction from raw pipe
 * throughput. The producer registers itself as an `ICallback`, drains `BurstPerCallbackV` messages
 * per tick until `messages` are sent, then self-kills; the shared
 * `qb::bench::CountAndKillSinkActor<BurstMsg>` ends the engine the instant it has counted them all.
 *
 * `registerCallback` / tick scheduling is runtime-defined, so this measures end-to-end throughput
 * under that policy, not an abstract "callback cost". To make that policy observable, the producer
 * counts the LoopEvent ticks it actually serviced into a process-global atom; the bench publishes BOTH
 * the OBSERVED callback count (what the scheduler delivered) and the DERIVED lower bound
 * (`ceil(messages / burst)`, the minimum ticks needed if every tick drained a full burst). Comparing
 * the two reveals tick granularity vs. the ideal.
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - per-iteration engine construction is hoisted out of the timed region with
 *     `PauseTiming()`/`ResumeTiming()`; only `start(true)` + `join()` is measured;
 *   - `SetItemsProcessed` / `SetBytesProcessed` and the counters (messages_per_s, observed vs derived
 *     callbacks, burst_size) are assigned ONCE after the loop, from the LAST run's observed count;
 *   - a one-shot, out-of-loop probe runs one full topology, `DoNotOptimize`s the observed-callback
 *     total, and verifies the producer drained at least the derived minimum of ticks — so a producer
 *     that silently stops early is caught before any timing.
 *
 * Rewritten from the former `bm-producer-burst.cpp` onto the shared sink skeleton.
 */

#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkActors.h"
#include "../../shared/BenchmarkCores.h"

namespace {

struct BurstMsg final : qb::Event {};

// Observed callback (LoopEvent tick) count from the last run — mirrored from the producer worker
// thread to the benchmark thread, read after join().
std::atomic<std::uint64_t> g_observed_callbacks{0};

// Producer: registers as ICallback; each tick drains up to BurstPerCallbackV messages until `total`
// are sent, counting the ticks it serviced, then self-kills.
template <std::uint32_t BurstPerCallbackV>
class BurstCallbackProducerActor final
    : public qb::Actor
    , public qb::ICallback {
    const qb::ActorId   _dst;
    const std::uint64_t _total;
    std::uint64_t       _sent      = 0;
    std::uint64_t       _callbacks = 0;

public:
    BurstCallbackProducerActor(qb::ActorId const dst, std::uint64_t const total)
        : _dst(dst)
        , _total(total) {}

    qb::io::async::task<bool>
    onInit() final {
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &) final {
        ++_callbacks;
        constexpr std::uint32_t kBurst = BurstPerCallbackV;
        const std::uint64_t     chunk  = std::min<std::uint64_t>(kBurst, _total - _sent);
        for (std::uint64_t i = 0; i < chunk; ++i)
            push<BurstMsg>(_dst);
        _sent += chunk;
        if (_sent >= _total) {
            g_observed_callbacks.store(_callbacks, std::memory_order_relaxed);
            kill();
        }
    }
};

// Build the producer→sink topology into `main`; returns the count the sink must observe.
template <std::uint32_t BurstPerCallbackV>
[[nodiscard]] std::uint64_t
build_topology(qb::Main &main, std::uint64_t const total, std::uint32_t const producer_c, std::uint32_t const consumer_c) {
    auto const sink = main.addActor<qb::bench::CountAndKillSinkActor<BurstMsg>>(consumer_c, total);
    main.addActor<BurstCallbackProducerActor<BurstPerCallbackV>>(producer_c, sink, total);
    return total;
}

template <std::uint32_t BurstPerCallbackV>
void
BM_ProducerBurst_OneWay(benchmark::State &state) {
    const auto total      = static_cast<std::uint64_t>(state.range(0));
    const auto producer_c = static_cast<std::uint32_t>(state.range(1));
    const auto consumer_c = static_cast<std::uint32_t>(state.range(2));

    constexpr std::uint64_t kBurst        = static_cast<std::uint64_t>(BurstPerCallbackV);
    const std::uint64_t     derived_ticks = (total + kBurst - 1ull) / kBurst; // minimum ticks if every tick drains a full burst

    // One-shot out-of-loop probe: the producer must drain at least the derived minimum of ticks (a
    // producer that stopped early would under-deliver and hang the sink's join()).
    {
        qb::Main probe;
        g_observed_callbacks.store(0, std::memory_order_relaxed);
        auto expect = build_topology<BurstPerCallbackV>(probe, total, producer_c, consumer_c);
        probe.start(true);
        probe.join();
        std::uint64_t observed = g_observed_callbacks.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(expect);
        benchmark::DoNotOptimize(observed);
        if (observed < derived_ticks) {
            state.SkipWithError("producer serviced fewer callbacks than the derived minimum (drained early)");
            return;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        g_observed_callbacks.store(0, std::memory_order_relaxed);
        (void) build_topology<BurstPerCallbackV>(main, total, producer_c, consumer_c);
        state.ResumeTiming();

        main.start(true);
        main.join();
    }

    const std::uint64_t observed = g_observed_callbacks.load(std::memory_order_relaxed);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * total));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * total * sizeof(BurstMsg)));
    state.counters["messages_per_s"]          = benchmark::Counter(static_cast<double>(total), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["observed_callbacks_per_s"] = benchmark::Counter(static_cast<double>(observed), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["derived_callbacks_per_s"] =
        benchmark::Counter(static_cast<double>(derived_ticks), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["burst_size"] = static_cast<double>(BurstPerCallbackV);
}

void
ApplyBurstArgs(benchmark::internal::Benchmark *b) {
    const auto             cap    = qb::bench::cappedBenchmarkCores();
    constexpr std::int64_t kTotal = 500000;
    b->Args({kTotal, 0, 0});
    if (cap > 1u)
        b->Args({kTotal, 0, static_cast<std::int64_t>(cap - 1)});
}

} // namespace

BENCHMARK_TEMPLATE(BM_ProducerBurst_OneWay, 1)
    ->Apply(ApplyBurstArgs)
    ->ArgNames({"messages", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_ProducerBurst_OneWay, 4)
    ->Apply(ApplyBurstArgs)
    ->ArgNames({"messages", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_ProducerBurst_OneWay, 16)
    ->Apply(ApplyBurstArgs)
    ->ArgNames({"messages", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_ProducerBurst_OneWay, 64)
    ->Apply(ApplyBurstArgs)
    ->ArgNames({"messages", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
