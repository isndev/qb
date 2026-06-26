/**
 * @file qb/io/tests/benchmark/bm-coroutine.cpp
 * @brief Benchmarks for qb coroutine scheduler and timer workloads.
 *
 * These scenarios replace the former benchmark-style GTest cases. They keep
 * timing-sensitive coroutine spawn, frame, nested await, and timer throughput
 * measurements in Google Benchmark instead of the functional test suite.
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

#include <array>
#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <qb/io/async/coroutine.h>

namespace {

using namespace qb::io::async;
using namespace std::chrono_literals;

void
reset_async_context() {
    qb::io::async::listener::current.clear();
    qb::io::async::init();
}

template <typename Predicate>
void
drain_until(Predicate &&predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        coro_scheduler().run_ready();
        qb::io::async::run_for(1ms);
    }
}

task<void>
complete_immediately(std::atomic<int> *completed) {
    completed->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

task<void>
sleep_and_complete(std::atomic<int> *completed, std::chrono::milliseconds delay) {
    co_await sleep(delay);
    completed->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

task<void>
chain_sleep_and_complete(std::atomic<int> *completed, int chain_length) {
    for (int i = 0; i < chain_length; ++i)
        co_await sleep(1ms);
    completed->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

task<void>
nested_await(std::atomic<int> *completed, int depth) {
    if (depth <= 0) {
        completed->fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    co_await sleep(1ms);
    co_await nested_await(completed, depth - 1);
}

struct LargeFrame {
    std::array<char, 1024> bytes{};
};

task<void>
large_frame_task(std::atomic<int> *completed, LargeFrame frame) {
    co_await sleep(1ms);
    benchmark::DoNotOptimize(frame.bytes.data());
    completed->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

void
BM_Coroutine_SpawnImmediate(benchmark::State &state) {
    const auto count = static_cast<int>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<int> completed{0};
        state.ResumeTiming();

        for (int i = 0; i < count; ++i)
            coro_scheduler().spawn(complete_immediately(&completed));

        drain_until([&completed, count]() { return completed.load() == count; }, 250ms);
        benchmark::DoNotOptimize(completed.load(std::memory_order_relaxed));

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * count);
}

void
BM_Coroutine_LargeFrame(benchmark::State &state) {
    const auto count = static_cast<int>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<int> completed{0};
        state.ResumeTiming();

        for (int i = 0; i < count; ++i) {
            LargeFrame frame{};
            frame.bytes[0] = static_cast<char>(i & 0xff);
            coro_scheduler().spawn(large_frame_task(&completed, frame));
        }

        drain_until([&completed, count]() { return completed.load() == count; }, 500ms);
        benchmark::DoNotOptimize(completed.load(std::memory_order_relaxed));

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * count);
}

void
BM_Coroutine_ConcurrentTimers(benchmark::State &state) {
    const auto count    = static_cast<int>(state.range(0));
    const auto delay_ms = static_cast<int>(state.range(1));
    const auto delay    = std::chrono::milliseconds(delay_ms);

    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<int> completed{0};
        state.ResumeTiming();

        for (int i = 0; i < count; ++i)
            coro_scheduler().spawn(sleep_and_complete(&completed, delay));

        drain_until([&completed, count]() { return completed.load() == count; }, delay + 500ms);
        benchmark::DoNotOptimize(completed.load(std::memory_order_relaxed));

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * count);
}

void
BM_Coroutine_ChainedTimers(benchmark::State &state) {
    const auto chains       = static_cast<int>(state.range(0));
    const auto chain_length = static_cast<int>(state.range(1));

    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<int> completed{0};
        state.ResumeTiming();

        for (int i = 0; i < chains; ++i)
            coro_scheduler().spawn(chain_sleep_and_complete(&completed, chain_length));

        drain_until([&completed, chains]() { return completed.load() == chains; }, std::chrono::milliseconds(chain_length) + 500ms);
        benchmark::DoNotOptimize(completed.load(std::memory_order_relaxed));

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * chains * chain_length);
}

void
BM_Coroutine_NestedAwait(benchmark::State &state) {
    const auto iterations = static_cast<int>(state.range(0));
    const auto depth      = static_cast<int>(state.range(1));

    for (auto _ : state) {
        state.PauseTiming();
        reset_async_context();
        std::atomic<int> completed{0};
        state.ResumeTiming();

        for (int i = 0; i < iterations; ++i)
            coro_scheduler().spawn(nested_await(&completed, depth));

        drain_until([&completed, iterations]() { return completed.load() == iterations; }, std::chrono::milliseconds(depth) + 500ms);
        benchmark::DoNotOptimize(completed.load(std::memory_order_relaxed));

        state.PauseTiming();
        reset_async_context();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * iterations * depth);
}

} // namespace

BENCHMARK(BM_Coroutine_SpawnImmediate)->Args({100})->Args({1000})->Args({5000})->ArgName("coroutines")->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Coroutine_LargeFrame)->Args({64})->Args({256})->ArgName("coroutines")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Coroutine_ConcurrentTimers)
    ->Args({20, 1})
    ->Args({100, 1})
    ->Args({500, 1})
    ->ArgNames({"coroutines", "delay_ms"})
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();
BENCHMARK(BM_Coroutine_ChainedTimers)
    ->Args({10, 5})
    ->Args({50, 5})
    ->ArgNames({"chains", "chain_length"})
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();
BENCHMARK(BM_Coroutine_NestedAwait)
    ->Args({20, 4})
    ->Args({50, 8})
    ->ArgNames({"coroutines", "depth"})
    ->Unit(benchmark::kMicrosecond)
    ->UseRealTime();

BENCHMARK_MAIN();
