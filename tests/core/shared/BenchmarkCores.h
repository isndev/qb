/**
 * @file qb/source/core/tests/shared/BenchmarkCores.h
 * @brief Core-count helpers for Google Benchmark actor/throughput benches
 *
 * Throughput benches only need to know how many VirtualCore workers to spread actors
 * across; they do NOT need the cross-thread latency sink (mutex + global) that lives in
 * BenchmarkIterationSink.h. This header carries just the core-count helpers so a pure
 * throughput bench can include it without dragging in the latency-sink machinery.
 *
 * \c effectiveHardwareCores() reports at least 1 even when
 * \c std::thread::hardware_concurrency() returns 0; \c cappedBenchmarkCores() clamps that
 * to \c kMaxBenchmarkCores so registration / placement stays bounded on big machines.
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

#ifndef QB_BENCHMARK_CORES_H
#define QB_BENCHMARK_CORES_H

#include <algorithm>
#include <cstdint>
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

} // namespace qb::bench

#endif // QB_BENCHMARK_CORES_H
