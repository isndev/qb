/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/forward-vs-direct.cpp
 * @brief Direct ping-pong vs a one-hop `forward()` relay (same logical TTL).
 *
 *   DIRECT : Ping → Pong → reply → Ping … (steady loop)
 *   FORWARD: Ping → Relay (`forward`) → Pong → reply → Ping … (relay routes only the FIRST hop;
 *            it then `kill()`s itself so the steady Ping↔Pong loop is identical to DIRECT).
 *
 * The whole point of `forward()` (vs `push`/`send`) is that it preserves the original `source`, so
 * Pong sees the *Ping* actor as `event.getSource()` and its `reply()` returns to Ping — not to the
 * Relay. If `forward()` wrongly rewrote the source, the reply would loop back to the dead Relay and
 * the chain would stall.
 *
 * forward()-preserved-source guard (NOT a `EXPECT_LT(duration,…)` ctest gate — perf harness): the
 * Ping actor tallies the round-trips it actually completed into a per-run
 * `shared_ptr<std::atomic<std::uint64_t>>`. A one-shot, out-of-loop probe run of BOTH modes asserts
 * the tally equals `initial_ttl` — proving `forward()` preserved the source (otherwise the relayed
 * chain would never reach full TTL) before any timing.
 *
 * Shutdown note: a VirtualCore exits only when ALL its actors are removed. The relay shares the ping
 * core, so it MUST `kill()` itself after its single forward, or `join()` would block forever.
 *
 * Timing: `start(true)` is INSIDE the timed region (wall time via `UseRealTime`), unlike the
 * join()-only ping-pong throughput bench — compare only within the same timing policy. Counters
 * (`round_trips_per_s`, `logical_messages_per_s`, `actor_deliveries_per_s`, `bytes_per_s`) are
 * assigned once after the loop.
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <memory>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"

namespace {

struct TinyEvent final : qb::Event {
    std::uint64_t _ttl = 0;
    explicit TinyEvent(std::uint64_t const ttl)
        : _ttl(ttl) {}
};

class BenchPong final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<TinyEvent>(*this);
        co_return true;
    }

    void
    on(TinyEvent &event) {
        --event._ttl;
        reply(event); // returns to event.getSource() — must be Ping, even via the relay
    }
};

class BenchRelay final : public qb::Actor {
    const qb::ActorId _pong;

public:
    explicit BenchRelay(qb::ActorId const pong)
        : _pong(pong) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<TinyEvent>(*this);
        co_return true;
    }

    void
    on(TinyEvent &event) {
        forward(_pong, event); // preserves source == Ping, so Pong's reply returns to Ping
        // One-shot relay: subsequent Ping↔Pong traffic bypasses us. We must kill() or the ping
        // core never reaches _actors.empty() and join() hangs.
        kill();
    }
};

class BenchPing final : public qb::Actor {
    const std::uint64_t                         _max;
    const qb::ActorId                           _first_hop;
    std::uint64_t                               _round_trips = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> _tally;

public:
    BenchPing(std::uint64_t const max, qb::ActorId const first_hop, std::shared_ptr<std::atomic<std::uint64_t>> tally)
        : _max(max)
        , _first_hop(first_hop)
        , _tally(std::move(tally)) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<TinyEvent>(*this);
        send<TinyEvent>(_first_hop, _max);
        co_return true;
    }

    void
    on(TinyEvent &event) {
        ++_round_trips;
        if (event._ttl)
            reply(event);
        else {
            _tally->store(_round_trips, std::memory_order_release);
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

template <bool WithRelay>
void
build_forward_vs_direct(qb::Main &main, std::uint64_t const ttl, std::uint32_t const ping_core, std::uint32_t const pong_core,
                        std::shared_ptr<std::atomic<std::uint64_t>> const &tally) {
    auto const pong = main.addActor<BenchPong>(pong_core);
    if constexpr (WithRelay) {
        auto const relay = main.addActor<BenchRelay>(ping_core, pong);
        main.addActor<BenchPing>(ping_core, ttl, relay, tally);
    } else {
        main.addActor<BenchPing>(ping_core, ttl, pong, tally);
    }
}

template <bool WithRelay>
void
BM_ForwardVsDirect_PingPong(benchmark::State &state) {
    const auto ttl       = static_cast<std::uint64_t>(state.range(0));
    const auto ping_core = static_cast<std::uint32_t>(state.range(1));
    const auto pong_core = static_cast<std::uint32_t>(state.range(2));

    // One-shot forward()-preserved-source guard: the relayed chain must reach full TTL.
    {
        qb::Main   probe;
        auto const tally = std::make_shared<std::atomic<std::uint64_t>>(0);
        build_forward_vs_direct<WithRelay>(probe, ttl, ping_core, pong_core, tally);
        probe.start(true);
        probe.join();
        if (tally->load(std::memory_order_acquire) != ttl)
            state.SkipWithError("forward() source-preservation guard failed: relayed chain did not reach full TTL");
    }

    auto const tally = std::make_shared<std::atomic<std::uint64_t>>(0);
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        build_forward_vs_direct<WithRelay>(main, ttl, ping_core, pong_core, tally);
        state.ResumeTiming();
        main.start(true);
        main.join();
    }

    double actor_deliveries = static_cast<double>(2ull * ttl + 1ull);
    if constexpr (WithRelay)
        actor_deliveries += 1.0; // the extra Ping→Relay→Pong first-hop delivery

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * ttl));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * 2ull * ttl * sizeof(TinyEvent)));
    state.counters["round_trips_per_s"] = benchmark::Counter(static_cast<double>(ttl), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["logical_messages_per_s"] =
        benchmark::Counter(static_cast<double>(2ull * ttl + 1ull), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["actor_deliveries_per_s"] = benchmark::Counter(actor_deliveries, benchmark::Counter::kIsIterationInvariantRate);
}

void
ApplyForwardReplyArgs(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    const auto t   = 1ull << 14;
    b->Args({static_cast<std::int64_t>(t), 0, 0});
    if (cap > 1u)
        b->Args({static_cast<std::int64_t>(t), 0, static_cast<std::int64_t>(cap - 1)});
}

} // namespace

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
