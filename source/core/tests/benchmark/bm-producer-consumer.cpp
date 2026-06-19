/**
 * @file qb/core/tests/benchmark/bm-producer-consumer.cpp
 * @brief Producer-consumer throughput for the QB Actor Framework
 *
 * One producer issues \c kBenchMessages one-way \c push calls to a consumer that
 * terminates the run with \c broadcast<KillEvent> after the last message. Mono-core and
 * multi-core variants; multi uses \c SkipWithError unless \c cappedBenchmarkCores() >= 2.
 *
 * Counters: \c pushes_per_s, \c messages_per_s (same total). Uses \c UseRealTime() with
 * \c main.start(true). Core counts use \c qb::bench::cappedBenchmarkCores().
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
 * @ingroup Benchmarks
 */

#include <benchmark/benchmark.h>
#include <cstdint>
#include <qb/actor.h>
#include <qb/main.h>

#include "../shared/BenchmarkIterationSink.h"

namespace {
constexpr std::uint64_t kBenchMessages = 1'000'000ull;
}

struct PcMsg final : qb::Event {
    std::uint64_t seq = 0;
};

class ConsumerActor final : public qb::Actor {
    std::uint64_t _received = 0;

public:
    bool
    onInit() final {
        registerEvent<PcMsg>(*this);
        return true;
    }

    void
    on(PcMsg const &) {
        if (++_received == kBenchMessages)
            broadcast<qb::KillEvent>();
    }
};

class ProducerActor final : public qb::Actor {
    const qb::pipe _to_pipe;

public:
    ProducerActor() = delete;
    explicit ProducerActor(qb::ActorId const to)
        : _to_pipe(getPipe(to)) {}

    bool
    onInit() final {
        for (std::uint64_t i = 1; i <= kBenchMessages; ++i)
            _to_pipe.push<PcMsg>().seq = i;
        return true;
    }
};

static void
BM_Mono_Producer_Consumer(benchmark::State &state) {
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        main.addActor<ProducerActor>(0, main.addActor<ConsumerActor>(0));
        state.ResumeTiming();

        main.start(true);
        main.join();
        const double n                   = static_cast<double>(kBenchMessages);
        state.counters["pushes_per_s"]   = benchmark::Counter(n, benchmark::Counter::kIsIterationInvariantRate);
        state.counters["messages_per_s"] = benchmark::Counter(n, benchmark::Counter::kIsIterationInvariantRate);
    }
}

static void
BM_Multi_Producer_Consumer(benchmark::State &state) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    if (cap < 2u) {
        state.SkipWithError("BM_Multi_Producer_Consumer requires at least 2 benchmark cores for "
                            "cross-core placement");
        return;
    }
    const std::uint32_t consumer_core = cap - 1u;

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        main.addActor<ProducerActor>(0, main.addActor<ConsumerActor>(consumer_core));
        state.ResumeTiming();

        main.start(true);
        main.join();
        const double n                   = static_cast<double>(kBenchMessages);
        state.counters["pushes_per_s"]   = benchmark::Counter(n, benchmark::Counter::kIsIterationInvariantRate);
        state.counters["messages_per_s"] = benchmark::Counter(n, benchmark::Counter::kIsIterationInvariantRate);
    }
}

BENCHMARK(BM_Mono_Producer_Consumer)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Multi_Producer_Consumer)->Unit(benchmark::kMillisecond)->UseRealTime();

BENCHMARK_MAIN();
