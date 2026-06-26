/**
 * @file qb/io/tests/benchmark/coroutine/coroutine-scope.cpp
 * @brief Benchmarks for qb-io structured-concurrency fan-out primitives.
 *
 * Two fan-out shapes that dominate real coroutine workloads:
 *   - `parallel_map(items, fn, max)` + the implicit `join_all()` — bounded
 *     concurrency over a range, the worker-pool pattern;
 *   - `make_shared_task<T>()` awaited by N consumers — a single computation
 *     whose result is shared by many waiters (no recomputation).
 *
 * Both run on the single-thread cooperative scheduler, so throughput here is the
 * per-task spawn + schedule + resume cost as N grows. Frameworks live under
 * qb/io/async/coroutine/{scope,shared_task}.h.
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
#include <numeric>
#include <vector>

#include <qb/io/async/coroutine.h>

namespace {

using namespace qb::io::async;
using namespace std::chrono_literals;

void
reset_async_context() {
    qb::io::async::listener::current.clear();
    qb::io::async::init();
}

// Drive the coro scheduler + libev loop until `pred()` holds or the deadline
// elapses (the bench-side mirror of the test pump; a broken bench cannot wedge
// the run).
template <typename Predicate>
void
drain_until(Predicate &&pred, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        coro_scheduler().run_ready();
        qb::io::async::run_for(1ms);
    }
}

// A trivial async map function: double the value with no suspension. Keeps the
// benchmark on the spawn/schedule/join machinery rather than on a sleep timer.
task<std::uint64_t>
double_value(std::uint64_t v) {
    co_return v * 2u;
}

// The shared computation: a cheap value that all N waiters observe.
task<std::uint64_t>
compute_shared(std::uint64_t seed) {
    co_return seed * seed + 7u;
}

// One waiter on the shared_task: awaits the handle and accumulates the result.
task<void>
shared_waiter(shared_task<std::uint64_t> sh, std::atomic<std::uint64_t> *sink, std::atomic<int> *done) {
    auto v = co_await sh;
    sink->fetch_add(v, std::memory_order_relaxed);
    done->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Coordinator that runs parallel_map and records completion.
task<void>
run_parallel_map(const std::vector<std::uint64_t> *items, std::size_t max_concurrency, std::atomic<std::uint64_t> *sink,
                 std::atomic<bool> *done) {
    auto results = co_await parallel_map(*items, [](std::uint64_t v) { return double_value(v); }, max_concurrency);
    std::uint64_t acc = 0;
    for (auto r : results)
        acc += r;
    sink->store(acc, std::memory_order_relaxed);
    done->store(true, std::memory_order_relaxed);
    co_return;
}

// ---------------------------------------------------------------------------
// parallel_map(N items, fn, max) + join_all fan-out. Per-task throughput is the
// items processed per second as the bounded-concurrency pool drains the range.
// ---------------------------------------------------------------------------
void
BM_Scope_ParallelMap(benchmark::State &state) {
    const auto count           = static_cast<std::size_t>(state.range(0));
    const auto max_concurrency = static_cast<std::size_t>(state.range(1));

    std::vector<std::uint64_t> items(count);
    std::iota(items.begin(), items.end(), std::uint64_t{1});
    const std::uint64_t expected = std::accumulate(items.begin(), items.end(), std::uint64_t{0}) * 2u;

    std::uint64_t last_acc = 0;
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<std::uint64_t> sink{0};
        std::atomic<bool>          done{false};
        state.ResumeTiming();

        coro_scheduler().spawn(run_parallel_map(&items, max_concurrency, &sink, &done));
        drain_until([&done] { return done.load(std::memory_order_relaxed); });

        last_acc = sink.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(last_acc);

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    if (last_acc != expected)
        state.SkipWithError("parallel_map produced an unexpected accumulated result");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

// ---------------------------------------------------------------------------
// make_shared_task with N waiters: one computation, N consumers each co_await
// the same handle. Per-task throughput is waiters resumed per second — the cost
// of the multi-awaiter flush as N grows.
// ---------------------------------------------------------------------------
void
BM_SharedTask_FanOut(benchmark::State &state) {
    const auto waiters = static_cast<std::size_t>(state.range(0));

    std::uint64_t last_sink = 0;
    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<std::uint64_t> sink{0};
        std::atomic<int>           done{0};
        state.ResumeTiming();

        auto sh = make_shared_task(compute_shared(static_cast<std::uint64_t>(waiters)));
        for (std::size_t i = 0; i < waiters; ++i)
            coro_scheduler().spawn(shared_waiter(sh, &sink, &done));

        drain_until([&done, waiters] { return done.load(std::memory_order_relaxed) == static_cast<int>(waiters); });

        last_sink = sink.load(std::memory_order_relaxed);
        benchmark::DoNotOptimize(last_sink);

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    // All N waiters observe the same value; the sum is value * N.
    const std::uint64_t value    = static_cast<std::uint64_t>(waiters) * static_cast<std::uint64_t>(waiters) + 7u;
    const std::uint64_t expected = value * static_cast<std::uint64_t>(waiters);
    if (last_sink != expected)
        state.SkipWithError("shared_task fan-out produced an unexpected sum");

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(waiters));
}

} // namespace

BENCHMARK(BM_Scope_ParallelMap)
    ->Args({64, 8})
    ->Args({256, 16})
    ->Args({1024, 32})
    ->ArgNames({"items", "max_concurrency"})
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();
BENCHMARK(BM_SharedTask_FanOut)
    ->Arg(16)
    ->Arg(128)
    ->Arg(512)
    ->ArgName("waiters")
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

BENCHMARK_MAIN();
