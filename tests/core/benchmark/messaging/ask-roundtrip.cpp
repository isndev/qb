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
 *   - SERVICE-LOGICAL / SERVICE-INSTANCE — the same service and Trader on core 0. Only the
 *     captured destination differs: a stable logical address or the exact service incarnation.
 *     Every ask retains that destination; replies do not replace it with their source ID.
 *
 * Latency hand-off: the trader owns a `shared_ptr<pg::latency>` captured into the coroutine; its
 * destructor publishes mean + sample count into the cross-thread sink via the shared
 * `shared/LatencyFlush.h` idiom (`QB_ACTOR_BENCH_HISTOGRAM=1` also dumps percentiles).
 *
 * Sample-count guard (NOT a `EXPECT_LT(duration,…)` ctest gate — this is a perf harness): every ask
 * must resolve exactly once, so `latency_samples` MUST equal `kBenchAsks`. A one-shot, out-of-loop
 * probe run checks every sequence and response value as well as that equality. Its verification
 * specialization is separate from the measured Trader, which has no per-response check branch.
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
#include <type_traits>

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

namespace ask_roundtrip_benchmark {
constexpr std::uint32_t kBenchAsks = 50'000u;

struct ServiceMarketTag {};
using ServiceMarket = qb::test::MarketResponder<qb::ServiceActor<ServiceMarketTag>>;
enum class ServiceDestination { Logical, Instance };

struct ReplyCheck {
    std::uint64_t completed = 0;
    std::uint64_t invalid   = 0;
};
struct NoReplyCheck {};

// Self-signal: shut the trader down cleanly once its coroutine has run to completion (kill-based
// shutdown reaps completed frames; calling qb::Main::stop() from inside the coroutine can leave a
// just-completed frame un-drained at teardown — see the ask-roundtrip system test).
struct AskBenchDone final : qb::Event {};

// Drives `kBenchAsks` sequential asks at `_market`, sampling each round-trip into `_latency`.
template <bool VerifyReplies>
class BenchTrader final : public qb::Actor {
    const qb::ActorId                                                                   _market;
    std::shared_ptr<pg::latency<1000 * 1000, 900000>>                                   _latency;
    [[no_unique_address]] std::conditional_t<VerifyReplies, ReplyCheck *, NoReplyCheck> _check{};

public:
    BenchTrader(qb::ActorId market, std::shared_ptr<pg::latency<1000 * 1000, 900000>> latency, ReplyCheck *check)
        : _market(market)
        , _latency(std::move(latency)) {
        if constexpr (VerifyReplies)
            _check = check;
    }

    ~BenchTrader() final {
        qb::bench::flush_latency_to_sink(*_latency);
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Ping>(*this);
        registerEvent<AskBenchDone>(*this);
        auto mkt   = _market;
        auto lat   = _latency;
        auto check = _check;
        spawn([mkt, lat, check](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            static_cast<void>(check);
            for (std::uint32_t i = 0; i < kBenchAsks; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                auto       r  = co_await qb::ask(ctx, mkt, Ping{static_cast<int>(i)}, 500ms);
                lat->add(std::chrono::steady_clock::now() - t0);
                benchmark::DoNotOptimize(r.response);
                if constexpr (VerifyReplies) {
                    ++check->completed;
                    if (r.seq != static_cast<int>(i) || r.response != static_cast<int>(i * 2u))
                        ++check->invalid;
                }
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

template <bool VerifyReplies>
bool
build_ask_roundtrip(qb::Main &main, std::uint32_t const market_core, std::shared_ptr<pg::latency<1000 * 1000, 900000>> const &latency,
                    ReplyCheck *check) {
    auto const mkt    = main.addActor<Market>(market_core);
    const auto trader = main.addActor<BenchTrader<VerifyReplies>>(0, mkt, latency, check);
    return mkt.is_valid() && trader.is_valid();
}

// This check and destination selection run while constructing the engine, outside the ask loop.
// c73dfecc has one stable service ID; ActorId64 separates its logical address and instance ID.
template <typename Id>
bool
service_destination_valid(Id const instance, Id const address, Id const selected, ServiceDestination const mode) noexcept {
    if (!instance.is_valid() || !address.is_valid() || instance.index() != address.index())
        return false;
    if constexpr (requires {
                      address.is_service_address();
                      instance.is_service_instance();
                      address.service_tag();
                  }) {
        if (!address.is_service_address() || !instance.is_service_instance() || address.service_tag() != instance.service_tag()
            || address == instance)
            return false;
        return mode == ServiceDestination::Logical ? selected == address && selected.is_service_address()
                                                   : selected == instance && selected.is_service_instance();
    } else {
        return address == instance && selected == instance;
    }
}

template <ServiceDestination Mode, bool VerifyReplies>
bool
build_service_ask_roundtrip(qb::Main &main, std::uint32_t const market_core, std::shared_ptr<pg::latency<1000 * 1000, 900000>> const &latency,
                            ReplyCheck *check) {
    const auto instance = main.addActor<ServiceMarket>(market_core);
    const auto address  = qb::Actor::getServiceId<ServiceMarketTag>(static_cast<qb::CoreId>(market_core));
    const auto target   = Mode == ServiceDestination::Logical ? address : instance;
    if (!service_destination_valid(instance, address, target, Mode))
        return false;
    // BenchTrader captures this value once and passes it to every ask; replies never replace it.
    const auto trader = main.addActor<BenchTrader<VerifyReplies>>(0, target, latency, check);
    return trader.is_valid();
}

void
record_ask_counters(benchmark::State &state) {
    const auto lat                      = qb::bench::last_latency_stats_snapshot();
    state.counters["round_trips_per_s"] = benchmark::Counter(static_cast<double>(kBenchAsks), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["latency_samples"]   = static_cast<double>(lat.samples);
    qb::bench::record_mean_rtt_counter(state, lat);
}

template <typename Probe, typename Build>
void
run_ask_roundtrip(benchmark::State &state, std::uint32_t const market_core, Probe const &build_probe, Build const &build) {
    // The untimed specialization validates every response. The measured loop has no check branch.
    {
        qb::bench::reset_last_latency_stats();
        qb::Main   probe;
        ReplyCheck check;
        auto const latency = std::make_shared<pg::latency<1000 * 1000, 900000>>();
        if (!build_probe(probe, market_core, latency, &check)) {
            state.SkipWithError("ask round-trip fixture has invalid IDs or the service address mode is inconsistent");
            return;
        }
        probe.start(false);
        probe.join();
        const auto lat = qb::bench::last_latency_stats_snapshot();
        if (probe.hasError() || lat.samples != kBenchAsks || check.completed != kBenchAsks || check.invalid != 0) {
            state.SkipWithError("ask round-trip oracle failed: engine error, missing reply or incorrect sequence/response");
            return;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        qb::bench::reset_last_latency_stats();
        qb::Main   main;
        auto const latency = std::make_shared<pg::latency<1000 * 1000, 900000>>();
        if (!build(main, market_core, latency, nullptr)) {
            state.ResumeTiming();
            state.SkipWithError("ask round-trip fixture has invalid IDs or the service address mode is inconsistent");
            return;
        }
        state.ResumeTiming();

        main.start(false);
        main.join();
        state.PauseTiming();
        if (main.hasError() || qb::bench::last_latency_stats_snapshot().samples != kBenchAsks) {
            state.ResumeTiming();
            state.SkipWithError("ask round-trip measured iteration lost replies or reported an engine error");
            break;
        }
        record_ask_counters(state);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kBenchAsks));
}

void
BM_Ask_RoundTrip_SameCore(benchmark::State &state) {
    run_ask_roundtrip(state, 0, build_ask_roundtrip<true>, build_ask_roundtrip<false>);
}

void
BM_Ask_RoundTrip_CrossCore(benchmark::State &state) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    if (cap < 2u) {
        state.SkipWithError("requires-multicore: cross-core ask round-trip needs >= 2 benchmark cores");
        return;
    }
    run_ask_roundtrip(state, cap - 1u, build_ask_roundtrip<true>, build_ask_roundtrip<false>);
}

void
BM_Ask_RoundTrip_ServiceLogical_SameCore(benchmark::State &state) {
    run_ask_roundtrip(state, 0, build_service_ask_roundtrip<ServiceDestination::Logical, true>,
                      build_service_ask_roundtrip<ServiceDestination::Logical, false>);
}

void
BM_Ask_RoundTrip_ServiceInstance_SameCore(benchmark::State &state) {
    run_ask_roundtrip(state, 0, build_service_ask_roundtrip<ServiceDestination::Instance, true>,
                      build_service_ask_roundtrip<ServiceDestination::Instance, false>);
}

} // namespace ask_roundtrip_benchmark

using namespace ask_roundtrip_benchmark;

BENCHMARK(BM_Ask_RoundTrip_SameCore)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Ask_RoundTrip_CrossCore)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Ask_RoundTrip_ServiceLogical_SameCore)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Ask_RoundTrip_ServiceInstance_SameCore)->Unit(benchmark::kMillisecond)->UseRealTime();

BENCHMARK_MAIN();
