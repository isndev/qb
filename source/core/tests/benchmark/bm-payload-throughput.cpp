/**
 * @file qb/core/tests/benchmark/bm-payload-throughput.cpp
 * @brief Ping-pong throughput vs padded event size (deterministic payload)
 *
 * \c ExtraWords is the number of additional \c std::uint64_t words after \c _ttl.
 *
 * Counters:
 *   - \c round_trips_per_s — \c initial_ttl completed round-trips per wall second
 *   - \c messages_per_s — \c 2 * initial_ttl + 1 (includes terminal \c KillEvent)
 *   - \c approx_payload_bytes_per_s — \c 2 * ttl * sizeof(SizedPingEvent<ExtraWords>);
 *     excludes control envelopes, final \c KillEvent size, and runtime copies; use as a
 *     deterministic payload-volume proxy only.
 *
 * Uses \c UseRealTime() with \c main.start(true).
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

#include <array>
#include <benchmark/benchmark.h>
#include <qb/actor.h>
#include <qb/main.h>

template <std::size_t ExtraWords>
struct SizedPingEvent final : qb::Event {
    std::uint64_t                         _ttl = 0;
    std::array<std::uint64_t, ExtraWords> _pad{};

    SizedPingEvent() = default;
    explicit SizedPingEvent(std::uint64_t const ttl)
        : _ttl(ttl) {}
};

template <std::size_t ExtraWords>
class SizedPongActor final : public qb::Actor {
public:
    bool
    onInit() final {
        registerEvent<SizedPingEvent<ExtraWords>>(*this);
        return true;
    }

    void
    on(SizedPingEvent<ExtraWords> &event) {
        --event._ttl;
        reply(event);
    }
};

template <std::size_t ExtraWords>
class SizedPingActor final : public qb::Actor {
    const std::uint64_t _max;
    const qb::ActorId   _peer;

public:
    SizedPingActor(std::uint64_t const max, qb::ActorId const peer)
        : _max(max)
        , _peer(peer) {}

    bool
    onInit() final {
        registerEvent<SizedPingEvent<ExtraWords>>(*this);
        send<SizedPingEvent<ExtraWords>>(_peer, _max);
        return true;
    }

    void
    on(SizedPingEvent<ExtraWords> &event) {
        if (event._ttl)
            reply(event);
        else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

template <std::size_t ExtraWords>
static void
BM_PayloadPingPong_Mono(benchmark::State &state) {
    const auto ttl = static_cast<std::uint64_t>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main   main;
        auto const pong = main.addActor<SizedPongActor<ExtraWords>>(0);
        main.addActor<SizedPingActor<ExtraWords>>(0, ttl, pong);
        state.ResumeTiming();
        main.start(true);
        main.join();
        state.counters["round_trips_per_s"] = benchmark::Counter(static_cast<double>(ttl), benchmark::Counter::kIsIterationInvariantRate);
        const double msgs                   = static_cast<double>(2ull * ttl + 1ull);
        state.counters["messages_per_s"]    = benchmark::Counter(msgs, benchmark::Counter::kIsIterationInvariantRate);
        const double approx_payload_bytes   = static_cast<double>(2ull * ttl * sizeof(SizedPingEvent<ExtraWords>));
        state.counters["approx_payload_bytes_per_s"] = benchmark::Counter(approx_payload_bytes, benchmark::Counter::kIsIterationInvariantRate);
    }
}

template <std::size_t ExtraWords>
static void
ApplyPayloadTtls(benchmark::internal::Benchmark *b) {
    static constexpr std::int64_t kTtls[] = {4096, 16384, 65536, 262144};
    for (std::int64_t const t : kTtls) {
        if (ExtraWords >= 127 && t > 65536)
            continue;
        b->Args({t});
    }
}

BENCHMARK_TEMPLATE(BM_PayloadPingPong_Mono, 0)->Apply(ApplyPayloadTtls<0>)->ArgName("initial_ttl")->UseRealTime();
BENCHMARK_TEMPLATE(BM_PayloadPingPong_Mono, 1)->Apply(ApplyPayloadTtls<1>)->ArgName("initial_ttl")->UseRealTime();
BENCHMARK_TEMPLATE(BM_PayloadPingPong_Mono, 7)->Apply(ApplyPayloadTtls<7>)->ArgName("initial_ttl")->UseRealTime();
BENCHMARK_TEMPLATE(BM_PayloadPingPong_Mono, 31)->Apply(ApplyPayloadTtls<31>)->ArgName("initial_ttl")->UseRealTime();
BENCHMARK_TEMPLATE(BM_PayloadPingPong_Mono, 127)->Apply(ApplyPayloadTtls<127>)->ArgName("initial_ttl")->UseRealTime();

BENCHMARK_MAIN();
