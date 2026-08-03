/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/patterns/pubsub-dispatch.cpp
 * @brief Topic publish fan-out throughput: `qb::PubSub<Topic>` to N same-core subscribers.
 *
 * `qb::PubSub<Topic>` (<qb/core/patterns/pubsub.h>) is a per-VirtualCore `ServiceActor`: `publish()`
 * builds a fresh `Topic` and `push<Topic>`es a copy to every subscriber synchronously. This bench
 * prices that fan-out — one publisher pushes `kTicks` topics; each of `N` subscribers receives all
 * `kTicks`, so the engine moves `kTicks * N` deliveries per run. Deliveries/s is the headline.
 *
 * Topology + termination (deterministic, no sleeps): the bus is added first; each subscriber
 * `subscribe()`s in `onInit()` then hands the publisher a `SubReady`. Once the publisher has all `N`
 * readies it publishes `kTicks` topics and kills itself. Every delivery fetch-adds a shared counter;
 * the subscriber that observes the final delivery (`== kTicks * N`) ends the run with
 * `broadcast<KillEvent>()` (which also reaps the bus service actor so the core stops). A one-shot
 * out-of-loop probe asserts the shared counter equals `kTicks * N` — a dropped topic (e.g. a
 * subscriber that forgot `registerEvent<Topic>`) is caught before any number is reported.
 *
 * Methodology: per-iteration engine construction hoisted out of the timed region (`PauseTiming`);
 * `start(true)` + `join()` measured under `UseRealTime()`.
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <cstdint>
#include <memory>
#include <qb/actor.h>
#include <qb/core/patterns/pubsub.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"

namespace {

constexpr std::uint64_t kTicks = 10'000ull;

struct Tick final : qb::Event {
    std::uint64_t seq;
    explicit Tick(std::uint64_t s)
        : seq(s) {}
};

struct SubReady final : qb::Event {};

class SubscriberActor final : public qb::Actor {
    const qb::ActorId                           _publisher;
    std::shared_ptr<std::atomic<std::uint64_t>> _total;
    const std::uint64_t                         _expected_total;

public:
    SubscriberActor(qb::ActorId const publisher, std::shared_ptr<std::atomic<std::uint64_t>> total, std::uint64_t const expected)
        : _publisher(publisher)
        , _total(std::move(total))
        , _expected_total(expected) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Tick>(*this);
        getService<qb::PubSub<Tick>>()->subscribe(id());
        push<SubReady>(_publisher);
        co_return true;
    }

    void
    on(Tick const &) {
        if (_total->fetch_add(1, std::memory_order_relaxed) + 1 == _expected_total)
            broadcast<qb::KillEvent>();
    }
};

class PublisherActor final : public qb::Actor {
    const std::uint64_t _n_subs;
    const std::uint64_t _ticks;
    std::uint64_t       _ready = 0;

public:
    PublisherActor(std::uint64_t const n, std::uint64_t const ticks)
        : _n_subs(n)
        , _ticks(ticks) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<SubReady>(*this);
        co_return true;
    }

    void
    on(SubReady const &) {
        if (++_ready == _n_subs) {
            auto *bus = getService<qb::PubSub<Tick>>();
            for (std::uint64_t i = 0; i < _ticks; ++i)
                bus->publish(i);
            kill();
        }
    }
};

// Build the bus + publisher + N subscribers on core 0 (PubSub is per-core; subscribers must share it).
void
build_pubsub(qb::Main &main, std::uint64_t const n_subs, std::shared_ptr<std::atomic<std::uint64_t>> const &total) {
    main.addActor<qb::PubSub<Tick>>(0);
    const auto pub = main.addActor<PublisherActor>(0, n_subs, kTicks);
    for (std::uint64_t i = 0; i < n_subs; ++i)
        main.addActor<SubscriberActor>(0, pub, total, kTicks * n_subs);
}

void
BM_PubSub_Dispatch(benchmark::State &state) {
    const auto n_subs = static_cast<std::uint64_t>(state.range(0));

    // One-shot out-of-loop correctness probe: every subscriber must receive every tick.
    {
        auto     total = std::make_shared<std::atomic<std::uint64_t>>(0);
        qb::Main probe;
        build_pubsub(probe, n_subs, total);
        probe.start(true);
        probe.join();
        if (total->load(std::memory_order_relaxed) != kTicks * n_subs) {
            state.SkipWithError("pub/sub dropped topics: deliveries != kTicks * N");
            return;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        auto     total = std::make_shared<std::atomic<std::uint64_t>>(0);
        qb::Main main;
        build_pubsub(main, n_subs, total);
        state.ResumeTiming();

        main.start(true);
        main.join();
    }

    const auto deliveries = kTicks * n_subs;
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * deliveries));
    state.counters["deliveries_per_s"] = benchmark::Counter(static_cast<double>(deliveries), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["topics_per_s"]     = benchmark::Counter(static_cast<double>(kTicks), benchmark::Counter::kIsIterationInvariantRate);
}

} // namespace

BENCHMARK(BM_PubSub_Dispatch)->Arg(1)->Arg(8)->Arg(64)->Arg(256)->ArgNames({"subscribers"})->Unit(benchmark::kMillisecond)->UseRealTime();

BENCHMARK_MAIN();
