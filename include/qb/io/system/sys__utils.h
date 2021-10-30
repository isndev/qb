/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 *
 *         many thanks to lib yasio : https://github.com/yasio/yasio
 */

#ifndef QB_IO_UTILS_H
#define QB_IO_UTILS_H
#include <algorithm>
#include <assert.h>
#include <chrono>
#include <qb/utility/build_macros.h>

namespace qb {
// typedefs
typedef long long highp_time_t;
typedef std::chrono::high_resolution_clock steady_clock_t;
typedef std::chrono::system_clock system_clock_t;

// The high precision nano seconds timestamp since epoch
template <typename _Ty = steady_clock_t>
inline highp_time_t
xhighp_clock() {
    auto duration = _Ty::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}
// The high precision micro seconds timestamp since epoch
template <typename _Ty = steady_clock_t>
inline highp_time_t
highp_clock() {
    return xhighp_clock<_Ty>() / std::milli::den;
}
// The normal precision milli seconds timestamp since epoch
template <typename _Ty = steady_clock_t>
inline highp_time_t
clock() {
    return xhighp_clock<_Ty>() / std::micro::den;
}
// The time now in seconds since epoch, better performance than chrono on win32
// see: win10 sdk ucrt/time/time.cpp:common_time
// https://docs.microsoft.com/en-us/windows/desktop/sysinfo/acquiring-high-resolution-time-stamps
inline highp_time_t
time_now() {
    return ::time(nullptr);
}

#if QB__HAS_CXX17
using std::clamp;
#else
template <typename _Ty>
const _Ty &
clamp(const _Ty &v, const _Ty &lo, const _Ty &hi) {
    assert(!(hi < lo));
    return v < lo ? lo : hi < v ? hi : v;
}
#endif

template <typename _Ty>
inline void
invoke_dtor(_Ty *p) {
    p->~_Ty();
}
} // namespace qb

#endif