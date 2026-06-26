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

#include <atomic>
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

// ---------------------------------------------------------------------------
// Pump the libev loop with EVRUN_NOWAIT until `pred()` holds or `max_passes`
// passes are exhausted (deadline guard so a broken bench never wedges the run).
// ---------------------------------------------------------------------------
template <typename Predicate>
void
drain_until(Predicate &&pred, std::size_t max_passes = 1'000'000u) {
    auto &listener = qb::io::async::listener::current;
    for (std::size_t pass = 0; pass < max_passes && !pred(); ++pass)
        listener.run(EVRUN_NOWAIT);
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
// async::callback(fn, 0) fast path: schedule a zero-delay one-shot Timeout and
// let the loop dispatch it. Measures the per-callback create→fire round-trip.
// Each iteration is one full dispatch so timers/sec == items/sec.
// ---------------------------------------------------------------------------
void
BM_Callback_ImmediateDispatch(benchmark::State &state) {
    qb::io::async::init();
    std::atomic<std::size_t> fired{0};

    for (auto _ : state) {
        qb::io::async::callback([&fired] { fired.fetch_add(1, std::memory_order_relaxed); }, qb::duration::zero());
        drain_until([&fired, n = fired.load(std::memory_order_relaxed)] { return fired.load(std::memory_order_relaxed) > n; });
    }

    const auto total = fired.load(std::memory_order_relaxed);
    qb::io::async::listener::current.clear();

    // One out-of-loop correctness assert: a broken bench (callback never fires)
    // is caught here rather than silently reporting bogus throughput.
    if (total != static_cast<std::size_t>(state.iterations()))
        state.SkipWithError("async::callback dispatch count mismatch");

    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// Bulk dispatch: arm `count` zero-delay callbacks, then drain the whole batch.
// Amortizes the per-pass loop overhead across many timers — the IntensiveAsync
// shape (thousands of timers armed at once). Reports timers/sec.
// ---------------------------------------------------------------------------
void
BM_Callback_BulkDispatch(benchmark::State &state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    qb::io::async::init();
    std::atomic<std::size_t> fired{0};

    for (auto _ : state) {
        const auto base = fired.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < count; ++i)
            qb::io::async::callback([&fired] { fired.fetch_add(1, std::memory_order_relaxed); }, qb::duration::zero());
        drain_until([&] { return fired.load(std::memory_order_relaxed) >= base + count; });
    }

    const auto total = fired.load(std::memory_order_relaxed);
    qb::io::async::listener::current.clear();

    if (total != static_cast<std::size_t>(state.iterations()) * count)
        state.SkipWithError("bulk callback dispatch count mismatch");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

// ---------------------------------------------------------------------------
// scoped_callback: owned one-shot timer construct + immediate-fire (0 delay) +
// destroy. The caller holds the unique_ptr, so there is no self-delete dance —
// this measures the RAII-owned timer hot path (per-connection deadlines).
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

BENCHMARK(BM_Callback_ImmediateDispatch)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_Callback_BulkDispatch)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->ArgName("timers")
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();
BENCHMARK(BM_ScopedCallback_ConstructFireDestroy)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_WithTimeout_Rearm)->Unit(benchmark::kNanosecond)->UseRealTime();

BENCHMARK_MAIN();
