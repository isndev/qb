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

#ifndef QB_TYPE_TRAITS_H
#define QB_TYPE_TRAITS_H

namespace qb {
    template <typename T, bool cond>
    struct remove_reference_if {
        typedef T type;
        constexpr static bool value = false;
    };

    template <typename T>
    struct remove_reference_if<T, true> {
        typedef typename std::remove_reference<T>::type type;
        constexpr static bool value = true;
    };
}

#endif //QB_TYPE_TRAITS_H
