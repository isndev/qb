/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/patterns/saga-orchestration.cpp
 * @brief `qb::run_saga` multi-step orchestration overhead vs the same steps as raw `qb::ask`s.
 *
 * A saga (<qb/core/patterns/saga.h>) runs N forward steps, each registering a compensation, and on
 * failure unwinds them LIFO. This bench prices the SUCCESS path: a `SagaActor` awaits `run_saga`
 * with `steps` sequential `qb::ask`s to a `FastResponder`, registering a compensation per step
 * (never executed here). The baseline runs the identical `steps` asks WITHOUT the saga wrapper, so
 * the delta is exactly the SagaScope bookkeeping (compensation-stack push per step + the coroutine
 * frame) — the tax you pay for rollback safety.
 *
 * Correctness + termination: each step folds its responder-computed reply (`response = seq + 1`)
 * into a shared counter; a one-shot out-of-loop probe asserts the sum equals `steps*(steps+1)/2`
 * (every step completed). The coroutine signals completion back to its actor via
 * `ctx.push<SagaDone>()` (self), and the actor ends the run with `broadcast<KillEvent>()` — the
 * standard talk-back-via-context + broadcast-kill shutdown (capture everything by value; the actor
 * may be gone while the coroutine is parked).
 *
 * Methodology: per-iteration engine construction is hoisted out of the timed region (`PauseTiming`);
 * `start(true)` + `join()` is measured under `UseRealTime()`.
 */

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <qb/actor.h>
#include <qb/core/patterns.h>
#include <qb/main.h>

#include "../../shared/ProbeResponders.h"

namespace {

using qb::test::FastResponder;
using qb::test::Probe;

struct SagaDone final : qb::Event {};

class SagaActor final : public qb::Actor {
    const qb::ActorId                           _responder;
    const int                                   _steps;
    std::shared_ptr<std::atomic<std::uint64_t>> _result;
    const bool                                  _with_saga;

public:
    SagaActor(qb::ActorId const responder, int const steps, std::shared_ptr<std::atomic<std::uint64_t>> result, bool const with_saga)
        : _responder(responder)
        , _steps(steps)
        , _result(std::move(result))
        , _with_saga(with_saga) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Probe>(*this); // route our own ask replies
        registerEvent<SagaDone>(*this);

        const auto responder = _responder;
        const int  steps     = _steps;
        const auto result    = _result;

        if (_with_saga) {
            spawn([responder, steps, result](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                co_await qb::run_saga(ctx, [responder, steps, result](qb::ScopedCoroContext c,
                                                                      qb::SagaScope       &saga) -> qb::io::async::task<void> {
                    for (int i = 0; i < steps; ++i) {
                        auto r = co_await qb::ask(c, responder, Probe{i}, std::chrono::seconds(2));
                        result->fetch_add(static_cast<std::uint64_t>(r.response), std::memory_order_relaxed);
                        saga.on_compensate([c, responder, i]() -> qb::io::async::task<void> {
                            (void) co_await qb::ask(c, responder, Probe{-(i + 1)}, std::chrono::seconds(2));
                        });
                    }
                });
                ctx.push<SagaDone>(); // to our own actor (self)
            });
        } else {
            spawn([responder, steps, result](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
                for (int i = 0; i < steps; ++i) {
                    auto r = co_await qb::ask(ctx, responder, Probe{i}, std::chrono::seconds(2));
                    result->fetch_add(static_cast<std::uint64_t>(r.response), std::memory_order_relaxed);
                }
                ctx.push<SagaDone>(); // to our own actor (self)
            });
        }
        co_return true;
    }

    void
    on(Probe &e) {
        resolve_ask(e);
    }

    void
    on(SagaDone const &) {
        broadcast<qb::KillEvent>();
    }
};

void
build_saga(qb::Main &main, int const steps, std::shared_ptr<std::atomic<std::uint64_t>> const &result, bool const with_saga) {
    const auto responder = main.addActor<FastResponder>(0);
    main.addActor<SagaActor>(0, responder, steps, result, with_saga);
}

void
run_saga_bench(benchmark::State &state, bool const with_saga) {
    const int           steps    = static_cast<int>(state.range(0));
    const std::uint64_t expected = static_cast<std::uint64_t>(steps) * (static_cast<std::uint64_t>(steps) + 1ull) / 2ull;

    // One-shot out-of-loop correctness probe: every step must complete (sum of seq+1 replies).
    {
        auto     result = std::make_shared<std::atomic<std::uint64_t>>(0);
        qb::Main probe;
        build_saga(probe, steps, result, with_saga);
        probe.start(true);
        probe.join();
        if (result->load(std::memory_order_relaxed) != expected) {
            state.SkipWithError("saga did not complete every step (reply sum mismatch)");
            return;
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        auto     result = std::make_shared<std::atomic<std::uint64_t>>(0);
        qb::Main main;
        build_saga(main, steps, result, with_saga);
        state.ResumeTiming();

        main.start(true);
        main.join();
    }

    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * static_cast<std::uint64_t>(steps)));
    state.counters["steps_per_s"] = benchmark::Counter(static_cast<double>(steps), benchmark::Counter::kIsIterationInvariantRate);
}

void
BM_Saga_WithCompensation(benchmark::State &state) {
    run_saga_bench(state, /*with_saga=*/true);
}

void
BM_Saga_RawAsks(benchmark::State &state) {
    run_saga_bench(state, /*with_saga=*/false);
}

} // namespace

BENCHMARK(BM_Saga_WithCompensation)->Arg(1)->Arg(4)->Arg(16)->Arg(64)->ArgNames({"steps"})->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Saga_RawAsks)->Arg(1)->Arg(4)->Arg(16)->Arg(64)->ArgNames({"steps"})->Unit(benchmark::kMicrosecond)->UseRealTime();

BENCHMARK_MAIN();
