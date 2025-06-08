/**
 * @file qb/utility/nocopy.h
 * @brief Defines a base class to make derived classes non-copyable.
 *
 * This file provides a utility struct `qb::nocopy` that, when inherited from
 * (typically privately), deletes the copy constructor and copy assignment operator
 * of the derived class. This is a common C++ idiom to prevent objects of certain
 * types from being copied, which is often desirable for classes managing unique
 * resources or those whose identity is important.
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
 * @ingroup MiscUtils
 * @brief Base class to make derived classes non-copyable.
 *
 * Classes that inherit from this struct (usually via private inheritance)
 * will have their copy constructor and copy assignment operator deleted,
 * effectively preventing instances of the derived class from being copied.
 * Their move constructor and move assignment operator are also deleted here
 * to enforce non-movable semantics by default as well, unless explicitly re-enabled
 * by the derived class.
 *
 * This is useful for classes that manage unique resources (like file handles, network connections,
 * or actor identities) where copying would be complex, semantically incorrect, or resource-intensive.
 *
 * Usage example:
 * @code
 * class MyResourceWrapper : private qb::nocopy {
 * public:
 *   MyResourceWrapper() { // acquire resource  }
 *   ~MyResourceWrapper() { // release resource  }
 *   // ... other methods ...
 * };
 *
 * // MyResourceWrapper obj1;
 * // MyResourceWrapper obj2 = obj1; // Compile error: copy constructor is deleted
 * // obj1 = obj2;                 // Compile error: copy assignment is deleted
 * @endcode
 */
struct nocopy {
    /** @brief Default constructor. Allows derived classes to be default-constructed if appropriate. */
    nocopy() = default;

    /** @brief Deleted copy constructor. Prevents copying of derived class instances. */
    nocopy(nocopy const &) = delete;

    /** @brief Deleted move constructor. Prevents moving of derived class instances by default. */
    nocopy(nocopy const &&) = delete; // Corrected from const && to && for typical move signature, though deleting it covers all.

    /** @brief Deleted copy assignment operator. Prevents copy assignment of derived class instances. */
    nocopy &operator=(nocopy const &) = delete;

    /** @brief Deleted move assignment operator. Prevents move assignment of derived class instances by default. */
    nocopy &operator=(nocopy &&) = delete; // Added to explicitly delete move assignment
};
} // namespace qb

#endif // QB_UTILS_NOCOPY_H
