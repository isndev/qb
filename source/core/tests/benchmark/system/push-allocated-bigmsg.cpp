/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/system/push-allocated-bigmsg.cpp
 * @brief Large one-way messages: `Actor::push` vs `Pipe::allocated_push`, sweeping the alloc hint.
 *
 * One source actor emits `messages` copies of a ~1 KiB `BigPipeMsg` (a `uint64_t seq` + 127 padding
 * words) at one sink, two ways:
 *   - `push<BigPipeMsg>(dst, i)` — the regular per-message allocator path;
 *   - `getPipe(dst).allocated_push<BigPipeMsg>(extra_alloc_bytes, i)` — the bucket path, handed an
 *     extra pre-allocation hint so its allocator behaviour differs from plain `push`.
 * The `UseAllocatedPush` template bool selects the path; the run is parameterised on
 * `extra_alloc_bytes` (swept) so you can watch the allocated path's cost as the hint grows. The sink
 * is the shared `qb::bench::CountAndKillSinkActor<BigPipeMsg>` — it counts deliveries and ends the
 * engine with `broadcast<KillEvent>()` the instant it has seen exactly `messages`.
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - per-iteration engine construction is hoisted out of the timed region with
 *     `PauseTiming()`/`ResumeTiming()`; only `start(true)` + `join()` (the delivery work) is timed;
 *   - `SetItemsProcessed` / `SetBytesProcessed` (the logical payload-byte volume,
 *     `sizeof(BigPipeMsg) * messages`) and `messages_per_s` are assigned ONCE after the loop;
 *   - a one-shot, out-of-loop probe runs one full topology and `DoNotOptimize`s the expected count,
 *     so a sink that never reaches its quota hangs HERE (caught) instead of skewing a timing number.
 *
 * Rewritten from the former `bm-allocated-push.cpp` onto the shared sink/source skeleton.
 */

#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkActors.h"
#include "../../shared/BenchmarkCores.h"

namespace {

// ~1 KiB one-way message: seq + 127 padding words.
struct BigPipeMsg final : qb::Event {
    std::uint64_t seq = 0;
    std::uint64_t pad[127]{};

    explicit BigPipeMsg(std::uint64_t const s)
        : seq(s) {}
};

// Source: emit `count` BigPipeMsg at `dst` (regular push or allocated_push), then self-kill. The
// shared CountAndKillSinkActor<BigPipeMsg> terminates the engine once it has counted them all.
template <bool UseAllocatedPush>
class AllocPipeSourceActor final : public qb::Actor {
    const qb::ActorId   _dst;
    const std::uint64_t _count;
    const std::size_t   _extra_alloc;

public:
    AllocPipeSourceActor(qb::ActorId const dst, std::uint64_t const count, std::size_t const extra_alloc)
        : _dst(dst)
        , _count(count)
        , _extra_alloc(extra_alloc) {}

    qb::io::async::task<bool>
    onInit() final {
        if constexpr (UseAllocatedPush) {
            qb::pipe const p = getPipe(_dst);
            for (std::uint64_t i = 0; i < _count; ++i)
                static_cast<void>(p.allocated_push<BigPipeMsg>(_extra_alloc, i));
        } else {
            for (std::uint64_t i = 0; i < _count; ++i)
                push<BigPipeMsg>(_dst, i);
        }
        kill();
        co_return true;
    }
};

// Build the source→sink topology into `main`; returns the count the sink must observe.
template <bool UseAllocatedPush>
[[nodiscard]] std::uint64_t
build_topology(qb::Main &main, std::uint64_t const count, std::uint32_t const producer_c, std::uint32_t const consumer_c,
               std::size_t const extra) {
    auto const sink = main.addActor<qb::bench::CountAndKillSinkActor<BigPipeMsg>>(consumer_c, count);
    main.addActor<AllocPipeSourceActor<UseAllocatedPush>>(producer_c, sink, count, extra);
    return count;
}

template <bool UseAllocatedPush>
void
BM_AllocVsPush_BigMsg(benchmark::State &state) {
    const auto count      = static_cast<std::uint64_t>(state.range(0));
    const auto producer_c = static_cast<std::uint32_t>(state.range(1));
    const auto consumer_c = static_cast<std::uint32_t>(state.range(2));
    const auto extra      = static_cast<std::size_t>(state.range(3));

    // One-shot out-of-loop correctness probe: a sink that never reaches its quota would hang join()
    // here, catching a structurally broken topology before any timing.
    {
        qb::Main probe;
        auto     expect = build_topology<UseAllocatedPush>(probe, count, producer_c, consumer_c, extra);
        probe.start(true);
        probe.join();
        benchmark::DoNotOptimize(expect);
    }

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        (void) build_topology<UseAllocatedPush>(main, count, producer_c, consumer_c, extra);
        state.ResumeTiming();

        main.start(true);
        main.join();
    }

    const double bytes = static_cast<double>(count) * static_cast<double>(sizeof(BigPipeMsg));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * count));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * static_cast<std::uint64_t>(bytes)));
    state.counters["messages_per_s"] = benchmark::Counter(static_cast<double>(count), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["approx_payload_bytes_per_s"] = benchmark::Counter(bytes, benchmark::Counter::kIsIterationInvariantRate);
    state.counters["extra_alloc_bytes"]          = static_cast<double>(extra);
}

void
ApplyAllocPushArgs(benchmark::internal::Benchmark *b) {
    const auto             cap    = qb::bench::cappedBenchmarkCores();
    constexpr std::int64_t kCount = 200000;
    // Sweep the extra pre-allocation hint handed to allocated_push (regular push ignores it).
    for (std::int64_t extra : {0, 128, 512, 4096}) {
        b->Args({kCount, 0, 0, extra}); // same-core
        if (cap > 1u)
            b->Args({kCount, 0, static_cast<std::int64_t>(cap - 1), extra}); // cross-core
    }
}

} // namespace

BENCHMARK_TEMPLATE(BM_AllocVsPush_BigMsg, false)
    ->Apply(ApplyAllocPushArgs)
    ->ArgNames({"messages", "producer_core", "consumer_core", "extra_alloc_bytes"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_AllocVsPush_BigMsg, true)
    ->Apply(ApplyAllocPushArgs)
    ->ArgNames({"messages", "producer_core", "consumer_core", "extra_alloc_bytes"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
