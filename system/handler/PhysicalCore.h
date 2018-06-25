
#ifndef CUBE_PHYSICALCORE_H
# define CUBE_PHYSICALCORE_H
#if defined(unix) || defined(__unix) || defined(__unix__)
#include <sched.h>
    #include <errno.h>
    #include <unistd.h>
    #include <pthread.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <process.h>
#endif
# include "Pipe.h"

namespace cube {
    using namespace std::chrono;

    template<typename _ParentHandler, std::size_t _CoreIndex, typename _SharedData>
    class PhysicalCoreHandler
            : nocopy
    {
        friend _ParentHandler;
        typedef _ParentHandler parent_t;
    public:
        //////// Constexpr
        constexpr static const std::uint64_t MaxBufferEvents = ((std::numeric_limits<uint16_t>::max)());
        constexpr static const std::uint64_t MaxRingEvents = ((std::numeric_limits<uint16_t>::max)()) / CUBE_LOCKFREE_CACHELINE_BYTES;
        constexpr static const std::size_t nb_core = 1;
        //////// Types
        using SPSCBuffer = lockfree::spsc::ringbuffer<CacheLine, MaxRingEvents>;
        using MPSCBuffer = lockfree::mpsc::ringbuffer<CacheLine, MaxRingEvents, 0>;
        using EventBuffer = std::array<CacheLine, MaxRingEvents>;

        class IActor {
        public:
            virtual ~IActor() {}

            virtual bool onInit() = 0;
            virtual void onEvent(Event *) = 0;
        };

        class ICallback {
        public:
            virtual ~ICallback() {}
            virtual void onCallback() = 0;
        };

        struct ActorProxy {
            uint64_t const _id;
            IActor *_this;
            void *const _handler;

            ActorProxy() : _id(0), _this(nullptr), _handler(nullptr) {}
            ActorProxy(uint64_t const id, IActor *actor, void *const handler)
                    : _id(id), _this(actor), _handler(handler) {
            }
        };

        static inline ActorId generate_id() {
            static std::size_t pid = 0;
            return ActorId(static_cast<uint32_t >(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() + pid++), _CoreIndex);
        }

    private:
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
            for (int i = 0; i < 3; ++i) {
                if (static_cast<bool>(_eventManager->_spsc_buffer.enqueue(reinterpret_cast<CacheLine const *>(&event),
                                                                          event.bucket_size)))
                    return true;
                std::this_thread::yield();
            }

            return false;
        }

        inline bool receive_from_unlinked_core(Event const &event) {
            for (int i = 0; i < 3; ++i) {
                if (static_cast<bool>(_eventManager->_mpsc_buffer.enqueue(reinterpret_cast<CacheLine const *>(&event),
                                                                          event.bucket_size)))
                    return true;
                std::this_thread::yield();
            }

            return false;
        }

    public:
        using Pipe = pipe_allocator<CacheLine>;
    private:
        class EventManager : nocopy {
            friend class PhysicalCoreHandler;

            using PipeMap = std::unordered_map<uint32_t, Pipe>;
            PhysicalCoreHandler &_core;
            SPSCBuffer _spsc_buffer;
            MPSCBuffer _mpsc_buffer;
            EventBuffer _event_buffer;
            PipeMap _pipes;

            EventManager(PhysicalCoreHandler &core)
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
                        ret = false;
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
                return ret;
            }

            Pipe &getPipe(uint32_t core) {
                return _pipes[core];
            }

            void __receive(CacheLine *buffer, std::size_t const nb_events) {
                if (!nb_events)
                    return;
                uint64_t i = 0;
                while (i < nb_events) {
                    auto event = reinterpret_cast<Event *>(buffer + i);
                    auto actor = _core._actors.find(event->dest);
                    if (likely(actor != std::end(_core._actors))) {
                        actor->second._this->onEvent(event);
                    } else {
                        LOG_WARN << "Failed Event" << _core
                                 << " [Source](" << event->source << ")"
                                 << " [Dest](" << event->dest << ") NOT FOUND";
                    }
                    //assert(i + nb_buckets > nb_events);
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
            _ParentHandler::parent_t::sync_start.store(std::numeric_limits<uint64_t >::max());
        }

        void updateTimer() {
            const auto now = Timestamp::nano();
            auto best = getBestTime();
            _nano_timer = now - _nano_timer;
            if (reinterpret_cast<uint8_t const *>(&best)[sizeof(_nano_timer) - 1] == _index) {
                if (_nano_timer > best) {
                    reinterpret_cast<uint8_t *>(&_nano_timer)[sizeof(_nano_timer) - 1] = _index;
                    _ParentHandler::parent_t::sync_start.store(_nano_timer);
                }
            } else if (_nano_timer < best) {
                reinterpret_cast<uint8_t *>(&_nano_timer)[sizeof(_nano_timer) - 1] = _index;
                _ParentHandler::parent_t::sync_start.store(_nano_timer);
            }
            _nano_timer = now;
        }

        void __workflow() {
            if (init()) {
                __wait__all__cores__ready();

                LOG_INFO << "StartSequence Init " << *this << " Success";
                while (likely(true)) {
                    updateTimer();
                    _eventManager->receive();

                    for (const auto &callback : _actor_callbacks)
                        callback.second->onCallback();

                    _eventManager->flush();

                    if (unlikely(!_actor_to_remove.empty())) {
                        // remove dead actor
                        for (auto const &actor : _actor_to_remove)
                            removeActor(actor);
                        _actor_to_remove.clear();
                        if (_actors.empty()) {
                            break;
                        }
                    }
                }
                // receive and flush residual events
                _eventManager->receive();
                while (_eventManager->flush_all())
                    std::this_thread::yield();
            } else {
                LOG_CRIT << "StartSequence Init " << *this << " Failed";
            }
        }

        //////// Members
        _ParentHandler  *_parent = nullptr;
        EventManager    *_eventManager = nullptr;
        _SharedData     *_sharedData = nullptr;
        std::thread      _thread;

        std::unordered_map<uint64_t, ActorProxy>  _actors;
        std::unordered_map<uint64_t, ICallback *> _actor_callbacks;
        std::vector<ActorId> _actor_to_remove;
        std::uint64_t _nano_timer;
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
            _thread = std::thread(&PhysicalCoreHandler::__workflow, this);
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
            LOG_DEBUG << "New Actor[" << actor._id << "] in " << *this;
        }

        inline void removeActor(ActorId const &id) {
            const auto it = _actors.find(id);
            if (it != _actors.end()) {
                LOG_DEBUG << "Delete Actor[" << id << "] in " << *this;
                delete it->second._this;
                _actors.erase(id);
                unRegisterCallback(id);
            }
        }

        template<std::size_t _CoreIndex_
                , typename _Actor
                , typename ..._Init>
        inline ActorId addActor(_Init &&...init) {
            auto actor = new _Actor(std::forward<_Init>(init)...);
            actor->_handler = this;
            addActor(actor->proxy());

            return actor->id();
        };

        template< std::size_t _CoreIndex_
                , template<typename _Handler> typename _Actor
                , typename ..._Init >
        ActorId addActor(_Init &&...init) {
            if constexpr (_CoreIndex_ == _index) {
                return addActor<_CoreIndex_, _Actor<PhysicalCoreHandler>>
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
                return addActor<_CoreIndex_, _Actor<PhysicalCoreHandler, _Trait>>
                        (std::forward<_Init>(init)...);
            }
            return ActorId::NotFound{};
        }

    public:
        constexpr static const std::size_t _index = _CoreIndex;

        PhysicalCoreHandler() = delete;
        PhysicalCoreHandler(_ParentHandler *parent)
                : _parent(parent)
                , _eventManager(new EventManager(*this))
        {}

        ~PhysicalCoreHandler() {
            for (auto &it : _actors) {
                delete it.second._this;
            }

            if (_eventManager)
                delete _eventManager;
            if constexpr (!std::is_void<_SharedData>::value)
                delete _sharedData;
            LOG_INFO << "Deleted " << *this;
        }

        uint64_t getTime() const {
            return _nano_timer;
        }

        uint64_t getBestTime() const {
           return _ParentHandler::parent_t::sync_start.load();
        }

        uint32_t getBestCore() const {
            const auto best_time = getBestTime();
            LOG_INFO << "BEST TIME[" << best_time << "]";
            return reinterpret_cast<uint8_t const *>(&best_time)[sizeof(_nano_timer) - 1];
        }

        template<typename _Actor, typename ..._Init>
        _Actor *addReferencedActor(_Init &&...init) {
            auto actor = new _Actor(std::forward<_Init>(init)...);
            actor->_handler = this;

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

        void unRegisterCallback(ActorId const &id) {
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
            return addReferencedActor<_Actor<PhysicalCoreHandler>>
                    (std::forward<_Init>(init)...);
        }

        template< template <typename _Handler, typename _Trait> typename _Actor
                , typename _Trait
                , typename ..._Init >
        inline auto addReferencedActor(_Init &&...init) {
            return addReferencedActor<_Actor<PhysicalCoreHandler, _Trait>>
                    (std::forward<_Init>(init)...);
        }

    public:
        //sender
        inline bool try_send(Event const &event) {
            return _parent->send(event);
        }

        auto &push(Event const &event) {
            auto &pipe = _eventManager->getPipe(event.dest._index);
            auto &data = pipe.template recycle(event, event.bucket_size);
            return data;
        }

        void send(Event const &event) {
            if (unlikely(!try_send(event)))
                push(event);
        }

        template <typename T, typename ..._Init>
        void send(ActorId const dest, ActorId const source, _Init &&...init) {
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
        T &push(ActorId const &dest, ActorId const &source, _Init &&...init) {
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
        T &fast_push(ActorId const &dest, ActorId const &source, _Init &&...init) {
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

        inline auto &sharedData() {
            return *_sharedData;
        }
    };

    template<typename _ParentHandler, std::size_t _CoreIndex, typename _SharedData>
    cube::io::stream &operator<<(cube::io::stream &os, PhysicalCoreHandler<_ParentHandler, _CoreIndex, _SharedData> const &core) {
        std::stringstream ss;
        ss << "PhysicalCore(" << core._index << ").id(" << std::this_thread::get_id() << ")";
        os << ss.str();
        return os;
    };

    using ServiceHandler = PhysicalCoreHandler<void, 0, void>;
}


#endif //CUBE_PHYSICALCORE_H
