//
// Created by isnDev on 2/24/2019.
//

#include <benchmark/benchmark.h>
#include <cube/actor.h>
#include <cube/main.h>

struct TinyEvent : cube::Event {
    uint64_t x;
    TinyEvent(uint64_t y) : x(y) {}
};

struct BigEvent : cube::Event {
    uint64_t x;
    uint64_t padding[127];
    BigEvent(uint64_t y) : x(y) {}
};

struct DynamicEvent : cube::Event {
    uint64_t x;
    std::vector<int> vec;
    DynamicEvent(uint64_t y) : x(y), vec(512, 8) {}
};

template<typename EventTrait>
class ActorPong : public cube::Actor {
    const uint64_t max_sends;
    const cube::ActorId actor_to_send;

public:
    ActorPong(uint64_t const max, cube::ActorId const id = cube::ActorId())
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
        cube::Main main({0});

        const auto max_events = state.range(0);
        const auto nb_actor = state.range(1);
        for (int i = 0; i < nb_actor; ++i) {
            main.addActor<ActorPong<EventTrait>>(0, max_events, main.addActor<ActorPong<EventTrait>>(0, max_events));
        }

        state.ResumeTiming();
        main.start(false);
    }
}

BENCHMARK_TEMPLATE(BM_PINGPONG_MONO_CORE, TinyEvent)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG_MONO_CORE, BigEvent)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG_MONO_CORE, DynamicEvent)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);

template<typename EventTrait>
static void BM_PINGPONG_DUAL_CORE(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        cube::Main main({0, 2});

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

BENCHMARK_TEMPLATE(BM_PINGPONG_DUAL_CORE, TinyEvent)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG_DUAL_CORE, BigEvent)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG_DUAL_CORE, DynamicEvent)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);

template<typename EventTrait>
static void BM_PINGPONG_QUAD_CORE(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        cube::Main main({0, 1, 2, 3});

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

BENCHMARK_TEMPLATE(BM_PINGPONG_QUAD_CORE, TinyEvent)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG_QUAD_CORE, BigEvent)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_PINGPONG_QUAD_CORE, DynamicEvent)->Ranges({{8, 8<<10}, {8, 1024}})->Unit(benchmark::kMillisecond);


BENCHMARK_MAIN();