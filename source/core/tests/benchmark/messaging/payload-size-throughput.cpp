/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/payload-size-throughput.cpp
 * @brief Ping-pong throughput vs padded event size (deterministic payload volume).
 *
 * `ExtraWords` is the number of trailing `std::uint64_t` words after `_ttl` in
 * `SizedPingEvent<ExtraWords>` (shared `shared/TestEvent.h`, the single source of truth for the
 * payload-size axis). A ping bounces a TTL-decrementing event off a pong actor `initial_ttl`
 * times; only the event size changes across the sweep, isolating per-message copy cost.
 *
 * Two placement variants:
 *   - MONO  — ping and pong share core 0;
 *   - XCORE — pong on the far core (`cappedBenchmarkCores() - 1`), so every hop crosses the
 *     lock-free MPSC pipe. Registered ONLY when `cappedBenchmarkCores() >= 2`, so it never
 *     silently collapses into the mono case on a single-core runner.
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - engine construction is hoisted out of the timed region with `PauseTiming()`;
 *   - `round_trips_per_s` is the comparable rate; `bytes_per_s` (via `SetBytesProcessed`) makes the
 *     payload-volume column comparable across sizes — `2 * ttl` event-sized copies per run,
 *     excluding control envelopes and the terminal `KillEvent`;
 *   - counters are assigned once after the timed loop;
 *   - a one-shot out-of-loop probe run (`DoNotOptimize`) catches a chain that never terminates.
 */

#include <array>
#include <atomic>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"
#include "../../shared/TestEvent.h"

namespace {

template <std::size_t ExtraWords>
class SizedPongActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<SizedPingEvent<ExtraWords>>(*this);
        co_return true;
    }

    void
    on(SizedPingEvent<ExtraWords> &event) {
        --event._ttl;
        reply(event);
    }
};

template <std::size_t ExtraWords>
class SizedPingActor final : public qb::Actor {
    const std::uint64_t                         _max;
    const qb::ActorId                           _peer;
    std::uint64_t                               _completed = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> _tally; // optional cross-thread round-trip count

public:
    SizedPingActor(std::uint64_t const max, qb::ActorId const peer, std::shared_ptr<std::atomic<std::uint64_t>> tally = nullptr)
        : _max(max)
        , _peer(peer)
        , _tally(std::move(tally)) {}

    ~SizedPingActor() final {
        if (_tally)
            _tally->store(_completed, std::memory_order_relaxed);
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<SizedPingEvent<ExtraWords>>(*this);
        send<SizedPingEvent<ExtraWords>>(_peer, _max);
        co_return true;
    }

    void
    on(SizedPingEvent<ExtraWords> &event) {
        ++_completed; // one inbound hop per completed round-trip ⇒ ends at exactly `_max`
        if (event._ttl)
            reply(event);
        else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

template <std::size_t ExtraWords>
void
build_payload_pingpong(qb::Main &main, std::uint64_t const ttl, std::uint32_t const pong_core,
                       std::shared_ptr<std::atomic<std::uint64_t>> const &tally = nullptr) {
    auto const pong = main.addActor<SizedPongActor<ExtraWords>>(pong_core);
    main.addActor<SizedPingActor<ExtraWords>>(0, ttl, pong, tally);
}

template <std::size_t ExtraWords>
void
run_payload_pingpong(benchmark::State &state, std::uint64_t const ttl, std::uint32_t const pong_core) {
    // One-shot out-of-loop correctness probe: assert the chain completed exactly `ttl` round-trips
    // (a dropped cross-core reply is caught by a positive check, not only by a join() hang).
    {
        auto     tally = std::make_shared<std::atomic<std::uint64_t>>(0);
        qb::Main probe;
        build_payload_pingpong<ExtraWords>(probe, ttl, pong_core, tally);
        probe.start(true);
        probe.join();
        if (tally->load(std::memory_order_relaxed) != ttl) {
            state.SkipWithError("payload ping-pong dropped round-trips: completed != initial_ttl");
            return;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        build_payload_pingpong<ExtraWords>(main, ttl, pong_core);
        state.ResumeTiming();
        main.start(true);
        main.join();
    }

    const std::uint64_t round_trips = ttl;
    const std::uint64_t payload     = 2ull * ttl * sizeof(SizedPingEvent<ExtraWords>);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * round_trips));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * payload));
    state.counters["round_trips_per_s"] = benchmark::Counter(static_cast<double>(round_trips), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["messages_per_s"] =
        benchmark::Counter(static_cast<double>(2ull * ttl + 1ull), benchmark::Counter::kIsIterationInvariantRate);
}

template <std::size_t ExtraWords>
void
BM_PayloadPingPong_Mono(benchmark::State &state) {
    run_payload_pingpong<ExtraWords>(state, static_cast<std::uint64_t>(state.range(0)), 0);
}

template <std::size_t ExtraWords>
void
BM_PayloadPingPong_XCore(benchmark::State &state) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    if (cap < 2u) {
        state.SkipWithError("requires-multicore: cross-core payload ping-pong needs >= 2 benchmark cores");
        return;
    }
    run_payload_pingpong<ExtraWords>(state, static_cast<std::uint64_t>(state.range(0)), cap - 1u);
}

template <std::size_t ExtraWords>
void
ApplyPayloadTtls(benchmark::internal::Benchmark *b) {
    static constexpr std::int64_t kTtls[] = {4096, 16384, 65536, 262144};
    for (std::int64_t const t : kTtls) {
        if (ExtraWords >= 127 && t > 65536)
            continue;
        b->Args({t});
    }
}

} // namespace

#define REGISTER_PAYLOAD_MONO(WORDS)                   \
    BENCHMARK_TEMPLATE(BM_PayloadPingPong_Mono, WORDS) \
        ->Apply(ApplyPayloadTtls<WORDS>)               \
        ->ArgName("initial_ttl")                       \
        ->Unit(benchmark::kMillisecond)                \
        ->UseRealTime()

REGISTER_PAYLOAD_MONO(0);
REGISTER_PAYLOAD_MONO(1);
REGISTER_PAYLOAD_MONO(7);
REGISTER_PAYLOAD_MONO(31);
REGISTER_PAYLOAD_MONO(127);

// Cross-core variant: registered unconditionally (it self-skips at run time when < 2 cores), so the
// row is always visible in the report even on a single-core runner.
#define REGISTER_PAYLOAD_XCORE(WORDS)                   \
    BENCHMARK_TEMPLATE(BM_PayloadPingPong_XCore, WORDS) \
        ->Apply(ApplyPayloadTtls<WORDS>)                \
        ->ArgName("initial_ttl")                        \
        ->Unit(benchmark::kMillisecond)                 \
        ->UseRealTime()

REGISTER_PAYLOAD_XCORE(0);
REGISTER_PAYLOAD_XCORE(1);
REGISTER_PAYLOAD_XCORE(7);
REGISTER_PAYLOAD_XCORE(31);
REGISTER_PAYLOAD_XCORE(127);

BENCHMARK_MAIN();
