/**
 * @file qb/source/core/tests/shared/BenchmarkActorArgs.h
 * @brief Shared Google Benchmark argument generators for actor throughput benches
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

#ifndef QB_BENCHMARK_ACTOR_ARGS_H
#define QB_BENCHMARK_ACTOR_ARGS_H

#include <algorithm>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>

#include "BenchmarkCores.h"

namespace qb::bench {

/**
 * Core id for multicast / pipeline consumer \p consumer_index when \p nb_core is the arg
 * spread divisor. Producer stays on core \c 0; consumers cycle \c 1 .. nb_core-1 so the
 * producer core is not reused when \p nb_core > 1. If \p nb_core <= 1, all actors share
 * core \c 0.
 */
[[nodiscard]] inline std::uint32_t
multicast_consumer_core_for_index(std::size_t const consumer_index, std::uint32_t const nb_core) {
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

/**
 * Registers (NB_EVENTS, NB_ACTORS, NB_CORE) tuples for the pipeline-CHAIN latency bench.
 *
 * The chain bench is strictly LATENCY-bound: its `ProducerActor` keeps exactly one event in
 * flight (the producer's `_idList` has a single entry — the chain head — so it re-injects the
 * next event only after the previous one has traversed the entire chain back). Wall time is
 * therefore ≈ NB_EVENTS × NB_ACTORS × per-hop latency, and cross-core hops cost ~1.5 µs each
 * (vs ~70 ns same-core). Reusing the multicast 1M-event grid makes the deep cross-core cells
 * (e.g. 80 actors / 8 cores ⇒ 80M serial hops) take minutes — they cannot finish in a CI budget.
 *
 * So we sweep the SAME (NB_ACTORS, NB_CORE) shape as multicast (for comparability) but bound the
 * per-cell work by total deliveries: NB_EVENTS = clamp(kDeliveryBudget / NB_ACTORS, kMin, kMax).
 * A latency mean is stable well below 1M samples, so kMin keeps shallow chains meaningful while
 * the budget keeps deep ones runnable (heaviest cell ≈ a few seconds). The chain producer refills
 * every `_idList.size()`==1 events, so any NB_EVENTS satisfies the `samples == NB_EVENTS` hop guard
 * (no divisibility constraint, unlike the fan-out multicast bench).
 */
inline void
apply_pipeline_chain_args(::benchmark::internal::Benchmark *b) {
    constexpr std::int64_t kDeliveryBudget = 1'500'000; // bounded hops per cell
    constexpr std::int64_t kMinSamples     = 20'000;    // enough for a stable latency mean
    constexpr std::int64_t kMaxSamples     = 400'000;

    const auto nb_core = cappedBenchmarkCores();
    const int  max_j   = static_cast<int>(nb_core) * 10;

    for (auto i = 1u; i <= nb_core; i *= 2) {
        const int j0 = std::max(1, static_cast<int>(i));
        for (int j = j0; j <= max_j; j *= 10) {
            const std::int64_t nb_events = std::clamp(kDeliveryBudget / static_cast<std::int64_t>(j), kMinSamples, kMaxSamples);
            b->Args({nb_events, j, static_cast<std::int64_t>(i)});
        }
    }
}

} // namespace qb::bench

#endif // QB_BENCHMARK_ACTOR_ARGS_H
