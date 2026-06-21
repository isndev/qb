/**
 * @file qb/core/tests/benchmark/bm-forward-reply.cpp
 * @brief Direct ping–pong vs one-hop \c forward relay (same logical ttl)
 *
 * \e Direct: Ping → Pong → reply → Ping … \e Forward: Ping → Relay (\c forward) → Pong
 * → reply → Ping … Relay only re-routes; payload and hop count to complete ttl are the
 * same from Ping’s perspective.
 *
 * Timing: \c main.start(true) is inside the timed region (wall time via \c UseRealTime),
 * unlike \c bm-ping-pong.cpp which times only \c join() after \c start() — intentional
 * heterogeneity; compare benches only within the same timing policy.
 *
 * Counters:
 *   - \c round_trips_per_s — \c initial_ttl (TTL convention, same as other ping–pong benches).
 *   - \c logical_messages_per_s — \c 2 * ttl + 1 (Ping-visible protocol convention: each
 *     TTL hop pair plus terminal \c KillEvent); identical in both modes, not physical
 *     runtime deliveries.
 *   - \c actor_deliveries_per_s — analytic count of \c FwTinyEvent + terminal \c KillEvent
 *     deliveries to actors: direct \c 2*ttl+1; relay \c 2*ttl+2 (one extra hop: Ping→Relay
 *     and Relay→Pong before the steady Ping↔Pong loop, which then matches direct).
 *
 * Relay uses \c forward() so Pong still sees the Ping actor as \c event.source; \c reply
 * therefore returns to Ping (\c Actor::forward must not replace \c source — see
 * \c Actor.cpp).
 *
 * \b Shutdown: each \c VirtualCore worker only exits its run loop when \e all actors on
 * that core are removed (\c VirtualCore::__workflow__, \c _actors.empty()). With relay,
 * ping and relay share the ping core; Ping calls \c kill() but Relay would otherwise stay
 * alive forever and \c main.join() would block. The relay therefore \c kill()s itself
 * after the single forward (this bench only uses relay for the first hop).
 *
 * Actor order: pong, optional relay, ping so \c onInit / registration completes before
 * \c start().
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

struct FwTinyEvent final : qb::Event {
    std::uint64_t _ttl = 0;
    explicit FwTinyEvent(std::uint64_t const ttl)
        : _ttl(ttl) {}
};

class FwPongActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<FwTinyEvent>(*this);
        co_return true;
    }

    void
    on(FwTinyEvent &event) {
        --event._ttl;
        reply(event);
    }
};

class FwRelayActor final : public qb::Actor {
    const qb::ActorId _pong;

public:
    explicit FwRelayActor(qb::ActorId const pong)
        : _pong(pong) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<FwTinyEvent>(*this);
        co_return true;
    }

    void
    on(FwTinyEvent &event) {
        forward(_pong, event);
        // One-shot relay: further Ping↔Pong traffic bypasses us. If we stay alive, the
        // ping core never reaches _actors.empty() and VirtualCore::__workflow__ never
        // terminates → join() hangs.
        kill();
    }
};

class FwPingActor final : public qb::Actor {
    const std::uint64_t _max;
    const qb::ActorId   _first_hop;

public:
    FwPingActor(std::uint64_t const max, qb::ActorId const first_hop)
        : _max(max)
        , _first_hop(first_hop) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<FwTinyEvent>(*this);
        send<FwTinyEvent>(_first_hop, _max);
        co_return true;
    }

    void
    on(FwTinyEvent &event) {
        if (event._ttl)
            reply(event);
        else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

template <bool WithRelay>
static void
BM_ForwardVsDirect_PingPong(benchmark::State &state) {
    const auto ttl       = static_cast<std::uint64_t>(state.range(0));
    const auto ping_core = static_cast<std::uint32_t>(state.range(1));
    const auto pong_core = static_cast<std::uint32_t>(state.range(2));

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main   main;
        auto const pong = main.addActor<FwPongActor>(pong_core);
        if constexpr (WithRelay) {
            auto const relay = main.addActor<FwRelayActor>(ping_core, pong);
            main.addActor<FwPingActor>(ping_core, ttl, relay);
        } else {
            main.addActor<FwPingActor>(ping_core, ttl, pong);
        }
        state.ResumeTiming();
        main.start(true);
        main.join();
        state.counters["round_trips_per_s"]      = benchmark::Counter(static_cast<double>(ttl), benchmark::Counter::kIsIterationInvariantRate);
        const double logical                     = static_cast<double>(2ull * ttl + 1ull);
        state.counters["logical_messages_per_s"] = benchmark::Counter(logical, benchmark::Counter::kIsIterationInvariantRate);
        double actor_deliveries                  = static_cast<double>(2ull * ttl + 1ull);
        if constexpr (WithRelay)
            actor_deliveries += 1.0;
        state.counters["actor_deliveries_per_s"] = benchmark::Counter(actor_deliveries, benchmark::Counter::kIsIterationInvariantRate);
    }
}

static void
ApplyForwardReplyArgs(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    const auto t   = 1ull << 14;
    b->Args({static_cast<std::int64_t>(t), 0, 0});
    if (cap > 1u)
        b->Args({static_cast<std::int64_t>(t), 0, static_cast<std::int64_t>(cap - 1)});
}

BENCHMARK_TEMPLATE(BM_ForwardVsDirect_PingPong, false)
    ->Apply(ApplyForwardReplyArgs)
    ->ArgNames({"initial_ttl", "ping_core", "pong_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK_TEMPLATE(BM_ForwardVsDirect_PingPong, true)
    ->Apply(ApplyForwardReplyArgs)
    ->ArgNames({"initial_ttl", "ping_core", "pong_core"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

BENCHMARK_MAIN();
