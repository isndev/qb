/**
 * @file qb/io/tests/benchmark/bm-io-plan.cpp
 * @brief Benchmarks for async IO hot paths tracked by QB_IO_PLAN.
 *
 * These benchmarks keep the former IO micro-benchmarks under the same Google
 * Benchmark and CMake conventions as qb-core benchmarks. They focus on
 * allocation-heavy async paths: listener event registration, immediate
 * callbacks, scoped callbacks, and broadcast scratch reuse.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
#include <memory>
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

class DummySession {
public:
    explicit DummySession(int id = 0) noexcept
        : _id(id) {}

    [[nodiscard]] int
    id() const noexcept {
        return _id;
    }

private:
    int _id;
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

void
BM_BroadcastScratch_Reuse(benchmark::State &state) {
    const auto sessions = static_cast<std::size_t>(state.range(0));

    std::vector<std::unique_ptr<DummySession>> owned;
    owned.reserve(sessions);
    for (std::size_t i = 0; i < sessions; ++i)
        owned.push_back(std::make_unique<DummySession>(static_cast<int>(i)));

    std::vector<DummySession *> pool;
    pool.reserve(sessions);
    for (auto &session : owned)
        pool.push_back(session.get());

    std::vector<DummySession *> scratch;
    scratch.reserve(sessions);

    std::size_t touched = 0;
    for (auto _ : state) {
        scratch.clear();
        for (auto *session : pool)
            scratch.push_back(session);

        for (auto *session : scratch)
            touched += static_cast<std::size_t>(session->id() & 1);

        benchmark::DoNotOptimize(touched);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(sessions));
}

} // namespace

BENCHMARK(BM_Listener_RegisterUnregister)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_AsyncCallback_ImmediateFire)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_ScopedCallback_ConstructCancel)->Unit(benchmark::kNanosecond)->UseRealTime();
BENCHMARK(BM_BroadcastScratch_Reuse)
    ->Args({16})
    ->Args({256})
    ->Args({4096})
    ->ArgNames({"sessions"})
    ->Unit(benchmark::kNanosecond)
    ->UseRealTime();

BENCHMARK_MAIN();
