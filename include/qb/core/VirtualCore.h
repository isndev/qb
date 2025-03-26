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

#ifndef QB_CORE_H
#define QB_CORE_H
#include <iostream>
#include <set>
#include <thread>
#include <vector>

#if defined(unix) || defined(__unix) || defined(__unix__)
#    include <cerrno>
#    include <pthread.h>
#    include <sched.h>
#    include <unistd.h>
#elif defined(_WIN32) || defined(_WIN64)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif // !WIN32_LEAN_AND_MEAN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <process.h>
#    include <windows.h>
#endif

// include from qb
#include "Actor.h"
#include "Event.h"
#include "ICallback.h"
#include "Main.h"
#include "Pipe.h"
#include <qb/system/allocator/pipe.h>
#include <qb/system/container/unordered_map.h>
#include <qb/system/event/router.h>
#include <qb/system/lockfree/mpsc.h>
#include <qb/system/timestamp.h>

namespace qb {

/*!
 * @class VirtualCore core/VirtualCore.h qb/core.h
 * @ingroup Core
 * @brief Manages a virtual processing core in the actor system
 * @details
 * VirtualCore represents a logical processing unit that manages actors, events,
 * and communication within the actor system. It handles event routing, actor lifecycle,
 * and inter-core communication.
 */
class VirtualCore {
    thread_local static VirtualCore *_handler;
    static ServiceId _nb_service;
    static qb::unordered_map<TypeId, ServiceId> &
    getServices() {
        static qb::unordered_map<TypeId, ServiceId> service_ids;
        return service_ids;
    }

public:
    /*!
     * @enum Error core/VirtualCore.h qb/core.h
     * @brief Error codes for virtual core operations
     */
    enum Error : uint64_t {
        BadInit = (1u << 9u),        ///< Initialization error
        NoActor = (1u << 10u),       ///< Actor not found
        BadActorInit = (1u << 11u),  ///< Actor initialization error
        ExceptionThrown = (1u << 12u) ///< Exception occurred during execution
    };

private:
    friend class Actor;
    friend class Service;
    friend class CoreInitializer;
    friend class Main;
    ////////////
    constexpr static const uint64_t MaxRingEvents =
        ((std::numeric_limits<uint16_t>::max)() + 1) / QB_LOCKFREE_EVENT_BUCKET_BYTES;
    // Types
    using Mailbox = SharedCoreCommunication::Mailbox;
    using EventBuffer = std::array<EventBucket, MaxRingEvents>;
    using ActorMap = qb::unordered_map<ActorId, Actor *>;
    using CallbackMap = qb::unordered_map<ActorId, ICallback *>;
    using PipeMap = std::vector<VirtualPipe>;
    using RemoveActorList = qb::unordered_set<ActorId>;
    using AvailableIdList = std::set<ServiceId>;

    //! Types
private:
    // Members
    const CoreId _index;
    const CoreId _resolved_index;
    SharedCoreCommunication &_engine;
    // event reception
    Mailbox &_mail_box;
    EventBuffer &_event_buffer;
    router::memh<Event> _router;
    // event flush
    PipeMap _pipes;
    VirtualPipe &_mono_pipe_swap;
    VirtualPipe &_mono_pipe;
    // actors management
    AvailableIdList _ids;
    ActorMap _actors;
    CallbackMap _actor_callbacks;
    RemoveActorList _actor_to_remove;
    // --- loop

    struct {
        uint64_t _sleep_count = 0;
        uint64_t _nb_event_io = 0;
        uint64_t _nb_event_received = 0;
        uint64_t _nb_bucket_received = 0;
        uint64_t _nb_event_sent_try = 0;
        uint64_t _nb_event_sent = 0;
        uint64_t _nb_bucket_sent = 0;
        uint64_t _nanotimer = 0;
//
        inline void
        reset() {
            //*this = {};
            *this = {(
                (_sleep_count + _nb_event_sent + _nb_event_received + _nb_event_io + _nb_event_sent_try))};
        }
//
    } _metrics;
    // !Members

    VirtualCore(CoreId id, SharedCoreCommunication &engine) noexcept;
    ~VirtualCore() noexcept;

    /*!
     * @brief Generate a new actor ID
     * @return Newly generated actor ID
     */
    [[nodiscard]] ActorId __generate_id__() noexcept;

    // Event Management
    template <typename _Event, typename _Actor>
    void registerEvent(_Actor &actor) noexcept;
    template <typename _Event, typename _Actor>
    void unregisterEvent(_Actor &actor) noexcept;
    void unregisterEvents(ActorId id) noexcept;
    /*!
     * @brief Get or create a pipe to a specific core
     * @param core Target core ID
     * @return Reference to the virtual pipe
     */
    [[nodiscard]] VirtualPipe &__getPipe__(CoreId core) noexcept;
    void __receive_events__(EventBucket *buffer, std::size_t nb_events);
    void __receive__();
    bool __flush_all__() noexcept;
    //! Event Management

    // Workflow
    bool __init__(CoreIdSet const &cores);
    bool __init__actors__() const;
    void __workflow__();
    //! Workflow

    // Actor Management
    /*!
     * @brief Initialize a new actor
     * @param actor Actor to initialize
     * @param doInit Whether to call the actor's init method
     * @return ID of the initialized actor
     */
    [[nodiscard]] ActorId initActor(Actor &actor, bool doInit) noexcept;
    /*!
     * @brief Add an actor to the core
     * @param actor Actor to add
     * @param doInit Whether to call the actor's init method
     * @return ID of the added actor
     */
    [[nodiscard]] ActorId appendActor(Actor &actor, bool doInit = false) noexcept;
    void removeActor(ActorId id) noexcept;
    //! Actor Management

private:
    template <typename _Actor, typename... _Init>
    [[nodiscard]] _Actor *addReferencedActor(_Init &&... init) noexcept;
    template <typename _ServiceActor>
    [[nodiscard]] _ServiceActor *getService() const noexcept;

    void killActor(ActorId id) noexcept;

    template <typename _Actor>
    void registerCallback(_Actor &actor) noexcept;
    void __unregisterCallback(ActorId id) noexcept;
    void unregisterCallback(ActorId id) noexcept;

private:
    // Event Api
    /*!
     * @brief Get a proxy pipe between two actors
     * @param dest Destination actor ID
     * @param source Source actor ID
     * @return Pipe connecting the actors
     */
    [[nodiscard]] Pipe getProxyPipe(ActorId dest, ActorId source) noexcept;
    [[nodiscard]] bool try_send(Event const &event) const noexcept;
    void send(Event const &event) noexcept;
    /*!
     * @brief Push an event to the event queue
     * @param event Event to push
     * @return Reference to the pushed event
     */
    Event &push(Event const &event) noexcept;
    void reply(Event &event) noexcept;
    void forward(ActorId dest, Event &event) noexcept;

    template <typename T>
    static inline void fill_event(T &data, ActorId dest, ActorId source) noexcept;
    template <typename T, typename... _Init>
    void send(ActorId dest, ActorId source, _Init &&... init) noexcept;
    template <typename T, typename... _Init>
    void broadcast(ActorId source, _Init &&... init) noexcept;
    template <typename T, typename... _Init>
    T &push(ActorId dest, ActorId source, _Init &&... init) noexcept;
    //! Event Api

public:
    VirtualCore() = delete;

    /*!
     * @brief Get the core's index
     * @return Core index
     */
    [[nodiscard]] CoreId getIndex() const noexcept;

    /*!
     * @brief Get the set of cores this core can communicate with
     * @return Set of core IDs
     */
    [[nodiscard]] const qb::unordered_set<CoreId> &getCoreSet() const noexcept;

    /*!
     * @brief Get the current time in nanoseconds
     * @return Current time in nanoseconds
     */
    [[nodiscard]] uint64_t time() const noexcept;
};
#    ifdef QB_LOGGER
qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::VirtualCore const &core);
#endif
std::ostream &operator<<(std::ostream &os, qb::VirtualCore const &core);

} // namespace qb
#endif // QB_CORE_H

