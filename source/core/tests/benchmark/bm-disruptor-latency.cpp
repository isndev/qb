/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include <benchmark/benchmark.h>
#include <qb/main.h>
#include "../shared/TestProducer.h"
#include "../shared/TestConsumer.h"
#include "../shared/TestEvent.h"
#include "../shared/TestLatency.h"

template <typename Event>
static void BM_Unicast_Latency(benchmark::State& state) {
    for (auto _ : state) {
        const auto nb_events = state.range(0);
        qb::Main  main({0, 1});

        main.addActor<ProducerActor<Event>>(0, qb::ActorIds{main.addActor<ConsumerActor<Event>>(1)}, nb_events);

        main.start(false);
        main.join();
    }
}

template <typename Event>
static void BM_Pipeline_Latency(benchmark::State& state) {
    for (auto _ : state) {
        const auto nb_events = state.range(0);
        qb::Main  main({0, 1, 2, 3});

        main.addActor<ProducerActor<Event>>(0,
                qb::ActorIds{main.addActor<ConsumerActor<Event>>(1,
                        qb::ActorIds{main.addActor<ConsumerActor<Event>>(2,
                                qb::ActorIds{main.addActor<ConsumerActor<Event>>(3)})})},
                nb_events);

        main.start(false);
        main.join();
    }
}

template <typename Event>
static void BM_Pipeline_Shared_Latency(benchmark::State& state) {
    for (auto _ : state) {
        const auto nb_events = state.range(0);
        qb::Main  main({0, 1});

        main.addActor<ProducerActor<Event>>(0,
                qb::ActorIds{main.addActor<ConsumerActor<Event>>(1,
                        qb::ActorIds{main.addActor<ConsumerActor<Event>>(1,
                                qb::ActorIds{main.addActor<ConsumerActor<Event>>(1)})})},
                                            nb_events);

        main.start(false);
        main.join();
    }
}

template <typename Event>
static void BM_Multicast_Latency(benchmark::State& state) {
    for (auto _ : state) {
        const auto nb_events = state.range(0);
        qb::Main  main({0, 1, 2, 3});

        main.addActor<ProducerActor<Event>>(0,
                qb::ActorIds
                { main.addActor<ConsumerActor<Event>>(1),
                  main.addActor<ConsumerActor<Event>>(2),
                  main.addActor<ConsumerActor<Event>>(3)
                }, nb_events);

        main.start(false);
        main.join();
    }
}

template <typename Event>
static void BM_Multicast_Shared_Latency(benchmark::State& state) {
    for (auto _ : state) {
        const auto nb_events = state.range(0);
        qb::Main  main({0, 1});

        main.addActor<ProducerActor<Event>>(0,
                                            qb::ActorIds
                                                    { main.addActor<ConsumerActor<Event>>(1),
                                                      main.addActor<ConsumerActor<Event>>(1),
                                                      main.addActor<ConsumerActor<Event>>(1)
                                                    }, nb_events);

        main.start(false);
        main.join();
    }
}

template <typename Event>
static void BM_Diamond_Latency(benchmark::State& state) {
    for (auto _ : state) {
        const auto nb_events = state.range(0);
        qb::Main  main({0, 1, 2, 3});

        auto id_end = main.addActor<ConsumerActor<Event>>(3);
        main.addActor<ProducerActor<Event>>(0,
                                            qb::ActorIds{
                                                    main.addActor<ConsumerActor<Event>>(1, qb::ActorIds{id_end}),
                                                    main.addActor<ConsumerActor<Event>>(2, qb::ActorIds{id_end})
                                            }, nb_events);

        main.start(false);
        main.join();
    }
}

template <typename Event>
static void BM_Diamond_Shared_Latency(benchmark::State& state) {
    for (auto _ : state) {
        const auto nb_events = state.range(0);
        qb::Main  main({0, 1, 2});

        auto id_end = main.addActor<ConsumerActor<Event>>(2);
        main.addActor<ProducerActor<Event>>(0,
                                            qb::ActorIds{
                                                    main.addActor<ConsumerActor<Event>>(1, qb::ActorIds{id_end}),
                                                    main.addActor<ConsumerActor<Event>>(1, qb::ActorIds{id_end})
                                            }, nb_events);

        main.start(false);
        main.join();
    }
}

// Register the function as a benchmark
BENCHMARK_TEMPLATE(BM_Unicast_Latency, LightEvent)
        ->Arg(1000000)
        ->ArgName("NB_EVENTS")
        ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Pipeline_Latency, LightEvent)
        ->Arg(1000000)
        ->ArgName("NB_EVENTS")
        ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Pipeline_Shared_Latency, LightEvent)
        ->Arg(1000000)
        ->ArgName("NB_EVENTS")
        ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Multicast_Latency, LightEvent)
        ->Arg(1000000)
        ->ArgName("NB_EVENTS")
        ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Multicast_Shared_Latency, LightEvent)
        ->Arg(1000000)
        ->ArgName("NB_EVENTS")
        ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Diamond_Latency, LightEvent)
        ->Arg(1000000)
        ->ArgName("NB_EVENTS")
        ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Diamond_Shared_Latency, LightEvent)
        ->Arg(1000000)
        ->ArgName("NB_EVENTS")
        ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();