/**
 * @file qb/io/tests/benchmark/async/timer-dispatch.cpp
 * @brief Benchmarks for the qb-io async timer / callback dispatch machinery.
 *
 * The async loop's one-shot timer path (`async::Timeout<F>` behind
 * `async::callback`), the owned `async::scoped_callback`, and the activity-reset
 * `with_timeout<>` rearm are the hot scheduling primitives behind every session
 * deadline, retry watchdog, and deferred dispatch in the framework. These
 * benchmarks isolate the create→fire round-trip and the rearm cost from any
 * socket I/O, driving a real (socket-free) libev loop via `listener::current`.
 *
 * Four kinds of case, each under its own name (Huly QB-300):
 *   - INLINE: `callback(fn, d <= 0)` calls `fn` before it returns and never
 *     reaches the loop (`qb/io/async/io.h`); `BM_Callback_ZeroDelayInline` and
 *     the zero-delay `scoped_callback` time that path -- controls, not costs;
 *   - TIMER DISPATCH: `callback(fn, d > 0)` arms a `Timeout` that the loop fires
 *     on a later pass; `BM_Callback_TimerDispatch` measures arm -> pass -> fire;
 *   - BULK: `BM_Callback_BulkTimerDispatch` arms a batch, then drains it;
 *   - REARM ONLY: `BM_WithTimeout_Rearm`, a deadline pushed forward, never fired.
 * Each case asserts, outside its measurement, the path it claims: a dispatch
 * case counts the loop passes between arm and fire and refuses a fire that
 * needed none; an inline case refuses a callback that registered a watcher.
 *
 * Seeded from the demoted perf smoke tests in the former test-async-io
 * (IntensiveAsyncOperations / ManyConcurrentTimers, ~4000 timers) — now
 * relocated to system/async/timer-stress.cpp as correctness cases. Throughput
 * (timers/sec) and dispatch latency belong here, not in a ctest gate.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup IO
 */

#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <qb/io/async.h>
#include <qb/io/async/event/timer.h>
#include <qb/io/async/listener.h>

using namespace std::chrono_literals;

namespace {

// The smallest positive delay. It takes `callback`'s timer path -- a `Timeout` armed on the
// loop, fired by the first pass whose clock reading lies strictly past its deadline -- where
// any non-positive delay calls the function inline, before `callback` returns, and never
// reaches the loop at all (`qb/io/async/io.h`). The counters below are plain: every callback
// runs on this thread, from inside `listener::run()`, an opaque call for the optimiser.
constexpr qb::duration kSmallestDelay{1};

// ---------------------------------------------------------------------------
// Pump the libev loop with EVRUN_NOWAIT until `pred()` holds or `max_passes`
// passes are exhausted (deadline guard so a broken bench never wedges the run),
// and return the number of passes that ran: 0 when `pred()` already held. That
// count is the dispatch cases' witness -- a callback the loop fired needed at
// least one pass; one that fired inline, before the drain, needed none.
// ---------------------------------------------------------------------------
template <typename Predicate>
std::size_t
drain_until(Predicate &&pred, std::size_t max_passes = 1'000'000u) {
    auto       &listener = qb::io::async::listener::current;
    std::size_t passes   = 0;
    while (passes < max_passes && !pred()) {
        listener.run(EVRUN_NOWAIT);
        ++passes;
    }
    return passes;
}

// A with_timeout handler whose deadline we keep rearming via updateTimeout().
// Its on(event::timer) is reached only when the deadline truly elapses; the
// rearm bench never lets it fire — it measures the updateTimeout() cost.
class RearmTimer : public qb::io::async::with_timeout<RearmTimer> {
public:
    std::size_t fires = 0;

    explicit RearmTimer(qb::duration timeout)
        : with_timeout(timeout) {}

    void
    on(qb::io::async::event::timer const &) noexcept {
        ++fires;
    }
};

// ---------------------------------------------------------------------------
// async::callback(fn, 0): the CONTROL of the zero-delay path. A non-positive
// delay calls `fn` inline, before `callback` returns -- no `Timeout`, no
// watcher, no pass -- so it times the inline path (g++ -O3 folds the call into
// the counter: a path control, not a cost figure), never a dispatch. The
// loop is not run here at all: every fire counted is an inline one, and a
// callback that registered a watcher would leave the listener's count moved.
// ---------------------------------------------------------------------------
void
BM_Callback_ZeroDelayInline(benchmark::State &state) {
    qb::io::async::init();
    auto             &listener   = qb::io::async::listener::current;
    const std::size_t registered = listener.size();
    std::size_t       fired      = 0;

    for (auto _ : state) {
        qb::io::async::callback([&fired] { ++fired; }, qb::duration::zero());
        benchmark::DoNotOptimize(fired);
    }

    const bool armed_a_watcher = listener.size() != registered;
    listener.clear();

    // Out of the measurement: each iteration fired once, with no pass run and no watcher left.
    if (fired != static_cast<std::size_t>(state.iterations()))
        state.SkipWithError("a zero-delay callback did not fire inline, before callback() returned");
    else if (armed_a_watcher)
        state.SkipWithError("a zero-delay callback registered a watcher: it no longer runs inline");

    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// async::callback(fn, d > 0): one timer armed, then the loop pumped until it
// fires. Measures the real create -> pass -> fire round-trip (the `Timeout`
// from its freelist, its watcher started on a refreshed clock, the pass that
// fires it, the self-delete). Each iteration is one dispatch, so timers/sec ==
// items/sec, and `passes_per_fire` says how many passes the loop took.
// ---------------------------------------------------------------------------
void
BM_Callback_TimerDispatch(benchmark::State &state) {
    qb::io::async::init();
    std::size_t fired  = 0;
    std::size_t passes = 0;

    for (auto _ : state) {
        const std::size_t before = fired; // captured BEFORE arming: the fire must come later
        qb::io::async::callback([&fired] { ++fired; }, kSmallestDelay);
        const std::size_t ran = drain_until([&fired, before] { return fired != before; });
        passes += ran;
        if (ran == 0 || fired == before) [[unlikely]] {
            state.SkipWithError(ran == 0 ? "a positive-delay callback fired inline, before any loop pass"
                                         : "a positive-delay callback never fired within the pass budget");
            break;
        }
    }

    const auto total = fired;
    qb::io::async::listener::current.clear();

    // Out of the measurement: one fire per iteration, every one of them through at least one pass.
    if (total != static_cast<std::size_t>(state.iterations()) || passes < total)
        state.SkipWithError("timer dispatch count mismatch");

    state.counters["passes_per_fire"] = benchmark::Counter(static_cast<double>(passes), benchmark::Counter::kAvgIterations);
    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// Bulk dispatch: arm `count` positive-delay callbacks, then drain the whole
// batch. Amortizes the per-pass loop overhead across many timers — the
// IntensiveAsync shape (thousands of timers armed at once). Nothing may fire
// while the batch is armed; `passes_per_batch` says how many passes fired it.
// Reports timers/sec.
// ---------------------------------------------------------------------------
void
BM_Callback_BulkTimerDispatch(benchmark::State &state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    qb::io::async::init();
    std::size_t fired  = 0;
    std::size_t passes = 0;

    for (auto _ : state) {
        const std::size_t base = fired;
        for (std::size_t i = 0; i < count; ++i)
            qb::io::async::callback([&fired] { ++fired; }, kSmallestDelay);
        const bool none_inline = fired == base;
        passes += drain_until([&] { return fired - base >= count; });
        if (!none_inline || fired - base != count) [[unlikely]] {
            state.SkipWithError(!none_inline ? "a positive-delay callback fired while its batch was being armed"
                                             : "the batch did not fire exactly once per callback within the pass budget");
            break;
        }
    }

    const auto total = fired;
    qb::io::async::listener::current.clear();

    if (total != static_cast<std::size_t>(state.iterations()) * count || passes < static_cast<std::size_t>(state.iterations()))
        state.SkipWithError("bulk timer dispatch count mismatch");

    state.counters["passes_per_batch"] = benchmark::Counter(static_cast<double>(passes), benchmark::Counter::kAvgIterations);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

// ---------------------------------------------------------------------------
// scoped_callback: owned one-shot timer construct + immediate-fire (0 delay) +
// destroy. The caller holds the unique_ptr, so there is no self-delete dance —
// this measures the RAII-owned timer hot path (per-connection deadlines). An
// INLINE case like BM_Callback_ZeroDelayInline: the watcher is registered and
// unregistered, never started, and the call happens inside the constructor.
// ---------------------------------------------------------------------------
void
BM_ScopedCallback_ConstructFireDestroy(benchmark::State &state) {
    qb::io::async::init();
    std::size_t fired = 0;

    for (auto _ : state) {
        auto handle = qb::io::async::scoped_callback([&fired] { ++fired; }, qb::duration::zero());
        benchmark::DoNotOptimize(handle);
        // 0-delay scoped_callback fires inline at construction (callback() semantics).
    }

    qb::io::async::listener::current.clear();

    if (fired != static_cast<std::size_t>(state.iterations()))
        state.SkipWithError("scoped_callback inline-fire count mismatch");

    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// with_timeout rearm: a single live handler whose deadline is repeatedly pushed
// forward via updateTimeout() (the activity-reset every session does on each
// read). The timer is armed far out so it never fires; we measure only the
// rearm bookkeeping (ev_now_update + last_activity stamp).
// ---------------------------------------------------------------------------
void
BM_WithTimeout_Rearm(benchmark::State &state) {
    qb::io::async::init();
    RearmTimer timer{60s};

    for (auto _ : state) {
        timer.updateTimeout();
        benchmark::DoNotOptimize(timer.getTimeout());
    }

    benchmark::DoNotOptimize(timer.fires);
    qb::io::async::listener::current.clear();

    // The handler must never have fired during the rearm loop.
    if (timer.fires != 0)
        state.SkipWithError("with_timeout rearm should not let the handler fire");

    state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(BM_Callback_ZeroDelayInline)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_Callback_TimerDispatch)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_Callback_BulkTimerDispatch)->Arg(64)->Arg(1024)->Arg(4096)->ArgName("timers")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_ScopedCallback_ConstructFireDestroy)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_WithTimeout_Rearm)->Unit(benchmark::kNanosecond)->UseRealTime();

BENCHMARK_MAIN();
