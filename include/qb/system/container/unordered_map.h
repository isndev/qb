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


#ifndef QB_UNORDERED_MAP_H
#define QB_UNORDERED_MAP_H
# include <unordered_map>
# include <ska_hash/unordered_map.hpp>

namespace qb {

    template<typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>, typename A = std::allocator<std::pair<const K, V>>>
    using unordered_flat_map = ska::flat_hash_map<K, V, H, E, A>;
#ifdef NDEBUG
    template<typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>, typename A = std::allocator<std::pair<const K, V>>>
    using unordered_map = ska::unordered_map<K, V, H, E, A>;
#else
    template<typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>, typename A = std::allocator<std::pair<const K, V>>>
    using unordered_map = std::unordered_map<K, V, H, E, A>;
#endif
} // namespace qb

#endif //QB_UNORDERED_MAP_H
