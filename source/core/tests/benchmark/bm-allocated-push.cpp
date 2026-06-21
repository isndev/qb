/**
 * @file qb/core/tests/benchmark/bm-allocated-push.cpp
 * @brief Large one-way messages: \c Actor::push vs \c Pipe::allocated_push
 *
 * Same \c BigPipeMsg layout (~1 KiB of \c uint64_t payload after \c seq). The allocated
 * path passes an extra pre-allocation hint (\c extra_alloc_bytes) to \c allocated_push so
 * the bucket path differs from the regular \c push allocator.
 *
 * Counters: \c messages_per_s, \c approx_payload_bytes_per_s (\c sizeof(BigPipeMsg) *
 * count; logical payload bytes enqueued, not full runtime bytes including envelopes).
 * Uses \c UseRealTime() with \c main.start(true). Sink registers events in \c onInit().
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
#include <cstdint>
#include <qb/actor.h>
#include <qb/main.h>

#include "../shared/BenchmarkIterationSink.h"

struct BigPipeMsg final : qb::Event {
    std::uint64_t seq = 0;
    std::uint64_t pad[127]{};

    explicit BigPipeMsg(std::uint64_t const s)
        : seq(s) {}
};

class AllocPipeSinkActor final : public qb::Actor {
    const std::uint64_t _expect;
    std::uint64_t       _got = 0;

public:
    explicit AllocPipeSinkActor(std::uint64_t const expect)
        : _expect(expect) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<BigPipeMsg>(*this);
        co_return true;
    }

    void
    on(BigPipeMsg const &) {
        if (++_got == _expect)
            broadcast<qb::KillEvent>();
    }
};

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

template <bool UseAllocatedPush>
static void
BM_AllocVsPush_BigMsg(benchmark::State &state) {
    const auto count      = static_cast<std::uint64_t>(state.range(0));
    const auto producer_c = static_cast<std::uint32_t>(state.range(1));
    const auto consumer_c = static_cast<std::uint32_t>(state.range(2));
    const auto extra      = static_cast<std::size_t>(state.range(3));

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main   main;
        auto const sink = main.addActor<AllocPipeSinkActor>(consumer_c, count);
        main.addActor<AllocPipeSourceActor<UseAllocatedPush>>(producer_c, sink, count, extra);
        state.ResumeTiming();
        main.start(true);
        main.join();
        const double bytes               = static_cast<double>(count) * static_cast<double>(sizeof(BigPipeMsg));
        state.counters["messages_per_s"] = benchmark::Counter(static_cast<double>(count), benchmark::Counter::kIsIterationInvariantRate);
        state.counters["approx_payload_bytes_per_s"] = benchmark::Counter(bytes, benchmark::Counter::kIsIterationInvariantRate);
    }
}

static void
ApplyAllocPushArgs(benchmark::internal::Benchmark *b) {
    const auto             cap    = qb::bench::cappedBenchmarkCores();
    constexpr std::int64_t kCount = 200000;
    constexpr std::int64_t kExtra = 128;
    b->Args({kCount, 0, 0, kExtra});
    if (cap > 1u)
        b->Args({kCount, 0, static_cast<std::int64_t>(cap - 1), kExtra});
}

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
