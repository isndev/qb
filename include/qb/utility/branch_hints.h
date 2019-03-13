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

#ifndef QB_UTILS_BRANCH_HINTS_H
#define QB_UTILS_BRANCH_HINTS_H

namespace qb {
    /** \brief hint for the branch prediction */
    inline bool likely(bool expr) {
#ifdef __GNUC__
        return __builtin_expect(expr, true);
#else
        return expr;
#endif
    }

    /** \brief hint for the branch prediction */
    inline bool unlikely(bool expr) {
#ifdef __GNUC__
        return __builtin_expect(expr, false);
#else
        return expr;
#endif
    }

} /* namespace qb */

#endif /* QB_UTILS_BRANCH_HINTS_H */
