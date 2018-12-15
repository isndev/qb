//
// Created by isndev on 12/4/18.
//

#ifndef CUBE_CORE_H
#define CUBE_CORE_H
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
#include <windows.h>
#include <process.h>
#endif

# include "../../utils/timestamp.h"
# include "../../allocator/pipe.h"
# include "../lockfree/mpsc.h"
# include "ICallback.h"
# include "ProxyPipe.h"
# include "Event.h"
# include "Actor.h"
# include "Cube.h"

class Core;
cube::io::stream &operator<<(cube::io::stream &os, cube::Core const &core);

namespace cube {


    class Core {
        friend class Cube;
    public:
        ////////////
        constexpr static const uint64_t MaxRingEvents = ((std::numeric_limits<uint16_t>::max)()) / CUBE_LOCKFREE_CACHELINE_BYTES;
        // Types
        using MPSCBuffer = Cube::MPSCBuffer;
        using EventBuffer = std::array<CacheLine, MaxRingEvents>;
        using ActorMap = std::unordered_map<uint32_t, Actor *>;
        using CallbackMap = std::unordered_map<uint32_t, ICallback *>;
        using PipeMap = std::unordered_map<uint32_t, Pipe>;
        using RemoveActorList = std::vector<ActorId>;
        //!Types
    private:

        // Members
        const uint8_t   _index;
        Cube           &_engine;
        MPSCBuffer     &_mail_box;
        ActorMap        _actors;
        CallbackMap     _actor_callbacks;
        RemoveActorList _actor_to_remove;
        PipeMap         _pipes;
        EventBuffer     _event_buffer;
        std::thread     _thread;
        uint64_t        _nano_timer;
        // !Members

        Core() = delete;
        Core(uint8_t const id, Cube &engine)
                : _index(id)
                , _engine(engine)
                , _mail_box(engine.getMailBox(id))
                , _nano_timer(Timestamp::nano()) {}

        ActorId __generate_id__() {
            static std::size_t pid = 10000;
            //duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()
            //Timestamp::nano() % std::numeric_limits<uint32_t>::max() + pid++)
            return ActorId(static_cast<uint32_t >(++pid), _index);
        }

        // Event Management
        Pipe &__getPipe__(uint32_t core) {
            return _pipes[core];
        }
        void __receive_events__(CacheLine *buffer, std::size_t const nb_events) {
            if (!nb_events)
                return;
            std::size_t i = 0;
            while (i < nb_events) {
                auto event = reinterpret_cast<Event *>(buffer + i);
                auto actor = _actors.find(event->dest);
                if (likely(actor != std::end(_actors))) {
                    actor->second->on(event);
                } else {
                    LOG_WARN << "Failed Event" << *this
                             << " [Source](" << event->source << ")"
                             << " [Dest](" << event->dest << ") NOT FOUND";
                }

                i += event->bucket_size;
            }
            LOG_DEBUG << "Events " << *this << " received " << nb_events << " buckets";
        }
        void __receive__() {
            // global_core_events
            _mail_box.dequeue([this](CacheLine *buffer, std::size_t const nb_events){
                __receive_events__(buffer, nb_events);
            }, _event_buffer.data(), MaxRingEvents);
        }
        void __flush__() {
            for (auto &it : _pipes) {
                auto &pipe = it.second;
                if (pipe.end()) {
                    auto i = pipe.begin();
                    while (i < pipe.end()) {
                        const auto &event = *reinterpret_cast<const Event *>(pipe.data() + i);
                        if (!try_send(event))
                            break;
                        i += event.bucket_size;
                    }
                    pipe.reset(i);
                }
            }
        }
        bool __flush_all__() {
            bool ret = false;
            for (auto &it : _pipes) {
                auto &pipe = it.second;
                if (pipe.end()) {
                    ret = true;
                    auto i = pipe.begin();
                    while (i < pipe.end()) {
                        const auto &event = *reinterpret_cast<const Event *>(pipe.data() + i);
                        while (!try_send(event))
                            break;
                        i += event.bucket_size;
                    }
                    pipe.reset(i);
                }
            }
            return ret;
        }
        //!Event Management

        // Workflow
        bool __init__actors__() const {
            // Init StaticActors
            for (const auto &it : _actors) {
                if (!it.second->onInit()) {
                    LOG_WARN << "Actor at " << *this << " failed to init";
                    return false;
                }
            }
            return true;
        }
        bool __init__() {
            bool ret(true);
#if defined(unix) || defined(__unix) || defined(__unix__)
            cpu_set_t cpuset;

            CPU_ZERO(&cpuset);
            CPU_SET(_index, &cpuset);

            pthread_t current_thread = pthread_self();
            ret = !pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
#elif defined(_WIN32) || defined(_WIN64)
            #ifdef _MSC_VER
            DWORD_PTR mask = (1 << (_index < 0 ? 0 : _index));
            ret = (SetThreadAffinityMask(GetCurrentThread(), mask));
#else
#warning "Cannot set affinity on windows with GNU Compiler"
#endif
#endif
            _actor_to_remove.reserve(_actors.size());
            return ret;
        }
        void __wait__all__cores__ready() {
            const auto total_core = _engine.getNbCore();
            Cube::sync_start.fetch_add(1, std::memory_order_acq_rel);
			LOG_INFO << "[READY]" << *this;
            while (Cube::sync_start.load(std::memory_order_acquire) < total_core)
                std::this_thread::yield();
        }
        void __updateTime__() {
            const auto now = Timestamp::nano();
            _nano_timer = now;
        }
        void __spawn__() {
            try {
                __init__actors__();
                if (__init__()) {
                    __wait__all__cores__ready();

                    LOG_INFO << "StartSequence Init " << *this << " Success";
                    while (likely(true)) {
                        __updateTime__();
                        __receive__();

                        for (const auto &callback :  _actor_callbacks)
                            callback.second->onCallback();

                        __flush__();

                        if (unlikely(!_actor_to_remove.empty())) {
                            // remove dead actors
                            for (auto const &actor :  _actor_to_remove)
                                removeActor(actor);
                            _actor_to_remove.clear();
                            if (_actors.empty()) {
                                break;
                            }
                        }
                    }
                    // receive and flush residual events
                    do {
                        __receive__();
                    } while (__flush_all__());
                } else {
                    LOG_CRIT << "StartSequence Init " << *this << " Failed";
                }
            } catch (std::exception &e) {
                LOG_CRIT << "Exception thrown on " << *this
                         << "what:" << e.what();
            }
        }
        //!Workflow

        // Actor Management
        void addActor(Actor *actor) {
            _actors.insert({actor->id(), actor});
            LOG_DEBUG << "New Actor[" << actor->_id << "] Core(" << _index << ")";
        }
        void removeActor(ActorId const &id) {
            const auto it = _actors.find(id);
            if (it != _actors.end()) {
                LOG_DEBUG << "Delete Actor[" << id << "] Core(" << _index << ")";
                delete it->second;
                _actors.erase(id);
                unregisterCallback(id);
            }
        }

        template<typename _Actor, typename ..._Init>
        ActorId addActor(_Init &&...init) {
            auto actor = new _Actor(std::forward<_Init>(init)...);
            actor->_handler = this;
            if constexpr (!std::is_base_of<ServiceActor, _Actor>::value) {
                actor->__set_id(__generate_id__());
            } else {
                actor->_index = _index;
            }
            addActor(actor);

            return actor->id();
        };
        template<typename _Actor
                , typename ..._Init >
        ActorId addActor(std::size_t index, _Init &&...init) {
            return addActor<_Actor>
                    (std::forward<_Init>(init)...);
        }
        template<template<typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        ActorId addActor(std::size_t index, _Init &&...init) {
            return addActor<_Actor<_Trait>>
                    (std::forward<_Init>(init)...);
        }
        //!Actor Management

        void start() {
            _thread = std::thread(&Core::__spawn__, this);
            if (_thread.get_id() == std::thread::id())
                std::runtime_error("failed to start a PhysicalCore");
        }
        void join() {
            _thread.join();
        }

    public:
        template<typename _Actor, typename ..._Init>
        _Actor *addReferencedActor(_Init &&...init) {
            auto actor = new _Actor(std::forward<_Init>(init)...);
            actor->_handler = this;
            if constexpr (!std::is_base_of<ServiceActor, _Actor>::value) {
                actor->__set_id(__generate_id__());
            } else {
                actor->_index = _index;
            }

            if (unlikely(!actor->onInit())) {
                delete actor;
                return nullptr;
            }
            addActor(actor);

            return actor;
        };
        template<template <typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init>
        auto addReferencedActor(_Init &&...init) {
            return addReferencedActor<_Actor<_Trait>>
                    (std::forward<_Init>(init)...);
        }
        void killActor(ActorId const &id) {
            _actor_to_remove.push_back(id);
        }

        template <typename _Actor>
        void registerCallback(_Actor &actor) {
            _actor_callbacks.insert({actor.id(), &actor});
        }
        void unregisterCallback(ActorId const &id) {
            auto it =_actor_callbacks.find(id);
            if (it != _actor_callbacks.end())
                _actor_callbacks.erase(it);
        }

    public:
        //Event Api
        ProxyPipe getPipeProxy(ActorId const dest, ActorId const source) {
            return {__getPipe__(dest._index), dest, source};
        }
        bool try_send(Event const &event) const {
            if (event.dest._index == _index) {
                _actors.find(event.dest)->second->on(const_cast<Event *>(&event));
                return true;
            }
            return _engine.send(event);
        }
        void send(Event const &event) {
            if (unlikely(!try_send(event))) {
                auto &pipe = __getPipe__(event.dest._index);
                pipe.recycle(event, event.bucket_size);
            }
        }
        auto &push(Event const &event) {
            auto &pipe = __getPipe__(event.dest._index);
            return pipe.recycle_back(event, event.bucket_size);
        }
        void reply(Event &event) {
            std::swap(event.dest, event.source);
            event.state[0] = 1;
            send(event);
        }
        void forward(ActorId const dest, Event &event) {
            event.source = event.dest;
            event.dest = dest;
            event.state[0] = 1;
            send(event);
        }

        template<typename T, typename ..._Init>
        void send(ActorId const dest, ActorId const source, _Init &&...init) {
            auto &pipe = __getPipe__(dest._index);
            auto &data = pipe.template allocate<T>(std::forward<_Init>(init)...);
            data.id = type_id<T>();
            data.dest = dest;
            data.source = source;
            if constexpr (std::is_base_of<ServiceEvent, T>::value) {
                data.forward = source;
                std::swap(data.id, data.service_event_id);
            }
            data.state = 0;
            data.bucket_size = allocator::getItemSize<T, CacheLine>();
            if (likely(_engine.send(data)))
                pipe.free(data.bucket_size);
        }
        template<typename T, typename ..._Init>
        T &push(ActorId const &dest, ActorId const &source, _Init &&...init) {
            auto &pipe = __getPipe__(dest._index);
            auto &data = pipe.template allocate_back<T>(std::forward<_Init>(init)...);
            data.id = type_id<T>();
            data.dest = dest;
            data.source = source;
            if constexpr (std::is_base_of<ServiceEvent, T>::value) {
                data.forward = source;
                std::swap(data.id, data.service_event_id);
            }

            data.state = 0;
            data.bucket_size = allocator::getItemSize<T, CacheLine>();
            return data;
        }
        template<typename T, typename ..._Init>
        T &fast_push(ActorId const &dest, ActorId const &source, _Init &&...init) {
            auto &pipe = __getPipe__(dest._index);
            auto &data = pipe.template allocate_back<T>(std::forward<_Init>(init)...);
            data.id = type_id<T>();
            data.dest = dest;
            data.source = source;
            if constexpr (std::is_base_of<ServiceEvent, T>::value) {
                data.forward = source;
                std::swap(data.id, data.service_event_id);
            }

            data.state = 0;
            data.bucket_size = allocator::getItemSize<T, CacheLine>();
            if (likely(_engine.send(data)))
                pipe.free_back(data.bucket_size);
        }
        //!Event Api

    public:

        uint16_t getIndex() const {
            return _index;
        }
        uint64_t bestTime() const {
            return Cube::sync_start.load();
        }
        uint64_t time() const {
            return _nano_timer;
        }

    };

}

#endif //CUBE_CORE_H
