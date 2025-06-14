/**
 * @file qb/core/tests/benchmark/bm-pipeline-latency.cpp
 * @brief Pipeline latency benchmark for the QB Actor Framework
 *
 * This file contains benchmark tests measuring the latency of unicast communication
 * in a pipeline pattern in the QB Actor Framework. It tests how efficiently events
 * can flow through a chain of actors across different cores.
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
#include <qb/main.h>
#include "../shared/TestConsumer.h"
#include "../shared/TestEvent.h"
#include "../shared/TestProducer.h"

template <typename Event>
static void
BM_Unicast_Latency(benchmark::State &state) {
    for (auto _ : state) {
        const auto nb_events = state.range(0);
        const auto nb_actor  = state.range(1);
        const auto nb_core   = static_cast<uint32_t>(state.range(2));
        qb::Main   main;

        qb::ActorIdList ids = {};
        for (auto i = 0; i < nb_actor; ++i) {
            const auto coreid = (i % (nb_core - (nb_core > 1))) + (nb_core > 1);
            ids = {main.addActor<ConsumerActor<Event>>(coreid, qb::ActorIdList(ids))};
        }
        main.addActor<ProducerActor<Event>>(0, qb::ActorIdList(ids), nb_events);

        main.start(false);
        main.join();
    }
}

static void
CustomArguments(benchmark::internal::Benchmark *b) {
    auto nb_core = std::thread::hardware_concurrency();
    for (auto i = 1u; i <= nb_core; i *= 2) {
        for (int j = i - 1; j <= static_cast<int>(nb_core * 10); j *= 10) {
            if (!j)
                j = 1;
            b->Args({1000000, j, i});
        }
    }
}

// Register the function as a benchmark
BENCHMARK_TEMPLATE(BM_Unicast_Latency, LightEvent)
    ->Apply(CustomArguments)
    ->ArgNames({"NB_EVENTS", "NB_ACTORS", "NB_CORE"})
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();