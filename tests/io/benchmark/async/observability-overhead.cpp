/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
 * Licensed under the Apache License, Version 2.0. See LICENSE for details.
 */

/**
 * @file benchmark/async/observability-overhead.cpp
 * @brief Listener dispatch timing, offload snapshot request and coroutine suspension tracking costs, on
 *        identical bounded work.
 *
 * Dispatch: a real active timer watcher is fed once per NOWAIT turn. It is armed far
 * beyond the process watchdog, so only EV_CUSTOM may fire. The current ev_run invokes
 * its dispatcher on entry and at the tail of its one NOWAIT iteration: two invocations
 * per turn, including the empty tail, not one timing span per callback. This fixture
 * has no prepare/fork watcher, nested run or deferred/coroutine work in that family.
 *
 * Offload: 64 sequential real loop->pool->loop jobs in both modes, with the process-wide
 * pool warmed first. Requested snapshots are consumed after each resume; the control
 * consumes the matching local ordinal without that getter. Counters remain enabled in
 * both modes. This measures an end-to-end incremental snapshot request/consumption cost,
 * not the isolated latency of five atomic loads and not a promise to resolve a small delta.
 *
 * Tracking: a root task awaits a yielding child 65536 times -- two awaiters of qb per step (the
 * root's task await, a transfer; the child's yield) and one frame destruction -- with tracking
 * off, with the root spawned under a name (the destructors' gate alone), with tracking on, and
 * with tracking on plus a dump every 64 steps. run_ready() drains the ready queue until it is
 * empty, so the chain runs inside one loop turn; the first child takes the in-flight view after
 * its yield through the public dump, one dump per batch in every mode alike.
 *
 * Setup, fixture resets, common before/after snapshots, verdicts and cleanup are outside
 * timing. Dispatch feed/run and offload pump/deadline checks are timed. Every batch has
 * an independent callback/result checksum and a counter-delta oracle. Paired preflights
 * use the real drivers. No wall-time threshold is a performance correctness oracle.
 */

#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <qb/io/async.h>
#include <qb/io/async/coroutine/cancellation.h>
#include <qb/io/async/coroutine/offload.h>

namespace io_observability_bench {

using Listener = qb::io::async::listener;
using Clock    = std::chrono::steady_clock;
using qb::io::async::cancellation_token;
using qb::io::async::coro_scheduler;
using qb::io::async::CoroutineScheduler;
using qb::io::async::current_offload_stats;
using qb::io::async::null_token;
using qb::io::async::offload;
using qb::io::async::offload_stats;
using qb::io::async::task;
using qb::io::async::yield_or_cancel;
constexpr std::uint64_t kDispatches    = 4096;
constexpr std::uint64_t kJobs          = 64;
constexpr auto          kBudget        = std::chrono::seconds{2};
static bool             cleanup_failed = false;

std::uint64_t
triangular(std::uint64_t n) noexcept {
    return n * (n + 1u) / 2u; // Both rosters are fixed and bounded above.
}

// Construct after any borrowed result, before spawning a coroutine which refers to it.
// An abandoned worker callable owns only its integer argument, never result/loop state.
class AsyncContext {
public:
    AsyncContext() {
        if (cleanup_failed)
            throw std::runtime_error("an earlier async cleanup failed");
        qb::io::async::init(); // Deliberately not treated as a reset.
        Listener::current.set_dispatch_timing(false);
        if (Listener::current.has_work())
            throw std::runtime_error("benchmark requires a quiescent owned listener");
        if (CoroutineScheduler::suspension_tracking())
            throw std::runtime_error("benchmark requires suspension tracking off on entry");
    }
    AsyncContext(AsyncContext const &)            = delete;
    AsyncContext &operator=(AsyncContext const &) = delete;
    ~AsyncContext() {
        try {
            auto &listener = Listener::current;
            listener.set_dispatch_timing(false);
            if (listener.has_coro_scheduler()) {
                (void) coro_scheduler().set_suspension_tracking(false); // Drops the records; the names go with the frames.
                listener.reset_coro_scheduler();                        // Reclaim frames while their borrowed result is alive.
            }
            const auto deadline = Clock::now() + kBudget;
            while (listener.has_work() && Clock::now() < deadline)
                listener.run(EVRUN_NOWAIT); // Drain an abandoned, finite offload completion if necessary.
            if (listener.has_work())
                cleanup_failed = true;
            listener.clear();
        } catch (...) {
            cleanup_failed = true;
        }
    }
};

class DispatchFixture {
    struct ev_loop *_loop;
    ev_timer        _watcher{};
    std::uint64_t   _ordinal = 0;

    static void
    callback(struct ev_loop *, ev_timer *watcher, int events) noexcept {
        auto &self = *static_cast<DispatchFixture *>(watcher->data);
        ++self.callbacks;
        self.checksum += self._ordinal;
        if (events != EV_CUSTOM || self._ordinal != self.callbacks)
            self.order_error = true;
    }

public:
    std::uint64_t callbacks   = 0;
    std::uint64_t checksum    = 0;
    bool          order_error = false;

    DispatchFixture()
        : _loop(static_cast<struct ev_loop *>(Listener::current.loop())) {
        ev_timer_init(&_watcher, &DispatchFixture::callback, 3600.0, 0.0);
        _watcher.data = this;
        ev_now_update(_loop);
        ev_timer_start(_loop, &_watcher);
    }
    DispatchFixture(DispatchFixture const &)            = delete;
    DispatchFixture &operator=(DispatchFixture const &) = delete;
    ~DispatchFixture() {
        ev_timer_stop(_loop, &_watcher);
        (void) ev_clear_pending(_loop, &_watcher);
    }
    void
    reset() noexcept {
        callbacks = checksum = _ordinal = 0;
        order_error                     = false;
    }
    bool
    run(std::uint64_t count) {
        const auto deadline = Clock::now() + kBudget;
        for (std::uint64_t i = 1; i <= count; ++i) {
            _ordinal = i;
            ev_feed_event(_loop, &_watcher, EV_CUSTOM);
            Listener::current.run(EVRUN_NOWAIT);
            if (Clock::now() >= deadline)
                return false;
        }
        return true;
    }
};

template <bool Enabled>
bool
valid_dispatch(DispatchFixture const &fixture, std::uint64_t count, qb::TimingStats const &before, qb::TimingStats const &after,
               bool complete) {
    if (!complete || fixture.callbacks != count || fixture.checksum != triangular(count) || fixture.order_error)
        return false;
    if constexpr (Enabled) {
        // ev.c: ev_run's entry invoke and tail invoke, one NOWAIT iteration. No prepare/fork hooks here.
        return after.count - before.count == 2u * count;
    } else {
        // clear() and disabling timing preserve accumulated stats. recent_max may age, so do not compare it.
        return after.count == before.count && after.total == before.total && after.last == before.last && after.max == before.max;
    }
}

template <bool Enabled>
bool
dispatch_preflight() {
    AsyncContext    context;
    DispatchFixture fixture; // Stopped/detached before context clears the loop.
    Listener::current.set_dispatch_timing(Enabled);
    const auto before   = Listener::current.dispatch_timing();
    const bool complete = fixture.run(kDispatches);
    const auto after    = Listener::current.dispatch_timing();
    return valid_dispatch<Enabled>(fixture, kDispatches, before, after, complete);
}

bool
publish_work(benchmark::State &state, std::uint64_t work) {
    if (state.iterations() > std::numeric_limits<std::int64_t>::max() / static_cast<std::int64_t>(work)) {
        state.SkipWithError("IO benchmark item counter would overflow");
        return false;
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(work));
    state.counters["work_units"] = static_cast<double>(work);
    return true;
}

template <bool Enabled>
void
measure_dispatch(benchmark::State &state) {
    bool paused = false;
    try {
        if (state.range(0) != static_cast<std::int64_t>(kDispatches)) {
            state.SkipWithError("dispatch roster requires 4096 turns");
            return;
        }
        if (!dispatch_preflight<false>() || !dispatch_preflight<true>() || cleanup_failed) {
            state.SkipWithError("dispatch pair preflight failed callbacks, order or timing deltas");
            return;
        }
        AsyncContext    context;
        DispatchFixture fixture;
        Listener::current.set_dispatch_timing(Enabled);
        for (auto _ : state) {
            state.PauseTiming();
            paused = true;
            fixture.reset();
            const auto before = Listener::current.dispatch_timing();
            state.ResumeTiming();
            paused              = false;
            const bool complete = fixture.run(kDispatches);
            state.PauseTiming();
            paused           = true;
            const auto after = Listener::current.dispatch_timing();
            const bool valid = valid_dispatch<Enabled>(fixture, kDispatches, before, after, complete);
            benchmark::DoNotOptimize(fixture.checksum);
            state.ResumeTiming();
            paused = false;
            if (!valid) {
                state.SkipWithError("measured dispatch batch failed callbacks, order or timing deltas");
                return;
            }
        }
        if (!publish_work(state, kDispatches))
            return;
        state.counters["watcher_callbacks"]      = static_cast<double>(kDispatches);
        state.counters["loop_turns"]             = static_cast<double>(kDispatches);
        state.counters["dispatcher_invocations"] = static_cast<double>(2u * kDispatches);
        state.counters["recorded_spans"]         = Enabled ? static_cast<double>(2u * kDispatches) : 0.0;
        state.counters["enabled"]                = Enabled ? 1.0 : 0.0;
        state.SetLabel("raw-fed-timer; feed+NOWAIT; two dispatcher invocations/turn; oracle snapshots untimed");
    } catch (const std::exception &error) {
        if (paused)
            state.ResumeTiming();
        state.SkipWithError(error.what());
    }
}

struct OffloadResult {
    std::uint64_t      completed            = 0;
    std::uint64_t      checksum             = 0;
    std::uint64_t      observation_checksum = 0;
    std::exception_ptr error;
    bool               done = false;
};

// A named-namespace coroutine; only this owner thread writes result. Worker sees value copies only.
template <bool Snapshot>
task<void>
sequential_jobs(OffloadResult *result, std::uint64_t count, std::uint64_t completed_before) {
    try {
        for (std::uint64_t i = 1; i <= count; ++i) {
            const auto value = co_await offload([](std::uint64_t x) noexcept { return x; }, i);
            result->checksum += value;
            ++result->completed;
            auto observed = i;
            if constexpr (Snapshot) {
                auto snapshot = current_offload_stats();
                benchmark::DoNotOptimize(snapshot);
                observed = snapshot.completed - completed_before;
            }
            result->observation_checksum += observed;
        }
    } catch (...) {
        result->error = std::current_exception();
    }
    result->done = true; // No suspension or borrowed-state access follows this write.
}

bool
pump_offloads(OffloadResult const &result) {
    const auto deadline = Clock::now() + kBudget;
    while (!result.done && Clock::now() < deadline)
        Listener::current.run(EVRUN_NOWAIT);
    return result.done;
}

bool
valid_offload(OffloadResult const &result, std::uint64_t count, offload_stats const &before, offload_stats const &after, bool complete) {
    return complete && result.done && !result.error && result.completed == count && result.checksum == triangular(count)
           && result.observation_checksum == triangular(count) && after.threads == 1u && after.queued == 0u
           && after.submitted - before.submitted == count && after.completed - before.completed == count && after.discarded == before.discarded;
}

template <bool Snapshot>
bool
offload_preflight(std::uint64_t count) {
    OffloadResult result;
    AsyncContext  context; // Reclaims the frame before result dies, on every exit path.
    const auto    before = current_offload_stats();
    coro_scheduler().spawn(sequential_jobs<Snapshot>(&result, count, before.completed));
    const bool complete = pump_offloads(result);
    const auto after    = current_offload_stats();
    return valid_offload(result, count, before, after, complete);
}

template <bool Snapshot>
void
measure_offload(benchmark::State &state) {
    bool paused = false;
    try {
        if (state.range(0) != static_cast<std::int64_t>(kJobs)) {
            state.SkipWithError("offload roster requires 64 sequential jobs");
            return;
        }
        // First job creates the process-wide pool and this thread's completion port outside timing.
        if (!offload_preflight<false>(1) || !offload_preflight<false>(kJobs) || !offload_preflight<true>(kJobs) || cleanup_failed) {
            state.SkipWithError("offload pair preflight failed result, observation or pool deltas");
            return;
        }
        for (auto _ : state) {
            state.PauseTiming();
            paused     = true;
            bool valid = false;
            {
                OffloadResult result;
                AsyncContext  context;
                const auto    before = current_offload_stats();
                coro_scheduler().spawn(sequential_jobs<Snapshot>(&result, kJobs, before.completed));
                state.ResumeTiming();
                paused              = false;
                const bool complete = pump_offloads(result);
                state.PauseTiming();
                paused           = true;
                const auto after = current_offload_stats();
                valid            = valid_offload(result, kJobs, before, after, complete);
                benchmark::DoNotOptimize(result);
            } // Frame cleanup and idle-port drain outside timing, while result is still alive.
            state.ResumeTiming();
            paused = false;
            if (!valid || cleanup_failed) {
                state.SkipWithError("measured offload batch failed result, observation or pool deltas");
                return;
            }
        }
        if (!publish_work(state, kJobs))
            return;
        state.counters["offloads_completed"]  = static_cast<double>(kJobs);
        state.counters["pool_threads"]        = 1.0;
        state.counters["requested_snapshots"] = Snapshot ? static_cast<double>(kJobs) : 0.0;
        state.counters["oracle_snapshots"]    = 2.0;
        state.SetLabel("sequential offload roundtrips; pool warm; snapshot-request+consume delta; counters always on");
    } catch (const std::exception &error) {
        if (paused)
            state.ResumeTiming();
        state.SkipWithError(error.what());
    }
}

// ---- coroutine suspension tracking (coroutine/tracking.h): off, named, on, dumped ------------------
//
// Every awaiter of qb calls track_suspension() or track_then() first thing in await_suspend, and every
// task frame's destruction calls track_frame_destroyed(): with nothing tracked or named anywhere, each
// is one load of a global and one branch. The chain below pays both on every step -- the root's task
// await (a transfer), the child's yield to the ready queue, the child frame's destruction -- in four
// states of the same thread:
//   Off     tracking off, root unnamed: the shape every program pays;
//   Named   tracking off, root spawned under a name: the destructors' gate is open, so every child
//           destruction takes the cold path (an erase miss in the names table);
//   On      tracking on: each suspension records its kind and CPU stamp, each destruction erases it;
//   Dumped  tracking on, and the root reads the public dump every kDumpEvery steps, as a diagnostic
//           would (a running coroutine sees its own last suspension: one entry, allocated and sorted).
// run_ready() drains the ready queue until it is empty, so the whole chain runs inside ONE loop turn
// and no pump can observe it in flight: the first child takes that view after its yield, through the
// public dump, while the root awaits it -- with tracking on exactly the root (kind "task") and itself
// (kind "yield"); off, the root alone with nothing recorded, carrying its name when it has one. That is
// one dump per batch in every mode alike. The scheduler's tracking flag is read after the run and
// before the context turns it off, outside timing.
constexpr std::uint64_t    kSteps     = 65536;
constexpr std::uint64_t    kDumpEvery = 64;
constexpr std::string_view kRootName  = "tracking-chain";

enum class Tracking { Off, Named, On, Dumped };

constexpr bool
records(Tracking mode) noexcept {
    return mode == Tracking::On || mode == Tracking::Dumped;
}

struct TrackingResult {
    std::uint64_t      completed        = 0;
    std::uint64_t      checksum         = 0;
    std::uint64_t      dumps            = 0;
    std::uint64_t      dump_entries     = 0;
    std::uint64_t      seen_entries     = 0; ///< the first child's in-flight view: every entry
    std::uint64_t      seen_task_roots  = 0; ///< ... the root, recorded awaiting a task
    std::uint64_t      seen_yields      = 0; ///< ... the child itself, recorded on its yield
    std::uint64_t      seen_blank_roots = 0; ///< ... the root listed with nothing recorded
    std::uint64_t      seen_named       = 0; ///< ... entries carrying kRootName
    std::exception_ptr error;
    bool               done = false;
};

// One yield to the ready queue (kind "yield"), then the ordinal. The first child takes the in-flight view.
task<std::uint64_t>
tracked_child(TrackingResult *result, std::uint64_t ordinal, bool observe) {
    co_await yield_or_cancel(cancellation_token{null_token});
    if (observe) {
        for (auto const &parked : coro_scheduler().dump()) {
            ++result->seen_entries;
            if (parked.root && parked.kind && std::string_view{parked.kind} == "task")
                ++result->seen_task_roots;
            else if (!parked.root && parked.kind && std::string_view{parked.kind} == "yield")
                ++result->seen_yields;
            else if (parked.root && !parked.kind)
                ++result->seen_blank_roots;
            if (parked.name == kRootName)
                ++result->seen_named;
        }
    }
    co_return ordinal;
}

// A named-namespace coroutine; only this owner thread writes result.
task<void>
tracking_chain(TrackingResult *result, std::uint64_t steps, bool dumped) {
    try {
        for (std::uint64_t i = 1; i <= steps; ++i) {
            const auto value = co_await tracked_child(result, i, i == 1u);
            result->checksum += value;
            ++result->completed;
            if (dumped && i % kDumpEvery == 0u) {
                const auto parked = coro_scheduler().dump(); // the root's own last suspension: one entry
                benchmark::DoNotOptimize(parked.size());
                ++result->dumps;
                result->dump_entries += parked.size();
            }
        }
    } catch (...) {
        result->error = std::current_exception();
    }
    result->done = true; // No suspension or borrowed-state access follows this write.
}

template <Tracking Mode>
void
spawn_chain(TrackingResult *result) {
    if constexpr (Mode == Tracking::Named)
        coro_scheduler().spawn(kRootName, tracking_chain(result, kSteps, false));
    else
        coro_scheduler().spawn(tracking_chain(result, kSteps, Mode == Tracking::Dumped));
}

bool
pump_chain(TrackingResult const &result) {
    const auto deadline = Clock::now() + kBudget;
    while (!result.done && Clock::now() < deadline)
        Listener::current.run(EVRUN_NOWAIT);
    return result.done;
}

bool
valid_tracking(Tracking mode, TrackingResult const &result, std::uint64_t steps, bool complete, bool tracking_after) {
    const bool          on    = records(mode);
    const std::uint64_t dumps = mode == Tracking::Dumped ? steps / kDumpEvery : 0u;
    return complete && result.done && !result.error && result.completed == steps && result.checksum == triangular(steps) && tracking_after == on
           && result.seen_entries == (on ? 2u : 1u) && result.seen_task_roots == (on ? 1u : 0u) && result.seen_yields == (on ? 1u : 0u)
           && result.seen_blank_roots == (on ? 0u : 1u) && result.seen_named == (mode == Tracking::Named ? 1u : 0u) && result.dumps == dumps
           && result.dump_entries == dumps;
}

template <Tracking Mode>
bool
tracking_preflight() {
    TrackingResult result;
    AsyncContext   context; // Reclaims the frames and turns tracking off before result dies, on every exit path.
    if (!coro_scheduler().set_suspension_tracking(records(Mode)))
        return false;
    spawn_chain<Mode>(&result);
    const bool complete = pump_chain(result);
    return valid_tracking(Mode, result, kSteps, complete, CoroutineScheduler::suspension_tracking());
}

template <Tracking Mode>
void
measure_tracking(benchmark::State &state) {
    bool paused = false;
    try {
        if (state.range(0) != static_cast<std::int64_t>(kSteps)) {
            state.SkipWithError("tracking roster requires 65536 chain steps");
            return;
        }
        if (!tracking_preflight<Tracking::Off>() || !tracking_preflight<Tracking::Named>() || !tracking_preflight<Tracking::On>()
            || !tracking_preflight<Tracking::Dumped>() || cleanup_failed) {
            state.SkipWithError("tracking preflight failed records, names, count or checksum");
            return;
        }
        for (auto _ : state) {
            state.PauseTiming();
            paused     = true;
            bool valid = false;
            {
                TrackingResult result;
                AsyncContext   context;
                const bool     armed = coro_scheduler().set_suspension_tracking(records(Mode));
                spawn_chain<Mode>(&result);
                state.ResumeTiming();
                paused              = false;
                const bool complete = pump_chain(result);
                state.PauseTiming();
                paused = true;
                valid  = armed && valid_tracking(Mode, result, kSteps, complete, CoroutineScheduler::suspension_tracking());
                benchmark::DoNotOptimize(result);
            } // Frame reclaim, name erasure and tracking off outside timing, while result is still alive.
            state.ResumeTiming();
            paused = false;
            if (!valid || cleanup_failed) {
                state.SkipWithError("measured tracking batch failed records, names, count or checksum");
                return;
            }
        }
        if (!publish_work(state, kSteps))
            return;
        state.counters["steps"]                = static_cast<double>(kSteps);
        state.counters["recorded_suspensions"] = records(Mode) ? static_cast<double>(2u * kSteps) : 0.0;
        state.counters["named_roots"]          = Mode == Tracking::Named ? 1.0 : 0.0;
        state.counters["dumps"]                = Mode == Tracking::Dumped ? static_cast<double>(kSteps / kDumpEvery) : 0.0;
        state.counters["tracking"]             = records(Mode) ? 1.0 : 0.0;
        state.SetLabel("root awaits a yielding child per step; two qb awaiters and one frame destruction per step; one in-flight dump per "
                       "batch in every mode");
    } catch (const std::exception &error) {
        if (paused)
            state.ResumeTiming();
        state.SkipWithError(error.what());
    }
}

void
BM_IODispatchTimingOff(benchmark::State &state) {
    measure_dispatch<false>(state);
}
void
BM_IODispatchTimingOn(benchmark::State &state) {
    measure_dispatch<true>(state);
}
void
BM_IOOffloadStatsUnrequested(benchmark::State &state) {
    measure_offload<false>(state);
}
void
BM_IOOffloadStatsRequested(benchmark::State &state) {
    measure_offload<true>(state);
}
void
BM_CoroTrackingOff(benchmark::State &state) {
    measure_tracking<Tracking::Off>(state);
}
void
BM_CoroTrackingNamed(benchmark::State &state) {
    measure_tracking<Tracking::Named>(state);
}
void
BM_CoroTrackingOn(benchmark::State &state) {
    measure_tracking<Tracking::On>(state);
}
void
BM_CoroTrackingDumped(benchmark::State &state) {
    measure_tracking<Tracking::Dumped>(state);
}

BENCHMARK(BM_IODispatchTimingOff)->Arg(kDispatches)->ArgNames({"dispatches"})->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_IODispatchTimingOn)->Arg(kDispatches)->ArgNames({"dispatches"})->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_IOOffloadStatsUnrequested)->Arg(kJobs)->ArgNames({"jobs"})->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_IOOffloadStatsRequested)->Arg(kJobs)->ArgNames({"jobs"})->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CoroTrackingOff)->Arg(kSteps)->ArgNames({"steps"})->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CoroTrackingNamed)->Arg(kSteps)->ArgNames({"steps"})->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CoroTrackingOn)->Arg(kSteps)->ArgNames({"steps"})->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CoroTrackingDumped)->Arg(kSteps)->ArgNames({"steps"})->UseRealTime()->Unit(benchmark::kMicrosecond);

} // namespace io_observability_bench

int
main(int argc, char **argv) {
    if (!qb::io::async::set_offload_threads(1)) {
        std::cerr << "IO observability requires setting its one-thread pool before first use\n";
        return 2;
    }
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    if (io_observability_bench::cleanup_failed) {
        std::cerr << "IO observability could not quiesce its async cleanup\n";
        return 2;
    }
    return 0;
}
