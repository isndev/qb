/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/ask-roundtrip.cpp
 * @brief Native coroutine `qb::ask` request/response round-trip latency (same-core and cross-core).
 *
 * A Trader actor `spawn()`s one scoped coroutine that issues `kBenchAsks` back-to-back
 * `co_await qb::ask(ctx, market, Ping{seq}, timeout)` calls against a `Market` responder (shared
 * `shared/AskResponders.h` — always answers `seq * 2`). Each ask's wall-clock round-trip
 * (`steady_clock` delta around the `co_await`) is folded into a `pg::latency` histogram, so this
 * prices the FULL coroutine RPC path: request send, correlation registry, awaiter park/resume, and
 * reply routing — not just a raw event hop.
 *
 * Two placement variants:
 *   - SAME-CORE — trader and market on core 0 (the awaiter, timer and registry are all core-local;
 *     only nothing crosses);
 *   - CROSS-CORE — market on the far core (`cappedBenchmarkCores() - 1`): the request and reply
 *     traverse the lock-free pipes while the awaiter/timer stay on the trader's core. Registered
 *     unconditionally but SELF-SKIPS via `SkipWithError` when `cappedBenchmarkCores() < 2`, so it is
 *     always visible in the report yet never degenerates into the same-core case.
 *
 * Latency hand-off: the trader owns a `shared_ptr<pg::latency>` captured into the coroutine; its
 * destructor publishes mean + sample count into the cross-thread sink via the shared
 * `shared/LatencyFlush.h` idiom (`QB_ACTOR_BENCH_HISTOGRAM=1` also dumps percentiles).
 *
 * Sample-count guard (NOT a `EXPECT_LT(duration,…)` ctest gate — this is a perf harness): every ask
 * must resolve exactly once, so `latency_samples` MUST equal `kBenchAsks`. A one-shot, out-of-loop
 * probe run asserts that equality, catching a dropped/mis-correlated reply before any timing.
 *
 * Benchmark methodology: engine construction is hoisted out of the timed region (`PauseTiming()`);
 * `start(false)` + `join()` is timed under `UseRealTime()`; counters (`round_trips_per_s`,
 * `mean_rtt_ns`, `latency_samples`) are read from the sink and assigned once per iteration.
 *
 * Run/built like the rest of the actor-coroutine suites; cross-core asks leave a fixed, benign
 * teardown residual (harmless to timing).
 */

#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <memory>

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
constexpr std::uint32_t kBenchAsks = 50'000u;

// Self-signal: shut the trader down cleanly once its coroutine has run to completion (kill-based
// shutdown reaps completed frames; calling qb::Main::stop() from inside the coroutine can leave a
// just-completed frame un-drained at teardown — see the ask-roundtrip system test).
struct AskBenchDone final : qb::Event {};

// Drives `kBenchAsks` sequential asks at `_market`, sampling each round-trip into `_latency`.
class BenchTrader final : public qb::Actor {
    const qb::ActorId                                 _market;
    std::shared_ptr<pg::latency<1000 * 1000, 900000>> _latency;

public:
    BenchTrader(qb::ActorId market, std::shared_ptr<pg::latency<1000 * 1000, 900000>> latency)
        : _market(market)
        , _latency(std::move(latency)) {}

    ~BenchTrader() final {
        qb::bench::flush_latency_to_sink(*_latency);
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Ping>(*this);
        registerEvent<AskBenchDone>(*this);
        auto mkt = _market;
        auto lat = _latency;
        spawn([mkt, lat](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            for (std::uint32_t i = 0; i < kBenchAsks; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                auto       r  = co_await qb::ask(ctx, mkt, Ping{static_cast<int>(i)}, 500ms);
                lat->add(std::chrono::steady_clock::now() - t0);
                benchmark::DoNotOptimize(r.response);
            }
            ctx.push<AskBenchDone>(); // shut down via our own handler, cleanly
        });
        co_return true;
    }

    void
    on(Ping &e) {
        resolve_ask(e); // route the reply to the parked coroutine
    }

    void
    on(AskBenchDone const &) {
        push<qb::KillEvent>(_market);
        kill();
    }
};

void
build_ask_roundtrip(qb::Main &main, std::uint32_t const market_core, std::shared_ptr<pg::latency<1000 * 1000, 900000>> const &latency) {
    auto const mkt = main.addActor<Market>(market_core);
    main.addActor<BenchTrader>(0, mkt, latency);
}

void
record_ask_counters(benchmark::State &state) {
    const auto lat                      = qb::bench::last_latency_stats_snapshot();
    state.counters["round_trips_per_s"] = benchmark::Counter(static_cast<double>(kBenchAsks), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["latency_samples"]   = static_cast<double>(lat.samples);
    if (lat.samples)
        state.counters["mean_rtt_ns"] = benchmark::Counter(lat.mean_round_trip_ns, benchmark::Counter::kAvgIterations);
}

void
run_ask_roundtrip(benchmark::State &state, std::uint32_t const market_core) {
    // One-shot out-of-loop sample-count guard: every ask must resolve exactly once.
    {
        qb::bench::reset_last_latency_stats();
        qb::Main   probe;
        auto const latency = std::make_shared<pg::latency<1000 * 1000, 900000>>();
        build_ask_roundtrip(probe, market_core, latency);
        probe.start(false);
        probe.join();
        const auto lat = qb::bench::last_latency_stats_snapshot();
        if (lat.samples != kBenchAsks) {
            state.SkipWithError("ask round-trip sample count != kBenchAsks: a reply was dropped or mis-correlated");
            return;
        }
    }

    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        state.PauseTiming();
        qb::Main   main;
        auto const latency = std::make_shared<pg::latency<1000 * 1000, 900000>>();
        build_ask_roundtrip(main, market_core, latency);
        state.ResumeTiming();

        main.start(false);
        main.join();
        record_ask_counters(state);
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kBenchAsks));
}

void
BM_Ask_RoundTrip_SameCore(benchmark::State &state) {
    run_ask_roundtrip(state, 0);
}

void
BM_Ask_RoundTrip_CrossCore(benchmark::State &state) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    if (cap < 2u) {
        state.SkipWithError("requires-multicore: cross-core ask round-trip needs >= 2 benchmark cores");
        return;
    }
    run_ask_roundtrip(state, cap - 1u);
}

} // namespace

BENCHMARK(BM_Ask_RoundTrip_SameCore)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Ask_RoundTrip_CrossCore)->Unit(benchmark::kMillisecond)->UseRealTime();

BENCHMARK_MAIN();
