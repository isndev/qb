/**
 * @file qb/io/system/sys__utils.h
 * @brief System utilities including high-precision clock functions and generic helpers.
 *
 * This file provides utility functions related to time measurement and
 * performance tracking, particularly high-precision clocks. It also offers
 * helper functions for value manipulation (like `clamp`) and object management (`invoke_dtor`).
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

#ifndef QB_IO_UTILS_H
#define QB_IO_UTILS_H
#include <algorithm>
#include <assert.h>
#include <chrono>
#include <qb/utility/build_macros.h>

namespace qb {
/**
 * @typedef highp_time_t
 * @ingroup Time
 * @brief Type for representing high-precision time values, typically in nanoseconds.
 *        Defined as `long long`.
 */
typedef long long highp_time_t;

/**
 * @typedef steady_clock_t
 * @ingroup Time
 * @brief Alias for `std::chrono::high_resolution_clock`.
 * @details This clock is used to measure time intervals with high precision, suitable for
 *          benchmarking and performance measurement. It is generally monotonic.
 */
typedef std::chrono::high_resolution_clock steady_clock_t;

/**
 * @typedef system_clock_t
 * @ingroup Time
 * @brief Alias for `std::chrono::system_clock`.
 * @details This clock represents the system-wide real time wall clock. It may be adjusted
 *          (e.g., by the user or NTP synchronization) and is not guaranteed to be monotonic.
 */
typedef std::chrono::system_clock system_clock_t;

/**
 * @brief Gets a timestamp in nanoseconds since epoch from a specified clock.
 * @ingroup Time
 * @tparam _Ty Clock type to use (e.g., `steady_clock_t`, `system_clock_t`). Defaults to `steady_clock_t`.
 * @return Timestamp in nanoseconds as `highp_time_t` (`long long`).
 * @details This function returns a high-precision timestamp by converting the duration
 *          since the clock's epoch to nanoseconds.
 */
template <typename _Ty = steady_clock_t>
inline highp_time_t
xhighp_clock() {
    auto duration = _Ty::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

/**
 * @brief Gets a timestamp in microseconds since epoch from a specified clock.
 * @ingroup Time
 * @tparam _Ty Clock type to use. Defaults to `steady_clock_t`.
 * @return Timestamp in microseconds as `highp_time_t`.
 * @details This is a convenience wrapper around `xhighp_clock`, dividing by 1000.
 */
template <typename _Ty = steady_clock_t>
inline highp_time_t
highp_clock() {
    return xhighp_clock<_Ty>() / std::milli::den;
}

/**
 * @brief Gets a timestamp in milliseconds since epoch from a specified clock.
 * @ingroup Time
 * @tparam _Ty Clock type to use. Defaults to `steady_clock_t`.
 * @return Timestamp in milliseconds as `highp_time_t`.
 * @details This is a convenience wrapper around `xhighp_clock`, dividing by 1,000,000.
 */
template <typename _Ty = steady_clock_t>
inline highp_time_t
clock() {
    return xhighp_clock<_Ty>() / std::micro::den;
}

/**
 * @brief Gets the current calendar time in seconds since epoch (00:00:00 UTC, January 1, 1970).
 * @ingroup Time
 * @return Timestamp in seconds as `highp_time_t` (effectively `time_t` cast to `long long`).
 * @details This function calls `::time(nullptr)`. It may offer better performance than
 *          chrono-based equivalents on some platforms for second-precision wall time.
 * @note Subject to system clock adjustments.
 */
inline highp_time_t
time_now() {
    return ::time(nullptr);
}

#if QB__HAS_CXX17
using std::clamp;
#else
/**
 * @brief Constrains a value to be within a specified range [lo, hi].
 * @ingroup MiscUtils
 * @tparam _Ty Type of the value and bounds. Must support comparison operators.
 * @param v The value to constrain.
 * @param lo The lower bound of the range.
 * @param hi The upper bound of the range.
 * @return The value `v` clamped to the range [`lo`, `hi`]. If `v < lo`, returns `lo`.
 *         If `v > hi`, returns `hi`. Otherwise, returns `v`.
 * @note This is a compatibility implementation of `std::clamp` for C++ versions prior to C++17.
 *       Asserts that `!(hi < lo)`.
 */
template <typename _Ty>
const _Ty &
clamp(const _Ty &v, const _Ty &lo, const _Ty &hi) {
    assert(!(hi < lo));
    return v < lo ? lo : hi < v ? hi : v;
}
#endif

/**
 * @brief Explicitly invokes the destructor of an object without deallocating its memory.
 * @ingroup MiscUtils
 * @tparam _Ty Type of the object.
 * @param p Pointer to the object whose destructor should be called.
 * @details This function is typically used in advanced scenarios involving manual memory management,
 *          such as when objects are constructed using placement new in a custom memory buffer.
 *          Misuse can lead to undefined behavior (e.g., double destruction).
 */
template <typename _Ty>
inline void
invoke_dtor(_Ty *p) {
    p->~_Ty();
}
} // namespace qb

#endif