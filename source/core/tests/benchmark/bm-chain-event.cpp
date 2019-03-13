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
#include <qb/actor.h>
#include <qb/main.h>

struct ChainEvent : qb::Event
{
    qb::ActorId first;
    uint64_t creation_time;
    uint64_t loop = 0;
};

class ActorTest : public qb::Actor {
    const uint64_t max_events;
    const bool first;
    const qb::ActorId to_send;

public:
    ActorTest(uint64_t const max, qb::ActorId const id = {}, bool infirst = false)
            : max_events(max)
            , first(infirst)
            , to_send(id) {}

    bool onInit() override final {
        registerEvent<ChainEvent>(*this);
        if (first) {
            auto &event = push<ChainEvent>(to_send);
            event.first = id();
            event.creation_time = time();
        }
        return true;
    }

    void on(ChainEvent &event) const {
        if (event.loop >= max_events) {
            kill();
            if (!to_send)
                LOG_INFO << "Event Time To Arrive " << time() - event.creation_time << "ns";
        }
        if (first)
            event.creation_time = time();
        if (!to_send) {
            ++event.loop;
        }
        forward(to_send ? to_send : event.first, event);
    }
};

static void BM_CHAIN_EVENT_MONO_CORE(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main({0});

        const auto max_events = state.range(0);
        const auto nb_actor = state.range(1);
        for (int i = 0; i < nb_actor; ++i) {
            main.addActor<ActorTest>(0, max_events, main.addActor<ActorTest>(0, max_events), true);
        }

        state.ResumeTiming();
        main.start(false);
    }
}

BENCHMARK(BM_CHAIN_EVENT_MONO_CORE)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);

static void BM_CHAIN_EVENT_DUAL_CORE(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main({0, 2});

        const auto max_events = state.range(0);
        const auto nb_actor = state.range(1);
        for (int i = 0; i < nb_actor; ++i) {
            main.addActor<ActorTest>(0, max_events, main.addActor<ActorTest>(2, max_events), true);
        }
        main.start();
        state.ResumeTiming();
        main.join();
    }
}

BENCHMARK(BM_CHAIN_EVENT_DUAL_CORE)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);

static void BM_CHAIN_EVENT_QUAD_CORE(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main({0, 1, 2, 3});

        const auto max_events = state.range(0);
        const auto nb_actor = state.range(1) / 2;
        for (int i = 0; i < nb_actor; ++i) {
            main.addActor<ActorTest>(0, max_events,
                                     main.addActor<ActorTest>(1, max_events,
                                             main.addActor<ActorTest>(2, max_events,
                                                     main.addActor<ActorTest>(3, max_events)))
                    , true);
        }
        main.start();
        state.ResumeTiming();
        main.join();
    }
}

BENCHMARK(BM_CHAIN_EVENT_QUAD_CORE)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);


BENCHMARK_MAIN();