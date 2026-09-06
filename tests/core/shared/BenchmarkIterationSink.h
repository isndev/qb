/**
 * @file qb/source/core/tests/shared/BenchmarkIterationSink.h
 * @brief Cross-thread last-run latency stats for Google Benchmark (actor benchmarks)
 *
 * With \c Main::start(true), actors (and their destructors) run on VirtualCore worker
 * threads while the benchmark thread reads counters after \c join(). A \c thread_local sink
 * would store writes on the worker and reads on the benchmark thread in different TLS
 * slots, so stats must live in shared storage protected by a mutex.
 *
 * The core-count helpers (\c cappedBenchmarkCores / \c effectiveHardwareCores) moved to
 * BenchmarkCores.h so pure throughput benches can size their core spread without dragging
 * in this latency-sink mutex/global; this header re-exports them for back-compat.
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
 * @ingroup Core
 */

#ifndef QB_BENCHMARK_ITERATION_SINK_H
#define QB_BENCHMARK_ITERATION_SINK_H

#include <cstdint>
#include <mutex>
#include <benchmark/benchmark.h>

#include "BenchmarkCores.h"

namespace qb::bench {

struct LastLatencyStats {
    double        mean_round_trip_ns = 0.;
    std::uint64_t samples            = 0;
};

inline std::mutex       g_last_latency_mutex;
inline LastLatencyStats g_last_latency_stats;

inline void
reset_last_latency_stats() {
    const std::scoped_lock lock(g_last_latency_mutex);
    g_last_latency_stats = {};
}

inline void
record_last_latency(double mean_ns, std::uint64_t samples) {
    const std::scoped_lock lock(g_last_latency_mutex);
    g_last_latency_stats.mean_round_trip_ns = mean_ns;
    g_last_latency_stats.samples            = samples;
}

[[nodiscard]] inline LastLatencyStats
last_latency_stats_snapshot() {
    const std::scoped_lock lock(g_last_latency_mutex);
    return g_last_latency_stats;
}

/**
 * @brief Publish the run's mean round-trip as the `mean_rtt_ns` counter, once per iteration.
 * @details ACCUMULATES under `kAvgIterations`, so the reported figure is the mean of the
 *          per-iteration means. Every latency harness used to assign a fresh
 *          `Counter(mean, kAvgIterations)` each iteration: that overwrote the value with the
 *          LAST iteration's mean and then had Google Benchmark divide it by the iteration
 *          count — a reported 59 ns for a measured 1337 ns `qb::ask` round trip at 23
 *          iterations, only visible once someone ran `--benchmark_min_time=1x`.
 */
inline void
record_mean_rtt_counter(benchmark::State &state, LastLatencyStats const &lat) {
    if (!lat.samples)
        return;
    auto &c = state.counters["mean_rtt_ns"];
    c.flags = benchmark::Counter::kAvgIterations;
    c.value += lat.mean_round_trip_ns;
}

} // namespace qb::bench

#endif // QB_BENCHMARK_ITERATION_SINK_H
