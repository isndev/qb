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

#ifndef QB_ICALLBACK_H
#define QB_ICALLBACK_H

namespace qb {

    /*!
     * @interface ICallback core/ICallback.h qb/icallback.h
     * @ingroup Core
     * @brief Actor callback interface
     * @details
     * DerivedActor must inherit from this interface
     * to register loop callback
     */
    class ICallback {
    public:
        virtual ~ICallback() noexcept {}

        virtual void onCallback() = 0;
    };

} // namespace qb

#endif //QB_ICALLBACK_H
