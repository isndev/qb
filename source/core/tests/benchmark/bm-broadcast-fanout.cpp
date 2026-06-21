/**
 * @file qb/core/tests/benchmark/bm-broadcast-fanout.cpp
 * @brief BroadcastId(core) vs explicit per-actor push with matched total deliveries
 *
 * All sinks sit on \c consumer_core. The producer issues \c waves broadcast waves (each
 * delivery hits every sink) or \c waves * N explicit \c push calls. Stops when
 * \c N * waves deliveries are observed (atomic counter). Requires
 * \c producer_core != consumer_core for the broadcast case so the producer is not a
 * recipient on that core.
 *
 * Routing semantics (this is not a pure API diff): \c BroadcastId(core) delivers to the
 * runtime broadcast domain for that core, while explicit mode targets a fixed list of
 * actor ids. Both are tuned so total deliveries match (\c sinks * \c waves).
 *
 * Counters: \c deliveries_per_s, \c waves_per_s, wall time via \c UseRealTime().
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Core
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <qb/actor.h>
#include <qb/main.h>

#include "../shared/BenchmarkIterationSink.h"

struct FanoutMsg final : qb::Event {};

static std::atomic<std::uint64_t> g_fanout_deliveries{0};
static std::atomic<std::uint64_t> g_fanout_target{0};

class FanoutSinkActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<FanoutMsg>(*this);
        co_return true;
    }

    void
    on(FanoutMsg const &) {
        const auto v = g_fanout_deliveries.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (v == g_fanout_target.load(std::memory_order_acquire))
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
            for (std::uint64_t w = 0; w < _waves; ++w) {
                for (auto const &id : _ids)
                    push<FanoutMsg>(id);
            }
        }
        kill();
        co_return true;
    }
};

template <FanoutMode Mode>
static void
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

    for (auto _ : state) {
        g_fanout_deliveries.store(0, std::memory_order_relaxed);
        g_fanout_target.store(tot, std::memory_order_release);

        state.PauseTiming();
        qb::Main        main;
        qb::ActorIdList ids;
        ids.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
            ids.push_back(main.addActor<FanoutSinkActor>(consumer_c));

        const qb::CoreId bcore = static_cast<qb::CoreId>(consumer_c);
        if constexpr (Mode == FanoutMode::Broadcast) {
            main.addActor<FanoutProducerActor<FanoutMode::Broadcast>>(producer_c, bcore, qb::ActorIdList{}, waves);
        } else {
            main.addActor<FanoutProducerActor<FanoutMode::Explicit>>(producer_c, bcore, ids, waves);
        }
        state.ResumeTiming();
        main.start(true);
        main.join();
        state.counters["deliveries_per_s"] = benchmark::Counter(static_cast<double>(tot), benchmark::Counter::kIsIterationInvariantRate);
        state.counters["waves_per_s"]      = benchmark::Counter(static_cast<double>(waves), benchmark::Counter::kIsIterationInvariantRate);
    }
}

static void
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

static void
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
