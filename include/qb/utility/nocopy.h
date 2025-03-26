/**
 * @file qb/utility/nocopy.h
 * @brief Non-copyable base class
 *
 * This file defines a base class that can be inherited from to prevent copying
 * of derived classes. It's a utility class used throughout the framework to
 * enforce non-copyable semantics for classes where copying would be problematic.
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

#ifndef QB_UTILS_NOCOPY_H
#define QB_UTILS_NOCOPY_H

namespace qb {
/**
 * @struct nocopy
 * @brief Base class that prevents copying of derived classes
 *
 * Classes that inherit from this struct will have their copy constructor
 * and copy assignment operator deleted, making them non-copyable.
 * This is useful for classes that manage resources where copying
 * semantics would be complex or unwanted, such as actors, I/O handles,
 * or system resources.
 *
 * Usage example:
 * @code
 * class MyNonCopyableClass : private qb::nocopy {
 *     // Class implementation...
 * };
 * @endcode
 */
struct nocopy {
    /** @brief Default constructor */
    nocopy() = default;

    /** @brief Deleted copy constructor */
    nocopy(nocopy const &) = delete;

    /** @brief Deleted move constructor */
    nocopy(nocopy const &&) = delete;

    /** @brief Deleted copy assignment operator */
    nocopy &operator=(nocopy const &) = delete;
};
} // namespace qb

#endif // QB_UTILS_NOCOPY_H
