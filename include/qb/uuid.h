/**
 * @file qb/uuid.h
 * @brief Universally Unique Identifier (UUID) support
 * 
 * This file provides a wrapper around the third-party UUID library,
 * offering type aliases and utility functions for generating and
 * manipulating UUIDs (RFC 4122). UUIDs are 128-bit identifiers
 * designed to be unique across space and time, suitable for
 * distributed systems without central coordination.
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

#include <uuid/include/uuid.h>

#ifndef QB_UUID_H
#define QB_UUID_H

namespace qb {
    /**
     * @brief UUID type alias for the underlying implementation
     * 
     * This type represents a 128-bit universally unique identifier
     * as defined in RFC 4122. The implementation is provided by
     * the third-party uuids library.
     */
    using uuid = ::uuids::uuid;

    /**
     * @brief Generates a random (version 4) UUID
     * 
     * Creates a new randomly generated UUID using a high-quality
     * random number generator. This function produces UUIDs that are
     * suitable for most distributed applications where uniqueness
     * is required without central coordination.
     * 
     * @return A new randomly generated UUID
     */
    uuid generate_random_uuid();

} // namespace qb

#endif // QB_UUID_H
