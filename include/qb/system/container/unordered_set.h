/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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
 */


#ifndef QB_UNORDERED_SET_H
#define QB_UNORDERED_SET_H
# include <unordered_set>
# include <ska_hash/unordered_map.hpp>

namespace qb {

    template<typename T, typename H = std::hash<T>, typename E = std::equal_to<T>, typename A = std::allocator<T>>
    using unordered_flat_set = ska::flat_hash_set<T, H, E, A>;
#ifdef NDEBUG
    template<typename T, typename H = std::hash<T>, typename E = std::equal_to<T>, typename A = std::allocator<T>>
    using unordered_set = ska::unordered_set<T, H, E, A>;
#else
    template<typename T, typename H = std::hash<T>, typename E = std::equal_to<T>, typename A = std::allocator<T>>
    using unordered_set = std::unordered_set<T, H, E, A>;
#endif

} // namespace qb

#endif //QB_UNORDERED_SET_H
