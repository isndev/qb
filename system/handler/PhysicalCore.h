
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
        constexpr static const std::uint64_t MaxEvents = ((std::numeric_limits<uint16_t>::max)());
        constexpr static const std::size_t nb_core = 1;
        //////// Types
        using SPSCBuffer = lockfree::spsc::ringbuffer<CacheLine, MaxEvents>;
        using MPSCBuffer = lockfree::mpsc::ringbuffer<CacheLine, MaxEvents, 0>;
        using EventBuffer = std::array<CacheLine, MaxEvents>;

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
        //////// Event Manager

        inline ActorId __generate_id() const {
            static std::size_t pid = 0;
            return ActorId(static_cast<uint32_t >(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() + pid++), _CoreIndex);
        }

        class Pipe : public nocopy {
            friend class PhysicalCoreHandler::EventManager;

            std::uint32_t _index;
            char __padding2__[CUBE_LOCKFREE_CACHELINE_BYTES - sizeof(std::size_t)];
            EventBuffer _buffer;
            std::uint32_t *_last_context_size;
            ActorId _last_actor;

            inline void reset() {
                _index = 0;
                _last_context_size = nullptr;
                _last_actor = ActorId::NotFound{};
            }

//            Pipe(Pipe const &) = delete;
//            Pipe(Pipe&&) = default;

        public:
            Pipe() : _index(0), _last_context_size(nullptr), _last_actor(ActorId::NotFound{}) {}

            ~Pipe() = default;

			template<typename T, typename ..._Init>
			T &allocate(_Init const &...init) {
				constexpr std::size_t extra = (sizeof(T) % CUBE_LOCKFREE_CACHELINE_BYTES ? 1 : 0);
				T &ret = *(new (reinterpret_cast<T *>(_buffer.data() + _index)) T(init...));
				ret.bucket_size = (sizeof(T) / CUBE_LOCKFREE_CACHELINE_BYTES) + extra;
				return ret;
			}

			template<typename T>
            T &recycle(T const &data) {
                T &ret = *reinterpret_cast<T *>(_buffer.data() + _index);
                std::memcpy(&ret, &data, sizeof(T));
                ret.alive = true;
                return push(ret);
            }

            template<typename T>
            T &push(T &event) {
                _index += event.bucket_size;
//                if (unlikely(_index >= MaxEvents))
//                    throw std::runtime_error("push events exceed MaxSize");
                if (event.dest != _last_actor) {
                    event.context_size = event.bucket_size;
                    _last_actor = event.dest;
                    _last_context_size = &(event.context_size);
                } else {
                    *_last_context_size += event.bucket_size;
                }
                return event;
            }
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
                    if (pipe._index) {
                        _core.send(pipe._buffer.data(), it.first, pipe._index);
                        pipe.reset();
                    }
                }
            }

            Pipe &getPipe(uint32_t core) {
                return _pipes[core];
            }

            void __receive(CacheLine *buffer, std::size_t const nb_events) {
                uint64_t i = 0;
                while (i < nb_events) {
                    auto event = reinterpret_cast<Event *>(buffer + i);
                    const auto nb_buckets = event->context_size;
                    auto actor = _core._shared_core_actor.find(event->dest);
                    if (likely(actor != std::end(_core._shared_core_actor))) {
                        actor->second._this->hasEvent(event);
                    } else {
                        LOG_WARN << "Failed Event" << _core
                                 << " [Source](" << event->source << ")"
                                 << " [Dest](" << event->dest << ") NOT FOUND";
                    }
                    //assert(i + nb_buckets > nb_events);
                    i += nb_buckets;
                }
            }

            void receive() {
                if constexpr (!std::is_same<_ParentHandler, typename _ParentHandler::parent_t>::value) {
                    // linked_core_events
                    __receive(_event_buffer.data(), _spsc_buffer.dequeue(_event_buffer.data(), MaxEvents));
                }

                // global_core_events
                _mpsc_buffer.dequeue([this](CacheLine *buffer, std::size_t nb_events){
                        __receive(buffer, nb_events);
                    }, _event_buffer.data(), MaxEvents);

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

        // Start Sequence Usage
        bool __alloc__event() {
            if constexpr (!std::is_void<_SharedData>::value) {
                _sharedData = new _SharedData();
            }
            _eventManager = new EventManager(*this);
            // Init StaticActors
            for (auto &it : _shared_core_actor) {
                if (static_cast<int>(it.second._this->init()))
                    return false;
            }
            return true;
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

        inline void receive(CacheLine const *data, uint32_t const size) {
            while(unlikely(!_eventManager->_spsc_buffer.enqueue(data, size)))
                std::this_thread::yield();
        }

        inline bool receive_from_different_core(CacheLine const *data, uint32_t const source, uint32_t const index, uint32_t const size) {
            if (_index != index)
                return false;
            while(unlikely(!_eventManager->_mpsc_buffer.enqueue(source, data, size)))
                std::this_thread::yield();
            return true;
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
        inline ActorId addActor(_Init const &...init) {
            auto actor = new _Actor(init...);
            actor->__set_id(__generate_id());
            actor->_handler = this;
            addActor(actor->proxy());
            return actor->id();
        };

        template<std::size_t _CoreIndex_
                , template<typename _Handler> typename _Actor
                , typename ..._Init>
        ActorId addActor(_Init const &...init) {
            if constexpr (_CoreIndex_ == _index) {
                return addActor<_CoreIndex_, _Actor<PhysicalCoreHandler>>(init...);
            }
            return ActorId::NotFound{};
        }

        template<std::size_t _CoreIndex_
                , template<typename _Trait, typename _Handler> typename _Actor
                , typename _Trait
                , typename ..._Init>
        ActorId addActor(_Init const &...init) {
            if constexpr (_CoreIndex_ == _index) {
                return addActor<_CoreIndex_, _Actor<_Trait, PhysicalCoreHandler>>(init...);
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
        _Actor *addReferencedActor(_Init const &...init) {
            auto actor = new _Actor(init...);
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
        inline auto addReferencedActor(_Init const &...init) {
            return addReferencedActor<_Actor<PhysicalCoreHandler>>(init...);
        }

        template<template <typename _Trait, typename _Handler> typename _Actor
                , typename _Trait
                , typename ..._Init>
        inline auto addReferencedActor(_Init const &...init) {
            return addReferencedActor<_Actor<_Trait, PhysicalCoreHandler>>(init...);
        }

    public:

        template<typename T, typename ..._Init>
        T &push(ActorId const &dest, ActorId const &source, _Init const &...init) {
			auto &pipe = _eventManager->getPipe(dest._index);
            auto &ret = pipe.template allocate<T, _Init...>(init...);
			ret.id = type_id<T>();
			ret.dest = dest;
			ret.source = source;
			return pipe.template push<T>(ret);
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

        void send(CacheLine const *data, uint32_t const index, uint32_t const size) {
            _parent->send(data, _index, index, size);
        }

        auto &sharedData() {
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
