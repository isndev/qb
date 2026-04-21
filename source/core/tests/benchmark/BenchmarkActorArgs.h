/**
 * @file qb/core/tests/benchmark/BenchmarkActorArgs.h
 * @brief Shared Google Benchmark argument generators for actor throughput benches
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
 * @ingroup Core
 */

#ifndef QB_BENCHMARK_ACTOR_ARGS_H
#define QB_BENCHMARK_ACTOR_ARGS_H

#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>

#include "../shared/BenchmarkIterationSink.h"

namespace qb::bench {

/**
 * Core id for multicast / pipeline consumer \p consumer_index when \p nb_core is the arg
 * spread divisor. Producer stays on core \c 0; consumers cycle \c 1 .. nb_core-1 so the
 * producer core is not reused when \p nb_core > 1. If \p nb_core <= 1, all actors share
 * core \c 0.
 */
[[nodiscard]] inline std::uint32_t
multicast_consumer_core_for_index(std::size_t const consumer_index,
                                  std::uint32_t const nb_core) {
    if (nb_core <= 1u)
        return 0u;
    const std::uint32_t span = nb_core - 1u;
    return 1u + static_cast<std::uint32_t>(consumer_index % span);
}

/**
 * Registers (NB_EVENTS, NB_ACTORS, NB_CORE) tuples for pipeline / multicast latency benches.
 *
 * \b Critical: the inner loop must never start at \c j == 0 with \c j *= 10 as the step,
 * because \c 0 * 10 stays \c 0 and Google Benchmark registration hangs forever.
 */
inline void
apply_pipeline_multicast_args(::benchmark::internal::Benchmark *b) {
    const auto nb_core = cappedBenchmarkCores();
    const int  max_j   = static_cast<int>(nb_core) * 10;

    for (auto i = 1u; i <= nb_core; i *= 2) {
        const int j0 = std::max(1, static_cast<int>(i));
        for (int j = j0; j <= max_j; j *= 10) {
            b->Args({1000000, j, static_cast<std::int64_t>(i)});
        }
    }
}

} // namespace qb::bench

#endif // QB_BENCHMARK_ACTOR_ARGS_H
