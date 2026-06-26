/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/topology/topology-zoo.cpp
 * @brief The DIAMOND topology + shared-core placement variants (the non-parametrized residue).
 *
 * Pruned from the former `bm-disruptor-latency` "topology zoo": the unicast, plain pipeline and
 * plain multicast rows were REDUNDANT with the parametrized `pipeline-chain-latency.cpp` /
 * `multicast-latency.cpp` siblings (which sweep `NB_ACTORS` × `NB_CORE`) and have been DROPPED. What
 * remains here is only what those siblings do NOT cover:
 *   - DIAMOND          — producer → {left, right} → end (a fan-out then fan-in, the only topology
 *     with a join node);
 *   - DIAMOND (shared) — the same diamond collapsed onto fewer cores (placement-cost contrast);
 *   - PIPELINE (shared)  / MULTICAST (shared) — the all-on-one-core placement variants of the chain
 *     and the fan-out, which the spread-only parametrized siblings never exercise.
 *
 * Counter semantics (topology-aware so rows stay comparable despite different internal message
 * counts): `deliveries_per_s` is the generic inter-actor throughput; `completed_paths_per_s` counts
 * end-to-end terminal paths (diamond = 2 branch paths/event, pipeline = 1, multicast = 3);
 * `topology_hops_per_event` and `fanout_factor` describe the shape; `mean_rtt_ns` is the end-to-end
 * latency when the shared `ProducerActor` publishes samples.
 *
 * Measured delivery tally (NOT a `EXPECT_LT(duration,…)` ctest gate — this is a perf harness): the
 * producer completes one round-trip (one latency sample) per source event, so after `join()` the
 * recorded `latency_samples` MUST equal `NB_EVENTS`. A one-shot, out-of-loop probe run asserts that
 * equality, catching a topology that drops a path before any timing.
 *
 * Benchmark methodology: topology construction is hoisted out of the timed region (`PauseTiming()`);
 * `start(true)` + `join()` is timed under `UseRealTime()` so the benchmark thread never hosts a
 * core's workflow inline; counters are assigned once per iteration. The dead `name` member of the
 * old `TopologyMetrics` struct is removed.
 */

#include <benchmark/benchmark.h>
#include <cstdint>
#include <qb/main.h>

#include "../../shared/BenchmarkIterationSink.h"
#include "../../shared/LatencyFlush.h"
#include "../../shared/TestConsumer.h"
#include "../../shared/TestEvent.h"
#include "../../shared/TestProducer.h"

namespace {

// Topology shape descriptor (the old `name` member is removed — it was never read).
struct TopologyMetrics {
    std::uint64_t deliveries_per_source_event;
    std::uint64_t completed_paths_per_source_event;
    std::uint64_t hops_per_source_event;
    std::uint64_t fanout_factor;
};

constexpr TopologyMetrics kPipelineMetrics{3, 1, 3, 1}; // p -> c1 -> c2 -> c3
constexpr TopologyMetrics kMulticastMetrics{3, 3, 1, 3}; // producer -> {c1,c2,c3}
constexpr TopologyMetrics kDiamondMetrics{4, 2, 2, 2};   // producer -> {left,right} -> end

template <typename Event, typename Setup>
void
run_topology_benchmark(benchmark::State &state, TopologyMetrics const &metrics, Setup &&setup) {
    const auto nb_events = static_cast<std::uint64_t>(state.range(0));

    const double source_events_total   = static_cast<double>(nb_events);
    const double deliveries_total      = static_cast<double>(nb_events) * static_cast<double>(metrics.deliveries_per_source_event);
    const double completed_paths_total = static_cast<double>(nb_events) * static_cast<double>(metrics.completed_paths_per_source_event);

    // One-shot out-of-loop measured delivery tally: one completed round-trip per source event.
    {
        qb::bench::reset_last_latency_stats();
        qb::Main probe;
        setup(probe, nb_events);
        probe.start(true);
        probe.join();
        const auto lat = qb::bench::last_latency_stats_snapshot();
        if (lat.samples != nb_events) {
            state.SkipWithError("topology delivery tally failed: latency_samples != NB_EVENTS (a path was dropped)");
            return;
        }
    }

    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();

        state.PauseTiming();
        qb::Main main;
        setup(main, nb_events);
        state.ResumeTiming();

        main.start(true);
        main.join();

        state.counters["source_events_per_s"]     = benchmark::Counter(source_events_total, benchmark::Counter::kIsIterationInvariantRate);
        state.counters["deliveries_per_s"]         = benchmark::Counter(deliveries_total, benchmark::Counter::kIsIterationInvariantRate);
        state.counters["completed_paths_per_s"]    = benchmark::Counter(completed_paths_total, benchmark::Counter::kIsIterationInvariantRate);
        state.counters["topology_hops_per_event"]  = static_cast<double>(metrics.hops_per_source_event);
        state.counters["fanout_factor"]            = static_cast<double>(metrics.fanout_factor);

        const auto lat                    = qb::bench::last_latency_stats_snapshot();
        state.counters["latency_samples"] = static_cast<double>(lat.samples);
        if (lat.samples)
            state.counters["mean_rtt_ns"] = benchmark::Counter(lat.mean_round_trip_ns, benchmark::Counter::kAvgIterations);
    }
}

template <typename Event>
void
BM_Pipeline_Shared_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(state, kPipelineMetrics, [](qb::Main &main, std::uint64_t nb_events) {
        main.addActor<ProducerActor<Event>>(
            0,
            qb::ActorIdList{main.addActor<ConsumerActor<Event>>(
                1, qb::ActorIdList{main.addActor<ConsumerActor<Event>>(1, qb::ActorIdList{main.addActor<ConsumerActor<Event>>(1)})})},
            nb_events);
    });
}

template <typename Event>
void
BM_Multicast_Shared_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(state, kMulticastMetrics, [](qb::Main &main, std::uint64_t nb_events) {
        main.core(0).addActor<ProducerActor<Event>>(main.core(1)
                                                        .builder()
                                                        .template addActor<ConsumerActor<Event>>()
                                                        .template addActor<ConsumerActor<Event>>()
                                                        .template addActor<ConsumerActor<Event>>()
                                                        .idList(),
                                                    nb_events);
    });
}

template <typename Event>
void
BM_Diamond_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(state, kDiamondMetrics, [](qb::Main &main, std::uint64_t nb_events) {
        const auto id_end = main.addActor<ConsumerActor<Event>>(3);
        main.addActor<ProducerActor<Event>>(
            0,
            qb::ActorIdList{
                main.addActor<ConsumerActor<Event>>(1, qb::ActorIdList{id_end}), main.addActor<ConsumerActor<Event>>(2, qb::ActorIdList{id_end})
            },
            nb_events);
    });
}

template <typename Event>
void
BM_Diamond_Shared_Latency(benchmark::State &state) {
    run_topology_benchmark<Event>(state, kDiamondMetrics, [](qb::Main &main, std::uint64_t nb_events) {
        const auto id_end = main.addActor<ConsumerActor<Event>>(2);
        main.addActor<ProducerActor<Event>>(
            0,
            qb::ActorIdList{
                main.addActor<ConsumerActor<Event>>(1, qb::ActorIdList{id_end}), main.addActor<ConsumerActor<Event>>(1, qb::ActorIdList{id_end})
            },
            nb_events);
    });
}

} // namespace

BENCHMARK_TEMPLATE(BM_Pipeline_Shared_Latency, LightEvent)->Arg(1000000)->ArgName("NB_EVENTS")->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Multicast_Shared_Latency, LightEvent)->Arg(1000000)->ArgName("NB_EVENTS")->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Diamond_Latency, LightEvent)->Arg(1000000)->ArgName("NB_EVENTS")->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Diamond_Shared_Latency, LightEvent)->Arg(1000000)->ArgName("NB_EVENTS")->UseRealTime()->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
