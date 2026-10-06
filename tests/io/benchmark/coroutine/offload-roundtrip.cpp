/**
 * @file qb/io/tests/benchmark/coroutine/offload-roundtrip.cpp
 * @brief What an `offload` costs: one round trip loop -> pool thread -> loop, and many at once (Huly QB-69).
 *
 * The call is trivial (it returns its argument), so every cell measures the hop and nothing else: the job's one
 * allocation, the pool's queue and condition variable, the pool thread's wake, the completion port's mutex and
 * `ev_async` send, the loop noticing it, and the coroutine's resume. Three shapes:
 *
 *  - SpinningLoop: one coroutine awaits `kBatch` offloads one after the other while this thread pumps
 *    `run(EVRUN_NOWAIT)` without pause -- the loop of a `VirtualCore` at latency 0;
 *  - ParkedLoop: the same, but the loop PARKS between two (`run_once_for`, a 100 ms cap) and only the pool's send
 *    wakes it -- the loop of a `VirtualCore` with a latency, and the shape a missed wake would show as 100 ms;
 *  - Burst: `kBurst` coroutines each await one offload at once, through the default two pool threads -- throughput.
 *
 * The figures are per round trip (`per_offload`, seconds, inverted rate). A pool thread's wake from its condition
 * variable is the dominant term and is the operating system's, so compare hosts, not builds.
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
#include <qb/io/async.h>

namespace {

using namespace qb::io::async;
using namespace std::chrono_literals;

constexpr int kBatch = 64;  ///< sequential round trips per benchmark iteration
constexpr int kBurst = 256; ///< concurrent offloads per benchmark iteration

task<void>
sequential(int n, std::atomic<bool> *done) {
    for (int i = 0; i < n; ++i)
        benchmark::DoNotOptimize(co_await offload([](int x) { return x; }, i));
    done->store(true, std::memory_order_relaxed);
}

task<void>
one(std::atomic<int> *left) {
    benchmark::DoNotOptimize(co_await offload([] { return 1; }));
    left->fetch_sub(1, std::memory_order_relaxed);
}

void
per_offload(benchmark::State &state, int per_iteration) {
    state.counters["per_offload"] =
        benchmark::Counter(static_cast<double>(per_iteration), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

void
BM_Offload_RoundTrip_SpinningLoop(benchmark::State &state) {
    listener::current.clear();
    for (auto _ : state) {
        std::atomic<bool> done{false};
        coro_scheduler().spawn(sequential(kBatch, &done));
        while (!done.load(std::memory_order_relaxed))
            listener::current.run(EVRUN_NOWAIT);
    }
    per_offload(state, kBatch);
}

void
BM_Offload_RoundTrip_ParkedLoop(benchmark::State &state) {
    listener::current.clear();
    listener::current.arm_wake();
    for (auto _ : state) {
        std::atomic<bool> done{false};
        coro_scheduler().spawn(sequential(kBatch, &done));
        while (!done.load(std::memory_order_relaxed))
            (void) listener::current.run_once_for(100ms); // parks until the pool's send ends the park
    }
    listener::current.disarm_wake();
    per_offload(state, kBatch);
}

void
BM_Offload_Burst(benchmark::State &state) {
    listener::current.clear();
    for (auto _ : state) {
        std::atomic<int> left{kBurst};
        for (int i = 0; i < kBurst; ++i)
            coro_scheduler().spawn(one(&left));
        while (left.load(std::memory_order_relaxed) != 0)
            listener::current.run(EVRUN_NOWAIT);
    }
    per_offload(state, kBurst);
}

} // namespace

BENCHMARK(BM_Offload_RoundTrip_SpinningLoop)->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Offload_RoundTrip_ParkedLoop)->Unit(benchmark::kMicrosecond)->UseRealTime();
BENCHMARK(BM_Offload_Burst)->Unit(benchmark::kMicrosecond)->UseRealTime();

BENCHMARK_MAIN();
