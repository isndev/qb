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
    uint64_t _ttl;
    explicit TinyEvent(uint64_t y) : _ttl(y) {}
};

struct BigEvent : qb::Event {
    uint64_t _ttl;
    uint64_t padding[127];
    explicit BigEvent(uint64_t y) : _ttl(y), padding() {}
};

struct DynamicEvent : qb::Event {
    uint64_t _ttl;
    std::vector<int> vec;
    explicit DynamicEvent(uint64_t y) : _ttl(y), vec(512, 8) {}
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
            push<EventTrait>(actor_to_send, 0u);
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

template<typename TestEvent>
class PongActor : public qb::Actor {
public:
    virtual bool onInit() override final {
        registerEvent<TestEvent>(*this);
        return true;
    }

    void on(TestEvent &event) {
        --event._ttl;
        reply(event);
    }

};

template<typename TestEvent>
class PingActor : public qb::Actor {
    const uint64_t max_sends;
    const qb::ActorId actor_to_send;
public:
    PingActor(uint64_t const max, qb::ActorId const id)
            : max_sends(max), actor_to_send(id) {}
    ~PingActor() = default;

    virtual bool onInit() override final {
        registerEvent<TestEvent>(*this);
        send<TestEvent>(actor_to_send, max_sends);
        return true;
    }

    void on(TestEvent &event) {
        if (event._ttl)
            reply(event);
        else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

template<typename EventTrait>
static void BM_PINGPONG(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        const auto nb_core = static_cast<uint32_t>(state.range(0));
        qb::Main main(qb::CoreSet::build(nb_core));
        const auto max_events = state.range(1);
        const auto nb_actor = state.range(2) / nb_core;
        for (int i = 0; i < nb_actor; ++i) {
            for (auto j = 0u; j < nb_core; ++j) {
                main.addActor<PingActor<EventTrait>>(j, max_events,
                                                     main.addActor<PongActor<EventTrait>>(((j + 1) % nb_core)));
            }
        }

        main.start();
        state.ResumeTiming();
        main.join();
    }
}

BENCHMARK_TEMPLATE(BM_PINGPONG, TinyEvent)
        ->RangeMultiplier(2)
        ->Ranges({{1, std::thread::hardware_concurrency()}, {4096, 8<<10}, {512, 1024}})
        ->ArgNames({"NB_CORE", "NB_PING_PER_ACTOR", "NB_ACTOR"})
        ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG, BigEvent)
        ->RangeMultiplier(2)
        ->Ranges({{1, std::thread::hardware_concurrency()}, {4096, 8<<10}, {512, 1024}})
        ->ArgNames({"NB_CORE", "NB_PING_PER_ACTOR", "NB_ACTOR"})
        ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG, DynamicEvent)
        ->RangeMultiplier(2)
        ->Ranges({{1, std::thread::hardware_concurrency()}, {4096, 8<<10}, {512, 1024}})
        ->ArgNames({"NB_CORE", "NB_PING_PER_ACTOR", "NB_ACTOR"})
        ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();