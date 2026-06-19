/**
 * @file qb/source/core/tests/shared/BenchmarkIterationSink.h
 * @brief Cross-thread last-run latency stats for Google Benchmark (actor benchmarks)
 *
 * With \c Main::start(true), actors (and their destructors) run on VirtualCore worker
 * threads while the benchmark thread reads counters after \c join(). A \c thread_local sink
 * would store writes on the worker and reads on the benchmark thread in different TLS
 * slots, so stats must live in shared storage protected by a mutex.
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

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <thread>

namespace qb::bench {

constexpr std::uint32_t kMaxBenchmarkCores = 8u;

[[nodiscard]] inline std::uint32_t
effectiveHardwareCores() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0u ? 1u : static_cast<std::uint32_t>(hw);
}

[[nodiscard]] inline std::uint32_t
cappedBenchmarkCores() {
    return std::min(effectiveHardwareCores(), kMaxBenchmarkCores);
}

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

} // namespace qb::bench

#endif // QB_BENCHMARK_ITERATION_SINK_H
