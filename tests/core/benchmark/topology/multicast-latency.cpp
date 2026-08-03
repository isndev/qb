/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/topology/multicast-latency.cpp
 * @brief Producer → N consumers MULTICAST latency (one event fans out to every consumer once).
 *
 * One producer at core 0 multicasts each source event to all `NB_ACTORS` consumers (a single hop,
 * `NB_ACTORS`-wide), unlike the pipeline chain where one event walks a serial chain. `NB_CORE` is
 * the consumer placement spread divisor (`multicast_consumer_core_for_index`: producer on 0,
 * consumers cycle `1 .. NB_CORE-1`). Deliveries per run = `NB_EVENTS * NB_ACTORS`.
 *
 * The latency sample lives on the shared `ProducerActor`; it is published to the cross-thread sink
 * via the `shared/LatencyFlush.h` / `BenchmarkIterationSink` idiom and read on the benchmark thread
 * after `join()` (`QB_ACTOR_BENCH_HISTOGRAM=1` dumps percentiles).
 *
 * Delivery guard (NOT a `EXPECT_LT(duration,…)` ctest gate — this is a perf harness): the producer
 * completes one round-trip (and takes one latency sample) per source event, so after `join()` the
 * recorded `latency_samples` MUST equal `NB_EVENTS`. A one-shot, out-of-loop probe run asserts that
 * equality, catching a multicast that drops a branch before any timing.
 *
 * Benchmark methodology: topology construction is hoisted out of the timed region (`PauseTiming()`);
 * `start(true)` + `join()` is timed under `UseRealTime()`; counters (`source_events_per_s`,
 * `deliveries_per_s`, `mean_rtt_ns`, `latency_samples`) are assigned once per iteration.
 */

#include <benchmark/benchmark.h>
#include <cstdint>
#include <qb/main.h>

#include "../../shared/BenchmarkActorArgs.h"
#include "../../shared/LatencyFlush.h"
#include "../../shared/TestConsumer.h"
#include "../../shared/TestEvent.h"
#include "../../shared/TestProducer.h"

namespace {

template <typename Event>
void
build_multicast(qb::Main &main, std::uint64_t const nb_events, std::uint32_t const nb_actor, std::uint32_t const nb_core) {
    qb::ActorIdList ids = {};
    for (std::size_t i = 0; i < static_cast<std::size_t>(nb_actor); ++i) {
        const auto coreid = qb::bench::multicast_consumer_core_for_index(i, nb_core);
        ids.push_back(main.addActor<ConsumerActor<Event>>(coreid));
    }
    main.addActor<ProducerActor<Event>>(0, ids, nb_events);
}

template <typename Event>
void
BM_Multicast_Latency(benchmark::State &state) {
    const auto nb_events = static_cast<std::uint64_t>(state.range(0));
    const auto nb_actor  = static_cast<std::uint32_t>(state.range(1));
    const auto nb_core   = static_cast<std::uint32_t>(state.range(2));

    // One-shot out-of-loop delivery guard: one completed round-trip per source event.
    {
        qb::bench::reset_last_latency_stats();
        qb::Main probe;
        build_multicast<Event>(probe, nb_events, nb_actor, nb_core);
        probe.start(true);
        probe.join();
        const auto lat = qb::bench::last_latency_stats_snapshot();
        if (lat.samples != nb_events) {
            state.SkipWithError("multicast delivery guard failed: latency_samples != NB_EVENTS (a branch was dropped)");
            return;
        }
    }

    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        state.PauseTiming();
        qb::Main main;
        build_multicast<Event>(main, nb_events, nb_actor, nb_core);
        state.ResumeTiming();

        main.start(true);
        main.join();

        const double source_events            = static_cast<double>(nb_events);
        const double deliveries               = static_cast<double>(nb_events) * static_cast<double>(nb_actor);
        state.counters["source_events_per_s"] = benchmark::Counter(source_events, benchmark::Counter::kIsIterationInvariantRate);
        state.counters["deliveries_per_s"]    = benchmark::Counter(deliveries, benchmark::Counter::kIsIterationInvariantRate);
        const auto lat                        = qb::bench::last_latency_stats_snapshot();
        state.counters["latency_samples"]     = static_cast<double>(lat.samples);
        if (lat.samples)
            state.counters["mean_rtt_ns"] = benchmark::Counter(lat.mean_round_trip_ns, benchmark::Counter::kAvgIterations);
    }
}

} // namespace

BENCHMARK_TEMPLATE(BM_Multicast_Latency, LightEvent)
    ->Apply(qb::bench::apply_pipeline_multicast_args)
    ->ArgNames({"NB_EVENTS", "NB_ACTORS", "NB_CORE"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
