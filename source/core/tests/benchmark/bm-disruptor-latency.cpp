/**
 * @file qb/core/tests/benchmark/bm-disruptor-latency.cpp
 * @brief Benchmark tests for actor communication topology throughput and latency
 *
 * This file benchmarks several actor communication topologies in the QB Actor Framework:
 *   - Unicast
 *   - Pipeline
 *   - Pipeline (shared core)
 *   - Multicast
 *   - Multicast (shared core)
 *   - Diamond
 *   - Diamond (shared core)
 *
 * The benchmark measures wall-clock execution time for a fixed number of source events
 * injected by the producer. It also publishes topology-aware counters so results remain
 * comparable across topologies with different internal message counts.
 *
 * Timing policy:
 *   - Topology construction is excluded from timing.
 *   - Runtime execution + drain are included.
 *   - Google Benchmark real time is used because the actor runtime is multi-threaded.
 *
 * Counter semantics:
 *   - source_events_per_s:
 *       Number of source events injected by the producer per second.
 *
 *   - deliveries_per_s:
 *       Number of inter-actor message deliveries performed by the topology per second.
 *       This is topology-dependent and is the best generic throughput metric for the runtime.
 *
 *   - completed_paths_per_s:
 *       Number of end-to-end completed topology paths per second.
 *       Examples:
 *         * unicast: 1 completed path per source event
 *         * pipeline: 1 completed path per source event
 *         * multicast: 3 completed terminal paths per source event
 *         * diamond: 2 completed branch paths per source event
 *
 *   - mean_rtt_ns:
 *       Mean end-to-end latency in nanoseconds if the test actors publish latency samples.
 *
 * Optional histogram dump:
 *   Set environment variable QB_ACTOR_BENCH_HISTOGRAM=1 if supported by the shared test
 *   infrastructure.
 *
 * Notes:
 *   - Main::start(true) is used so every VirtualCore runs on a worker thread and the
 *     benchmark thread does not host one core's workflow.
 *   - The Google Benchmark CPU column is not considered meaningful for analysis here;
 *     use real_time and the custom counters instead.
 */

#include <benchmark/benchmark.h>
#include <cstdint>
#include <qb/main.h>

#include "../shared/BenchmarkIterationSink.h"
#include "../shared/TestConsumer.h"
#include "../shared/TestEvent.h"
#include "../shared/TestProducer.h"

namespace {

struct TopologyMetrics {
    const char *name;
    std::uint64_t deliveries_per_source_event;
    std::uint64_t completed_paths_per_source_event;
    std::uint64_t hops_per_source_event;
    std::uint64_t fanout_factor;
};

constexpr TopologyMetrics kUnicastMetrics{
    "unicast",
    1, // producer -> consumer
    1, // one terminal path
    1,
    1};

constexpr TopologyMetrics kPipelineMetrics{
    "pipeline",
    3, // p -> c1 -> c2 -> c3
    1, // one terminal path
    3,
    1};

constexpr TopologyMetrics kMulticastMetrics{
    "multicast",
    3, // producer -> c1,c2,c3
    3, // three terminal paths
    1, // one hop depth, three branches
    3};

constexpr TopologyMetrics kDiamondMetrics{
    "diamond",
    4, // producer -> left,right and each branch -> end
    2, // two completed branch paths reaching end
    2, // depth 2 on each branch
    2};

template <typename Event, typename Setup>
static void
run_topology_benchmark(benchmark::State &state,
                       TopologyMetrics const &metrics,
                       Setup &&setup) {
    const auto nb_events = static_cast<std::uint64_t>(state.range(0));

    const double source_events_total = static_cast<double>(nb_events);
    const double deliveries_total =
        static_cast<double>(nb_events) *
        static_cast<double>(metrics.deliveries_per_source_event);
    const double completed_paths_total =
        static_cast<double>(nb_events) *
        static_cast<double>(metrics.completed_paths_per_source_event);

    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();

        state.PauseTiming();
        qb::Main main;
        setup(main, nb_events);
        state.ResumeTiming();

        main.start(true);
        main.join();

        state.counters["source_events_per_s"] =
            benchmark::Counter(source_events_total,
                               benchmark::Counter::kIsIterationInvariantRate);

        state.counters["deliveries_per_s"] =
            benchmark::Counter(deliveries_total,
                               benchmark::Counter::kIsIterationInvariantRate);

        state.counters["completed_paths_per_s"] =
            benchmark::Counter(completed_paths_total,
                               benchmark::Counter::kIsIterationInvariantRate);

        state.counters["topology_hops_per_event"] =
            static_cast<double>(metrics.hops_per_source_event);

        state.counters["fanout_factor"] =
            static_cast<double>(metrics.fanout_factor);

        const auto lat = qb::bench::last_latency_stats_snapshot();
        if (lat.samples) {
            state.counters["latency_samples"] =
                static_cast<double>(lat.samples);

            state.counters["mean_rtt_ns"] =
                benchmark::Counter(lat.mean_round_trip_ns,
                                   benchmark::Counter::kAvgIterations);
        }
    }
}

template <typename Event>
static void
BM_Unicast_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(
        state,
        kUnicastMetrics,
        [](qb::Main &main, std::uint64_t nb_events) {
            main.addActor<ProducerActor<Event>>(
                0,
                qb::ActorIdList{main.addActor<ConsumerActor<Event>>(1)},
                nb_events);
        });
}

template <typename Event>
static void
BM_Pipeline_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(
        state,
        kPipelineMetrics,
        [](qb::Main &main, std::uint64_t nb_events) {
            main.addActor<ProducerActor<Event>>(
                0,
                qb::ActorIdList{
                    main.addActor<ConsumerActor<Event>>(
                        1,
                        qb::ActorIdList{
                            main.addActor<ConsumerActor<Event>>(
                                2,
                                qb::ActorIdList{
                                    main.addActor<ConsumerActor<Event>>(3)})})},
                nb_events);
        });
}

template <typename Event>
static void
BM_Pipeline_Shared_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(
        state,
        kPipelineMetrics,
        [](qb::Main &main, std::uint64_t nb_events) {
            main.addActor<ProducerActor<Event>>(
                0,
                qb::ActorIdList{
                    main.addActor<ConsumerActor<Event>>(
                        1,
                        qb::ActorIdList{
                            main.addActor<ConsumerActor<Event>>(
                                1,
                                qb::ActorIdList{
                                    main.addActor<ConsumerActor<Event>>(1)})})},
                nb_events);
        });
}

template <typename Event>
static void
BM_Multicast_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(
        state,
        kMulticastMetrics,
        [](qb::Main &main, std::uint64_t nb_events) {
            main.addActor<ProducerActor<Event>>(
                0,
                qb::ActorIdList{
                    main.addActor<ConsumerActor<Event>>(1),
                    main.addActor<ConsumerActor<Event>>(2),
                    main.addActor<ConsumerActor<Event>>(3)},
                nb_events);
        });
}

template <typename Event>
static void
BM_Multicast_Shared_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(
        state,
        kMulticastMetrics,
        [](qb::Main &main, std::uint64_t nb_events) {
            main.core(0).addActor<ProducerActor<Event>>(
                main.core(1)
                    .builder()
                    .template addActor<ConsumerActor<Event>>()
                    .template addActor<ConsumerActor<Event>>()
                    .template addActor<ConsumerActor<Event>>()
                    .idList(),
                nb_events);
        });
}

template <typename Event>
static void
BM_Diamond_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(
        state,
        kDiamondMetrics,
        [](qb::Main &main, std::uint64_t nb_events) {
            const auto id_end = main.addActor<ConsumerActor<Event>>(3);
            main.addActor<ProducerActor<Event>>(
                0,
                qb::ActorIdList{
                    main.addActor<ConsumerActor<Event>>(1, qb::ActorIdList{id_end}),
                    main.addActor<ConsumerActor<Event>>(2, qb::ActorIdList{id_end})},
                nb_events);
        });
}

template <typename Event>
static void
BM_Diamond_Shared_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(
        state,
        kDiamondMetrics,
        [](qb::Main &main, std::uint64_t nb_events) {
            const auto id_end = main.addActor<ConsumerActor<Event>>(2);
            main.addActor<ProducerActor<Event>>(
                0,
                qb::ActorIdList{
                    main.addActor<ConsumerActor<Event>>(1, qb::ActorIdList{id_end}),
                    main.addActor<ConsumerActor<Event>>(1, qb::ActorIdList{id_end})},
                nb_events);
        });
}

} // namespace

BENCHMARK_TEMPLATE(BM_Unicast_Latency, LightEvent)
    ->Arg(1000000)
    ->ArgName("NB_EVENTS")
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Pipeline_Latency, LightEvent)
    ->Arg(1000000)
    ->ArgName("NB_EVENTS")
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Pipeline_Shared_Latency, LightEvent)
    ->Arg(1000000)
    ->ArgName("NB_EVENTS")
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Multicast_Latency, LightEvent)
    ->Arg(1000000)
    ->ArgName("NB_EVENTS")
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Multicast_Shared_Latency, LightEvent)
    ->Arg(1000000)
    ->ArgName("NB_EVENTS")
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Diamond_Latency, LightEvent)
    ->Arg(1000000)
    ->ArgName("NB_EVENTS")
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_TEMPLATE(BM_Diamond_Shared_Latency, LightEvent)
    ->Arg(1000000)
    ->ArgName("NB_EVENTS")
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();