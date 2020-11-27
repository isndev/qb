/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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
#include "ActorId.h"
#include "Event.h"

namespace qb {

/*!
 * @brief Object returned by Actor::getPipe()
 * @class Pipe Pipe.h qb/actor.h
 * @details
 * to define
 */
class Pipe {
    friend class VirtualCore;

    VirtualPipe *pipe;
    ActorId dest;
    ActorId source;

    Pipe(VirtualPipe &i_pipe, ActorId i_dest, ActorId i_source) noexcept
        : pipe(&i_pipe)
        , dest(i_dest)
        , source(i_source) {}

public:
    Pipe() = default;
    Pipe(Pipe const &) = default;
    Pipe &operator=(Pipe const &) = default;

    /*!
     *
     * @tparam T
     * @tparam _Args
     * @param args
     * @return
     */
    template <typename T, typename... _Args>
    T &push(_Args &&... args) const noexcept;

    /*!
     *
     * @tparam T
     * @tparam _Args
     * @param size
     * @param args
     * @return
     */
    template <typename T, typename... _Args>
    T &allocated_push(std::size_t size, _Args &&... args) const noexcept;

    /*!
     *
     * @return
     */
    [[nodiscard]] inline ActorId
    getDestination() const noexcept {
        return dest;
    }

    /*!
     *
     * @return
     */
    [[nodiscard]] inline ActorId
    getSource() const noexcept {
        return source;
    }
};

using pipe = Pipe;

} // namespace qb

#endif // QB_PROXYPIPE_H
