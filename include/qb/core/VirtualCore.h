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

class VirtualCore;
qb::io::stream &operator<<(qb::io::stream &os, qb::VirtualCore const &core);

namespace qb {

    class VirtualCore {
        thread_local static VirtualCore *_handler;
        static ServiceId _nb_service;
        static std::unordered_map<TypeId, ServiceId> &getServices() {
            static std::unordered_map<TypeId , ServiceId> service_ids;
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
        using AvailableIdList = std::unordered_set<ServiceId>;

        class IRegisteredEventBase {
        public:
            virtual ~IRegisteredEventBase() noexcept {}
            virtual uint32_t id() const noexcept = 0;
        };

        class IEventhandler {
        public:
            virtual ~IEventhandler() noexcept {}
            virtual void invoke(Event *data) const = 0;
            virtual void registerEvent(IRegisteredEventBase *iRegisteredEvent) noexcept = 0;
            virtual void unregisterEvent(ActorId const id) noexcept = 0;
        };

        template<typename _Event>
        class EventHandler : public IEventhandler {
            friend VirtualCore;

            class IRegisteredEvent : public IRegisteredEventBase {
            public:
                virtual ~IRegisteredEvent() noexcept {}
                virtual void invoke(_Event &data) const = 0;
                virtual uint32_t id() const noexcept = 0;
            };

            template<typename _Actor>
            class RegisteredEvent : public IRegisteredEvent {
                _Actor &_actor;
            public:
                explicit RegisteredEvent(_Actor &actor) noexcept
                        : _actor(actor) {}

                virtual void invoke(_Event &event) const override final {
                    if (likely(_actor.isAlive()))
                        _actor.on(event);
                }

                virtual uint32_t id() const noexcept override final {
                    return _actor.id();
                }
            };

            std::unordered_map<uint32_t, IRegisteredEvent *> _registered_events;

            EventHandler() = default;
            ~EventHandler() noexcept {
                for (auto revent : _registered_events)
                    delete revent.second;
            }

            virtual void invoke(Event *data) const override final {
                auto &event = *reinterpret_cast<_Event *>(data);

                event.state[0] = 0;
                if (event.dest.isBroadcast()) {
                    for (const auto registered_event : _registered_events) {
                        registered_event.second->invoke(event);
                    }
                } else {
                    const auto it = _registered_events.find(event.dest);
                    if (likely(it != _registered_events.cend()))
                        it->second->invoke(event);
                    else {
                        LOG_WARN("Failed Event"
                                         << " [Source](" << event.source << ")"
                                         << " [Dest](" << event.dest << ") NOT FOUND");
                    }
                }

                if (!event.state[0])
                    event.~_Event();
            }

            virtual void registerEvent(IRegisteredEventBase *ievent) noexcept override final {
                auto it = _registered_events.find(ievent->id());
                if (it != _registered_events.end())
                    unregisterEvent(ievent->id());
                _registered_events.insert({ievent->id(), static_cast<IRegisteredEvent *>(ievent)});
            }
            virtual void unregisterEvent(ActorId const id) noexcept override final {
                auto it = _registered_events.find(id);
                if (it != _registered_events.end()) {
                    delete it->second;
                    _registered_events.erase(it);
                }
            }
        };

        using EventMap = std::unordered_map<uint32_t, IEventhandler *>;

        //!Types
    private:
        // Members
        const CoreId   _index;
        Main           &_engine;
        MPSCBuffer     &_mail_box;
        AvailableIdList _ids;
        ActorMap        _actors;
        EventMap        _event_map;
        CallbackMap     _actor_callbacks;
        RemoveActorList _actor_to_remove;
        PipeMap         _pipes;
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
        Pipe &__getPipe__(uint32_t core) noexcept;
        void __receive_events__(CacheLine *buffer, std::size_t const nb_events);
        void __receive__();
        void __flush__() noexcept;
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
        void killActor(ActorId const id) noexcept;

        template <typename _Actor>
        void registerCallback(_Actor &actor) noexcept;
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
        uint64_t time() noexcept;
    };

}

#endif //QB_CORE_H
