/**
 * @file qb/core/tests/benchmark/bm-fan-in.cpp
 * @brief Many producers → one consumer (mailbox contention)
 *
 * Each producer sends \c msgs_per_producer one-way \c push messages to the same sink.
 * Counters: \c messages_per_s (total work), \c remote_producers (configuration; producers
 * strictly off the sink core when distributed and \c cap > 1).
 *
 * When \c producers_same_core_as_sink is false and there is more than one core, producers
 * are placed strictly on cores other than \c consumer_core (round-robin), so
 * \c remote_producers equals \c producers. With a single core, remote placement is
 * impossible and all producers share the sink core.
 *
 * Uses \c UseRealTime() for \c main.start(true) wall time.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

#include <benchmark/benchmark.h>
#include <qb/actor.h>
#include <qb/main.h>

#include "../shared/BenchmarkIterationSink.h"

struct FanInMsg final : qb::Event {};

class FanInSinkActor final : public qb::Actor {
    const std::uint64_t _expect;
    std::uint64_t       _received = 0;

public:
    explicit FanInSinkActor(std::uint64_t const expect)
        : _expect(expect) {
        registerEvent<FanInMsg>(*this);
    }

    void
    on(FanInMsg const &) {
        if (++_received == _expect)
            broadcast<qb::KillEvent>();
    }
};

class FanInSourceActor final : public qb::Actor {
    const qb::ActorId   _dst;
    const std::uint64_t _each;

public:
    FanInSourceActor(qb::ActorId const dst, std::uint64_t const each)
        : _dst(dst)
        , _each(each) {}

    bool
    onInit() final {
        for (std::uint64_t i = 0; i < _each; ++i)
            push<FanInMsg>(_dst);
        kill();
        return true;
    }
};

static void
BM_FanIn_OneWayPush(benchmark::State &state) {
    const auto producers        = static_cast<std::uint32_t>(state.range(0));
    const auto msgs_per         = static_cast<std::uint64_t>(state.range(1));
    const auto consumer_core    = static_cast<std::uint32_t>(state.range(2));
    const auto same_core_producers = state.range(3) != 0;

    const std::uint64_t total = static_cast<std::uint64_t>(producers) * msgs_per;
    const auto            cap = qb::bench::cappedBenchmarkCores();

    const std::uint32_t remote_producers =
        (same_core_producers || cap <= 1u)
            ? 0u
            : producers;

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        auto const sink = main.addActor<FanInSinkActor>(consumer_core, total);
        for (std::uint32_t i = 0; i < producers; ++i) {
            std::uint32_t pc = consumer_core;
            if (!same_core_producers && cap > 1u) {
                const std::uint32_t slot = i % (cap - 1u);
                std::uint32_t       walk = slot;
                for (std::uint32_t c = 0; c < cap; ++c) {
                    if (c == consumer_core)
                        continue;
                    if (walk == 0u) {
                        pc = c;
                        break;
                    }
                    --walk;
                }
            }
            main.addActor<FanInSourceActor>(pc, sink, msgs_per);
        }
        state.ResumeTiming();
        main.start(true);
        main.join();
        state.counters["messages_per_s"] =
            benchmark::Counter(static_cast<double>(total),
                               benchmark::Counter::kIsIterationInvariantRate);
        state.counters["remote_producers"] = static_cast<double>(remote_producers);
    }
}

static void
ArgsFanIn(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    for (std::int64_t same : {0, 1}) {
        const std::uint32_t pmax = std::min<std::uint32_t>(cap, 8u);
        for (std::uint32_t p = 1; p <= pmax; ++p) {
            b->Args({static_cast<std::int64_t>(p), 50000, 0, same});
            if (cap > 1u)
                b->Args({static_cast<std::int64_t>(p), 50000, static_cast<std::int64_t>(cap - 1),
                         same});
        }
    }
}

BENCHMARK(BM_FanIn_OneWayPush)
    ->Apply(ArgsFanIn)
    ->ArgNames({"producers", "msgs_per_producer", "consumer_core", "producers_same_core_as_sink"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
