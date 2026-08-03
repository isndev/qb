/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/system/engine-lifecycle.cpp
 * @brief Engine lifecycle cost: construct → `start(true)` → `join()` of a near-empty `qb::Main`.
 *
 * Measures the fixed overhead of bringing the engine up and tearing it down with no real work:
 * VirtualCore worker-thread spawn, per-core listener/scheduler init, the start barrier handshake,
 * and the join. Parameterised on `cores` (capped to `cappedBenchmarkCores()`).
 *
 * A core's run loop only terminates once its actor set drains to empty (`VirtualCore::__workflow__`
 * breaks on `_actors.empty()` *after* a removal); a core that starts with ZERO actors would spin
 * forever and hang `join()`. So "near-empty" is the honest minimum: each of `cores` cores hosts ONE
 * trivial actor whose `onInit()` immediately `kill()`s it. That keeps the measured cost dominated by
 * thread/loop spin-up + teardown (the actor does no messaging), while guaranteeing every core breaks
 * and `join()` returns. This grows the former dead `benchmark_example.cpp` stub (which constructed
 * `qb::Main{0}` — a ctor signature that no longer exists — and never measured anything real).
 *
 * Benchmark methodology (perf harness, never a ctest gate — no `EXPECT_LT(duration,…)`):
 *   - the WHOLE lifecycle is the unit of work, so engine construction + actor registration stay
 *     INSIDE the timed region deliberately (that setup IS the cost being measured) — there is no
 *     per-iteration work to hoist out;
 *   - `DoNotOptimize` on the `qb::Main` and on `hasError()` keeps the construct/start/join chain from
 *     being elided as dead;
 *   - `SetItemsProcessed` (one engine lifecycle per iteration) and the `cores` descriptor are
 *     assigned ONCE after the loop;
 *   - a one-shot, out-of-loop probe runs one full lifecycle and asserts `!hasError()`, so a topology
 *     that fails to come up (or never terminates) is caught before timing rather than as a duration
 *     gate.
 */

#include <benchmark/benchmark.h>
#include <cstdint>
#include <qb/actor.h>
#include <qb/main.h>

#include "../../shared/BenchmarkCores.h"

namespace {

// Trivial actor: self-kills the instant it initialises, so its core's run loop drains to empty and
// terminates. Does no messaging — its only purpose is to give each core exactly one actor so the
// engine spins the core up and then tears it down.
class SelfKillActor final : public qb::Actor {
public:
    SelfKillActor() = default;

    qb::io::async::task<bool>
    onInit() final {
        kill();
        co_return true;
    }
};

// Stand up one SelfKillActor on each of `cores` cores.
void
build_engine(qb::Main &main, std::uint32_t const cores) {
    for (std::uint32_t c = 0; c < cores; ++c)
        main.addActor<SelfKillActor>(c);
}

void
BM_Engine_Lifecycle(benchmark::State &state) {
    const auto requested = static_cast<std::uint32_t>(state.range(0));
    const auto cap       = qb::bench::cappedBenchmarkCores();
    const auto cores     = std::max<std::uint32_t>(1u, std::min(requested, cap));

    // One-shot out-of-loop correctness probe: the engine must come up and tear down cleanly. A
    // topology that fails to init (or never terminates) is caught here, before timing.
    {
        qb::Main probe;
        build_engine(probe, cores);
        probe.start(true);
        probe.join();
        bool err = probe.hasError();
        benchmark::DoNotOptimize(err);
        if (err) {
            state.SkipWithError("engine reported an error bringing up the near-empty topology");
            return;
        }
    }

    for (auto _ : state) {
        qb::Main main;
        build_engine(main, cores);
        main.start(true);
        main.join();
        benchmark::DoNotOptimize(main.hasError());
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.counters["cores"] = static_cast<double>(cores);
}

void
ArgsCores(benchmark::internal::Benchmark *b) {
    const auto cap = qb::bench::cappedBenchmarkCores();
    for (std::uint32_t c = 1u; c <= cap; ++c)
        b->Args({static_cast<std::int64_t>(c)});
}

} // namespace

BENCHMARK(BM_Engine_Lifecycle)->Apply(ArgsCores)->ArgNames({"cores"})->Unit(benchmark::kMillisecond)->UseRealTime();

BENCHMARK_MAIN();
