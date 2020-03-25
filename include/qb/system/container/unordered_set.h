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
# include <robin_hood/src/include/robin_hood.h>

namespace qb {

    template <typename Key, typename Hash = robin_hood::hash<Key>, typename KeyEqual = std::equal_to<Key>, size_t MaxLoadFactor100 = 80>
    using unordered_flat_set = robin_hood::unordered_flat_set<Key, Hash, KeyEqual, MaxLoadFactor100>;

    template <typename Key, typename Hash = robin_hood::hash<Key>, typename KeyEqual = std::equal_to<Key>, size_t MaxLoadFactor100 = 80>
    using unordered_node_set = robin_hood::unordered_node_set<Key, Hash, KeyEqual, MaxLoadFactor100>;

    template <typename Key, typename Hash = robin_hood::hash<Key>, typename KeyEqual = std::equal_to<Key>, size_t MaxLoadFactor100 = 80>
    using unordered_set = robin_hood::unordered_set<Key, Hash, KeyEqual, MaxLoadFactor100>;

} // namespace qb

#endif //QB_UNORDERED_SET_H
