/**
 * @file qb/source/core/tests/shared/BenchmarkCores.h
 * @brief Core-count helpers for Google Benchmark actor/throughput benches
 *
 * Throughput benches only need to know how many VirtualCore workers to spread actors
 * across; they do NOT need the cross-thread latency sink (mutex + global) that lives in
 * BenchmarkIterationSink.h. This header carries just the core-count helpers so a pure
 * throughput bench can include it without dragging in the latency-sink machinery.
 *
 * \c effectiveHardwareCores() is the number of CPUs this process may RUN on, at least 1:
 * \c std::thread::hardware_concurrency() (the machine's online CPUs), lowered on Linux to the
 * size of the affinity mask (a cgroup cpuset, \c taskset) and to the ceiling of a cgroup v2
 * \c cpu.max quota. \c cappedBenchmarkCores() clamps that to \c kMaxBenchmarkCores so
 * registration / placement stays bounded on big machines.
 *
 * Why the CPUs the process may run on and not the machine's: a VirtualCore at the default
 * latency never parks, so an engine with more cores than CPUs measures the OS scheduler. A CI
 * container whose cpuset held 6 of its host's 20 CPUs was given eight-core cells; a chain hop
 * then waited for the next time slice of a preempted core (Huly QB-1080).
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
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#endif

namespace qb::bench {

constexpr std::uint32_t kMaxBenchmarkCores = 8u;

namespace detail {

/// CPUs in the calling thread's affinity mask, where a cgroup cpuset and `taskset` both show;
/// 0 when unknown or not Linux (macOS has no such mask, Windows is not read here).
[[nodiscard]] inline std::uint32_t
affinityCpuCount() noexcept {
#if defined(__linux__) && defined(CPU_COUNT)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0)
            return static_cast<std::uint32_t>(n);
    }
#endif
    return 0u;
}

/// ceil(quota / period) of this process's cgroup v2 `cpu.max` (`docker --cpus`, systemd
/// `CPUQuota=`); 0 when unlimited, unreadable or not Linux. Ancestor cgroups are not read.
[[nodiscard]] inline std::uint32_t
cgroupQuotaCpuCount() {
#if defined(__linux__)
    std::ifstream self("/proc/self/cgroup");
    std::string   line;
    std::string   path;
    bool          unified = false;
    while (std::getline(self, line)) {
        if (line.starts_with("0::")) {
            path    = line.substr(3);
            unified = true;
            break;
        }
    }
    if (!unified)
        return 0u;
    if (path == "/")
        path.clear();
    std::ifstream limit("/sys/fs/cgroup" + path + "/cpu.max");
    std::string   quota;
    std::string   period;
    if (!(limit >> quota >> period) || quota == "max")
        return 0u;
    const long long q = std::strtoll(quota.c_str(), nullptr, 10);
    const long long p = std::strtoll(period.c_str(), nullptr, 10);
    if (q <= 0 || p <= 0)
        return 0u;
    return static_cast<std::uint32_t>((q + p - 1) / p);
#else
    return 0u;
#endif
}

[[nodiscard]] inline std::uint32_t
usableCpuCount() {
    std::uint32_t n = std::thread::hardware_concurrency();
    if (n == 0u)
        n = 1u;
    if (const auto affinity = affinityCpuCount(); affinity != 0u)
        n = std::min(n, affinity);
    if (const auto quota = cgroupQuotaCpuCount(); quota != 0u)
        n = std::min(n, quota);
    return n;
}

/// Read ONCE, during static initialisation, before any engine runs: `Main::start(false)` makes
/// the calling thread the last VirtualCore, which pins it to that core's CPU and leaves it
/// there, so the benchmark thread's mask read after one run would say one CPU.
inline const std::uint32_t kUsableCpusAtStartup = usableCpuCount();

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
