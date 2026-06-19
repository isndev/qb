/**
 * @file qb/core/tests/benchmark/bm-ping-pong.cpp
 * @brief Ping-pong throughput benchmark for the QB Actor Framework
 *
 * This benchmark measures the execution time of ping-pong communication patterns
 * in the QB Actor Framework with different event types:
 *   - TinyEvent
 *   - BigEvent
 *   - DynamicEvent
 *
 * Important semantics:
 *   - NB_PING_ACTOR is the real number of PingActor instances created.
 *   - The real number of PongActor instances created is the same.
 *   - Total actors = 2 * NB_PING_ACTOR.
 *   - NB_PING is the initial TTL / number of round-trips per ping-pong chain.
 *   - NB_CORE is the number of scheduler cores/workers used by qb::Main.
 *
 * Timing policy:
 *   - Actor graph construction and main.start() are excluded from timing.
 *   - Only the execution/drain phase (main.join()) is measured.
 *   - Other throughput benches often use \c main.start(true) inside the timed region with
 *     \c UseRealTime(); compare results only across benches that share the same policy.
 *
 * Counters:
 *   - round_trips_per_s ~= NB_PING_ACTOR * NB_PING / second
 *   - messages_per_s    ~= NB_PING_ACTOR * (2 * NB_PING + 1) / second
 *
 * Note:
 *   The final +1 per pair corresponds to the terminal qb::KillEvent sent by PingActor
 *   when the TTL reaches zero.
 */

#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <qb/actor.h>
#include <qb/main.h>
#include <vector>

#include "../shared/BenchmarkIterationSink.h"

#ifdef NDEBUG
#define MAX_BENCHMARK_ITERATION 10
#define SHIFT_NB_EVENT 15
#else
#define SHIFT_NB_EVENT 4
#define MAX_BENCHMARK_ITERATION 1
#endif

[[nodiscard]] static std::uint32_t
max_core_range_for_bench() {
    return std::max(1u, qb::bench::cappedBenchmarkCores());
}

struct TinyEvent : qb::Event {
    std::uint64_t _ttl;
    explicit TinyEvent(std::uint64_t ttl)
        : _ttl(ttl) {}
};

struct BigEvent : qb::Event {
    std::uint64_t _ttl;
    std::uint64_t padding[127];
    explicit BigEvent(std::uint64_t ttl)
        : _ttl(ttl)
        , padding() {}
};

struct DynamicEvent : qb::Event {
    std::uint64_t    _ttl;
    std::vector<int> vec;
    explicit DynamicEvent(std::uint64_t ttl)
        : _ttl(ttl)
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
    const std::uint64_t max_sends;
    const qb::ActorId   actor_to_send;

public:
    PingActor(std::uint64_t max, qb::ActorId id)
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
        if (event._ttl) {
            reply(event);
        } else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

template <typename EventTrait>
static void
BM_PINGPONG(benchmark::State &state) {
    const auto nb_ping_actor = static_cast<int>(state.range(0));
    const auto nb_ping       = static_cast<std::uint64_t>(state.range(1));
    const auto nb_core       = static_cast<std::uint32_t>(state.range(2));

    // Per ping/pong pair:
    //   - round trips / chain ~= nb_ping
    //   - delivered messages  ~= 2 * nb_ping + 1 (including final KillEvent)
    const double round_trips_total = static_cast<double>(nb_ping_actor) * static_cast<double>(nb_ping);
    const double messages_total    = static_cast<double>(nb_ping_actor) * static_cast<double>(2ull * nb_ping + 1ull);

    for (auto _ : state) {
        state.PauseTiming();

        qb::Main main;

        for (int i = 0; i < nb_ping_actor; ++i) {
            const auto ping_core = static_cast<std::uint32_t>(i % static_cast<int>(nb_core));
            const auto pong_core = static_cast<std::uint32_t>((ping_core + 1u) % nb_core);

            const auto pong_id = main.addActor<PongActor<EventTrait>>(pong_core);
            main.addActor<PingActor<EventTrait>>(ping_core, nb_ping, pong_id);
        }

        main.start();

        state.ResumeTiming();
        main.join();
    }

    state.counters["actual_ping_actors"]  = static_cast<double>(nb_ping_actor);
    state.counters["actual_pong_actors"]  = static_cast<double>(nb_ping_actor);
    state.counters["actual_total_actors"] = static_cast<double>(nb_ping_actor * 2);

    state.counters["round_trips_per_s"] = benchmark::Counter(round_trips_total, benchmark::Counter::kIsIterationInvariantRate);

    state.counters["messages_per_s"] = benchmark::Counter(messages_total, benchmark::Counter::kIsIterationInvariantRate);
}

BENCHMARK_TEMPLATE(BM_PINGPONG, TinyEvent)
    ->RangeMultiplier(2)
    ->Ranges({{1, 64}, {1u << SHIFT_NB_EVENT, 1u << SHIFT_NB_EVENT}, {1u, max_core_range_for_bench()}})
    ->ArgNames({"NB_PING_ACTOR", "NB_PING", "NB_CORE"})
    ->Iterations(MAX_BENCHMARK_ITERATION)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_PINGPONG, BigEvent)
    ->RangeMultiplier(2)
    ->Ranges({{1, 64}, {1u << SHIFT_NB_EVENT, 1u << SHIFT_NB_EVENT}, {1u, max_core_range_for_bench()}})
    ->ArgNames({"NB_PING_ACTOR", "NB_PING", "NB_CORE"})
    ->Iterations(MAX_BENCHMARK_ITERATION)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_PINGPONG, DynamicEvent)
    ->RangeMultiplier(2)
    ->Ranges({{1, 64}, {1u << SHIFT_NB_EVENT, 1u << SHIFT_NB_EVENT}, {1u, max_core_range_for_bench()}})
    ->ArgNames({"NB_PING_ACTOR", "NB_PING", "NB_CORE"})
    ->Iterations(MAX_BENCHMARK_ITERATION)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();