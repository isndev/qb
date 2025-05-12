/**
 * @file qb/core/VirtualCore.h
 * @brief Defines the VirtualCore class, representing a worker thread in the QB Actor Framework.
 *
 * This file contains the definition for the `VirtualCore` class, which is a fundamental
 * component of the QB Actor Framework. Each `VirtualCore` instance typically runs in its
 * own thread and is responsible for managing the lifecycle and event processing for a
 * set of actors assigned to it. It handles event queues, inter-core communication
 * via mailboxes, and the execution of actor event handlers.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup Core
 */

#ifndef QB_CORE_H
#define QB_CORE_H
#include <iostream>
#include <set>
#include <thread>
#include <vector>

#if defined(unix) || defined(__unix) || defined(__unix__)
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#elif defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#endif

// include from qb
#include <qb/system/allocator/pipe.h>
#include <qb/system/container/unordered_map.h>
#include <qb/system/event/router.h>
#include <qb/system/lockfree/mpsc.h>
#include <qb/system/timestamp.h>
#include "Actor.h"
#include "Event.h"
#include "ICallback.h"
#include "Main.h"
#include "Pipe.h"

namespace qb {

/*!
 * @class VirtualCore
 * @ingroup Engine
 * @brief Manages a virtual processing core (worker thread) in the actor system.
 * @details
 * A VirtualCore is responsible for executing actors assigned to it. It runs an
 * event loop that processes incoming events for its actors, manages actor lifecycles
 * (initialization, termination), and handles inter-core communication by dispatching
 * events to and from other VirtualCores via mailboxes.
 * Each VirtualCore typically runs in its own dedicated thread.
 */
class VirtualCore {
    thread_local static VirtualCore *_handler;
    static ServiceId                 _nb_service;
    static qb::unordered_map<TypeId, ServiceId> &
    getServices() {
        static qb::unordered_map<TypeId, ServiceId> service_ids;
        return service_ids;
    }

public:
    /*!
     * @enum Error
     * @ingroup Engine
     * @brief Error codes for virtual core operations and states.
     *        These flags can be combined to represent multiple error conditions.
     */
    enum Error : uint64_t {
        BadInit         = (1u << 9u),  ///< General initialization error for the VirtualCore.
        NoActor         = (1u << 10u), ///< An expected actor was not found or couldn't be processed.
        BadActorInit    = (1u << 11u), ///< An actor's `onInit()` method returned false or threw an exception.
        ExceptionThrown = (1u << 12u)  ///< An unhandled exception occurred during VirtualCore execution (e.g., in an actor event handler).
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
    using Mailbox         = SharedCoreCommunication::Mailbox;
    using EventBuffer     = std::array<EventBucket, MaxRingEvents>;
    using ActorMap        = qb::unordered_map<ActorId, Actor *>;
    using CallbackMap     = qb::unordered_map<ActorId, ICallback *>;
    using PipeMap         = std::vector<VirtualPipe>;
    using RemoveActorList = qb::unordered_set<ActorId>;
    using AvailableIdList = std::set<ServiceId>;

    //! Types

private:
    // Members
    const CoreId             _index;
    const CoreId             _resolved_index;
    SharedCoreCommunication &_engine;
    // event reception
    Mailbox                     &_mail_box;
    std::unique_ptr<EventBuffer> _event_buffer;
    router::memh<Event>          _router;
    // event flush
    PipeMap                      _pipes;
    VirtualPipe                 &_mono_pipe_swap;
    std::unique_ptr<VirtualPipe> _mono_pipe;
    // actors management
    AvailableIdList _ids;
    ActorMap        _actors;
    CallbackMap     _actor_callbacks;
    RemoveActorList _actor_to_remove;
    // --- loop

    struct {
        uint64_t _sleep_count        = 0;
        uint64_t _nb_event_io        = 0;
        uint64_t _nb_event_received  = 0;
        uint64_t _nb_bucket_received = 0;
        uint64_t _nb_event_sent_try  = 0;
        uint64_t _nb_event_sent      = 0;
        uint64_t _nb_bucket_sent     = 0;
        uint64_t _nanotimer          = 0;
        //
        inline void
        reset() {
            //*this = {};
            *this = {((_sleep_count + _nb_event_sent + _nb_event_received +
                       _nb_event_io + _nb_event_sent_try))};
        }
        //
    } _metrics;
    // !Members

    VirtualCore(CoreId id, SharedCoreCommunication &engine) noexcept;
    ~VirtualCore() noexcept;

    /*!
     * @brief Generate a new actor ID
     * @return Newly generated actor ID for use within this core
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
     * @return Reference to the virtual pipe for communication with the target core
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
     * @return ID of the initialized actor or Invalid ID if initialization failed
     */
    [[nodiscard]] ActorId initActor(Actor &actor, bool doInit) noexcept;
    /*!
     * @brief Add an actor to the core
     * @param actor Actor to add
     * @param doInit Whether to call the actor's init method
     * @return ID of the added actor or Invalid ID if addition failed
     */
    [[nodiscard]] ActorId appendActor(Actor &actor, bool doInit = false) noexcept;
    void                  removeActor(ActorId id) noexcept;
    //! Actor Management

private:
    /*!
     * @brief Create and add a new actor to this core
     * @tparam _Actor Type of actor to create
     * @tparam _Init Types of initialization parameters
     * @param init Parameters for actor initialization
     * @return Pointer to the newly created actor or nullptr if creation failed
     */
    template <typename _Actor, typename... _Init>
    [[nodiscard]] _Actor *addReferencedActor(_Init &&...init) noexcept;
    /*!
     * @brief Get a service actor of specified type
     * @tparam _ServiceActor Type of service actor to get
     * @return Pointer to the service actor or nullptr if not found
     */
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
     * @return Pipe connecting the source and destination actors
     */
    [[nodiscard]] Pipe getProxyPipe(ActorId dest, ActorId source) noexcept;
    /*!
     * @brief Attempt to send an event immediately
     * @param event Event to send
     * @return true if the event was sent successfully, false otherwise
     */
    [[nodiscard]] bool try_send(Event const &event) const noexcept;
    void               send(Event const &event) noexcept;
    /*!
     * @brief Push an event to the event queue
     * @param event Event to push
     * @return Reference to the pushed event in the queue
     */
    Event &push(Event const &event) noexcept;
    void   reply(Event &event) noexcept;
    void   forward(ActorId dest, Event &event) noexcept;

    template <typename T>
    static inline void fill_event(T &data, ActorId dest, ActorId source) noexcept;
    template <typename T, typename... _Init>
    void send(ActorId dest, ActorId source, _Init &&...init) noexcept;
    template <typename T, typename... _Init>
    void broadcast(ActorId source, _Init &&...init) noexcept;
    /*!
     * @brief Build and push an event to the event queue
     * @tparam T Type of event to create
     * @tparam _Init Types of initialization parameters
     * @param dest Destination actor ID
     * @param source Source actor ID
     * @param init Parameters for event initialization
     * @return Reference to the created and pushed event
     */
    template <typename T, typename... _Init>
    T &push(ActorId dest, ActorId source, _Init &&...init) noexcept;
    //! Event Api

public:
    VirtualCore() = delete;

    /*!
     * @brief Get the core's index.
     * @ingroup Engine
     * @return `CoreId` (unsigned short) representing the unique index of this VirtualCore.
     * @details This ID is assigned during engine initialization and is used in `ActorId` construction.
     */
    [[nodiscard]] CoreId getIndex() const noexcept;

    /*!
     * @brief Get the set of cores this VirtualCore is configured to communicate with.
     * @ingroup Engine
     * @return Const reference to a `CoreIdSet`.
     * @details This set typically includes all other VirtualCores in the system, allowing
     *          this core to send events to actors on those cores.
     */
    [[nodiscard]] const CoreIdSet &getCoreSet() const noexcept;

    /*!
     * @brief Get the current cached time for this VirtualCore's processing loop.
     * @ingroup Engine
     * @return `uint64_t` timestamp in nanoseconds since epoch.
     * @details
     * This timestamp is updated once at the beginning of each iteration of the VirtualCore's
     * main processing loop. All actors running on this core during that single iteration
     * will see the same value when calling `Actor::time()` (which internally calls this).
     * This is optimized for performance within a loop iteration but means it does not update
     * with true nanosecond precision *during* a single actor's event handling.
     * For a continuously updating high-precision clock, use `qb::NanoTimestamp()`.
     */
    [[nodiscard]] uint64_t time() const noexcept;
};
#ifdef QB_LOGGER
qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::VirtualCore const &core);
#endif
std::ostream &operator<<(std::ostream &os, qb::VirtualCore const &core);

} // namespace qb
#endif // QB_CORE_H
