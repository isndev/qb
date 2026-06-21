/**
 * @file qb/core/tests/benchmark/bm-messaging-api.cpp
 * @brief One-way throughput: isolate push vs send vs getPipe().push vs to().push
 *
 * Same topology (one producer, one consumer), same payload, only the send primitive
 * changes. Counters: \c messages_per_s (one API call per message; equals logical
 * \c api_calls_per_s for this bench). Includes terminal \c broadcast<KillEvent> shutdown.
 *
 * Sink registers events in \c onInit() like other actor benches. Uses \c UseRealTime()
 * with \c main.start(true).
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
#include <qb/actor.h>
#include <qb/main.h>

#include "../shared/BenchmarkIterationSink.h"

enum class MessagingApiKind : std::uint8_t { Push, Send, PipePush, ToPush };

struct MsgApiCountMsg final : qb::Event {};

class MsgApiSinkActor final : public qb::Actor {
    const std::uint64_t _expect;
    std::uint64_t       _received = 0;

public:
    explicit MsgApiSinkActor(std::uint64_t const expect)
        : _expect(expect) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<MsgApiCountMsg>(*this);
        co_return true;
    }

    void
    on(MsgApiCountMsg const &) {
        if (++_received == _expect)
            broadcast<qb::KillEvent>();
    }
};

template <MessagingApiKind Api>
class MsgApiSourceActor final : public qb::Actor {
    const qb::ActorId   _dst;
    const std::uint64_t _count;

public:
    MsgApiSourceActor(qb::ActorId const dst, std::uint64_t const count)
        : _dst(dst)
        , _count(count) {}

    qb::io::async::task<bool>
    onInit() final {
        for (std::uint64_t i = 0; i < _count; ++i) {
            if constexpr (Api == MessagingApiKind::Push) {
                push<MsgApiCountMsg>(_dst);
            } else if constexpr (Api == MessagingApiKind::Send) {
                send<MsgApiCountMsg>(_dst);
            } else if constexpr (Api == MessagingApiKind::PipePush) {
                getPipe(_dst).template push<MsgApiCountMsg>();
            } else if constexpr (Api == MessagingApiKind::ToPush) {
                to(_dst).template push<MsgApiCountMsg>();
            }
        }
        kill();
        co_return true;
    }
};

template <MessagingApiKind Api>
static void
BM_MessagingApi_OneWay(benchmark::State &state) {
    const auto n      = static_cast<std::uint64_t>(state.range(0));
    const auto p_core = static_cast<std::uint32_t>(state.range(1));
    const auto c_core = static_cast<std::uint32_t>(state.range(2));

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main   main;
        auto const consumer = main.addActor<MsgApiSinkActor>(c_core, n);
        main.addActor<MsgApiSourceActor<Api>>(p_core, consumer, n);
        state.ResumeTiming();
        main.start(true);
        main.join();
        state.counters["messages_per_s"] = benchmark::Counter(static_cast<double>(n), benchmark::Counter::kIsIterationInvariantRate);
    }
}

static void
ArgsMessagingApiCores(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    for (int n : {100000, 500000, 1000000}) {
        b->Args({n, 0, 0});
        if (cap > 1u) {
            b->Args({n, 0, cap - 1});
            b->Args({n, 0, 1});
        }
    }
}

BENCHMARK_TEMPLATE(BM_MessagingApi_OneWay, MessagingApiKind::Push)
    ->Apply(ArgsMessagingApiCores)
    ->ArgNames({"messages", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_MessagingApi_OneWay, MessagingApiKind::Send)
    ->Apply(ArgsMessagingApiCores)
    ->ArgNames({"messages", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_MessagingApi_OneWay, MessagingApiKind::PipePush)
    ->Apply(ArgsMessagingApiCores)
    ->ArgNames({"messages", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_MessagingApi_OneWay, MessagingApiKind::ToPush)
    ->Apply(ArgsMessagingApiCores)
    ->ArgNames({"messages", "producer_core", "consumer_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
