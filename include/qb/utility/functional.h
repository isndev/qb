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

#ifndef QB_FUNCTIONAL_H
#define QB_FUNCTIONAL_H
# include <functional>

namespace qb {

    template<typename T>
    void _hash_combine (size_t& seed, const T& val)
    {
        seed ^= std::hash<T>()(val) + 0x9e3779b9 + (seed<<6) + (seed>>2);
    }

    template<typename... Types>
    size_t hash_combine (const Types&... args)
    {
        size_t seed = 0;
        (_hash_combine(seed,args) , ... ); // create hash value with seed over all args
        return seed;
    }
}

#endif //QB_FUNCTIONAL_H
