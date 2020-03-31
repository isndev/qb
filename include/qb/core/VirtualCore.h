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

#ifndef QB_CORE_H
#define QB_CORE_H
# include <iostream>
# include <vector>
# include <set>
# include <thread>

#if defined(unix) || defined(__unix) || defined(__unix__)
#include <sched.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#elif defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#endif

// include from qb
# include <qb/system/timestamp.h>
# include <qb/system/container/unordered_map.h>
# include <qb/system/allocator/pipe.h>
# include <qb/system/lockfree/mpsc.h>
# include <qb/system/event/router.h>
# include "ICallback.h"
# include "ProxyPipe.h"
# include "Event.h"
# include "Actor.h"
# include "Main.h"

class VirtualCore;
qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::VirtualCore const &core);

namespace qb {

    class VirtualCore {
        thread_local static VirtualCore *_handler;
        static ServiceId _nb_service;
        static qb::unordered_map<TypeId, ServiceId> &getServices() {
            static qb::unordered_map<TypeId , ServiceId> service_ids;
            return service_ids;
        }

        enum Error : uint64_t {
            BadInit = (1 << 9),
            NoActor = (1 << 10),
            BadActorInit = (1 << 11),
            ExceptionThrown = (1 << 12)
        };

        friend class Actor;
        friend class Service;
        friend class Main;
        ////////////
        constexpr static const uint64_t MaxRingEvents = ((std::numeric_limits<uint16_t>::max)() + 1) / QB_LOCKFREE_EVENT_BUCKET_BYTES;
        // Types
        using MPSCBuffer = Main::MPSCBuffer;
        using EventBuffer = std::array<EventBucket, MaxRingEvents>;
        using ActorMap = qb::unordered_map<ActorId, Actor *>;
        using CallbackMap = qb::unordered_map<ActorId, ICallback *>;
        using PipeMap = std::vector<Pipe>;
        using RemoveActorList = qb::unordered_set<ActorId>;
        using AvailableIdList = std::set<ServiceId>;

        //!Types
    private:
        // Members
        const CoreId   _index;
        const CoreId   _resolved_index;
        Main           &_engine;
        // event reception
        MPSCBuffer     &_mail_box;
        router::memh<Event>   _router;
        // event flush
        PipeMap         _pipes;
        Pipe            &_mono_pipe_swap;
        Pipe            _mono_pipe;
        // actors management
        AvailableIdList _ids;
        ActorMap        _actors;
        CallbackMap     _actor_callbacks;
        RemoveActorList _actor_to_remove;
        // --- loop timer
        EventBuffer     _event_buffer;
        uint64_t        _nanotimer;
        // !Members

        VirtualCore() = delete;
        VirtualCore(CoreId const id, Main &engine) noexcept;
		~VirtualCore() noexcept;

        ActorId __generate_id__() noexcept;

        // Event Management
        template<typename _Event, typename _Actor>
        void registerEvent(_Actor &actor) noexcept;
        template<typename _Event, typename _Actor>
        void unregisterEvent(_Actor &actor) noexcept;
        void unregisterEvents(ActorId const id) noexcept;
        Pipe &__getPipe__(CoreId core) noexcept;
        void __receive_events__(EventBucket *buffer, std::size_t const nb_events);
        void __receive__();
//        void __receive_from__(CoreId const index) noexcept;
        bool __flush_all__() noexcept;
        //!Event Management

        // Workflow
        void __init__();
        void __init__actors__() const;
        bool __wait__all__cores__ready() noexcept;
        void __workflow__();
        //!Workflow

        // Actor Management
        ActorId initActor(Actor &actor, bool const is_service, bool const doInit) noexcept;
        ActorId appendActor(Actor &actor, bool const is_service, bool const doInit = false) noexcept;
        void removeActor(ActorId const id) noexcept;
        //!Actor Management

        void start();
        void join();

    private:
        template<typename _Actor, typename ..._Init>
        _Actor *addReferencedActor(_Init &&...init) noexcept;
        template <typename _ServiceActor>
        _ServiceActor *getService() const noexcept;


        void killActor(ActorId const id) noexcept;

        template <typename _Actor>
        void registerCallback(_Actor &actor) noexcept;
        void __unregisterCallback(ActorId const id) noexcept;
        void unregisterCallback(ActorId const id) noexcept;

    private:
        // Event Api
        ProxyPipe getProxyPipe(ActorId const dest, ActorId const source) noexcept;
        bool try_send(Event const &event) const noexcept;
        void send(Event const &event) noexcept;
        Event &push(Event const &event) noexcept;
        void reply(Event &event) noexcept;
        void forward(ActorId const dest, Event &event) noexcept;

        template <typename T>
        inline void fill_event(T &data, ActorId const dest, ActorId const source) const noexcept;
        template<typename T, typename ..._Init>
        void send(ActorId const dest, ActorId const source, _Init &&...init) noexcept;
        template<typename T, typename ..._Init>
        void broadcast(ActorId const source, _Init &&...init) noexcept;
        template<typename T, typename ..._Init>
        T &push(ActorId const dest, ActorId const source, _Init &&...init) noexcept;
        //!Event Api

    public:
        CoreId getIndex() const noexcept;
        uint64_t time() const noexcept;
    };

}

#endif //QB_CORE_H
