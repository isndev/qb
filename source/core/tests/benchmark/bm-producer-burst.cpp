/**
 * @file qb/core/tests/benchmark/bm-producer-burst.cpp
 * @brief One-way throughput: \c ICallback producer sends in bursts vs one message per tick
 *
 * Same total \c messages and same \c push API; only the number of \c push calls per
 * \c onCallback() invocation changes. Isolates scheduler / callback interaction from raw
 * pipe throughput.
 *
 * \c registerCallback / \c onCallback scheduling is entirely runtime-defined; this bench
 * measures end-to-end throughput under that policy, not an abstract “callback cost” in
 * isolation.
 *
 * Counters: \c messages_per_s, \c expected_callbacks_per_s (derived
 * \c ceil(messages / burst) if the producer drains exactly \c messages), \c burst_size.
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

#include <algorithm>
#include <benchmark/benchmark.h>
#include <qb/actor.h>
#include <qb/main.h>

#include "../shared/BenchmarkIterationSink.h"

struct BurstMsg final : qb::Event {};

class BurstSinkActor final : public qb::Actor {
    const std::uint64_t _expect;
    std::uint64_t       _got = 0;

public:
    explicit BurstSinkActor(std::uint64_t const expect)
        : _expect(expect) {}

    bool
    onInit() final {
        registerEvent<BurstMsg>(*this);
        return true;
    }

    void
    on(BurstMsg const &) {
        if (++_got == _expect)
            broadcast<qb::KillEvent>();
    }
};

template <std::uint32_t BurstPerCallbackV>
class BurstCallbackProducerActor final
    : public qb::Actor
    , public qb::ICallback {
    const qb::ActorId   _dst;
    const std::uint64_t _total;
    std::uint64_t       _sent = 0;

public:
    BurstCallbackProducerActor(qb::ActorId const dst, std::uint64_t const total)
        : _dst(dst)
        , _total(total) {}

    bool
    onInit() final {
        registerCallback(*this);
        return true;
    }

    void
    onCallback() final {
        constexpr std::uint32_t kBurst = BurstPerCallbackV;
        const std::uint64_t     chunk  = std::min<std::uint64_t>(kBurst, _total - _sent);
        for (std::uint64_t i = 0; i < chunk; ++i)
            push<BurstMsg>(_dst);
        _sent += chunk;
        if (_sent >= _total)
            kill();
    }
};

template <std::uint32_t BurstPerCallbackV>
static void
BM_ProducerBurst_OneWay(benchmark::State &state) {
    const auto total      = static_cast<std::uint64_t>(state.range(0));
    const auto producer_c = static_cast<std::uint32_t>(state.range(1));
    const auto consumer_c = static_cast<std::uint32_t>(state.range(2));

    constexpr std::uint64_t kBurst             = static_cast<std::uint64_t>(BurstPerCallbackV);
    const std::uint64_t     expected_callbacks = (total + kBurst - 1ull) / kBurst;

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main   main;
        auto const sink = main.addActor<BurstSinkActor>(consumer_c, total);
        main.addActor<BurstCallbackProducerActor<BurstPerCallbackV>>(producer_c, sink, total);
        state.ResumeTiming();
        main.start(true);
        main.join();
        state.counters["messages_per_s"] = benchmark::Counter(static_cast<double>(total), benchmark::Counter::kIsIterationInvariantRate);
        state.counters["expected_callbacks_per_s"] =
            benchmark::Counter(static_cast<double>(expected_callbacks), benchmark::Counter::kIsIterationInvariantRate);
        state.counters["burst_size"] = static_cast<double>(BurstPerCallbackV);
    }
}

static void
ApplyBurstArgs(benchmark::internal::Benchmark *b) {
    const auto             cap    = qb::bench::cappedBenchmarkCores();
    constexpr std::int64_t kTotal = 500000;
    b->Args({kTotal, 0, 0});
    if (cap > 1u)
        b->Args({kTotal, 0, static_cast<std::int64_t>(cap - 1)});
}

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
