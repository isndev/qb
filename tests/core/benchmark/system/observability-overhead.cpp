/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/system/observability-overhead.cpp
 * @brief Paired costs of pass timing, requested CoreStats snapshots and a dead-letter handler.
 *
 * Each pair performs the same bounded work on one core, with explicit NoAffinity in both modes.
 * Engine construction/factory registration/destruction and verdicts are outside timing; start(true) + join()
 * are timed, including startup, teardown and one final owner-thread snapshot per batch.
 * Pass/snapshot cells run 65536 callbacks; dead-letter cells enqueue 65536 unhandled events,
 * followed by a FIFO Done. A live actor with no Orphan subscription is the destination.
 *
 * The snapshot control consumes LoopEvent::iteration; its paired cell obtains that same value
 * from getCoreStats() and escapes the complete snapshot. This measures requesting/consuming a
 * snapshot, NOT turning the always-maintained counters on/off. Pass timing uses the same actor
 * type in both modes. The dead-letter hook does only a local increment; its capture outlives
 * the engine. Counting/log-gating on dead letters occurs in both modes. WARN output is filtered
 * at CRIT for the whole binary; emitted logging cost is outside this instrument's coverage.
 *
 * Both modes run an untimed preflight, and each measured batch is checked after timing stops.
 * No observer-mode branch is added per event/callback: snapshot choice is if constexpr; hook
 * installation and pass timing are configured before start. Checks concern counts and work,
 * never elapsed-time thresholds. TimingStats::count proves the timing path ran once per
 * completed pass; the final snapshot is taken during the current, not-yet-recorded pass.
 */

#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <qb/actor.h>
#include <qb/io.h>
#include <qb/main.h>

namespace observability_bench {

constexpr std::int64_t kWork = 65536;

struct Result {
    std::uint64_t completed  = 0;
    std::uint64_t checksum   = 0;
    std::uint64_t hook_calls = 0;
    qb::CoreStats stats{};
    bool          done = false;
};

// Reuses the callback/count/kill shape owned by system/engine/pass-timing.cpp.
// Plain result fields are written on the single worker and read only after join().
template <bool Snapshot>
class PassActor final
    : public qb::Actor
    , public qb::ICallback {
    Result *const       _result;
    const std::uint64_t _work;
    std::uint64_t       _completed = 0;
    std::uint64_t       _checksum  = 0;

public:
    PassActor(Result *result, std::uint64_t work) noexcept
        : _result(result)
        , _work(work) {}

    qb::io::async::task<bool>
    onInit() final {
        registerCallback(*this);
        co_return true;
    }

    void
    on(qb::LoopEvent const &loop) final {
        std::uint64_t iteration = loop.iteration;
        if constexpr (Snapshot) {
            auto stats = getCoreStats();
            benchmark::DoNotOptimize(stats);
            iteration = stats.loop_passes;
        }
        _checksum += iteration;
        if (++_completed == _work) {
            _result->completed = _completed;
            _result->checksum  = _checksum;
            _result->stats     = getCoreStats();
            _result->done      = true;
            kill();
        }
    }
};

struct Orphan final : qb::Event {};
struct Done final : qb::Event {};

// Reuses the FIFO Done protocol owned by system/engine/dead-letters.cpp.
class DeadLetterActor final : public qb::Actor {
    Result *const       _result;
    const std::uint64_t _work;

public:
    DeadLetterActor(Result *result, std::uint64_t work) noexcept
        : _result(result)
        , _work(work) {}

    qb::io::async::task<bool>
    onInit() final {
        registerEvent<Done>(*this);
        for (std::uint64_t i = 0; i < _work; ++i)
            push<Orphan>(id()); // alive destination, no handler for this type: unhandled
        push<Done>(id());       // same-core FIFO: every Orphan is processed before this
        co_return true;
    }

    void
    on(Done const &) {
        _result->stats     = getCoreStats();
        _result->completed = _result->stats.events_received; // current Done is not counted yet
        _result->done      = true;
        kill();
    }
};

enum class Family { PassTiming, Snapshot, DeadLetter };

template <Family F, bool Enabled>
void
configure(qb::Main &engine, Result &result, std::uint64_t work) {
    engine.core(0).setAffinity(qb::CoreIdSet{qb::NoAffinity}).setLatency(qb::duration::zero());
    if constexpr (F == Family::DeadLetter) {
        if constexpr (Enabled)
            engine.core(0).setDeadLetterHandler([&result](qb::DeadLetter const &) { ++result.hook_calls; });
        engine.addActor<DeadLetterActor>(0, &result, work);
    } else {
        engine.core(0).setPassTiming(F == Family::PassTiming && Enabled);
        engine.addActor<PassActor<F == Family::Snapshot && Enabled>>(0, &result, work);
    }
}

template <Family F, bool Enabled>
bool
valid(Result const &r, std::uint64_t work, bool engine_error) {
    if (engine_error || !r.done || r.completed != work || r.stats.events_sent != 0u || r.stats.buckets_sent != 0u
        || r.stats.events_dropped != 0u || r.stats.sends_blocked != 0u)
        return false;
    if constexpr (F == Family::DeadLetter) {
        if (r.stats.dead_letters != work || r.stats.events_received != work || r.hook_calls != (Enabled ? work : 0u))
            return false;
        for (std::size_t reason = 0; reason < qb::DeadLetterReasons; ++reason) {
            const auto expected = reason == static_cast<std::size_t>(qb::DeadLetterReason::unhandled) ? work : 0u;
            if (r.stats.dead_letters_by_reason[reason] != expected)
                return false;
        }
        return r.stats.pass_time.count == 0u;
    } else {
        const auto expected_spans = F == Family::PassTiming && Enabled ? work - 1u : 0u;
        return r.checksum == work * (work + 1u) / 2u && r.stats.loop_passes == work && r.stats.events_received == 0u
               && r.stats.dead_letters == 0u && r.hook_calls == 0u && r.stats.pass_time.count == expected_spans;
    }
}

template <Family F, bool Enabled>
bool
preflight(std::uint64_t work) {
    Result   result;
    qb::Main engine; // destroyed before the result and hook capture
    configure<F, Enabled>(engine, result, work);
    engine.start(true);
    engine.join();
    return valid<F, Enabled>(result, work, engine.hasError());
}

template <Family F, bool Enabled>
void
measure(benchmark::State &state) {
    const auto work = static_cast<std::uint64_t>(state.range(0));
    if (work < 2u || work > static_cast<std::uint64_t>(kWork)) {
        state.SkipWithError("work must be between 2 and 65536");
        return;
    }
    // The two actual configurations must conserve the same work before either is measured.
    if (!preflight<F, false>(work) || !preflight<F, true>(work)) {
        state.SkipWithError("observability pair preflight violated its work/count contract");
        return;
    }
    for (auto _ : state) {
        state.PauseTiming();
        bool okay = false;
        {
            Result   result;
            qb::Main engine;
            configure<F, Enabled>(engine, result, work);
            state.ResumeTiming();
            engine.start(true);
            engine.join();
            state.PauseTiming();
            okay = valid<F, Enabled>(result, work, engine.hasError());
            benchmark::DoNotOptimize(result);
        } // engine, hook and capture teardown outside timing
        state.ResumeTiming();
        if (!okay) {
            state.SkipWithError("measured observability batch violated its work/count contract");
            return;
        }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * work));
    state.counters["work_units_per_s"] = benchmark::Counter(static_cast<double>(work), benchmark::Counter::kIsIterationInvariantRate);
    state.counters["work_units"]       = static_cast<double>(work);
    state.counters["enabled"]          = Enabled ? 1.0 : 0.0;
    state.counters["cores"]            = 1.0;
    state.counters["warn_output"]      = 0.0;
    if constexpr (F == Family::PassTiming) {
        state.counters["timed_spans_at_snapshot"] = Enabled ? static_cast<double>(work - 1u) : 0.0;
        state.SetLabel("unit=callback-pass; window=start+join; final_snapshot=1");
    } else if constexpr (F == Family::Snapshot) {
        state.counters["requested_snapshots"] = Enabled ? static_cast<double>(work) : 0.0;
        state.SetLabel("unit=callback-pass; snapshot_vs_loop_index; final_snapshot=1");
    } else {
        state.counters["dead_letters"] = static_cast<double>(work);
        state.counters["hook_calls"]   = Enabled ? static_cast<double>(work) : 0.0;
        state.SetLabel("unit=unhandled-letter; counting_hook; warn_filtered; done_marker=1");
    }
}

void
BM_CorePassTimingOff(benchmark::State &s) {
    measure<Family::PassTiming, false>(s);
}
void
BM_CorePassTimingOn(benchmark::State &s) {
    measure<Family::PassTiming, true>(s);
}
void
BM_CoreStatsUnrequested(benchmark::State &s) {
    measure<Family::Snapshot, false>(s);
}
void
BM_CoreStatsRequested(benchmark::State &s) {
    measure<Family::Snapshot, true>(s);
}
void
BM_CoreDeadLetterUnset(benchmark::State &s) {
    measure<Family::DeadLetter, false>(s);
}
void
BM_CoreDeadLetterCounting(benchmark::State &s) {
    measure<Family::DeadLetter, true>(s);
}

BENCHMARK(BM_CorePassTimingOff)->Arg(kWork)->ArgNames({"passes"})->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_CorePassTimingOn)->Arg(kWork)->ArgNames({"passes"})->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_CoreStatsUnrequested)->Arg(kWork)->ArgNames({"passes"})->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_CoreStatsRequested)->Arg(kWork)->ArgNames({"passes"})->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_CoreDeadLetterUnset)->Arg(kWork)->ArgNames({"letters"})->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_CoreDeadLetterCounting)->Arg(kWork)->ArgNames({"letters"})->Unit(benchmark::kMicrosecond)->UseRealTime();

} // namespace observability_bench

int
main(int argc, char **argv) {
#if defined(QB_WITH_LOGGING)
    qb::io::log::setLevel(qb::io::log::Level::CRIT);
#elif defined(QB_STDOUT_LOGGING)
    std::cerr << "observability-overhead requires filterable logging or logging disabled; stdout logging cannot be filtered\n";
    return 2;
#endif
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
