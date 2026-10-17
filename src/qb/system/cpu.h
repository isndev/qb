/**
 * @file qb/system/cpu.h
 * @brief CPU information and utilities
 *
 * This file provides platform-independent access to CPU information such as
 * architecture, core count, and frequency. It also includes utilities for
 * CPU-specific operations like spinlock pauses.
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
 * @ingroup System
 */

#ifndef QB_SYSTEM_CPU_H
#define QB_SYSTEM_CPU_H

#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace qb {

/**
 * @brief Creates a unique_ptr with a custom deleter for a resource
 *
 * @tparam T Type of the resource handle
 * @tparam TCleaner Type of the deleter
 * @param handle Resource handle
 * @param cleaner Deleter function/object
 * @return unique_ptr managing the resource
 */
template <typename T, typename TCleaner>
[[nodiscard]] auto
resource(T handle, TCleaner cleaner) {
    return std::unique_ptr<std::remove_pointer_t<T>, TCleaner>(handle, std::move(cleaner));
}

/**
 * @brief Creates a unique_ptr with a custom deleter for a void* resource
 *
 * @tparam TCleaner Type of the deleter
 * @param handle Resource handle
 * @param cleaner Deleter function/object
 * @return unique_ptr managing the resource
 */
template <typename TCleaner>
[[nodiscard]] auto
resource(void *handle, TCleaner cleaner) {
    return std::unique_ptr<void, TCleaner>(handle, std::move(cleaner));
}

/**
 * @brief RAII scope guard that invokes a callable on destruction
 *
 * Replaces the previous dangling-pointer overload of resource().
 * Use as: auto guard = qb::scope_guard([]{ cleanup(); });
 *
 * @tparam F Callable type (lambda, function pointer, etc.)
 */
template <typename F>
class [[nodiscard]] scope_guard {
    F    _fn;
    bool _active;

public:
    explicit scope_guard(F fn) noexcept(std::is_nothrow_move_constructible_v<F>)
        : _fn(std::move(fn))
        , _active(true) {}
    ~scope_guard() {
        if (_active)
            _fn();
    }

    scope_guard(scope_guard &&other) noexcept(std::is_nothrow_move_constructible_v<F>)
        : _fn(std::move(other._fn))
        , _active(other._active) {
        other.dismiss();
    }

    void
    dismiss() noexcept {
        _active = false;
    }

    scope_guard(const scope_guard &)            = delete;
    scope_guard &operator=(const scope_guard &) = delete;
    scope_guard &operator=(scope_guard &&)      = delete;
};

template <typename F>
scope_guard(F) -> scope_guard<F>;

/**
 * @class CPU
 * @brief Platform-independent CPU information utilities
 */
class CPU {
public:
    CPU()                = delete;
    CPU(const CPU &)     = delete;
    CPU(CPU &&) noexcept = delete;
    ~CPU()               = delete;

    CPU &operator=(const CPU &)     = delete;
    CPU &operator=(CPU &&) noexcept = delete;

    /**
     * @brief Returns the CPU architecture / brand string
     */
    [[nodiscard]] static std::string Architecture();

    /**
     * @brief Returns the number of logical processors available
     */
    [[nodiscard]] static int Affinity();

    /**
     * @brief Returns the number of logical CPU cores
     */
    [[nodiscard]] static int LogicalCores();

    /**
     * @brief Returns the number of physical CPU cores, or -1 when the platform does not tell
     * @details Windows: `GetLogicalProcessorInformation` cores; macOS: `hw.physicalcpu`; Linux: the distinct
     *          core sibling sets of the online CPUs in sysfs (`qb::detail::linux_physical_cores`). Any other
     *          POSIX system, or a Linux whose sysfs topology cannot be read (some containers), reports -1 --
     *          never the logical count passed off as physical (before 3.3 Linux returned the logical count, so
     *          `HyperThreading()` was false on every SMT host; Huly QB-325).
     */
    [[nodiscard]] static int PhysicalCores();

    /**
     * @brief Returns both logical and physical core counts
     *
     * @return pair(logical, physical); either is -1 when unknown (see `PhysicalCores()`)
     */
    [[nodiscard]] static std::pair<int, int> TotalCores();

    /**
     * @brief Returns CPU clock speed in Hz, or -1 if unavailable
     */
    [[nodiscard]] static std::int64_t ClockSpeed();

    /**
     * @brief Returns true when logical cores differ from physical cores; false when either count is unknown
     */
    [[nodiscard]] static bool HyperThreading();

    /**
     * @brief Reports whether OS-level thread pinning actually takes effect on this host.
     *
     * @return `true` when the platform implements per-thread CPU pinning; `false` when the
     *         kernel refuses the request outright, so `CoreInitializer::setAffinity()` is a
     *         no-op and `VirtualCore` threads are placed wherever the OS scheduler wants.
     *
     * @details
     * Motivating case: **Apple Silicon**. macOS has no `pthread_setaffinity_np`, so qb emulates
     * one with `thread_policy_set(THREAD_AFFINITY_POLICY)`. arm64 macOS does not implement that
     * flavor — every call answers `KERN_NOT_SUPPORTED` (measured: Apple M4 Pro, Darwin 25.6.0,
     * `ret == 46` for every core) — and qb's shim deliberately reports *success* for that code,
     * because returning failure would make `VirtualCore::__init__` warn on every core of every
     * run. Pinning therefore does nothing there, silently. This query is what makes it
     * observable, so a test or a user can branch on it instead of assuming.
     *
     * Determined once per process and cached:
     * - macOS: runtime probe (`thread_policy_get(THREAD_AFFINITY_POLICY)`, side-effect free).
     *   Deliberately NOT a compile-time `#ifdef __aarch64__`: an x86_64 binary under Rosetta 2
     *   runs on an arm64 kernel, so only a runtime probe gives the right answer.
     * - Other POSIX: `true` — `pthread_setaffinity_np()` is implemented.
     * - Windows: `true` — `SetThreadAffinityMask()` is implemented.
     *
     * @note `true` means the *mechanism* exists, not that a particular request will succeed.
     *       Affinity stays best-effort: an out-of-range `CoreId` or a restrictive cgroup can
     *       still fail, and a failed pin never fails `VirtualCore` init.
     * @note Even where macOS does implement the flavor (Intel), `<mach/thread_policy.h>`
     *       describes it as experimental and as a scheduler *hint*: threads sharing an affinity
     *       tag are placed to share an L2 cache. It groups threads by tag; it is not a pin to
     *       CPU N. So `true` on macOS is a weaker guarantee than `true` on Linux or Windows.
     */
    [[nodiscard]] static bool ThreadPinningSupported() noexcept;
};

namespace detail {

/**
 * @brief The number of physical cores a Linux sysfs CPU tree describes, or -1 when the tree cannot be read.
 * @param cpu_root The CPU directory: `/sys/devices/system/cpu` on a live Linux host -- what `CPU::TotalCores()` reads
 *                 there -- or a fixture tree (the function is portable C++: the tests run it on every platform).
 * @details One physical core per distinct core sibling set over the ONLINE CPUs: `cpu<N>/topology/core_cpus_list`,
 *          else the older `thread_siblings_list`, else the `physical_package_id` / `die_id` / `core_id` triple. A
 *          sibling set names one core on every package and die, where `core_id` alone repeats across sockets. The
 *          online CPUs come from `online` (a cpulist: `0-3,8`); without that file, every `cpu<N>` directory counts.
 *          A malformed cpulist, an online CPU with no readable topology, or no CPU at all is -1: unknown, never a guess.
 */
[[nodiscard]] int linux_physical_cores(std::string const &cpu_root);

/**
 * @brief The CPUs a thread started by the calling thread inherits, ascending; empty where the platform has no mask.
 * @details What `Main` places its cores in (Huly QB-1081) and what `usable_cpus()` counts, read when called:
 *          - Linux: the calling thread's own affinity mask -- Linux has no process mask, every thread carries its own
 *            and a thread it creates starts with a copy. A `taskset`, a cgroup cpuset (`docker --cpuset-cpus`, a
 *            Kubernetes CPU manager) and a `sched_setaffinity` of the program's own all show here. Empty on a host
 *            whose mask does not fit a `cpu_set_t` (more than `CPU_SETSIZE`, 1024, CPUs).
 *          - Windows: the process's affinity mask (`GetProcessAffinityMask`), which every new thread starts with --
 *            `start /affinity`, a job object or Task Manager set it, never a thread's own pin. Numbers within the
 *            process's processor group; empty when the process spans several (the call then reports no mask).
 *          - macOS and any other system: empty -- macOS affinity is a scheduling tag, not a mask.
 */
[[nodiscard]] std::vector<unsigned> inherited_cpus();

/**
 * @brief The CPUs a cgroup v2 `cpu.max` quota lets this process run at once: ceil(quota / period) of the tightest
 *        on its cgroup's path, 0 when none limits it or none can be read.
 * @param self_cgroup The process's cgroup membership -- `/proc/self/cgroup` on a live Linux host, a fixture file in
 *                    the tests (the function is portable C++ and runs on every platform).
 * @param cgroup_root Where the unified hierarchy is mounted -- `/sys/fs/cgroup`, or a fixture tree.
 * @details The unified hierarchy's line (`0::<path>`) names the cgroup; every `cpu.max` from it up to the root
 *          (`<quota> <period>`, `max <period>` when unlimited) caps it, so the smallest ceiling wins -- a pod's limit
 *          above its container, a systemd slice's `CPUQuota=` above a service. A fraction rounds up: half a CPU of
 *          quota still runs one thread at a time. A cgroup v1 host (`cpu.cfs_quota_us`, no `0::` line) reports 0.
 */
[[nodiscard]] unsigned cgroup_cpu_limit(std::string const &self_cgroup, std::string const &cgroup_root);

/**
 * @brief How many threads this process can run at once: `std::thread::hardware_concurrency()`, lowered to the size of
 *        `inherited_cpus()` and, on Linux, to `cgroup_cpu_limit("/proc/self/cgroup", "/sys/fs/cgroup")`; at least 1.
 * @details What `Main::start()` compares its cores at latency 0 with (Huly QB-1082) and what the qb-core benchmarks
 *          size their engines to (Huly QB-1080). A Windows job object's CPU rate cap is not read.
 */
[[nodiscard]] unsigned usable_cpus();

/**
 * @brief Where a worker thread is pinned: `place_thread()`'s answer.
 */
struct thread_placement {
    std::vector<unsigned> pin;     ///< The CPUs to pin to, ascending; empty: no pin, the thread keeps the mask it inherited.
    std::vector<unsigned> refused; ///< The requested CPUs outside the inherited mask, left out of `pin`.
};

/**
 * @brief The CPUs a worker thread at @p position is pinned to -- the placement `Main` gives each core at start
 *        (Huly QB-1081), as a pure function of what it read.
 * @param position A VirtualCore's `CoreId`.
 * @param requested The CPUs `setAffinity()` named (real ids only); read only when @p explicit_request.
 * @param explicit_request Whether `setAffinity()` was called for this core.
 * @param inherited `inherited_cpus()`: ascending, empty where the platform has no mask.
 * @details With a mask, a default core takes the CPU of the mask at its position -- the first CPU for core 0, the
 *          second for core 1 -- and a core the mask has no CPU for is not pinned: it runs on the whole mask. An explicit
 *          request keeps the CPUs of the mask it names and refuses the others; with none left nothing is pinned. That
 *          is the rule a cgroup cpuset enforces on Linux and the one Windows enforces for a process's mask (a thread's
 *          must be a subset): a deployment's restriction is never widened by the program it restricts. Without a mask
 *          (macOS), the request as given -- by default CPU @p position -- as before 3.3.
 */
[[nodiscard]] thread_placement place_thread(unsigned position, std::span<unsigned const> requested, bool explicit_request,
                                            std::span<unsigned const> inherited);

/**
 * @brief Pins the calling thread to @p cpus.
 * @return 0, or the error the OS refused it with (`pthread_setaffinity_np`'s result, `GetLastError()`, a
 *         `kern_return_t`).
 * @details Linux: `pthread_setaffinity_np` (CPUs past `CPU_SETSIZE` are left out). MSVC Windows:
 *          `SetThreadAffinityMask`, in the thread's processor group (CPUs past 63 left out). macOS: the first CPU as a
 *          `THREAD_AFFINITY_POLICY` tag -- a hint that groups threads sharing it, not a pin -- and nothing at all on
 *          Apple Silicon, which answers `KERN_NOT_SUPPORTED`, taken as success (see `CPU::ThreadPinningSupported()`).
 *          A Windows GNU build applies nothing. An empty set, or one the platform cannot express, makes no call.
 */
[[nodiscard]] int pin_current_thread(std::span<unsigned const> cpus) noexcept;

/**
 * @brief A thread's affinity as the OS holds it, kept to be put back: `current_thread_affinity()`'s answer.
 */
struct thread_affinity {
    std::bitset<1024> cpus;          ///< Linux: the mask; Windows: the mask within `group`.
    std::uint16_t     group = 0;     ///< Windows: the processor group.
    int               tag   = 0;     ///< macOS: the `THREAD_AFFINITY_POLICY` tag (0, `THREAD_AFFINITY_TAG_NULL`, for none).
    bool              known = false; ///< Whether the OS answered: an unknown affinity is never put back.
};

/**
 * @brief The calling thread's affinity: its mask (Linux), its group and mask (Windows), its tag (macOS).
 */
[[nodiscard]] thread_affinity current_thread_affinity() noexcept;

/**
 * @brief Puts @p affinity back on the calling thread -- what `Main::start(false)` does to its caller (Huly QB-1083).
 * @return 0, or the error the OS refused it with; 0 without a call when @p affinity is not `known`.
 */
int restore_thread_affinity(thread_affinity const &affinity) noexcept;

} // namespace detail

} // namespace qb

#if defined(__SSE2__)
#include <emmintrin.h>
namespace qb {
inline void
spin_loop_pause() noexcept {
    _mm_pause();
}
} // namespace qb

#elif defined(_MSC_VER) && _MSC_VER >= 1800 && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
namespace qb {
inline void
spin_loop_pause() noexcept {
    _mm_pause();
}
} // namespace qb

#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
#include <intrin.h> // __dmb / _ARM64_BARRIER_SY intrinsics
#endif
namespace qb {
inline void
spin_loop_pause() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("yield" ::: "memory");
#elif defined(_MSC_VER)
    __dmb(_ARM64_BARRIER_SY);
#endif
}
} // namespace qb

#elif defined(__arm__)
namespace qb {
inline void
spin_loop_pause() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("yield" ::: "memory");
#endif
}
} // namespace qb

#else
namespace qb {
inline void
spin_loop_pause() noexcept {
    std::this_thread::yield();
}
} // namespace qb
#endif

#endif // QB_SYSTEM_CPU_H
