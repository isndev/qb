
#ifndef CUBE_PHYSICALCORE_H
# define CUBE_PHYSICALCORE_H
#if defined(unix) || defined(__unix) || defined(__unix__)
#define __USE_GNU
    #include <sched.h>
    #include <errno.h>
    #include <unistd.h>
    #include <pthread.h>
#elif defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    #include <process.h>
#endif
# include "Types.h"

namespace cube {
    using namespace std::chrono;

    template<typename _ParentHandler, std::size_t _CoreIndex, typename _SharedData>
    class PhysicalCoreHandler
		: public nocopy 
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
        using PipeBuffer = std::vector<CacheLine>;
        using EventBuffer = std::array<CacheLine, MaxRingEvents>;

        class IActor {
        public:
            virtual ~IActor() {}

            virtual ActorStatus init() = 0;
            virtual ActorStatus main() = 0;
            virtual void hasEvent(Event const *) = 0;
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

    private:
		using parent_ptr_t = _ParentHandler*;

        inline ActorId __generate_id() const {
            static std::size_t pid = 0;
            return ActorId(static_cast<uint32_t >(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() + pid++), _CoreIndex);
        }

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
                if (static_cast<bool>(_eventManager->_mpsc_buffer.enqueue(event.source._index,
                                                                          reinterpret_cast<CacheLine const *>(&event),
                                                                          event.bucket_size)))
                    return true;
                std::this_thread::yield();
            }

            return false;
        }

        //// Pipe Events Queue
        class Pipe : public nocopy {
            friend class PhysicalCoreHandler;
            friend class PhysicalCoreHandler::EventManager;

            std::size_t _begin;
            std::size_t _end;
            char __padding2__[CUBE_LOCKFREE_CACHELINE_BYTES - sizeof(std::size_t)];
            PipeBuffer _buffer;

            inline void setBegin(std::size_t const begin) {
                _begin = begin;
            }

            inline std::size_t begin() const {
                return _begin;
            }

            inline std::size_t end() const {
                return _end;
            }

            inline void free(std::size_t const size) {
                _end -= size;
            }

            inline void reset() {
                _begin = 0;
                _end = 0;
            }

            inline void check_allocation_size(std::size_t const size) {
                if (unlikely((_end + size) >= _buffer.size())) {
                    _buffer.resize(_buffer.size() + MaxBufferEvents);
                }
            }

            template<typename T, typename ..._Init>
            T &allocate(_Init &&...init) {
                constexpr std::size_t BUCKET_SIZE = (sizeof(T) / CUBE_LOCKFREE_CACHELINE_BYTES);
                check_allocation_size(BUCKET_SIZE);

                T &ret = *(new (reinterpret_cast<T *>(_buffer.data() + _end)) T(std::forward<_Init>(init)...));
                ret.id = type_id<T>();
                ret.bucket_size = BUCKET_SIZE;
                ret.alive = 0;
                _end += BUCKET_SIZE;
                return ret;
            }

            template<typename T>
            T &recycle(T const &data) {
                constexpr std::size_t BUCKET_SIZE = (sizeof(T) / CUBE_LOCKFREE_CACHELINE_BYTES);
                check_allocation_size(BUCKET_SIZE);

                T &ret = *reinterpret_cast<T *>(_buffer.data() + _end);
                std::memcpy(&ret, &data, sizeof(T));
                const_cast<T &>(data).alive = 1;
                _end += BUCKET_SIZE;
                return ret;
            }

            void __recycle(Event const &data) {
                std::memcpy((_buffer.data() + _end), &data, data.bucket_size * CUBE_LOCKFREE_CACHELINE_BYTES);
                _end += data.bucket_size;
            }

        public:
            Pipe() : _begin(0), _end(0) {
                _buffer.resize(MaxBufferEvents);
            }

            ~Pipe() = default;

            //Todo: User dynamic Allocation
        };

        class EventManager : public nocopy {
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
                    if (pipe._begin != pipe._end) {
                        const auto save_end = pipe._end;
                        for (auto i = pipe._begin; i < save_end;) {
                            pipe.check_allocation_size(reinterpret_cast<const Event *>(pipe._buffer.data() + i)->bucket_size);
                            const auto &event = *reinterpret_cast<const Event *>(pipe._buffer.data() + i);
                            if (!_core.send(event)) {
                                pipe.__recycle(event);
                            }
                            i += event.bucket_size;
                        }
                        if (save_end != pipe._end)
                            pipe._begin = save_end;
                        else
                            pipe.reset();
                    }
                }
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
                    auto actor = _core._shared_core_actor.find(event->dest);
                    if (likely(actor != std::end(_core._shared_core_actor))) {
                        actor->second._this->hasEvent(event);
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

        static void __start__physical_thread(PhysicalCoreHandler &core) {
            if (core.init()) {
                std::vector<ActorProxy> _actor_to_remove;
                _actor_to_remove.reserve(core._shared_core_actor.size());
                LOG_INFO << "StartSequence Init " << core << " Success";
                while (likely(true)) {
                    //! not sure to implement this maybe it has overhead
                    for (auto &actor: core._shared_core_actor) {
                        if (unlikely(static_cast<int>(actor.second._this->main()))) {
                            _actor_to_remove.push_back(actor.second);
                        }
                    }

                    core._eventManager->receive();
                    core._eventManager->flush();
                    //! next
                    if (unlikely(!_actor_to_remove.empty())) {
                        // remove dead actor
                        for (auto const &actor : _actor_to_remove) {
                            delete actor._this;
                            core.removeActor(actor._id);
                        }
                        _actor_to_remove.clear();
                        if (core._shared_core_actor.empty())
                            break;
                    }
                }
            } else {
                LOG_CRIT << "StartSequence Init " << core << " Failed";
            }
        }

        //////// Members
        _ParentHandler *_parent = nullptr;
        EventManager *_eventManager = nullptr;
        _SharedData *_sharedData = nullptr;

        std::unordered_map<uint64_t, ActorProxy> _shared_core_actor;
        std::thread _thread;
        //////// !Members
    public:
        // Start Sequence Usage
        bool __alloc__event() {
            if constexpr (!std::is_void<_SharedData>::value) {
                if (_sharedData == nullptr)
                    _sharedData = new _SharedData();
            }
            _eventManager = new EventManager(*this);
            return true;
        }

        bool __init__actor() {
            // Init StaticActors
            for (auto &it : _shared_core_actor) {
                if (static_cast<int>(it.second._this->init()))
                    return false;
            }
            return true;
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
            _thread = std::thread(__start__physical_thread, std::ref(*this));
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

            ret = !pthread_setaffinity_np(_thread.native_handle(), sizeof(cpu_set_t), &cpuset);
#elif defined(_WIN32) || defined(_WIN64)
    #ifdef _MSC_VER
                DWORD_PTR mask = (1 << (_index < 0 ? 0 : _index));
                ret = (SetThreadAffinityMask(GetCurrentThread(), mask));
    #else
        #warning "Cannot set affinity on windows with GNU Compiler"
    #endif
#endif
            return ret;
        }

        inline void addActor(ActorProxy const &actor) {
            _shared_core_actor.insert({actor._id, actor});
            LOG_DEBUG << "New Actor[" << actor._id << "] in " << *this;
        }

        inline void removeActor(ActorId const &id) {
            LOG_DEBUG << "Delete Actor[" << id << "] in " << *this;
            _shared_core_actor.erase(id);
        }

        template<std::size_t _CoreIndex_
                , typename _Actor
                , typename ..._Init>
        inline ActorId addActor(_Init &&...init) {
            auto actor = new _Actor(std::forward<_Init>(init)...);
            actor->__set_id(__generate_id());
            actor->_handler = this;
            addActor(actor->proxy());
            return actor->id();
        };

        template<std::size_t _CoreIndex_
                , template<typename _Handler> typename _Actor
                , typename ..._Init>
        ActorId addActor(_Init &&...init) {
            if constexpr (_CoreIndex_ == _index) {
                return addActor<_CoreIndex_, _Actor<PhysicalCoreHandler>>
                        (std::forward<_Init>(init)...);
            }
            return ActorId::NotFound{};
        }

        template<std::size_t _CoreIndex_
                , template<typename _Trait, typename _Handler> typename _Actor
                , typename _Trait
                , typename ..._Init>
        ActorId addActor(_Init &&...init) {
            if constexpr (_CoreIndex_ == _index) {
                return addActor<_CoreIndex_, _Actor<_Trait, PhysicalCoreHandler>>
                        (std::forward<_Init>(init)...);
            }
            return ActorId::NotFound{};
        }

    public:
        constexpr static const std::size_t _index = _CoreIndex;

        PhysicalCoreHandler() = delete;
        PhysicalCoreHandler(_ParentHandler *parent)
			: _parent(parent) {
        }

        ~PhysicalCoreHandler() {
            for (auto &it : _shared_core_actor) {
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
            actor->__set_id(__generate_id());
            actor->_handler = this;

            if (unlikely(static_cast<int>(actor->init()))) {
                delete actor;
                return nullptr;
            }
            addActor(actor->proxy());
            return actor;
        };

        template<template <typename _Handler> typename _Actor
                , typename ..._Init>
        inline auto addReferencedActor(_Init &&...init) {
            return addReferencedActor<_Actor<PhysicalCoreHandler>>
                    (std::forward<_Init>(init)...);
        }

        template<template <typename _Trait, typename _Handler> typename _Actor
                , typename _Trait
                , typename ..._Init>
        inline auto addReferencedActor(_Init &&...init) {
            return addReferencedActor<_Actor<_Trait, PhysicalCoreHandler>>
                    (std::forward<_Init>(init)...);
        }

    public:
        //sender
        bool send(Event const &data) {
            return _parent->send(data);
        }

        template <typename T, typename ..._Init>
        void send(ActorId const dest, ActorId const source, _Init &&...init) {
            auto &pipe = _eventManager->getPipe(dest._index);
            auto data = pipe.template allocate<T>(std::forward<_Init>(init)...);
            data.dest = dest;
            data.source = source;
            if (_parent->send(data))
                pipe.free(data.bucket_size);
        }

        template<typename T, typename ..._Init>
        T &push(ActorId const &dest, ActorId const &source, _Init &&...init) {
            auto &pipe = _eventManager->getPipe(dest._index);
            auto &data = pipe.template allocate<T, _Init...>(std::forward<_Init>(init)...);
            data.dest = dest;
            data.source = source;
            return data;
        }

        template<typename T>
        T &reply(T const &event) {
            auto &ret = _eventManager->getPipe(event.source._index).template recycle<T>(event);

            std::swap(ret.dest, ret.source);
            return ret;
        }

        template<typename T>
        T &forward(ActorId const dest, T const &event) {
            auto &ret = _eventManager->getPipe(dest._index).template recycle<T>(event);

            std::swap(ret.dest, ret.source);
            ret.dest = dest;
            return ret;
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
}

#endif //CUBE_PHYSICALCORE_H
