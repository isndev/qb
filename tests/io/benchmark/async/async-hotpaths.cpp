/**
 * @file qb/io/tests/benchmark/async/async-hotpaths.cpp
 * @brief Benchmarks for the qb-io async event-loop hot paths.
 *
 * These benchmarks isolate the allocation-heavy async paths that gate every
 * higher-level transport: listener event registration/unregistration (single
 * and batched), immediate (zero-delay) callbacks, delayed (non-zero-delay)
 * callbacks driven to completion through the loop, and scoped (RAII) callbacks.
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
#include <vector>

#include <qb/io/async.h>
#include <qb/io/async/event/io.h>
#include <qb/io/async/event/timer.h>
#include <qb/io/async/listener.h>

using namespace std::chrono_literals;

namespace {

struct NopActor {
    void
    on(qb::io::async::event::io const &) noexcept {}
    void
    on(qb::io::async::event::timer const &) noexcept {}
};

void
BM_Listener_RegisterUnregister(benchmark::State &state) {
    qb::io::async::init();
    NopActor actor;
    auto    &listener = qb::io::async::listener::current;

    for (auto _ : state) {
        auto &ev = listener.registerEvent<qb::io::async::event::io>(actor, -1, EV_NONE);
        benchmark::DoNotOptimize(ev._interface);
        listener.unregisterEvent(ev._interface);
    }

    listener.clear();
    state.SetItemsProcessed(state.iterations());
}

// Batched registration: register N watchers, then unregister all N. Exercises
// the listener's per-event slot allocator under churn (the pattern a server
// hits when a burst of connections registers their io/timer watchers in one
// loop tick), rather than the steady-state single register/unregister above.
void
BM_Listener_RegisterUnregister_Batch(benchmark::State &state) {
    qb::io::async::init();
    NopActor   actor;
    auto      &listener = qb::io::async::listener::current;
    const auto count    = static_cast<std::size_t>(state.range(0));

    std::vector<qb::io::async::IRegisteredKernelEvent *> handles;
    handles.reserve(count);

    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            auto &ev = listener.registerEvent<qb::io::async::event::io>(actor, -1, EV_NONE);
            handles.push_back(ev._interface);
        }
        benchmark::DoNotOptimize(handles.data());
        for (auto *h : handles)
            listener.unregisterEvent(h);
        handles.clear();
    }

    listener.clear();
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void
BM_AsyncCallback_ImmediateFire(benchmark::State &state) {
    qb::io::async::init();
    std::atomic<std::size_t> counter{0};
    auto                    &listener = qb::io::async::listener::current;

    for (auto _ : state) {
        qb::io::async::callback([&counter] { ++counter; }, qb::duration::zero());
        listener.run(EVRUN_NOWAIT);
    }

    benchmark::DoNotOptimize(counter.load(std::memory_order_relaxed));
    listener.clear();
    state.SetItemsProcessed(state.iterations());
}

void
BM_ScopedCallback_ConstructCancel(benchmark::State &state) {
    qb::io::async::init();

    for (auto _ : state) {
        auto handle = qb::io::async::scoped_callback([] {}, 1s);
        benchmark::DoNotOptimize(handle);
    }

    qb::io::async::listener::current.clear();
    state.SetItemsProcessed(state.iterations());
}

// Non-zero-delay callback driven to completion through the event loop. Unlike
// BM_AsyncCallback_ImmediateFire (which takes the inline d<=0 fast path and never
// arms a libev timer), this schedules a real `Timeout<>` watcher and pumps the
// loop until it fires — exercising the timer arm + qev_now_update + dispatch path.
// This is wall-clock bound by `delay_us`, so it must run with UseRealTime().
void
BM_AsyncCallback_DelayedFire(benchmark::State &state) {
    qb::io::async::init();
    const auto delay    = std::chrono::microseconds(state.range(0));
    auto      &listener = qb::io::async::listener::current;

    std::atomic<std::size_t> counter{0};
    std::size_t              expected = 0;
    for (auto _ : state) {
        const auto before = counter.load(std::memory_order_relaxed);
        qb::io::async::callback([&counter] { counter.fetch_add(1, std::memory_order_relaxed); }, delay);
        // Pump the loop until the timer fires (guarded so a broken timer can't
        // hang the harness forever).
        long guard = 0;
        while (counter.load(std::memory_order_relaxed) == before && ++guard < 4000000L)
            listener.run(EVRUN_NOWAIT);
        ++expected;
    }

    // Out-of-loop correctness assert: every scheduled callback must have fired,
    // otherwise the benchmark is measuring a no-op.
    if (counter.load(std::memory_order_relaxed) != expected)
        state.SkipWithError("delayed callback did not fire for every iteration");

    benchmark::DoNotOptimize(counter);
    listener.clear();
    state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(BM_Listener_RegisterUnregister)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_Listener_RegisterUnregister_Batch)->Arg(16)->Arg(256)->Arg(4096)->ArgName("watchers")->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_AsyncCallback_ImmediateFire)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_AsyncCallback_DelayedFire)->Arg(100)->Arg(1000)->ArgName("delay_us")->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_ScopedCallback_ConstructCancel)->Unit(benchmark::kNanosecond)->UseRealTime();

BENCHMARK_MAIN();
