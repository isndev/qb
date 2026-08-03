/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/ping-pong-latency.cpp
 * @brief Ping-pong round-trip latency (mono-core, cross-core, and a raw-SPSC reference).
 *
 * A ping actor bounces a `LightEvent` (carrying a `steady_clock` timestamp) off a pong actor a
 * fixed `kPingPongInitialTtl` times, sampling the round-trip delta of EVERY hop into a
 * `pg::latency` histogram. Three variants:
 *   - REFERENCE — two raw `qb::lockfree::spsc::ringbuffer` threads (the framework-free floor);
 *   - MONO      — ping and pong on core 0;
 *   - MULTI     — ping and pong on distinct cores (requires-multicore: SELF-SKIPS via
 *     `SkipWithError` unless there are >= 2 logical processors, so it never degenerates to mono).
 *
 * Latency hand-off uses the shared `shared/LatencyFlush.h` idiom: the accumulator lives on a worker
 * thread, so `flush_latency_to_sink()` publishes its mean + sample count into the cross-thread sink
 * (`BenchmarkIterationSink.h`) for the benchmark thread to read after `join()`; set
 * `QB_ACTOR_BENCH_HISTOGRAM=1` to also dump percentiles.
 *
 * Sample-count guard (NOT a `EXPECT_LT(duration,…)` ctest gate — this is a perf harness): each run
 * samples exactly one latency per round-trip, so after `join()` the recorded `latency_samples` MUST
 * equal `kPingPongInitialTtl`. A one-shot, out-of-loop probe run asserts that equality, catching a
 * chain that stalls early (e.g. a dropped cross-core reply) before any timing is reported.
 *
 * Benchmark methodology: engine construction is hoisted out of the timed region (`PauseTiming()`);
 * counters (`round_trips_per_s`, `messages_per_s`, `mean_rtt_ns`, `latency_samples`) are read from
 * the sink and assigned once per iteration under `UseRealTime()`.
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <qb/actor.h>
#include <qb/main.h>
#include <thread>

#include "../../shared/BenchmarkCores.h"
#include "../../shared/LatencyFlush.h"
#include "../../shared/TestEvent.h"
#include "../../shared/TestLatency.h"

namespace {
constexpr std::uint32_t kPingPongInitialTtl = 1'000'000u;
constexpr double        kRoundTripsPerIter  = static_cast<double>(kPingPongInitialTtl);
constexpr double        kMessagesPerIter    = 2.0 * static_cast<double>(kPingPongInitialTtl) + 1.0;

class PongActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<LightEvent>(*this);
        co_return true;
    }

    void
    on(LightEvent &event) {
        --event._ttl;
        reply(event);
    }
};

class PingActor final : public qb::Actor {
    pg::latency<1000 * 1000, 900000> _latency;

public:
    ~PingActor() final {
        qb::bench::flush_latency_to_sink(_latency);
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<qb::RequireEvent>(*this);
        registerEvent<LightEvent>(*this);
        require<PongActor>();
        co_return true;
    }

    void
    on(qb::RequireEvent const &event) {
        send<LightEvent>(event.getSource(), kPingPongInitialTtl);
    }

    void
    on(LightEvent const &event) {
        _latency.add(std::chrono::steady_clock::now() - event._timepoint);
        if (event._ttl)
            send<LightEvent>(event.getSource(), event._ttl);
        else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

void
thread_ping(qb::lockfree::spsc::ringbuffer<LightEvent, 4096> *spsc, std::atomic<bool> *running) {
    // pg::latency<..., 900000> holds ~900k size_t buckets (~7 MiB). On Windows the default thread
    // stack is ~1 MiB; keeping that object on the stack overflows and trips __chkstk.
    auto const latency = std::make_unique<pg::latency<1000 * 1000, 900000>>();
    LightEvent events[4096];

    spsc[1].enqueue(LightEvent(kPingPongInitialTtl));
    while (qb::likely(running->load(std::memory_order_relaxed))) {
        spsc[0].dequeue(
            [&](auto event, auto nb_events) {
                for (auto i = 0u; i < nb_events; ++i) {
                    latency->add(std::chrono::steady_clock::now() - event[i]._timepoint);
                    if (event[i]._ttl)
                        spsc[1].enqueue(LightEvent(event[i]._ttl));
                    else
                        running->store(false, std::memory_order_release);
                }
            },
            events, 4096u);
    }
    qb::bench::flush_latency_to_sink(*latency);
}

void
thread_pong(qb::lockfree::spsc::ringbuffer<LightEvent, 4096> *spsc, std::atomic<bool> *running) {
    LightEvent events[4096];

    while (qb::likely(running->load(std::memory_order_relaxed))) {
        spsc[1].dequeue(
            [&](auto event, auto nb_events) {
                for (auto i = 0u; i < nb_events; ++i) {
                    --event[i]._ttl;
                    spsc[0].enqueue(event[i]);
                }
            },
            events, 4096u);
    }
}

void
record_latency_counters(benchmark::State &state) {
    state.counters["round_trips_per_s"] = benchmark::Counter(kRoundTripsPerIter, benchmark::Counter::kIsIterationInvariantRate);
    state.counters["messages_per_s"]    = benchmark::Counter(kMessagesPerIter, benchmark::Counter::kIsIterationInvariantRate);

    const auto lat                    = qb::bench::last_latency_stats_snapshot();
    state.counters["latency_samples"] = static_cast<double>(lat.samples);
    if (lat.samples)
        state.counters["mean_rtt_ns"] = benchmark::Counter(lat.mean_round_trip_ns, benchmark::Counter::kAvgIterations);
}

// Build a mono/cross-core actor ping-pong into `main` (pong on `pong_core`).
void
build_actor_pingpong(qb::Main &main, std::uint32_t const pong_core) {
    main.core(0).addActor<PingActor>();
    main.core(pong_core).addActor<PongActor>();
}

// One-shot out-of-loop guard: exactly one latency sample per round-trip ⇒ samples == initial TTL.
void
assert_full_sample_count(benchmark::State &state, std::uint32_t const pong_core) {
    qb::bench::reset_last_latency_stats();
    qb::Main probe;
    build_actor_pingpong(probe, pong_core);
    probe.start(true);
    probe.join();
    const auto lat = qb::bench::last_latency_stats_snapshot();
    if (lat.samples != kPingPongInitialTtl)
        state.SkipWithError("ping-pong latency sample count != initial TTL: the chain dropped round-trips");
}

void
BM_Reference_Multi_PingPong_Latency(benchmark::State &state) {
    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        std::atomic<bool> running{true};
        auto              spsc = std::make_unique<qb::lockfree::spsc::ringbuffer<LightEvent, 4096>[]>(2);
        std::thread       threads[2];

        threads[0] = std::thread(thread_ping, spsc.get(), &running);
        threads[1] = std::thread(thread_pong, spsc.get(), &running);

        for (auto &thread : threads)
            if (thread.joinable())
                thread.join();
        record_latency_counters(state);
    }
}

void
BM_Mono_PingPong_Latency(benchmark::State &state) {
    assert_full_sample_count(state, 0);

    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        state.PauseTiming();
        qb::Main main;
        build_actor_pingpong(main, 0);
        state.ResumeTiming();

        main.start(true);
        main.join();
        record_latency_counters(state);
    }
}

void
BM_Multi_PingPong_Latency(benchmark::State &state) {
    const auto hw = qb::bench::effectiveHardwareCores();
    if (hw < 2u) {
        state.SkipWithError("requires-multicore: BM_Multi_PingPong_Latency needs >= 2 logical processors");
        return;
    }
    const auto pong_core = std::min<std::uint32_t>(hw >= 3u ? 2u : 1u, hw - 1u);

    assert_full_sample_count(state, pong_core);

    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        state.PauseTiming();
        qb::Main main;
        build_actor_pingpong(main, pong_core);
        state.ResumeTiming();

        main.start(true);
        main.join();
        record_latency_counters(state);
    }
}

} // namespace

BENCHMARK(BM_Reference_Multi_PingPong_Latency)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Mono_PingPong_Latency)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Multi_PingPong_Latency)->Unit(benchmark::kMillisecond)->UseRealTime();

BENCHMARK_MAIN();
