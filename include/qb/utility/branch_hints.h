/**
 * @file qb/utility/branch_hints.h
 * @brief Branch prediction hint utilities for performance optimization.
 *
 * This file provides utility functions (`qb::likely`, `qb::unlikely`) that give branch prediction hints
 * to the compiler. These functions can potentially improve performance by helping
 * the compiler make better decisions about code generation for conditional
 * branches, especially in performance-critical sections.
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
 * @ingroup Utility
 */

#ifndef QB_UTILS_BRANCH_HINTS_H
#define QB_UTILS_BRANCH_HINTS_H

namespace qb {
/**
 * @brief Hint for branch prediction when a condition is expected to be true.
 * @ingroup MiscUtils
 * @param expr Boolean expression to evaluate.
 * @return The result of evaluating `expr`.
 * @details Use this function to indicate to the compiler that the expression `expr` is
 *          expected to evaluate to `true` most of the time. This can help the compiler
 *          optimize code paths that are frequently taken, potentially improving performance
 *          by reducing branch mispredictions.
 *          Typically implemented using `__builtin_expect` on GCC/Clang.
 * @code
 * if (qb::likely(common_case_condition)) {
 *   // Optimized path for likely true
 * } else {
 *   // Path for less common false case
 * }
 * @endcode
 */
inline bool
likely(bool expr) {
#ifdef __GNUC__
    return __builtin_expect(expr, true);
#else
    return expr;
#endif
}

/**
 * @brief Hint for branch prediction when a condition is expected to be false.
 * @ingroup MiscUtils
 * @param expr Boolean expression to evaluate.
 * @return The result of evaluating `expr`.
 * @details Use this function to indicate to the compiler that the expression `expr` is
 *          expected to evaluate to `false` most of the time. This can help the compiler
 *          optimize for the more common case where the branch is not taken, or the alternative
 *          path is taken.
 *          Typically implemented using `__builtin_expect` on GCC/Clang.
 * @code
 * if (qb::unlikely(error_condition)) {
 *   // Path for rare error case
 * } else {
 *   // Optimized path for likely non-error
 * }
 * @endcode
 */
inline bool
unlikely(bool expr) {
#ifdef __GNUC__
    return __builtin_expect(expr, false);
#else
    return expr;
#endif
}

} /* namespace qb */

#endif /* QB_UTILS_BRANCH_HINTS_H */
