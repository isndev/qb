
#ifndef CUBE_BASECOREHANDLER_H
#define CUBE_BASECOREHANDLER_H
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
# include "../actor/Event.h"

namespace cube {

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT CacheLine {
        uint32_t __raw__[16];
    };

    template<std::size_t _CoreIndex, typename _ParentHandler, typename _Derived, typename _SharedData>
    class BaseCoreHandler
            : nocopy
    {
        friend _ParentHandler;
        typedef _ParentHandler parent_t;

    public:
        //////// Constexpr
        constexpr static const std::size_t _index = _CoreIndex;
        constexpr static const uint64_t MaxBufferEvents = ((std::numeric_limits<uint16_t>::max)());
        constexpr static const uint64_t MaxRingEvents = ((std::numeric_limits<uint16_t>::max)()) / CUBE_LOCKFREE_CACHELINE_BYTES;
        constexpr static const std::size_t nb_core = 1;
        //////// Types
        using SPSCBuffer = lockfree::spsc::ringbuffer<CacheLine, MaxRingEvents>;
        using MPSCBuffer = lockfree::mpsc::ringbuffer<CacheLine, MaxRingEvents, 0>;
        using EventBuffer = std::array<CacheLine, MaxRingEvents>;

        class IActor {
            class IRegisterEvent {
            public:
                virtual ~IRegisterEvent() {}
                virtual void invoke(Event *data) const = 0;
            };

            template<typename _Data, typename _Actor>
            class RegisterEvent : public IRegisterEvent {
                _Actor &_actor;
            public:
                RegisterEvent(_Actor &actor)
                        : _actor(actor) {}

                virtual void invoke(Event *data) const override final {
                    auto &event = *reinterpret_cast<_Data *>(data);
                    _actor.on(event);
                    if (!event.state[0])
                        event.~_Data();
                }
            };

            std::unordered_map<uint32_t, IRegisterEvent const *> _event_map;
        public:
            IActor() {
                _event_map.reserve(64);
            }

            virtual ~IActor() {
                for (const auto &revent : _event_map)
                    delete revent.second;
            }

            virtual bool onInit() = 0;

            void on(Event *event) const {
                // TODO: secure this if event not registred
                // branch fetch find
                _event_map.at(event->id)->invoke(event);
            }

        public:
            template<typename _Data, typename _Actor>
            inline void registerEvent(_Actor &actor) {
                auto it = _event_map.find(type_id<_Data>());
                if (it != _event_map.end())
                    delete it->second;
                _event_map.insert_or_assign(type_id<_Data>(), new RegisterEvent<_Data, _Actor>(actor));
            };

            template<typename _Data, typename _Actor>
            inline void unregisterEvent(_Actor &actor) {
                auto it = _event_map.find(type_id<_Data>());
                if (it != _event_map.end())
                    delete it->second;
                _event_map.insert_or_assign(type_id<_Data>(), new RegisterEvent<Event, _Actor>(actor));
            };
        };

        class ICallback {
        public:
            virtual ~ICallback() {}
            virtual void onCallback() = 0;
        };

        struct ActorProxy {
            uint64_t const _id;
            IActor*  const _this;

            ActorProxy() : _id(0), _this(nullptr){}
            ActorProxy(uint64_t const id, IActor *actor)
                    : _id(id), _this(actor) {
            }
        };

        static inline ActorId generate_id() {
            static std::size_t pid = 10000;
            //duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()
            //Timestamp::nano() % std::numeric_limits<uint32_t>::max() + pid++)
            return ActorId(static_cast<uint32_t >(++pid), _CoreIndex);
        }

    protected:
        using parent_ptr_t = _ParentHandler*;

        //////// Event Manager
        //// Receiver From Handler
        inline bool receive_from_different_core(Event const &event, bool &ret) {
            if (_index != event.dest._index)
                return false;
            ret = receive_from_unlinked_core(event);
            return true;
        }

        inline bool receive_from_linked_core(Event const &event) {
            return static_cast<bool>(_eventManager->_spsc_buffer.enqueue(reinterpret_cast<CacheLine const *>(&event), event.bucket_size));
        }

        inline bool receive_from_unlinked_core(Event const &event) {
            return static_cast<bool>(_eventManager->_mpsc_buffer.enqueue(reinterpret_cast<CacheLine const *>(&event), event.bucket_size));
        }

    public:
        using Pipe = allocator::pipe<CacheLine>;

        class ProxyPipe {
            ActorId dest;
            ActorId source;
            Pipe *pipe;

            template <typename T = cube::CacheLine>
            T *allocate(std::size_t &size) {
                if (size % sizeof(cube::CacheLine))
                    size = size * sizeof(T) / sizeof(CacheLine) + 1;
                else
                    size /= sizeof(CacheLine);

                return reinterpret_cast<T*>(pipe->allocate_back(size));
            }
        public:
            ProxyPipe() = default;
            ProxyPipe(ProxyPipe const &) = default;
            ProxyPipe &operator=(ProxyPipe const &) = default;
            ProxyPipe(Pipe &pipe, ActorId dest, ActorId source)
                    : pipe(&pipe), dest(dest), source(source)
            {}

            template <typename T, typename ..._Init>
            T &push(_Init &&...init) {
                auto &data = pipe->template allocate_back<T>(std::forward<_Init>(init)...);
                data.id = type_id<T>();
                data.dest = dest;
                data.source = source;
                if constexpr (std::is_base_of<ServiceEvent, T>::value) {
                    data.forward = source;
                    std::swap(data.id, data.service_event_id);
                }

                data.state = 0;
                data.bucket_size = sizeof(T) / CUBE_LOCKFREE_CACHELINE_BYTES;
                return data;
            }

            template <typename T, typename ..._Init>
            T &allocated_push(std::size_t size, _Init &&...init) {
                size += sizeof(T);
                auto &data = *(new (reinterpret_cast<T *>(this->template allocate<char>(size))) T(std::forward<_Init>(init)...));

                data.id = type_id<T>();
                data.dest = dest;
                data.source = source;
                if constexpr (std::is_base_of<ServiceEvent, T>::value) {
                    data.forward = source;
                    std::swap(data.id, data.service_event_id);
                }

                data.state = 0;
                data.bucket_size = size;
                return data;
            }


        };

    protected:
        class EventManager : nocopy {
            friend class BaseCoreHandler;

            using PipeMap = std::unordered_map<uint32_t, Pipe>;
            BaseCoreHandler &_core;
            SPSCBuffer _spsc_buffer;
            MPSCBuffer _mpsc_buffer;
            EventBuffer _event_buffer;
            PipeMap _pipes;

            EventManager(BaseCoreHandler &core)
                    : _core(core)
                    , _mpsc_buffer(_ParentHandler::parent_t::total_core - _ParentHandler::linked_core)
            {}

            void flush() {
                for (auto &it : _pipes) {
                    auto &pipe = it.second;
                    if (pipe.end()) {
                        auto i = pipe.begin();
                        while (i < pipe.end()) {
                            const auto &event = *reinterpret_cast<const Event *>(pipe.data() + i);
                            if (!_core.try_send(event))
                                break;
                            i += event.bucket_size;
                        }
                        pipe.reset(i);
                    }
                }
            }

            bool flush_all() {
                bool ret = false;
                for (auto &it : _pipes) {
                    auto &pipe = it.second;
                    if (pipe.end()) {
                        ret = true;
                        auto i = pipe.begin();
                        while (i < pipe.end()) {
                            const auto &event = *reinterpret_cast<const Event *>(pipe.data() + i);
                            while (!_core.try_send(event))
                                break;
                            i += event.bucket_size;
                        }
                        pipe.reset(i);
                    }
                }
                return ret;
            }

            Pipe &getPipe(uint32_t core) {
                return _pipes[core];
            }

            void __receive(CacheLine *buffer, std::size_t const nb_events) {
                if (!nb_events)
                    return;
                std::size_t i = 0;
                while (i < nb_events) {
                    auto event = reinterpret_cast<Event *>(buffer + i);
                    auto actor = _core._actors.find(event->dest);
                    if (likely(actor != std::end(_core._actors))) {
                        actor->second._this->on(event);
                    } else {
                        LOG_WARN << "Failed Event" << _core
                                 << " [Source](" << event->source << ")"
                                 << " [Dest](" << event->dest << ") NOT FOUND";
                    }

                    i += event->bucket_size;
                }
                LOG_DEBUG << "Events " << _core << " received " << nb_events << " buckets";
            }

            void receive() {
                if constexpr (!std::is_same<_ParentHandler, typename _ParentHandler::parent_t>::value) {
                    // linked_core_events
                    __receive(_event_buffer.data(), _spsc_buffer.dequeue(_event_buffer.data(), MaxRingEvents));
                }

                // global_core_events
                _mpsc_buffer.dequeue([this](CacheLine *buffer, std::size_t const nb_events){
                    __receive(buffer, nb_events);
                }, _event_buffer.data(), MaxRingEvents);

            }

        };

        /////// !Event Manager

        static void __wait__all__cores__ready() {
            const auto total_core = _ParentHandler::parent_t::total_core;
            ++_ParentHandler::parent_t::sync_start;
            while (_ParentHandler::parent_t::sync_start.load() < total_core)
                std::this_thread::yield();
            _ParentHandler::parent_t::sync_start.store((std::numeric_limits<uint64_t >::max)());
        }

        inline void onCallback() {}

        void spawn() {
            if (init() && static_cast<_Derived &>(*this).onInit()) {
                __wait__all__cores__ready();

                LOG_INFO << "StartSequence Init " << *this << " Success";
                while (likely(true)) {
                    static_cast<_Derived &>(*this).onCallback();
                    _eventManager->receive();

                    for (const auto &callback :  _actor_callbacks)
                        callback.second->onCallback();

                    _eventManager->flush();

                    if (unlikely(!_actor_to_remove.empty())) {
                        // remove dead actors
                        for (auto const &actor :  _actor_to_remove)
                            removeActor(actor);
                        _actor_to_remove.clear();
                        if ( _actors.empty()) {
                            break;
                        }
                    }
                }
                // receive and flush residual events
                do {
                    _eventManager->receive();
                }
                while (_eventManager->flush_all());
            } else {
                LOG_CRIT << "StartSequence Init " << *this << " Failed";
            }
        }

        //////// Members
        _ParentHandler  *_parent = nullptr;
        EventManager    *_eventManager = nullptr;
        _SharedData     *_sharedData = nullptr;
        std::thread      _thread;

        std::unordered_map<uint32_t, ActorProxy>  _actors;
        std::unordered_map<uint32_t, ICallback *> _actor_callbacks;
        std::vector<ActorId> _actor_to_remove;

        //////// !Members
    public:
        // Start Sequence Usage
        void __init__shared_data() {
            if constexpr (!std::is_void<_SharedData>::value) {
                if (_sharedData == nullptr)
                    _sharedData = new _SharedData();
            }
        }

        void __init__actors() const {
            // Init StaticActors
            for (const auto &it : _actors) {
                if (!it.second._this->onInit())
                    LOG_WARN << "Actor at " << *this << " failed to init";
            }
        }

        template <std::size_t _CoreIndex_, typename ..._Init>
        bool __init_shared(_Init &&...init) {
            if constexpr (_CoreIndex_ == _CoreIndex) {
                _sharedData = new _SharedData(std::forward<_Init>(init)...);
                return true;
            }
            return false;
        }

        void __start() {
            _thread = std::thread(&BaseCoreHandler::spawn, this);
            if (_thread.get_id() == std::thread::id())
                std::runtime_error("failed to start a PhysicalCore");
        }

        void __join() {
            _thread.join();
        }

        bool init() {
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

        inline void addActor(ActorProxy const &actor) {
            _actors.insert({actor._id, actor});
            LOG_DEBUG << "New Actor[" << actor._id << "] Core(" << _index << ")";
        }

        inline void removeActor(ActorId const &id) {
            const auto it = _actors.find(id);
            if (it != _actors.end()) {
                LOG_DEBUG << "Delete Actor[" << id << "] Core(" << _index << ")";
                delete it->second._this;
                _actors.erase(id);
                unregisterCallback(id);
            }
        }

        template<typename _Actor
                , typename ..._Init>
        inline ActorId addActor(_Init &&...init) {
            auto actor = new _Actor(std::forward<_Init>(init)...);
            actor->_handler = static_cast<_Derived *>(this);
            addActor(actor->proxy());

            return actor->id();
        };

        template< std::size_t _CoreIndex_
                , template<typename _Handler> typename _Actor
                , typename ..._Init >
        ActorId addActor(_Init &&...init) {
            if constexpr (_CoreIndex_ == _index) {
                return addActor<_Actor<_Derived>>
                        (std::forward<_Init>(init)...);
            }
            return ActorId::NotFound{};
        }

        template< std::size_t _CoreIndex_
                , template<typename _Handler, typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        ActorId addActor(_Init &&...init) {
            if constexpr (_CoreIndex_ == _index) {
                return addActor<_Actor<_Derived, _Trait>>
                        (std::forward<_Init>(init)...);
            }
            return ActorId::NotFound{};
        }

        template<template<typename _Handler> typename _Actor
                , typename ..._Init >
        ActorId addActor(std::size_t index, _Init &&...init) {
            if  (index == _index) {
                return addActor<_Actor<_Derived>>
                        (std::forward<_Init>(init)...);
            }
            return ActorId::NotFound{};
        }

        template<template<typename _Handler, typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        ActorId addActor(std::size_t index, _Init &&...init) {
            if (index == _index) {
                return addActor<_Actor<_Derived, _Trait>>
                        (std::forward<_Init>(init)...);
            }
            return ActorId::NotFound{};
        }

    public:

        BaseCoreHandler() = delete;
        BaseCoreHandler(_ParentHandler *parent)
                : _parent(parent)
                , _eventManager(new EventManager(*this))
        {}

        ~BaseCoreHandler() {
            for (auto &it : _actors) {
                delete it.second._this;
            }

            if (_eventManager)
                delete _eventManager;
            if constexpr (!std::is_void<_SharedData>::value)
                delete _sharedData;
            LOG_INFO << "Deleted " << *this;
        }

        template<typename _Actor, typename ..._Init>
        _Actor *addReferencedActor(_Init &&...init) {
            auto actor = new _Actor(std::forward<_Init>(init)...);
            actor->_handler = static_cast<_Derived *>(this);

            if (unlikely(!actor->onInit())) {
                delete actor;
                return nullptr;
            }
            addActor(actor->proxy());

            return actor;
        };

        template <typename _Actor>
        void registerCallback(_Actor &actor) {
            _actor_callbacks.insert({actor.id(), &actor});
        }

        void unregisterCallback(ActorId const &id) {
            auto it =_actor_callbacks.find(id);
            if (it != _actor_callbacks.end())
                _actor_callbacks.erase(it);
        }

        void killActor(ActorId const &id) {
            _actor_to_remove.push_back(id);
        }

        template< template <typename _Handler> typename _Actor
                , typename ..._Init >
        inline auto addReferencedActor(_Init &&...init) {
            return addReferencedActor<_Actor<_Derived>>
                    (std::forward<_Init>(init)...);
        }

        template< template <typename _Handler, typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        inline auto addReferencedActor(_Init &&...init) {
            return addReferencedActor<_Actor<_Derived, _Trait>>
                    (std::forward<_Init>(init)...);
        }

    public:
        // proxy pipe
        inline ProxyPipe getProxyPipe(ActorId const dest, ActorId const source) const {
            return {_eventManager->getPipe(dest._index), dest, source};
        }
        //sender
        inline bool try_send(Event const &event) const {
            if (event.dest._index == _index) {
                _actors.find(event.dest)->second._this->on(const_cast<Event *>(&event));
                return true;
            }
            return _parent->send(event);
        }

        auto &push(Event const &event) const {
            auto &pipe = _eventManager->getPipe(event.dest._index);
            return pipe.recycle_back(event, event.bucket_size);
        }

        void send(Event const &event) const {
            if (unlikely(!try_send(event))) {
                auto &pipe = _eventManager->getPipe(event.dest._index);
                pipe.recycle(event, event.bucket_size);
            }
        }

        template <typename T, typename ..._Init>
        void send(ActorId const dest, ActorId const source, _Init &&...init) const {
            auto &pipe = _eventManager->getPipe(dest._index);
            auto &data = pipe.template allocate<T>(std::forward<_Init>(init)...);
            data.id = type_id<T>();
            data.dest = dest;
            data.source = source;
            if constexpr (std::is_base_of<ServiceEvent, T>::value) {
                data.forward = source;
                std::swap(data.id, data.service_event_id);
            }
            data.state = 0;
            data.bucket_size = sizeof(T) / CUBE_LOCKFREE_CACHELINE_BYTES;
            if (likely(_parent->send(data)))
                pipe.free(data.bucket_size);
        }

        template <typename T, typename ..._Init>
        T &push(ActorId const &dest, ActorId const &source, _Init &&...init) const {
            auto &pipe = _eventManager->getPipe(dest._index);
            auto &data = pipe.template allocate_back<T>(std::forward<_Init>(init)...);
            data.id = type_id<T>();
            data.dest = dest;
            data.source = source;
            if constexpr (std::is_base_of<ServiceEvent, T>::value) {
                data.forward = source;
                std::swap(data.id, data.service_event_id);
            }

            data.state = 0;
            data.bucket_size = sizeof(T) / CUBE_LOCKFREE_CACHELINE_BYTES;
            return data;
        }

        template <typename T, typename ..._Init>
        T &fast_push(ActorId const &dest, ActorId const &source, _Init &&...init) const {
            auto &pipe = _eventManager->getPipe(dest._index);
            auto &data = pipe.template allocate_back<T>(std::forward<_Init>(init)...);
            data.id = type_id<T>();
            data.dest = dest;
            data.source = source;
            if constexpr (std::is_base_of<ServiceEvent, T>::value) {
                data.forward = source;
                std::swap(data.id, data.service_event_id);
            }

            data.state = 0;
            data.bucket_size = sizeof(T) / CUBE_LOCKFREE_CACHELINE_BYTES;
            if (likely(_parent->send(data)))
                pipe.free_back(data.bucket_size);
        }

        void reply(Event &event) const {
            std::swap(event.dest, event.source);
            event.state[0] = 1;
            send(event);
        }

        void forward(ActorId const dest, Event &event) const {
            event.source = event.dest;
            event.dest = dest;
            event.state[0] = 1;
            send(event);
        }

        inline auto &sharedData() const {
            return *_sharedData;
        }

        uint64_t bestTime() const {
            return _ParentHandler::parent_t::sync_start.load();
        }

        uint32_t bestCore() const {
            const auto best_time = bestTime();
            return reinterpret_cast<uint8_t const *>(&best_time)[0];
        }

        uint64_t time() const {
            return 0;
        }

    };

    template<std::size_t _CoreIndex, typename _ParentHandler, typename _Derived, typename _SharedData>
    cube::io::stream &operator<<(cube::io::stream &os, BaseCoreHandler<_CoreIndex, _ParentHandler, _Derived, _SharedData> const &core) {
        std::stringstream ss;
        ss << "PhysicalCore(" << core._index << ").id(" << std::this_thread::get_id() << ")";
        os << ss.str();
        return os;
    };

}

#endif //CUBE_BASECOREHANDLER_H
