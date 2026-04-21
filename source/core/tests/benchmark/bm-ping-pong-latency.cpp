/**
 * @file qb/core/tests/benchmark/bm-ping-pong-latency.cpp
 * @brief Ping-pong latency benchmark for the QB Actor Framework
 *
 * This file contains benchmark tests measuring the latency of ping-pong communication
 * patterns in the QB Actor Framework. It includes tests for mono-threaded and
 * multi-threaded scenarios, as well as a reference implementation using raw ringbuffers.
 *
 * Latency samples use \c std::chrono::steady_clock deltas (\c LightEvent::_timepoint).
 *
 * Google Benchmark counters (per iteration): \c mean_rtt_ns, \c latency_samples,
 * \c round_trips_per_s, \c messages_per_s (fixed workload: initial TTL 1e6). Uses
 * \c UseRealTime(). The SPSC reference stops both threads via \c running=false when TTL
 * hits zero; the actor path uses \c KillEvent — same logical rounds, different shutdown
 * primitive.
 *
 * Optional percentile dump: set environment variable \c QB_ACTOR_BENCH_HISTOGRAM=1.
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

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <qb/actor.h>
#include <qb/main.h>
#include <thread>

#include "../shared/BenchmarkIterationSink.h"
#include "../shared/TestEvent.h"
#include "../shared/TestLatency.h"

namespace {
constexpr std::uint32_t kPingPongInitialTtl = 1'000'000u;
constexpr double        kRoundTripsPerIter = static_cast<double>(kPingPongInitialTtl);
constexpr double        kMessagesPerIter =
    2.0 * static_cast<double>(kPingPongInitialTtl) + 1.0;
} // namespace

class PongActor final : public qb::Actor {
public:
    bool
    onInit() final {
        registerEvent<LightEvent>(*this);
        return true;
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
        if (_latency.sample_count()) {
            qb::bench::record_last_latency(_latency.mean_nanoseconds(),
                                          static_cast<std::uint64_t>(_latency.sample_count()));
        }
        if (std::getenv("QB_ACTOR_BENCH_HISTOGRAM") && _latency.sample_count()) {
            _latency.generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
        }
    }

    bool
    onInit() final {
        registerEvent<qb::RequireEvent>(*this);
        registerEvent<LightEvent>(*this);
        require<PongActor>();
        return true;
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
thread_ping(qb::lockfree::spsc::ringbuffer<LightEvent, 4096> *spsc,
            std::atomic<bool> *running) {
    // pg::latency<..., 900000> holds ~900k size_t buckets (~7 MiB). On Windows the default
    // thread stack is ~1 MiB; keeping that object on the stack overflows and trips __chkstk.
    auto const latency = std::make_unique<pg::latency<1000 * 1000, 900000>>();
    LightEvent                       events[4096];

    spsc[1].enqueue(LightEvent(kPingPongInitialTtl));
    while (qb::likely(running->load(std::memory_order_relaxed))) {
        spsc[0].dequeue(
            [&](auto event, auto nb_events) {
                for (auto i = 0u; i < nb_events; ++i) {
                    latency->add(std::chrono::steady_clock::now() - event[i]._timepoint);
                    if (event[i]._ttl)
                        spsc[1].enqueue(LightEvent(event[i]._ttl));
                    else {
                        running->store(false, std::memory_order_release);
                    }
                }
            },
            events, 4096u);
    }
    if (latency->sample_count()) {
        qb::bench::record_last_latency(latency->mean_nanoseconds(),
                                       static_cast<std::uint64_t>(latency->sample_count()));
    }
    if (std::getenv("QB_ACTOR_BENCH_HISTOGRAM") && latency->sample_count()) {
        latency->generate<std::ostream, std::chrono::nanoseconds>(std::cout, "ns");
    }
}

void
thread_pong(qb::lockfree::spsc::ringbuffer<LightEvent, 4096> *spsc,
            std::atomic<bool> *running) {
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

static void
record_latency_counters(benchmark::State &state) {
    state.counters["round_trips_per_s"] =
        benchmark::Counter(kRoundTripsPerIter,
                           benchmark::Counter::kIsIterationInvariantRate);
    state.counters["messages_per_s"] =
        benchmark::Counter(kMessagesPerIter, benchmark::Counter::kIsIterationInvariantRate);

    const auto lat = qb::bench::last_latency_stats_snapshot();
    state.counters["latency_samples"] = static_cast<double>(lat.samples);
    if (lat.samples) {
        state.counters["mean_rtt_ns"] =
            benchmark::Counter(lat.mean_round_trip_ns, benchmark::Counter::kAvgIterations);
    }
}

static void
BM_Reference_Multi_PingPong_Latency(benchmark::State &state) {
    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        std::atomic<bool> running{true};
        auto spsc = std::make_unique<qb::lockfree::spsc::ringbuffer<LightEvent, 4096>[]>(2);
        std::thread threads[2];

        threads[0] = std::thread(thread_ping, spsc.get(), &running);
        threads[1] = std::thread(thread_pong, spsc.get(), &running);

        for (auto &thread : threads) {
            if (thread.joinable())
                thread.join();
        }
        record_latency_counters(state);
    }
}

static void
BM_Mono_PingPong_Latency(benchmark::State &state) {
    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        state.PauseTiming();
        qb::Main main;
        main.addActor<PingActor>(0);
        main.addActor<PongActor>(0);
        state.ResumeTiming();

        main.start(true);
        main.join();
        record_latency_counters(state);
    }
}

static void
BM_Multi_PingPong_Latency(benchmark::State &state) {
    const auto hw = qb::bench::effectiveHardwareCores();
    if (hw < 2) {
        state.SkipWithError("BM_Multi_PingPong_Latency requires at least 2 logical processors");
        return;
    }
    const auto pong_core = std::min<std::uint32_t>(hw >= 3 ? 2u : 1u, hw - 1u);

    for (auto _ : state) {
        qb::bench::reset_last_latency_stats();
        state.PauseTiming();
        qb::Main main;
        main.core(0).addActor<PingActor>();
        main.core(pong_core).addActor<PongActor>();
        state.ResumeTiming();

        main.start(true);
        main.join();
        record_latency_counters(state);
    }
}

BENCHMARK(BM_Reference_Multi_PingPong_Latency)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK(BM_Mono_PingPong_Latency)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK(BM_Multi_PingPong_Latency)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
