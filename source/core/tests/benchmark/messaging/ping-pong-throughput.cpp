/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/ping-pong-throughput.cpp
 * @brief Ping-pong throughput across event payload shapes (tiny, big-inline, heap-owning, checksum).
 *
 * `NB_PING_ACTOR` independent ping/pong pairs each bounce a TTL-decrementing event `NB_PING` times,
 * spread across `NB_CORE` workers (ping on `i % NB_CORE`, pong on the next core). The event-trait
 * axis prices the per-message copy/move cost of four payload shapes:
 *   - `TinyEvent`   — a bare `_ttl` (the zero-axis baseline; stays LOCAL to this bench);
 *   - `BigEvent`    — ~1 KiB trivially-relocatable inline payload (shared `shared/TestEvent.h`);
 *   - `DynamicEvent`— a heap-owning `std::vector<int>` payload forcing real ctor/dtor/move per hop
 *     (shared `shared/TestEvent.h`);
 *   - `TestEvent`   — a 32-byte self-validating payload (shared `shared/TestEvent.h`): EVERY hop the
 *     pong/ping `checkSum()`-validates the bytes, so this trait additionally proves the payload
 *     travelled the mailbox intact at throughput scale (not just that *an* event of the right type
 *     arrived).
 *
 * Per-event checkSum: traits that expose `checkSum()` are validated on every delivery via a compile-
 * time `requires` probe; a corrupted payload trips the in-handler guard and publishes a failure into
 * a per-run `shared_ptr<std::atomic<bool>>`, asserted by a one-shot out-of-loop probe run — never a
 * `EXPECT_LT(duration,…)` ctest gate (this is a perf harness).
 *
 * Benchmark methodology: actor-graph construction and `start()` are excluded from timing; only the
 * drain (`join()`) is measured under `UseRealTime()`. Counters (`round_trips_per_s`, `messages_per_s`,
 * `bytes_per_s`) are assigned once after the loop. Compare results only against benches that share
 * this same "time join() only" policy (cf. the start(true)-inside-the-loop benches).
 */

#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <memory>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"
#include "../../shared/TestEvent.h"

#ifdef NDEBUG
#define MAX_BENCHMARK_ITERATION 10
#define SHIFT_NB_EVENT 15
#else
#define SHIFT_NB_EVENT 4
#define MAX_BENCHMARK_ITERATION 1
#endif

namespace {

[[nodiscard]] std::uint32_t
max_core_range_for_bench() {
    return std::max(1u, qb::bench::cappedBenchmarkCores());
}

/// Zero-axis baseline payload (LOCAL — not a shared building block).
struct TinyEvent : qb::Event {
    std::uint64_t _ttl;
    explicit TinyEvent(std::uint64_t ttl)
        : _ttl(ttl) {}
};

// Compile-time detector for a `checkSum()` member (only `TestEvent` exposes one).
template <typename E>
concept has_checksum = requires(E const &e) {
    { e.checkSum() } -> std::convertible_to<bool>;
};

// Validate `event` if its trait carries a checksum; on mismatch flag the shared corruption latch.
template <typename E>
void
validate_payload(E const &event, std::atomic<bool> &corrupt) {
    if constexpr (has_checksum<E>) {
        if (!event.checkSum())
            corrupt.store(true, std::memory_order_relaxed);
    }
}

template <typename EventTrait>
class PongActor final : public qb::Actor {
    std::shared_ptr<std::atomic<bool>> _corrupt;

public:
    explicit PongActor(std::shared_ptr<std::atomic<bool>> corrupt)
        : _corrupt(std::move(corrupt)) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<EventTrait>(*this);
        co_return true;
    }

    void
    on(EventTrait &event) {
        validate_payload(event, *_corrupt);
        --event._ttl;
        reply(event);
    }
};

template <typename EventTrait>
class PingActor final : public qb::Actor {
    const std::uint64_t                _max_sends;
    const qb::ActorId                  _peer;
    std::shared_ptr<std::atomic<bool>> _corrupt;

public:
    PingActor(std::uint64_t const max, qb::ActorId const peer, std::shared_ptr<std::atomic<bool>> corrupt)
        : _max_sends(max)
        , _peer(peer)
        , _corrupt(std::move(corrupt)) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<EventTrait>(*this);
        send<EventTrait>(_peer, _max_sends);
        co_return true;
    }

    void
    on(EventTrait &event) {
        validate_payload(event, *_corrupt);
        if (event._ttl) {
            reply(event);
        } else {
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

template <typename EventTrait>
void
build_pingpong(qb::Main &main, int const nb_ping_actor, std::uint64_t const nb_ping, std::uint32_t const nb_core,
               std::shared_ptr<std::atomic<bool>> const &corrupt) {
    for (int i = 0; i < nb_ping_actor; ++i) {
        const auto ping_core = static_cast<std::uint32_t>(i % static_cast<int>(nb_core));
        const auto pong_core = static_cast<std::uint32_t>((ping_core + 1u) % nb_core);
        const auto pong_id   = main.addActor<PongActor<EventTrait>>(pong_core, corrupt);
        main.addActor<PingActor<EventTrait>>(ping_core, nb_ping, pong_id, corrupt);
    }
}

template <typename EventTrait>
void
BM_PINGPONG(benchmark::State &state) {
    const auto nb_ping_actor = static_cast<int>(state.range(0));
    const auto nb_ping       = static_cast<std::uint64_t>(state.range(1));
    const auto nb_core       = static_cast<std::uint32_t>(state.range(2));

    const double round_trips_total = static_cast<double>(nb_ping_actor) * static_cast<double>(nb_ping);
    const double messages_total    = static_cast<double>(nb_ping_actor) * static_cast<double>(2ull * nb_ping + 1ull);

    // One-shot out-of-loop payload-integrity probe: validates every hop's checksum on a counted run.
    {
        qb::Main   probe;
        auto const corrupt = std::make_shared<std::atomic<bool>>(false);
        build_pingpong<EventTrait>(probe, nb_ping_actor, nb_ping, nb_core, corrupt);
        probe.start();
        probe.join();
        if (corrupt->load(std::memory_order_relaxed))
            state.SkipWithError("ping-pong payload checksum mismatch: an event was corrupted in transit");
    }

    auto const corrupt = std::make_shared<std::atomic<bool>>(false);
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        build_pingpong<EventTrait>(main, nb_ping_actor, nb_ping, nb_core, corrupt);
        main.start();
        state.ResumeTiming();
        main.join();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * static_cast<std::uint64_t>(round_trips_total)));
    state.SetBytesProcessed(
        static_cast<std::int64_t>(state.iterations() * 2ull * static_cast<std::uint64_t>(round_trips_total) * sizeof(EventTrait)));
    state.counters["round_trips_per_s"] = benchmark::Counter(round_trips_total, benchmark::Counter::kIsIterationInvariantRate);
    state.counters["messages_per_s"]    = benchmark::Counter(messages_total, benchmark::Counter::kIsIterationInvariantRate);
}

} // namespace

#define REGISTER_PINGPONG(TRAIT)                                                                            \
    BENCHMARK_TEMPLATE(BM_PINGPONG, TRAIT)                                                                  \
        ->RangeMultiplier(2)                                                                                \
        ->Ranges({{1, 64}, {1u << SHIFT_NB_EVENT, 1u << SHIFT_NB_EVENT}, {1u, max_core_range_for_bench()}}) \
        ->ArgNames({"NB_PING_ACTOR", "NB_PING", "NB_CORE"})                                                 \
        ->Iterations(MAX_BENCHMARK_ITERATION)                                                               \
        ->UseRealTime()                                                                                     \
        ->Unit(benchmark::kMillisecond)

REGISTER_PINGPONG(TinyEvent);
REGISTER_PINGPONG(BigEvent);
REGISTER_PINGPONG(DynamicEvent);
REGISTER_PINGPONG(TestEvent);

BENCHMARK_MAIN();
