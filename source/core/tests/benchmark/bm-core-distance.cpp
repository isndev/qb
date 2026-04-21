/**
 * @file qb/core/tests/benchmark/bm-core-distance.cpp
 * @brief Ping-pong throughput vs (ping_core, pong_core) — isolate cross-core cost
 *
 * Fixed \c send / \c reply path and payload; only core placement changes.
 *
 * Counters (initial_ttl is completed round-trip count for one chain):
 *   - \c round_trips_per_s — same as \c initial_ttl / wall second
 *   - \c messages_per_s — \c 2 * initial_ttl + 1 (includes terminal \c KillEvent)
 *
 * Uses \c UseRealTime() because \c main.start(true) runs on worker threads.
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

struct CdTinyEvent final : qb::Event {
    std::uint64_t _ttl = 0;
    explicit CdTinyEvent(std::uint64_t const ttl)
        : _ttl(ttl) {}
};

class CdPongActor final : public qb::Actor {
public:
    bool
    onInit() final {
        registerEvent<CdTinyEvent>(*this);
        return true;
    }

    void
    on(CdTinyEvent &event) {
        --event._ttl;
        reply(event);
    }
};

class CdPingActor final : public qb::Actor {
    const std::uint64_t _max;
    const qb::ActorId    _peer;

public:
    CdPingActor(std::uint64_t const max, qb::ActorId const peer)
        : _max(max)
        , _peer(peer) {}

    bool
    onInit() final {
        registerEvent<CdTinyEvent>(*this);
        send<CdTinyEvent>(_peer, _max);
        return true;
    }

    void
    on(CdTinyEvent &event) {
        if (event._ttl)
            reply(event);
        else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

static void
BM_CoreDistance_PingPong(benchmark::State &state) {
    const auto ping_core = static_cast<std::uint32_t>(state.range(0));
    const auto pong_core = static_cast<std::uint32_t>(state.range(1));
    const auto ttl       = static_cast<std::uint64_t>(state.range(2));

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        auto const pong = main.addActor<CdPongActor>(pong_core);
        main.addActor<CdPingActor>(ping_core, ttl, pong);
        state.ResumeTiming();
        main.start(true);
        main.join();
        state.counters["round_trips_per_s"] =
            benchmark::Counter(static_cast<double>(ttl),
                               benchmark::Counter::kIsIterationInvariantRate);
        const double msgs = static_cast<double>(2ull * ttl + 1ull);
        state.counters["messages_per_s"] =
            benchmark::Counter(msgs, benchmark::Counter::kIsIterationInvariantRate);
    }
}

static void
ArgsCoreDistanceGrid(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    const auto ttl = 1ull << 14;
    for (std::uint32_t p = 0; p < cap; ++p) {
        b->Args({static_cast<std::int64_t>(p), static_cast<std::int64_t>(p),
                 static_cast<std::int64_t>(ttl)});
        if (cap > 1u) {
            // Not named "far": Windows headers may #define far (16-bit legacy ABI).
            const std::uint32_t far_core = cap - 1u;
            if (p != far_core)
                b->Args({static_cast<std::int64_t>(p), static_cast<std::int64_t>(far_core),
                         static_cast<std::int64_t>(ttl)});
            const std::uint32_t adj = (p + 1u) % cap;
            if (adj != p)
                b->Args({static_cast<std::int64_t>(p), static_cast<std::int64_t>(adj),
                         static_cast<std::int64_t>(ttl)});
        }
    }
}

BENCHMARK(BM_CoreDistance_PingPong)
    ->Apply(ArgsCoreDistanceGrid)
    ->ArgNames({"ping_core", "pong_core", "initial_ttl"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
