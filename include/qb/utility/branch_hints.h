/**
 * @file qb/utility/branch_hints.h
 * @brief Branch prediction hint utilities
 * 
 * This file provides utility functions that give branch prediction hints
 * to the compiler. These functions can improve performance by helping
 * the compiler make better decisions about code generation for conditional
 * branches.
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
 * @brief Hint for branch prediction when the condition is likely true
 * 
 * Use this function to indicate to the compiler that the expression is
 * expected to evaluate to true most of the time. This can help optimize
 * code paths that are frequently taken.
 * 
 * @param expr Boolean expression to evaluate
 * @return The result of evaluating expr
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
 * @brief Hint for branch prediction when the condition is likely false
 * 
 * Use this function to indicate to the compiler that the expression is
 * expected to evaluate to false most of the time. This can help optimize
 * code paths that are rarely taken.
 * 
 * @param expr Boolean expression to evaluate
 * @return The result of evaluating expr
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
