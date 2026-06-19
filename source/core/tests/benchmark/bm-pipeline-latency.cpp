/**
 * @file qb/core/tests/benchmark/bm-pipeline-latency.cpp
 * @brief Pipeline latency benchmark for the QB Actor Framework
 *
 * This file measures producer→consumer→… **pipeline chain** latency: one producer at
 * core \c 0, \c NB_ACTORS consumers chained so each event walks the full pipeline.
 *
 * \c NB_CORE is the placement divisor for the consumer chain (see multicast benchmark).
 *
 * Uses \c Main::start(true) so the benchmark thread never runs a core’s workflow inline.
 *
 * Google Benchmark counters (per iteration): \c source_events_per_s, \c deliveries_per_s
 * (\c NB_EVENTS * NB_ACTORS hops in the chain), \c mean_rtt_ns, \c latency_samples. Optional
 * percentile dump: set environment variable \c QB_ACTOR_BENCH_HISTOGRAM=1.
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

#include <benchmark/benchmark.h>
#include <qb/main.h>

#include "BenchmarkActorArgs.h"
#include "../shared/TestConsumer.h"
#include "../shared/TestEvent.h"
#include "../shared/TestProducer.h"

template <typename Event>
static void
BM_Pipeline_Chain_Latency(benchmark::State &state) {
    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        const auto nb_events = static_cast<std::uint64_t>(state.range(0));
        const auto nb_actor  = static_cast<std::uint32_t>(state.range(1));
        const auto nb_core   = static_cast<std::uint32_t>(state.range(2));
        state.PauseTiming();
        qb::Main main;

        qb::ActorIdList ids = {};
        for (std::size_t i = 0; i < static_cast<std::size_t>(nb_actor); ++i) {
            const auto coreid = qb::bench::multicast_consumer_core_for_index(i, nb_core);
            ids               = {main.addActor<ConsumerActor<Event>>(coreid, qb::ActorIdList(ids))};
        }
        main.addActor<ProducerActor<Event>>(0, qb::ActorIdList(ids), nb_events);
        state.ResumeTiming();

        main.start(true);
        main.join();

        const double source_events            = static_cast<double>(nb_events);
        const double deliveries               = static_cast<double>(nb_events) * static_cast<double>(nb_actor);
        state.counters["source_events_per_s"] = benchmark::Counter(source_events, benchmark::Counter::kIsIterationInvariantRate);
        state.counters["deliveries_per_s"]    = benchmark::Counter(deliveries, benchmark::Counter::kIsIterationInvariantRate);
        const auto lat                        = qb::bench::last_latency_stats_snapshot();
        if (lat.samples) {
            state.counters["mean_rtt_ns"]     = benchmark::Counter(lat.mean_round_trip_ns, benchmark::Counter::kAvgIterations);
            state.counters["latency_samples"] = static_cast<double>(lat.samples);
        }
    }
}

BENCHMARK_TEMPLATE(BM_Pipeline_Chain_Latency, LightEvent)
    ->Apply(qb::bench::apply_pipeline_multicast_args)
    ->ArgNames({"NB_EVENTS", "NB_ACTORS", "NB_CORE"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
