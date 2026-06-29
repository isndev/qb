/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/producer-consumer-throughput.cpp
 * @brief One producer → one consumer one-way push throughput (mono-core and cross-core).
 *
 * A single producer issues `kBenchMessages` one-way `push<PcMsg>` calls down a cached `qb::pipe`
 * to one consumer; the consumer terminates the run with `broadcast<KillEvent>()` after the last
 * message. The mono variant places both actors on core 0; the multi variant puts the consumer on
 * the far core so every push crosses the lock-free MPSC ring.
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - engine construction is hoisted out of the timed region with `PauseTiming()`/`ResumeTiming()`,
 *     so only `start(true)` + `join()` is measured under `UseRealTime()`;
 *   - the two collinear `pushes_per_s` / `messages_per_s` rows of the old bench are collapsed into a
 *     single `messages_per_s` (one push == one delivered message here), plus `bytes_per_s` via
 *     `SetBytesProcessed`;
 *   - the counter is assigned once *after* the timed loop;
 *   - a one-shot, out-of-loop probe run (`DoNotOptimize`) catches a structurally broken topology.
 *
 * The multi-core variant SELF-SKIPS (`SkipWithError`) unless `cappedBenchmarkCores() >= 2`, so it
 * never silently degenerates into a mono-core run on a 1-core runner.
 */

#include <benchmark/benchmark.h>
#include <cstdint>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"

namespace {
constexpr std::uint64_t kBenchMessages = 1'000'000ull;

struct PcMsg final : qb::Event {
    std::uint64_t seq = 0;
};

class PcConsumerActor final : public qb::Actor {
    std::uint64_t _received = 0;

public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<PcMsg>(*this);
        co_return true;
    }

    void
    on(PcMsg const &) {
        if (++_received == kBenchMessages)
            broadcast<qb::KillEvent>();
    }
};

class PcProducerActor final : public qb::Actor {
    const qb::pipe _to_pipe;

public:
    PcProducerActor() = delete;
    explicit PcProducerActor(qb::ActorId const to)
        : _to_pipe(getPipe(to)) {}

    qb::io::async::task<bool>
    onInit() final {
        for (std::uint64_t i = 1; i <= kBenchMessages; ++i)
            _to_pipe.push<PcMsg>().seq = i;
        co_return true;
    }
};

// Build a producer→consumer pair on the given cores (producer always on core 0).
void
build_producer_consumer(qb::Main &main, std::uint32_t const consumer_core) {
    main.addActor<PcProducerActor>(0, main.addActor<PcConsumerActor>(consumer_core));
}

void
run_throughput(benchmark::State &state, std::uint32_t const consumer_core) {
    // One-shot out-of-loop correctness probe.
    {
        qb::Main probe;
        build_producer_consumer(probe, consumer_core);
        probe.start(true);
        probe.join();
        auto sentinel = consumer_core;
        benchmark::DoNotOptimize(sentinel);
    }

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        build_producer_consumer(main, consumer_core);
        state.ResumeTiming();

        main.start(true);
        main.join();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kBenchMessages));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * kBenchMessages * sizeof(PcMsg)));
    state.counters["messages_per_s"] = benchmark::Counter(static_cast<double>(kBenchMessages), benchmark::Counter::kIsIterationInvariantRate);
}

void
BM_Mono_Producer_Consumer(benchmark::State &state) {
    run_throughput(state, 0);
}

void
BM_Multi_Producer_Consumer(benchmark::State &state) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    if (cap < 2u) {
        state.SkipWithError("requires-multicore: BM_Multi_Producer_Consumer needs >= 2 benchmark cores for "
                            "cross-core placement");
        return;
    }
    run_throughput(state, cap - 1u);
}

} // namespace

BENCHMARK(BM_Mono_Producer_Consumer)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Multi_Producer_Consumer)->Unit(benchmark::kMillisecond)->UseRealTime();

BENCHMARK_MAIN();
