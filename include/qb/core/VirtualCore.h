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
# include <unordered_map>
# include <thread>

#if defined(unix) || defined(__unix) || defined(__unix__)
#include <sched.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#include <process.h>
#endif

// include from qb
# include <qb/system/timestamp.h>
# include <qb/system/allocator/pipe.h>
# include <qb/system/lockfree/mpsc.h>
# include "ICallback.h"
# include "ProxyPipe.h"
# include "Event.h"
# include "Actor.h"
# include "Main.h"

# define SERVICE_ACTOR_INDEX 10000

class VirtualCore;
qb::io::stream &operator<<(qb::io::stream &os, qb::VirtualCore const &core);

namespace qb {

    class VirtualCore {
		static uint16_t _nb_service;
		static std::unordered_map<uint32_t, uint16_t> &getServices() {
			static std::unordered_map<uint32_t, uint16_t> service_ids;
			return service_ids;
		}

        enum Error : uint64_t {
            BadInit = (1 << 9),
            NoActor = (1 << 10),
            BadActorInit = (1 << 11),
            ExceptionThrown = (1 << 12)
        };

        friend class Actor;
        friend class Main;
        ////////////
        constexpr static const uint64_t MaxRingEvents = ((std::numeric_limits<uint16_t>::max)() + 1) / QB_LOCKFREE_CACHELINE_BYTES * 4;
        // Types
        using MPSCBuffer = Main::MPSCBuffer;
        using EventBuffer = std::array<CacheLine, MaxRingEvents>;
        using ActorMap = std::unordered_map<uint32_t, Actor *>;
        using CallbackMap = std::unordered_map<uint32_t, ICallback *>; // TODO: try to transform in std::vector
        using PipeMap = std::unordered_map<uint32_t, Pipe>;
        using RemoveActorList = std::unordered_set<uint32_t>;
        using AvailableIdList = std::unordered_set<uint16_t>;
        //!Types
    private:
        // Members
        const uint8_t   _index;
        Main           &_engine;
        MPSCBuffer     &_mail_box;
        AvailableIdList _ids;
        ActorMap        _actors;
        CallbackMap     _actor_callbacks;
        RemoveActorList _actor_to_remove;
        PipeMap         _pipes;
        EventBuffer     _event_buffer;
        std::thread     _thread;
        uint64_t        _nano_timer;
        // !Members

        VirtualCore() = delete;
        VirtualCore(uint8_t const id, Main &engine);

        ActorId __generate_id__();

        // Event Management
        Pipe &__getPipe__(uint32_t core);
        void __receive_events__(CacheLine *buffer, std::size_t const nb_events);
        void __receive__();
        void __flush__();
        bool __flush_all__();
        //!Event Management

        // Workflow
        void __init__actors__() const;
        void __init__();
        bool __wait__all__cores__ready();
        void __updateTime__();
        void __spawn__();
        //!Workflow

        // Actor Management
        template<typename _Actor>
        bool initActor(_Actor * const actor, bool doinit);
        void addActor(Actor *actor);
        void removeActor(ActorId const id);

        template<typename _Actor, typename ..._Init>
        ActorId addActor(_Init &&...init);

        //!Actor Management

        void start();
        void join();

    private:
        template<typename _Actor, typename ..._Init>
        _Actor *addReferencedActor(_Init &&...init);

        void killActor(ActorId const id);

        template <typename _Actor>
        void registerCallback(_Actor &actor);
        void unregisterCallback(ActorId const id);

    private:
        // Event Api
        ProxyPipe getProxyPipe(ActorId const dest, ActorId const source);
        bool try_send(Event const &event) const;
        void send(Event const &event);
        Event &push(Event const &event);
        void reply(Event &event);
        void forward(ActorId const dest, Event &event);

        template <typename T>
        inline void fill_event(T &data, ActorId const dest, ActorId const source) const;
        template<typename T, typename ..._Init>
        void send(ActorId const dest, ActorId const source, _Init &&...init);
        template<typename T, typename ..._Init>
        T &push(ActorId const dest, ActorId const source, _Init &&...init);
        template<typename T, typename ..._Init>
        void fast_push(ActorId const dest, ActorId const source, _Init &&...init);
        //!Event Api

    public:
        uint16_t getIndex() const;
        uint64_t time() const;

    };

}

#endif //QB_CORE_H
