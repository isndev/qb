/**
 * @file qb/core/tests/benchmark/bm-ping-pong.cpp
 * @brief Ping-pong throughput benchmark for the QB Actor Framework
 *
 * This file contains benchmark tests measuring the throughput of ping-pong communication
 * patterns in the QB Actor Framework with different event types (tiny, big, and
 * dynamic). It tests how efficiently actors can exchange messages with various sizes and
 * complexity.
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

#ifdef NDEBUG
#define MAX_BENCHMARK_ITERATION 10
#define SHIFT_NB_EVENT 15
#else
#define SHIFT_NB_EVENT 4
#define MAX_BENCHMARK_ITERATION 1
#endif

struct TinyEvent : qb::Event {
    uint64_t _ttl;
    explicit TinyEvent(uint64_t y)
        : _ttl(y) {}
};

struct BigEvent : qb::Event {
    uint64_t _ttl;
    uint64_t padding[127];
    explicit BigEvent(uint64_t y)
        : _ttl(y)
        , padding() {}
};

struct DynamicEvent : qb::Event {
    uint64_t         _ttl;
    std::vector<int> vec;
    explicit DynamicEvent(uint64_t y)
        : _ttl(y)
        , vec(512, 8) {}
};

template <typename TestEvent>
class PongActor final : public qb::Actor {
public:
    bool
    onInit() final {
        registerEvent<TestEvent>(*this);
        return true;
    }

    void
    on(TestEvent &event) {
        --event._ttl;
        reply(event);
    }
};

template <typename TestEvent>
class PingActor final : public qb::Actor {
    const uint64_t    max_sends;
    const qb::ActorId actor_to_send;

public:
    PingActor(uint64_t const max, qb::ActorId const id)
        : max_sends(max)
        , actor_to_send(id) {}
    ~PingActor() final = default;

    bool
    onInit() final {
        registerEvent<TestEvent>(*this);
        send<TestEvent>(actor_to_send, max_sends);
        return true;
    }

    void
    on(TestEvent &event) {
        if (event._ttl)
            reply(event);
        else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

template <typename EventTrait>
static void
BM_PINGPONG(benchmark::State &state) {
    for (auto _ : state) {
        state.PauseTiming();
        const auto nb_core = static_cast<uint32_t>(state.range(2));
        qb::Main   main;
        const auto max_events = state.range(1);
        const auto nb_actor   = std::thread::hardware_concurrency() * 2 * state.range(0);
        for (int i = nb_actor; i > 0;) {
            for (auto j = 0u; j < nb_core && i > 0; ++j) {
                main.addActor<PingActor<EventTrait>>(
                    j, max_events,
                    main.addActor<PongActor<EventTrait>>(((j + 1) % nb_core)));
                --i;
            }
        }

        main.start();
        state.ResumeTiming();
        main.join();
    }
}

BENCHMARK_TEMPLATE(BM_PINGPONG, TinyEvent)
    ->RangeMultiplier(2)
    ->Ranges({{1, 64},
              {1u << SHIFT_NB_EVENT, 1u << SHIFT_NB_EVENT},
              {1u, std::thread::hardware_concurrency()}})
    ->ArgNames({"NB_PING_ACTOR", "NB_PING", "NB_CORE"})
    ->Iterations(MAX_BENCHMARK_ITERATION)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG, BigEvent)
    ->RangeMultiplier(2)
    ->Ranges({{1, 64},
              {1u << SHIFT_NB_EVENT, 1u << SHIFT_NB_EVENT},
              {1u, std::thread::hardware_concurrency()}})
    ->ArgNames({"NB_PING_ACTOR", "NB_PING", "NB_CORE"})
    ->Iterations(MAX_BENCHMARK_ITERATION)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG, DynamicEvent)
    ->RangeMultiplier(2)
    ->Ranges({{1, 64},
              {1u << SHIFT_NB_EVENT, 1u << SHIFT_NB_EVENT},
              {1u, std::thread::hardware_concurrency()}})
    ->ArgNames({"NB_PING_ACTOR", "NB_PING", "NB_CORE"})
    ->Iterations(MAX_BENCHMARK_ITERATION)
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();