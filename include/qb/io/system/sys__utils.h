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