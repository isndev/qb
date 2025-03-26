/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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
 * @class Pipe core/Pipe.h qb/actor.h
 * @ingroup Core
 * @brief Represents a communication channel between actors
 * @details
 * A Pipe object is returned by Actor::getPipe() and provides a way to send events
 * between actors. It maintains references to both the source and destination actors
 * and the underlying virtual pipe for communication.
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
     * @brief Push an event to the pipe
     * @tparam T Type of the event to push
     * @tparam _Args Types of the arguments to construct the event
     * @param args Arguments to construct the event
     * @return Reference to the pushed event
     */
    template <typename T, typename... _Args>
    T &push(_Args &&... args) const noexcept;

    /*!
     * @brief Push an event with pre-allocated size to the pipe
     * @tparam T Type of the event to push
     * @tparam _Args Types of the arguments to construct the event
     * @param size Pre-allocated size for the event
     * @param args Arguments to construct the event
     * @return Reference to the pushed event
     */
    template <typename T, typename... _Args>
    [[nodiscard]] T &allocated_push(std::size_t size, _Args &&... args) const noexcept;

    /*!
     * @brief Get the destination actor ID
     * @return ID of the destination actor
     */
    [[nodiscard]] inline ActorId
    getDestination() const noexcept {
        return dest;
    }

    /*!
     * @brief Get the source actor ID
     * @return ID of the source actor
     */
    [[nodiscard]] inline ActorId
    getSource() const noexcept {
        return source;
    }
};

using pipe = Pipe;

} // namespace qb
#endif // QB_PROXYPIPE_H

