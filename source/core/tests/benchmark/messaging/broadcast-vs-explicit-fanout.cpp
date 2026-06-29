/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/broadcast-vs-explicit-fanout.cpp
 * @brief `BroadcastId(core)` vs explicit per-actor `push` at matched total deliveries.
 *
 * All `sinks` sit on `consumer_core`. The producer issues either `waves` broadcast waves (each
 * wave delivers to every sink on that core) or `waves * sinks` explicit `push` calls — tuned so
 * both modes drive the SAME total deliveries (`sinks * waves`). The broadcast case requires
 * `producer_core != consumer_core` so the producer is not itself a recipient of its own broadcast.
 *
 * Delivery-latch design: the original bench coordinated shutdown through two FILE-SCOPE atomics
 * (`g_fanout_deliveries`, `g_fanout_target`). Those are replaced here by a single per-run
 * `shared_ptr<std::atomic<std::uint64_t>>` delivery latch (the count) plus a plain target value,
 * both injected into the sinks at construction. Each sink decrements/increments the shared latch
 * and the producer carries the target; no mutable global state leaks between iterations, so two
 * benches can never race a stale global, and the latch lifetime is the run's lifetime (held by the
 * actors that touch it).
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - per-iteration construction is hoisted out of the timed region with `PauseTiming()`;
 *   - `deliveries_per_s` (the comparable cross-mode throughput) and `bytes_per_s` are published;
 *   - counters are assigned once after the loop;
 *   - a one-shot out-of-loop probe run (`DoNotOptimize`) catches a topology that never completes.
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <memory>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"

namespace {

struct FanoutMsg final : qb::Event {};

/// Shared per-run delivery latch: every sink fetch-adds into `count`; whoever hits `target` ends
/// the run. Held by `shared_ptr` so it outlives the actors and is destroyed with the run.
struct DeliveryLatch {
    std::atomic<std::uint64_t> count{0};
    std::uint64_t              target{0};
};

class FanoutSinkActor final : public qb::Actor {
    std::shared_ptr<DeliveryLatch> _latch;

public:
    explicit FanoutSinkActor(std::shared_ptr<DeliveryLatch> latch)
        : _latch(std::move(latch)) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<FanoutMsg>(*this);
        co_return true;
    }

    void
    on(FanoutMsg const &) {
        const auto v = _latch->count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (v == _latch->target)
            broadcast<qb::KillEvent>();
    }
};

enum class FanoutMode : std::uint8_t { Broadcast, Explicit };

template <FanoutMode Mode>
class FanoutProducerActor final : public qb::Actor {
    const qb::CoreId      _broadcast_core;
    const qb::ActorIdList _ids;
    const std::uint64_t   _waves;

public:
    FanoutProducerActor(qb::CoreId const broadcast_core, qb::ActorIdList ids, std::uint64_t const waves)
        : _broadcast_core(broadcast_core)
        , _ids(std::move(ids))
        , _waves(waves) {}

    qb::io::async::task<bool>
    onInit() final {
        if constexpr (Mode == FanoutMode::Broadcast) {
            for (std::uint64_t w = 0; w < _waves; ++w)
                push<FanoutMsg>(qb::BroadcastId(static_cast<std::uint32_t>(_broadcast_core)));
        } else {
            for (std::uint64_t w = 0; w < _waves; ++w)
                for (auto const &id : _ids)
                    push<FanoutMsg>(id);
        }
        kill();
        co_return true;
    }
};

template <FanoutMode Mode>
void
build_fanout(qb::Main &main, std::uint32_t const n, std::uint64_t const waves, std::uint32_t const producer_c, std::uint32_t const consumer_c,
             std::shared_ptr<DeliveryLatch> const &latch) {
    latch->count.store(0, std::memory_order_relaxed);
    latch->target = static_cast<std::uint64_t>(n) * waves;

    qb::ActorIdList ids;
    ids.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i)
        ids.push_back(main.addActor<FanoutSinkActor>(consumer_c, latch));

    const auto bcore = static_cast<qb::CoreId>(consumer_c);
    if constexpr (Mode == FanoutMode::Broadcast)
        main.addActor<FanoutProducerActor<FanoutMode::Broadcast>>(producer_c, bcore, qb::ActorIdList{}, waves);
    else
        main.addActor<FanoutProducerActor<FanoutMode::Explicit>>(producer_c, bcore, ids, waves);
}

template <FanoutMode Mode>
void
BM_Fanout_Deliveries(benchmark::State &state) {
    const auto n          = static_cast<std::uint32_t>(state.range(0));
    const auto waves      = static_cast<std::uint64_t>(state.range(1));
    const auto producer_c = static_cast<std::uint32_t>(state.range(2));
    const auto consumer_c = static_cast<std::uint32_t>(state.range(3));
    const auto tot        = static_cast<std::uint64_t>(n) * waves;

    if constexpr (Mode == FanoutMode::Broadcast) {
        if (producer_c == consumer_c) {
            state.SkipWithError("broadcast fanout: producer_core must differ from consumer_core");
            return;
        }
    }

    // One-shot out-of-loop correctness probe (its own latch, not shared with the timed runs).
    {
        qb::Main   probe;
        auto const probe_latch = std::make_shared<DeliveryLatch>();
        build_fanout<Mode>(probe, n, waves, producer_c, consumer_c, probe_latch);
        probe.start(true);
        probe.join();
        benchmark::DoNotOptimize(probe_latch->count.load());
    }

    auto const latch = std::make_shared<DeliveryLatch>();
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        build_fanout<Mode>(main, n, waves, producer_c, consumer_c, latch);
        state.ResumeTiming();

        main.start(true);
        main.join();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * tot));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * tot * sizeof(FanoutMsg)));
    state.counters["deliveries_per_s"] = benchmark::Counter(static_cast<double>(tot), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["waves_per_s"]      = benchmark::Counter(static_cast<double>(waves), benchmark::Counter::kIsIterationInvariantRate);
}

void
ApplyFanoutExplicitArgs(benchmark::internal::Benchmark *b) {
    const auto              cap    = qb::bench::cappedBenchmarkCores();
    constexpr std::uint64_t kTotal = 500000;
    const std::uint32_t     nmax   = std::min<std::uint32_t>(8u, std::max(2u, cap));
    for (std::uint32_t n = 2; n <= nmax; ++n) {
        const std::uint64_t waves = kTotal / n;
        b->Args({static_cast<std::int64_t>(n), static_cast<std::int64_t>(waves), 0, 0});
        if (cap > 1u)
            b->Args({static_cast<std::int64_t>(n), static_cast<std::int64_t>(waves), 0, static_cast<std::int64_t>(cap - 1)});
    }
}

void
ApplyFanoutBroadcastArgs(benchmark::internal::Benchmark *b) {
    const auto              cap    = qb::bench::cappedBenchmarkCores();
    constexpr std::uint64_t kTotal = 500000;
    if (cap <= 1u) {
        b->Args({2, static_cast<std::int64_t>(kTotal / 2), 0, 0});
        return;
    }
    const std::uint32_t nmax = std::min<std::uint32_t>(8u, cap);
    for (std::uint32_t n = 2; n <= nmax; ++n) {
        const std::uint64_t waves = kTotal / n;
        b->Args({static_cast<std::int64_t>(n), static_cast<std::int64_t>(waves), 0, static_cast<std::int64_t>(cap - 1)});
    }
}

} // namespace

BENCHMARK_TEMPLATE(BM_Fanout_Deliveries, FanoutMode::Broadcast)
    ->Apply(ApplyFanoutBroadcastArgs)
    ->ArgNames({"sinks", "waves", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_Fanout_Deliveries, FanoutMode::Explicit)
    ->Apply(ApplyFanoutExplicitArgs)
    ->ArgNames({"sinks", "waves", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
