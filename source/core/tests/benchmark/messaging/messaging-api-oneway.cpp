/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/messaging/messaging-api-oneway.cpp
 * @brief One-way throughput per send primitive: isolate push vs send vs getPipe().push vs to().push.
 *
 * Same topology (one producer, one consumer), same empty payload — only the emit primitive changes,
 * so the four `messages_per_s` rows are a clean per-API comparison. The consumer ends the run with
 * `broadcast<KillEvent>()` once it has counted exactly `messages` deliveries.
 *
 * Delivery guard: the sink publishes its final observed `_received` into a per-run
 * `shared_ptr<std::atomic<std::uint64_t>>` (injected at construction). A one-shot, out-of-loop probe
 * run asserts that observed == `messages`, so a primitive that silently drops events (or a broken
 * registration) is caught BEFORE any timing — without ever becoming a `EXPECT_LT(duration,…)` ctest
 * gate (this is a perf harness).
 *
 * Args de-collision: on a 2-core machine the old generator emitted both `consumer_core = cap-1` and
 * `consumer_core = 1` — the SAME row when `cap == 2`. `ArgsMessagingApiCores` now de-dups the
 * candidate consumer cores, so each distinct placement appears exactly once.
 *
 * Benchmark methodology: construction hoisted out of the timed region (`PauseTiming()`); counters
 * (`messages_per_s` + `bytes_per_s`) assigned once after the loop under `UseRealTime()`.
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <memory>
#include <set>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"

namespace {

enum class MessagingApiKind : std::uint8_t { Push, Send, PipePush, ToPush };

struct MsgApiCountMsg final : qb::Event {};

class MsgApiSinkActor final : public qb::Actor {
    const std::uint64_t                         _expect;
    std::uint64_t                               _received = 0;
    std::shared_ptr<std::atomic<std::uint64_t>> _observed; ///< published once at quota for the delivery guard

public:
    MsgApiSinkActor(std::uint64_t const expect, std::shared_ptr<std::atomic<std::uint64_t>> observed)
        : _expect(expect)
        , _observed(std::move(observed)) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<MsgApiCountMsg>(*this);
        co_return true;
    }

    void
    on(MsgApiCountMsg const &) {
        if (++_received == _expect) {
            _observed->store(_received, std::memory_order_release);
            broadcast<qb::KillEvent>();
        }
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
void
build_messaging_api(qb::Main &main, std::uint64_t const n, std::uint32_t const p_core, std::uint32_t const c_core,
                    std::shared_ptr<std::atomic<std::uint64_t>> const &observed) {
    auto const consumer = main.addActor<MsgApiSinkActor>(c_core, n, observed);
    main.addActor<MsgApiSourceActor<Api>>(p_core, consumer, n);
}

template <MessagingApiKind Api>
void
BM_MessagingApi_OneWay(benchmark::State &state) {
    const auto n      = static_cast<std::uint64_t>(state.range(0));
    const auto p_core = static_cast<std::uint32_t>(state.range(1));
    const auto c_core = static_cast<std::uint32_t>(state.range(2));

    // One-shot out-of-loop delivery guard: every emitted message must reach the sink exactly once.
    {
        qb::Main   probe;
        auto const observed = std::make_shared<std::atomic<std::uint64_t>>(0);
        build_messaging_api<Api>(probe, n, p_core, c_core, observed);
        probe.start(true);
        probe.join();
        if (observed->load(std::memory_order_acquire) != n)
            state.SkipWithError("messaging-api delivery guard failed: sink did not observe every emitted message");
    }

    auto const observed = std::make_shared<std::atomic<std::uint64_t>>(0);
    for (auto _ : state) {
        state.PauseTiming();
        qb::Main main;
        build_messaging_api<Api>(main, n, p_core, c_core, observed);
        state.ResumeTiming();
        main.start(true);
        main.join();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * n));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * n * sizeof(MsgApiCountMsg)));
    state.counters["messages_per_s"] = benchmark::Counter(static_cast<double>(n), benchmark::Counter::kIsIterationInvariantRate);
}

void
ArgsMessagingApiCores(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    for (int n : {100000, 500000, 1000000}) {
        // De-dup the candidate consumer cores so colliding rows (e.g. cap-1 == 1 when cap == 2)
        // are not registered twice; producer always on core 0.
        std::set<std::uint32_t> consumer_cores{0u};
        if (cap > 1u) {
            consumer_cores.insert(cap - 1u);
            consumer_cores.insert(1u);
        }
        for (const auto c : consumer_cores)
            b->Args({n, 0, static_cast<std::int64_t>(c)});
    }
}

} // namespace

#define REGISTER_MSGAPI(KIND)                                          \
    BENCHMARK_TEMPLATE(BM_MessagingApi_OneWay, MessagingApiKind::KIND) \
        ->Apply(ArgsMessagingApiCores)                                 \
        ->ArgNames({"messages", "producer_core", "consumer_core"})     \
        ->Unit(benchmark::kMillisecond)                                \
        ->UseRealTime()

REGISTER_MSGAPI(Push);
REGISTER_MSGAPI(Send);
REGISTER_MSGAPI(PipePush);
REGISTER_MSGAPI(ToPush);

BENCHMARK_MAIN();
