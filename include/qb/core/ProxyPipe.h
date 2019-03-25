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

#ifndef QB_PROXYPIPE_H
#define QB_PROXYPIPE_H
# include <qb/system/allocator/pipe.h>
# include "ActorId.h"
# include "Event.h"

namespace qb {
    using Pipe = allocator::pipe<CacheLine>;

    /*!
     * @brief Object returned by Actor::getPipe()
     * @class ProxyPipe ProxyPipe.h qb/actor.h
     * @details
     * to define
     */
    class ProxyPipe {
        Pipe *pipe;
        ActorId dest;
        ActorId source;

    public:
        ProxyPipe() noexcept = default;
        ProxyPipe(ProxyPipe const &) noexcept = default;
        ProxyPipe &operator=(ProxyPipe const &) noexcept = default;

        ProxyPipe(Pipe &i_pipe, ActorId i_dest, ActorId i_source) noexcept
                : pipe(&i_pipe), dest(i_dest), source(i_source) {}

        /*!
         *
         * @tparam T
         * @tparam _Args
         * @param args
         * @return
         */
        template<typename T, typename ..._Args>
        T &push(_Args &&...args) const noexcept;

        /*!
         *
         * @tparam T
         * @tparam _Args
         * @param size
         * @param args
         * @return
         */
        template<typename T, typename ..._Args>
        T &allocated_push(std::size_t size, _Args &&...args) const noexcept;

        /*!
         *
         * @return
         */
        inline ActorId getDestination() const noexcept {
            return dest;
        }

        /*!
         *
         * @return
         */
        inline ActorId getSource() const noexcept {
            return source;
        }

    };

} // namespace qb

#endif //QB_PROXYPIPE_H
