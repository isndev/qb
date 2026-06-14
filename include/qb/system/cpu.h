/**
 * @file qb/system/cpu.h
 * @brief CPU information and utilities
 *
 * This file provides platform-independent access to CPU information such as
 * architecture, core count, and frequency. It also includes utilities for
 * CPU-specific operations like spinlock pauses.
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
 * @ingroup System
 */

#ifndef QB_SYSTEM_CPU_H
#define QB_SYSTEM_CPU_H

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

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
[[nodiscard]] auto resource(T handle, TCleaner cleaner) {
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
[[nodiscard]] auto resource(void* handle, TCleaner cleaner) {
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
    F _fn;
    bool _active;
public:
    explicit scope_guard(F fn) noexcept(std::is_nothrow_move_constructible_v<F>)
        : _fn(std::move(fn)), _active(true) {}
    ~scope_guard() { if (_active) _fn(); }

    scope_guard(scope_guard&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
        : _fn(std::move(other._fn)), _active(other._active) { other.dismiss(); }

    void dismiss() noexcept { _active = false; }

    scope_guard(const scope_guard&) = delete;
    scope_guard& operator=(const scope_guard&) = delete;
    scope_guard& operator=(scope_guard&&) = delete;
};

template <typename F>
scope_guard(F) -> scope_guard<F>;

/**
 * @class CPU
 * @brief Platform-independent CPU information utilities
 */
class CPU {
public:
    CPU() = delete;
    CPU(const CPU&) = delete;
    CPU(CPU&&) noexcept = delete;
    ~CPU() = delete;

    CPU& operator=(const CPU&) = delete;
    CPU& operator=(CPU&&) noexcept = delete;

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
     * @brief Returns the number of physical CPU cores
     */
    [[nodiscard]] static int PhysicalCores();

    /**
     * @brief Returns both logical and physical core counts
     *
     * @return pair(logical, physical)
     */
    [[nodiscard]] static std::pair<int, int> TotalCores();

    /**
     * @brief Returns CPU clock speed in Hz, or -1 if unavailable
     */
    [[nodiscard]] static std::int64_t ClockSpeed();

    /**
     * @brief Returns true when logical cores differ from physical cores
     */
    [[nodiscard]] static bool HyperThreading();
};

} // namespace qb

#if defined(__SSE2__)
#include <emmintrin.h>
namespace qb {
inline void spin_loop_pause() noexcept {
    _mm_pause();
}
} // namespace qb

#elif defined(_MSC_VER) && _MSC_VER >= 1800 && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
namespace qb {
inline void spin_loop_pause() noexcept {
    _mm_pause();
}
} // namespace qb

#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
#include <intrin.h> // __dmb / _ARM64_BARRIER_SY intrinsics
#endif
namespace qb {
inline void spin_loop_pause() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("yield" ::: "memory");
#elif defined(_MSC_VER)
    __dmb(_ARM64_BARRIER_SY);
#endif
}
} // namespace qb

#elif defined(__arm__)
namespace qb {
inline void spin_loop_pause() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("yield" ::: "memory");
#endif
}
} // namespace qb

#else
namespace qb {
inline void spin_loop_pause() noexcept {
    std::this_thread::yield();
}
} // namespace qb
#endif

#endif // QB_SYSTEM_CPU_H
