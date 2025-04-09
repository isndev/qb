/**
 * @file qb/io/system/sys__utils.h
 * @brief System utilities and high-precision clock functions
 *
 * This file provides utility functions related to time measurement and
 * performance tracking, particularly high-precision clocks. It also offers
 * helper functions for value manipulation and object management.
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
 */

#ifndef QB_IO_UTILS_H
#define QB_IO_UTILS_H
#include <algorithm>
#include <assert.h>
#include <chrono>
#include <qb/utility/build_macros.h>

namespace qb {
/**
 * @brief Type for representing high-precision time values (nanoseconds)
 */
typedef long long highp_time_t;

/**
 * @brief High-resolution clock for performance measurements
 *
 * This clock is used to measure time intervals with high precision,
 * particularly for benchmarking and performance measurement.
 */
typedef std::chrono::high_resolution_clock steady_clock_t;

/**
 * @brief System clock that can be adjusted
 *
 * This clock represents the system time, which can be modified by the user
 * or synchronized with an NTP server.
 */
typedef std::chrono::system_clock system_clock_t;

/**
 * @brief Gets a timestamp in nanoseconds since epoch
 *
 * This function returns a high-precision timestamp in nanoseconds
 * since the epoch (January 1, 1970).
 *
 * @tparam _Ty Clock type to use (default: steady_clock_t)
 * @return Timestamp in nanoseconds
 */
template <typename _Ty = steady_clock_t>
inline highp_time_t
xhighp_clock() {
    auto duration = _Ty::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

/**
 * @brief Gets a timestamp in microseconds since epoch
 *
 * This function returns a high-precision timestamp in microseconds
 * since the epoch (January 1, 1970).
 *
 * @tparam _Ty Clock type to use (default: steady_clock_t)
 * @return Timestamp in microseconds
 */
template <typename _Ty = steady_clock_t>
inline highp_time_t
highp_clock() {
    return xhighp_clock<_Ty>() / std::milli::den;
}

/**
 * @brief Gets a timestamp in milliseconds since epoch
 *
 * This function returns a normal-precision timestamp in milliseconds
 * since the epoch (January 1, 1970).
 *
 * @tparam _Ty Clock type to use (default: steady_clock_t)
 * @return Timestamp in milliseconds
 */
template <typename _Ty = steady_clock_t>
inline highp_time_t
clock() {
    return xhighp_clock<_Ty>() / std::micro::den;
}

/**
 * @brief Gets the current time in seconds since epoch
 *
 * This function offers better performance than chrono on Win32.
 * See: win10 sdk ucrt/time/time.cpp:common_time
 * https://docs.microsoft.com/en-us/windows/desktop/sysinfo/acquiring-high-resolution-time-stamps
 *
 * @return Timestamp in seconds
 */
inline highp_time_t
time_now() {
    return ::time(nullptr);
}

#if QB__HAS_CXX17
using std::clamp;
#else
/**
 * @brief Constrains a value between a lower and upper bound
 *
 * This function is a compatibility implementation of std::clamp for
 * versions prior to C++17.
 *
 * @tparam _Ty Type of the value and bounds
 * @param v Value to constrain
 * @param lo Lower bound
 * @param hi Upper bound
 * @return The value constrained between lo and hi
 */
template <typename _Ty>
const _Ty &
clamp(const _Ty &v, const _Ty &lo, const _Ty &hi) {
    assert(!(hi < lo));
    return v < lo ? lo : hi < v ? hi : v;
}
#endif

/**
 * @brief Explicitly invokes the destructor of an object without freeing its memory
 *
 * This function is useful for manual memory management, particularly
 * when objects are allocated with placement new.
 *
 * @tparam _Ty Type of the object
 * @param p Pointer to the object whose destructor should be called
 */
template <typename _Ty>
inline void
invoke_dtor(_Ty *p) {
    p->~_Ty();
}
} // namespace qb

#endif