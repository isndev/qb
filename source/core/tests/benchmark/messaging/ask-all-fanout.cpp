/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/ask-all-fanout.cpp
 * @brief Scatter-gather fan-out latency: `qb::ask_all` (await every reply) vs `qb::ask_any` (first wins).
 *
 * An asker actor `spawn()`s one scoped coroutine that runs `kBenchIters` iterations of a scatter
 * against `responders` `Market` actors (shared `shared/AskResponders.h`):
 *   - ALL — `co_await qb::ask_all(ctx, targets, Ping{seq}, timeout)`: N asks in flight at once, the
 *     gather resolves only when EVERY reply has arrived (tail-latency dominated);
 *   - ANY — `co_await qb::ask_any(ctx, targets, Ping{seq}, timeout)`: N asks race, the gather
 *     resolves on the FIRST reply (best-of-N latency); the losers linger to their own timeout.
 * Each scatter's wall-clock latency (`steady_clock` delta around the `co_await`) is folded into a
 * `pg::latency` histogram, pricing the whole `when_all` / `when_any` fan-out machinery.
 *
 * Responder placement spreads the N targets across `cappedBenchmarkCores()` (asker on core 0), so a
 * multi-core run keeps N cross-core asks genuinely in flight; on a single-core runner they share the
 * asker's core (still a valid, if uncontended, scatter).
 *
 * Latency hand-off via the shared `shared/LatencyFlush.h` idiom (asker owns a
 * `shared_ptr<pg::latency>` captured into the coroutine; its destructor publishes into the
 * cross-thread sink; `QB_ACTOR_BENCH_HISTOGRAM=1` dumps percentiles).
 *
 * Sample-count guard (NOT a `EXPECT_LT(duration,…)` ctest gate — this is a perf harness): one
 * latency sample per completed scatter ⇒ `latency_samples` MUST equal `kBenchIters`. A one-shot,
 * out-of-loop probe run asserts that equality, catching a scatter that never resolves (a dropped or
 * mis-correlated reply) before any timing.
 *
 * Benchmark methodology: engine construction hoisted out of the timed region (`PauseTiming()`);
 * `start(false)` + `join()` timed under `UseRealTime()`; counters (`scatters_per_s`,
 * `asks_issued_per_s`, `mean_rtt_ns`, `latency_samples`) assigned once per iteration. Built/run like
 * the rest of the actor-coroutine suites (benign cross-core teardown residual).
 */

#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/main.h>

#include "../../shared/AskResponders.h"
#include "../../shared/BenchmarkCores.h"
#include "../../shared/LatencyFlush.h"
#include "../../shared/TestLatency.h"

using namespace std::chrono_literals;
using qb::test::Market;
using qb::test::Ping;

namespace {
constexpr std::uint32_t kBenchIters = 20'000u;

enum class ScatterMode : std::uint8_t { All, Any };

// Self-signal: shut the asker down cleanly once its coroutine has run to completion.
struct ScatterBenchDone final : qb::Event {};

template <ScatterMode Mode>
class ScatterAsker final : public qb::Actor {
    const std::vector<qb::ActorId>                    _targets;
    std::shared_ptr<pg::latency<1000 * 1000, 900000>> _latency;

public:
    ScatterAsker(std::vector<qb::ActorId> targets, std::shared_ptr<pg::latency<1000 * 1000, 900000>> latency)
        : _targets(std::move(targets))
        , _latency(std::move(latency)) {}

    ~ScatterAsker() final {
        qb::bench::flush_latency_to_sink(*_latency);
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Ping>(*this);
        registerEvent<ScatterBenchDone>(*this);
        auto targets = _targets;
        auto lat     = _latency;
        spawn([targets, lat](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (std::uint32_t i = 0; i < kBenchIters; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                if constexpr (Mode == ScatterMode::All) {
                    auto r = co_await qb::ask_all(ctx, targets, Ping{static_cast<int>(i)}, 2s);
                    benchmark::DoNotOptimize(r.data());
                } else {
                    auto r = co_await qb::ask_any(ctx, targets, Ping{static_cast<int>(i)}, 2s);
                    benchmark::DoNotOptimize(r.response);
                }
                lat->add(std::chrono::steady_clock::now() - t0);
            }
            ctx.push<ScatterBenchDone>();
        });
        co_return true;
    }

    void
    on(Ping &e) {
        resolve_ask(e);
    }

    void
    on(ScatterBenchDone const &) {
        for (auto const t : _targets)
            push<qb::KillEvent>(t);
        kill();
    }
};

template <ScatterMode Mode>
void
build_scatter(qb::Main &main, std::uint32_t const responders, std::shared_ptr<pg::latency<1000 * 1000, 900000>> const &latency) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    std::vector<qb::ActorId> targets;
    targets.reserve(responders);
    for (std::uint32_t i = 0; i < responders; ++i) {
        // Spread responders across the cores other than the asker's (core 0) when multi-core.
        const std::uint32_t core = cap > 1u ? 1u + (i % (cap - 1u)) : 0u;
        targets.push_back(main.addActor<Market>(core));
    }
    main.addActor<ScatterAsker<Mode>>(0, std::move(targets), latency);
}

void
record_scatter_counters(benchmark::State &state, std::uint32_t const responders) {
    const auto lat                    = qb::bench::last_latency_stats_snapshot();
    state.counters["scatters_per_s"]  = benchmark::Counter(static_cast<double>(kBenchIters), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["asks_issued_per_s"] =
        benchmark::Counter(static_cast<double>(kBenchIters) * static_cast<double>(responders), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["latency_samples"] = static_cast<double>(lat.samples);
    if (lat.samples)
        state.counters["mean_rtt_ns"] = benchmark::Counter(lat.mean_round_trip_ns, benchmark::Counter::kAvgIterations);
}

template <ScatterMode Mode>
void
BM_Ask_Scatter(benchmark::State &state) {
    const auto responders = static_cast<std::uint32_t>(state.range(0));

    // One-shot out-of-loop sample-count guard: every scatter must resolve exactly once.
    {
        qb::bench::reset_last_latency_stats();
        qb::Main   probe;
        auto const latency = std::make_shared<pg::latency<1000 * 1000, 900000>>();
        build_scatter<Mode>(probe, responders, latency);
        probe.start(false);
        probe.join();
        const auto lat = qb::bench::last_latency_stats_snapshot();
        if (lat.samples != kBenchIters) {
            state.SkipWithError("scatter sample count != kBenchIters: a scatter never resolved (dropped/mis-correlated reply)");
            return;
        }
    }

    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        state.PauseTiming();
        qb::Main   main;
        auto const latency = std::make_shared<pg::latency<1000 * 1000, 900000>>();
        build_scatter<Mode>(main, responders, latency);
        state.ResumeTiming();

        main.start(false);
        main.join();
        record_scatter_counters(state, responders);
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kBenchIters * responders));
}

void
ApplyScatterArgs(benchmark::internal::Benchmark *b) {
    for (std::int64_t n : {2, 4, 8})
        b->Args({n});
}

} // namespace

BENCHMARK_TEMPLATE(BM_Ask_Scatter, ScatterMode::All)
    ->Apply(ApplyScatterArgs)
    ->ArgNames({"responders"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_Ask_Scatter, ScatterMode::Any)
    ->Apply(ApplyScatterArgs)
    ->ArgNames({"responders"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
