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

class VirtualCore {
    thread_local static VirtualCore *_handler;
    static ServiceId _nb_service;
    static qb::unordered_map<TypeId, ServiceId> &
    getServices() {
        static qb::unordered_map<TypeId, ServiceId> service_ids;
        return service_ids;
    }

public:
    enum Error : uint64_t {
        BadInit = (1u << 9u),
        NoActor = (1u << 10u),
        BadActorInit = (1u << 11u),
        ExceptionThrown = (1u << 12u)
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
    using MPSCBuffer = SharedCoreCommunication::MPSCBuffer;
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
    MPSCBuffer &_mail_box;
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
    bool _is_low_latency; // default no wait
    struct {
        uint64_t _sleep_count = 0;
        uint64_t _nb_event_io = 0;
        uint64_t _nb_event_received = 0;
        uint64_t _nb_bucket_received = 0;
        uint64_t _nb_event_sent_try = 0;
        uint64_t _nb_event_sent = 0;
        uint64_t _nb_bucket_sent = 0;
        uint64_t _nanotimer = 0;

        inline void
        reset() {
            *this = {};
            //*this = {_sleep_count +
            //         ((_nb_event_sent + _nb_event_received + _nb_event_io) << 3u)};
        }

    } _metrics;
    // !Members

    VirtualCore(CoreId id, SharedCoreCommunication &engine) noexcept;
    ~VirtualCore() noexcept;

    ActorId __generate_id__() noexcept;

    // Event Management
    template <typename _Event, typename _Actor>
    void registerEvent(_Actor &actor) noexcept;
    template <typename _Event, typename _Actor>
    void unregisterEvent(_Actor &actor) noexcept;
    void unregisterEvents(ActorId id) noexcept;
    VirtualPipe &__getPipe__(CoreId core) noexcept;
    void __receive_events__(EventBucket *buffer, std::size_t nb_events);
    void __receive__();
    //        void __receive_from__(CoreId const index) noexcept;
    bool __flush_all__() noexcept;
    //! Event Management

    // Workflow
    bool __init__(CoreIdSet const &cores);
    bool __init__actors__() const;
    void __workflow__();
    //! Workflow

    // Actor Management
    ActorId initActor(Actor &actor, bool doInit) noexcept;
    ActorId appendActor(Actor &actor, bool doInit = false) noexcept;
    void removeActor(ActorId id) noexcept;
    //! Actor Management

private:
    template <typename _Actor, typename... _Init>
    _Actor *addReferencedActor(_Init &&... init) noexcept;
    template <typename _ServiceActor>
    _ServiceActor *getService() const noexcept;

    void killActor(ActorId id) noexcept;

    template <typename _Actor>
    void registerCallback(_Actor &actor) noexcept;
    void __unregisterCallback(ActorId id) noexcept;
    void unregisterCallback(ActorId id) noexcept;

private:
    // Event Api
    Pipe getProxyPipe(ActorId dest, ActorId source) noexcept;
    [[nodiscard]] bool try_send(Event const &event) const noexcept;
    void send(Event const &event) noexcept;
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
    // IO strategy
    void setLowLatency(bool state) noexcept;

public:
    VirtualCore() = delete;

    [[nodiscard]] CoreId getIndex() const noexcept;
    [[nodiscard]] const qb::unordered_set<CoreId> &getCoreSet() const noexcept;
    [[nodiscard]] uint64_t time() const noexcept;

};

qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::VirtualCore const &core);
std::ostream &operator<<(std::ostream &os, qb::VirtualCore const &core);

} // namespace qb

#endif // QB_CORE_H
