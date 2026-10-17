/**
 * @file qb/source/core/tests/shared/BenchmarkCores.h
 * @brief Core-count helpers for Google Benchmark actor/throughput benches
 *
 * Throughput benches only need to know how many VirtualCore workers to spread actors
 * across; they do NOT need the cross-thread latency sink (mutex + global) that lives in
 * BenchmarkIterationSink.h. This header carries just the core-count helpers so a pure
 * throughput bench can include it without dragging in the latency-sink machinery.
 *
 * \c effectiveHardwareCores() is the number of threads this process can run at once, at least 1:
 * \c qb::detail::usable_cpus() (qb/system/cpu.h) -- \c std::thread::hardware_concurrency() lowered to the size of the
 * affinity mask a new thread inherits (a cgroup cpuset, \c taskset; the process mask on Windows) and, on Linux, to the
 * ceiling of the tightest cgroup v2 \c cpu.max quota on the process's path. \c cappedBenchmarkCores() clamps that to
 * \c kMaxBenchmarkCores so registration / placement stays bounded on big machines.
 *
 * Why the CPUs the process may run on and not the machine's: a VirtualCore at the default
 * latency never parks, so an engine with more cores than CPUs measures the OS scheduler. A CI
 * container whose cpuset held 6 of its host's 20 CPUs was given eight-core cells; a chain hop
 * then waited for the next time slice of a preempted core (Huly QB-1080). The count is the one
 * \c Main::start() warns with when an engine has more spinning cores than it (Huly QB-1082): one
 * implementation, the library's, not a copy here.
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
#include <qb/system/cpu.h>

namespace qb::bench {

constexpr std::uint32_t kMaxBenchmarkCores = 8u;

namespace detail {

/// Read ONCE, during static initialisation: every registration and every run of a binary sizes its engines from the
/// same count (a cgroup file read per call could otherwise move it between the two).
inline const std::uint32_t kUsableCpusAtStartup = qb::detail::usable_cpus();

} // namespace detail

[[nodiscard]] inline std::uint32_t
effectiveHardwareCores() {
    return detail::kUsableCpusAtStartup;
}

[[nodiscard]] inline std::uint32_t
cappedBenchmarkCores() {
    return std::min(effectiveHardwareCores(), kMaxBenchmarkCores);
}

} // namespace qb::bench

#endif // QB_BENCHMARK_CORES_H
