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

struct TinyEvent : qb::Event {
    uint64_t x;
    explicit TinyEvent(uint64_t y) : x(y) {}
};

struct BigEvent : qb::Event {
    uint64_t x;
    uint64_t padding[127];
    explicit BigEvent(uint64_t y) : x(y), padding() {}
};

struct DynamicEvent : qb::Event {
    uint64_t x;
    std::vector<int> vec;
    explicit DynamicEvent(uint64_t y) : x(y), vec(512, 8) {}
};

template<typename EventTrait>
class ActorPong : public qb::Actor {
    const uint64_t max_sends;
    const qb::ActorId actor_to_send;

public:
    ActorPong(uint64_t const max, qb::ActorId const id = qb::ActorId())
            : max_sends(max)
            , actor_to_send(id) {}

    bool onInit() override final {
        registerEvent<EventTrait>(*this);
        if (actor_to_send)
            push<EventTrait>(actor_to_send, 0);
        return true;
    }

    void on(EventTrait &event) const {
        if (event.x >= max_sends)
            kill();
        if (event.x <= max_sends) {
            ++event.x;
            reply(event);
        }
    }
};

template<typename EventTrait>
static void BM_PINGPONG_MONO_CORE(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main({0});

        const auto max_events = state.range(0);
        const auto nb_actor = state.range(1);
        for (int i = 0; i < nb_actor; ++i) {
            main.addActor<ActorPong<EventTrait>>(0, max_events, main.addActor<ActorPong<EventTrait>>(0, max_events));
        }

        state.ResumeTiming();
        main.start(false);
    }
}

//BENCHMARK_TEMPLATE(BM_PINGPONG_MONO_CORE, TinyEvent)->Ranges({{4096, 8<<10}, {256, 1024}})->Unit(benchmark::kMillisecond);
//BENCHMARK_TEMPLATE(BM_PINGPONG_MONO_CORE, BigEvent)->Ranges({{4096, 8<<10}, {256, 1024}})->Unit(benchmark::kMillisecond);
//BENCHMARK_TEMPLATE(BM_PINGPONG_MONO_CORE, DynamicEvent)->Ranges({{4096, 8<<10}, {256, 1024}})->Unit(benchmark::kMillisecond);

template<typename EventTrait>
static void BM_PINGPONG_DUAL_CORE(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main({0, 2});

        const auto max_events = state.range(0);
        const auto nb_actor = state.range(1);
        for (int i = 0; i < nb_actor; ++i) {
            main.addActor<ActorPong<EventTrait>>(0, max_events, main.addActor<ActorPong<EventTrait>>(2, max_events));
        }

        main.start();
        state.ResumeTiming();
        main.join();
    }
}

BENCHMARK_TEMPLATE(BM_PINGPONG_DUAL_CORE, TinyEvent)->Ranges({{4096, 8<<10}, {256, 1024}})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG_DUAL_CORE, BigEvent)->Ranges({{4096, 8<<10}, {256, 1024}})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG_DUAL_CORE, DynamicEvent)->Ranges({{4096, 8<<10}, {256, 1024}})->Unit(benchmark::kMillisecond);

template<typename EventTrait>
static void BM_PINGPONG_QUAD_CORE(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main({0, 1, 2, 3});

        const auto max_events = state.range(0);
        const auto nb_actor = state.range(1) / 2;
        for (int i = 0; i < nb_actor; ++i) {
            main.addActor<ActorPong<EventTrait>>(0, max_events, main.addActor<ActorPong<EventTrait>>(2, max_events));
            main.addActor<ActorPong<EventTrait>>(1, max_events, main.addActor<ActorPong<EventTrait>>(3, max_events));
        }

        main.start();
        state.ResumeTiming();
        main.join();
    }
}

//BENCHMARK_TEMPLATE(BM_PINGPONG_QUAD_CORE, TinyEvent)->Ranges({{4096, 8<<10}, {256, 1024}})->Unit(benchmark::kMillisecond);
//BENCHMARK_TEMPLATE(BM_PINGPONG_QUAD_CORE, BigEvent)->Ranges({{4096, 8<<10}, {256, 1024}})->Unit(benchmark::kMillisecond);
//BENCHMARK_TEMPLATE(BM_PINGPONG_QUAD_CORE, DynamicEvent)->Ranges({{4096, 8<<10}, {256, 1024}})->Unit(benchmark::kMillisecond);


BENCHMARK_MAIN();