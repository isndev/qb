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
 *   - the untimed probe verifies every returned payload; measured hops retain VerifyPayload=false;
 *   - each measured iteration captures one complete terminal EventT and publishes its count;
 *     engine/count/snapshot/terminal bytes are checked after join, outside timing. This terminal
 *     capture has nonzero cost and does not detect corruption repaired before the final delivery.
 */

#include <array>
#include <atomic>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"
#include "../../shared/TestEvent.h"

namespace payload_size_benchmark {

// Only these byte-only benchmark events may be captured; never copy an owning payload here.
template <typename EventT>
struct RunResult {
    static_assert(std::is_copy_constructible_v<EventT>);
    static_assert(std::is_trivially_destructible_v<EventT>);
    static_assert(std::is_trivially_copyable_v<EventT>);

    std::uint64_t         count = 0;
    std::optional<EventT> terminal;

    [[nodiscard]] bool
    valid(std::uint64_t const expected) const noexcept {
        if (count != expected || !terminal.has_value())
            return false;
        if constexpr (requires(EventT const &event) { event.payload_valid(); })
            return terminal->payload_valid();
        return true;
    }
};

template <typename EventT>
class SizedPongActor final : public qb::Actor {
public:
    qb::io::async::task<bool>
    onInit() final {
        registerEvent<EventT>(*this);
        co_return true;
    }

    void
    on(EventT &event) {
        --event._ttl;
        reply(event);
    }
};

template <typename EventT, bool VerifyPayload = false>
class SizedPingActor final : public qb::Actor {
    const std::uint64_t                _max;
    const qb::ActorId                  _peer;
    std::uint64_t                      _completed  = 0;
    bool                               _payload_ok = true;
    std::shared_ptr<RunResult<EventT>> _result; // one writer; read only after the workers join
    static_assert(sizeof(std::shared_ptr<RunResult<EventT>>) == sizeof(std::shared_ptr<std::atomic<std::uint64_t>>));

public:
    SizedPingActor(std::uint64_t const max, qb::ActorId const peer, std::shared_ptr<RunResult<EventT>> result = nullptr)
        : _max(max)
        , _peer(peer)
        , _result(std::move(result)) {}

    ~SizedPingActor() final {
        if (_result)
            _result->count = _payload_ok ? _completed : 0;
    }

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<EventT>(*this);
        send<EventT>(_peer, _max);
        co_return true;
    }

    void
    on(EventT &event) {
        if constexpr (VerifyPayload && requires { event.payload_valid(); })
            _payload_ok = _payload_ok && event.payload_valid();
        ++_completed; // one inbound hop per completed round-trip ⇒ ends at exactly `_max`
        if (event._ttl)
            reply(event);
        else {
            // One full snapshot per iteration, before kill; no new validation on the hot hops.
            if (_result)
                _result->terminal.emplace(event);
            kill();
            send<qb::KillEvent>(event.getSource());
        }
    }
};

template <typename EventT, bool VerifyPayload = false>
void
build_payload_pingpong(qb::Main &main, std::uint64_t const ttl, std::uint32_t const pong_core,
                       std::shared_ptr<RunResult<EventT>> const &result = nullptr) {
    auto const pong = main.addActor<SizedPongActor<EventT>>(pong_core);
    main.addActor<SizedPingActor<EventT, VerifyPayload>>(0, ttl, pong, result);
}

template <typename EventT>
bool
run_payload_pingpong(benchmark::State &state, std::uint64_t const ttl, std::uint32_t const pong_core) {
    // One-shot out-of-loop correctness probe: assert the chain completed exactly `ttl` round-trips
    // (a dropped cross-core reply is caught by a positive check, not only by a join() hang).
    {
        auto     result = std::make_shared<RunResult<EventT>>();
        qb::Main probe;
        build_payload_pingpong<EventT, true>(probe, ttl, pong_core, result);
        probe.start(true);
        probe.join();
        if (probe.hasError() || !result->valid(ttl)) {
            state.SkipWithError("payload ping-pong failed: engine error, altered payload, or incomplete round-trips");
            return false;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        auto     result = std::make_shared<RunResult<EventT>>();
        qb::Main main;
        build_payload_pingpong<EventT>(main, ttl, pong_core, result);
        state.ResumeTiming();
        main.start(true);
        main.join();
        state.PauseTiming();
        const bool valid = !main.hasError() && result->valid(ttl);
        result.reset(); // snapshot storage is freed outside timing, after its sole writer joined
        state.ResumeTiming();
        if (!valid) {
            state.SkipWithError("payload ping-pong measured iteration failed: engine error, incomplete round-trips, missing terminal snapshot, "
                                "or altered terminal payload");
            return false;
        }
    }

    const std::uint64_t round_trips = ttl;
    const std::uint64_t payload     = 2ull * ttl * sizeof(EventT);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * round_trips));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * payload));
    state.counters["round_trips_per_s"] = benchmark::Counter(static_cast<double>(round_trips), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["messages_per_s"] =
        benchmark::Counter(static_cast<double>(2ull * ttl + 1ull), benchmark::Counter::kIsIterationInvariantRate);
    return true;
}

template <std::size_t ExtraWords>
void
BM_PayloadPingPong_Mono(benchmark::State &state) {
    run_payload_pingpong<SizedPingEvent<ExtraWords>>(state, static_cast<std::uint64_t>(state.range(0)), 0);
}

template <std::size_t ExtraWords>
void
BM_PayloadPingPong_XCore(benchmark::State &state) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    if (cap < 2u) {
        state.SkipWithError("requires-multicore: cross-core payload ping-pong needs >= 2 benchmark cores");
        return;
    }
    run_payload_pingpong<SizedPingEvent<ExtraWords>>(state, static_cast<std::uint64_t>(state.range(0)), cap - 1u);
}

template <std::size_t ExtraWords>
void
ApplyPayloadTtls(benchmark::Benchmark *b) {
    static constexpr std::int64_t kTtls[] = {4096, 16384, 65536, 262144};
    for (std::int64_t const t : kTtls) {
        if (ExtraWords >= 127 && t > 65536)
            continue;
        b->Args({t});
    }
}

template <std::size_t Bytes>
struct ExactPayloadEvent final : qb::Event {
    static_assert(Bytes >= sizeof(std::uint64_t));
    std::uint64_t                                            _ttl;
    std::array<unsigned char, Bytes - sizeof(std::uint64_t)> data{};

    explicit ExactPayloadEvent(std::uint64_t ttl)
        : _ttl(ttl) {
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<unsigned char>((i * 37u + 11u) & 255u);
    }
    bool
    payload_valid() const noexcept {
        for (std::size_t i = 0; i < data.size(); ++i)
            if (data[i] != static_cast<unsigned char>((i * 37u + 11u) & 255u))
                return false;
        return true;
    }
};

template <std::size_t Bytes, bool CrossCore>
void
BM_ExactPayloadPingPong(benchmark::State &state) {
    const auto cores = qb::bench::cappedBenchmarkCores();
    if constexpr (CrossCore) {
        if (cores < 2) {
            state.SkipWithError("requires-multicore: exact payload ping-pong needs >= 2 cores");
            return;
        }
    }
    if (!run_payload_pingpong<ExactPayloadEvent<Bytes>>(state, static_cast<std::uint64_t>(state.range(0)), CrossCore ? cores - 1u : 0u))
        return;
    state.counters["payload_bytes"] = Bytes;
    state.counters["event_bytes"]   = sizeof(ExactPayloadEvent<Bytes>);
}

} // namespace payload_size_benchmark

using namespace payload_size_benchmark;

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

// Exact byte boundaries expose a layout change even when sizeof(Event) remains one bucket.
// The untimed probe verifies every returned payload; timed actors omit those checks entirely.
#define REGISTER_EXACT_PAYLOAD(BYTES)                                                                \
    BENCHMARK_TEMPLATE(BM_ExactPayloadPingPong, BYTES, false)->Arg(4096)->Arg(32768)->UseRealTime(); \
    BENCHMARK_TEMPLATE(BM_ExactPayloadPingPong, BYTES, true)->Arg(4096)->Arg(32768)->UseRealTime()

REGISTER_EXACT_PAYLOAD(32);
REGISTER_EXACT_PAYLOAD(40);
REGISTER_EXACT_PAYLOAD(41);
REGISTER_EXACT_PAYLOAD(48);
REGISTER_EXACT_PAYLOAD(49);

BENCHMARK_MAIN();
