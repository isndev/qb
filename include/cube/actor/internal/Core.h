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
#include <Windows.h>
#include <process.h>
#undef max
#undef min
#endif

// include from cube
# include <cube/system/timestamp.h>
# include <cube/system/allocator/pipe.h>
# include <cube/system/lockfree/mpsc.h>
# include "ICallback.h"
# include "ProxyPipe.h"
# include "Event.h"
# include "Actor.h"
# include "Main.h"

# define SERVICE_ACTOR_INDEX 10000

class Core;
cube::io::stream &operator<<(cube::io::stream &os, cube::Core const &core);

namespace cube {

    class Core {
        friend class Main;
    public:
        ////////////
        constexpr static const uint64_t MaxRingEvents = ((std::numeric_limits<uint16_t>::max)() + 1) / CUBE_LOCKFREE_CACHELINE_BYTES * 4;
        // Types
        using MPSCBuffer = Main::MPSCBuffer;
        using EventBuffer = std::array<CacheLine, MaxRingEvents>;
        using ActorMap = std::unordered_map<uint32_t, Actor *>;
        using CallbackMap = std::unordered_map<uint32_t, ICallback *>;
        using PipeMap = std::unordered_map<uint32_t, Pipe>;
        using RemoveActorList = std::vector<ActorId>;
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

        Core() = delete;
        Core(uint8_t const id, Main &engine);

        ActorId __generate_id__();

        // Event Management
        Pipe &__getPipe__(uint32_t core);
        void __receive_events__(CacheLine *buffer, std::size_t const nb_events);
        void __receive__();
        void __flush__();
        bool __flush_all__();
        //!Event Management

        // Workflow
        bool __init__actors__() const;
        bool __init__();
        void __wait__all__cores__ready();
        void __updateTime__();
        void __spawn__();
        //!Workflow

        // Actor Management
        template<typename _Actor>
        bool initActor(_Actor * const actor, bool doinit);
        void addActor(Actor *actor);
        void removeActor(ActorId const &id);

        template<typename _Actor, typename ..._Init>
        ActorId addActor(_Init &&...init);

        template<typename _Actor
                , typename ..._Init >
        ActorId addActor(std::size_t index, _Init &&...init);
        template<template<typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        ActorId addActor(std::size_t index, _Init &&...init);
        //!Actor Management

        void start();
        void join();

    public:
        template<typename _Actor, typename ..._Init>
        _Actor *addReferencedActor(_Init &&...init);
        template<template <typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init>
        auto addReferencedActor(_Init &&...init);

        void killActor(ActorId const &id);

        template <typename _Actor>
        void registerCallback(_Actor &actor);
        void unregisterCallback(ActorId const &id);

    public:
        // Event Api
        ProxyPipe getPipeProxy(ActorId const dest, ActorId const source);
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
        uint64_t bestTime() const;
        uint64_t time() const;

    };

}

#endif //CUBE_CORE_H
