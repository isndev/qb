
#ifndef CUBE_PHYSICALCORE_H
# define CUBE_PHYSICALCORE_H
#ifdef UNIX
#define __USE_GNU
    #include <sched.h>
    #include <errno.h>
    #include <unistd.h>
    #include <pthread.h>
#endif

#ifdef WIN32
    #include            <Windows.h>
    #include            <process.h>
#endif

# include <iostream>
# include <unordered_map>
# include <thread>
# include <limits>

# include "utils/TComposition.h"
# include "system/lockfree/spsc_queue.h"
# include "Types.h"
# include "IActor.h"

namespace cube {

    template<typename _Handler>
    class Actor;

    struct CUBE_LOCKFREE_CACHELINE_ALIGNMENT CacheLine {
        uint64_t __padding__[CUBE_LOCKFREE_CACHELINE_BYTES / sizeof(uint64_t)];
    };

    template<typename _ParentHandler, std::size_t _CoreIndex>
    class PhysicalCoreHandler {
    public:
        //////// Types
        constexpr static std::uint64_t MaxEvents = ((std::numeric_limits<uint16_t>::max)() + 1) / sizeof(CacheLine);
        using SPSCBuffer = lockfree::ringbuffer<CacheLine, MaxEvents>;
        using EventBuffer = std::array<CacheLine, MaxEvents>;
    private:
        //////// Event Manager

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
                _last_actor = ActorId::NotFound;
            }

//            Pipe(Pipe const &) = delete;
//            Pipe(Pipe&&) = default;

        public:
            Pipe() : _index(0), _last_context_size(nullptr), _last_actor(ActorId::NotFound) {}

            ~Pipe() = default;

            template<typename T>
            T &push(T const &event) {
                const auto index = _index;
                _index += event.bucket_size;
//                if (unlikely(_index >= MaxEvents))
//                    throw std::runtime_error();
                auto &ret = *reinterpret_cast<T *>(_buffer.data() + index);
                memcpy(&ret, &event, event.bucket_size * sizeof(CacheLine));
                if (event.dest != _last_actor) {
                    ret.context_size = event.bucket_size;
                    _last_actor = event.dest;
                    _last_context_size = &(ret.context_size);
                } else {
                    *_last_context_size += event.bucket_size;
                }
                return ret;
            }
        };

        class EventManager : public nocopy {
            friend class PhysicalCoreHandler;

            using PipeMap = std::unordered_map<uint32_t, Pipe>;
            PhysicalCoreHandler &_core;
            SPSCBuffer _spsc_buffer;
            EventBuffer _event_buffer;
            PipeMap _pipes;

            EventManager(PhysicalCoreHandler &core) : _core(core) {}

            void flush() {
                for (auto &it : _pipes) {
                    auto &pipe = it.second;
                    _core.send(pipe._buffer.data(), it.first, pipe._index);
                    pipe.reset();
                }
            }

            Pipe &getPipe(uint32_t core) {
                return _pipes[core];
            }

            void receive() {
                auto nb_events = _spsc_buffer.dequeue(_event_buffer.data(), MaxEvents);
                uint64_t i = 0;
                while (i < nb_events) {
                    auto event = reinterpret_cast<Event *>(_event_buffer.data() + i);
                    const auto nb_buckets = event->context_size;
                    auto actor = _core._shared_core_actor.find(event->dest);
                    if (likely(actor != std::end(_core._shared_core_actor))) {
                        actor->second._this->hasEvent(event);
                    } else {
                        cube::io::cout() << "ACTOR NOT FOUND" << std::endl;
                        //assert(true);
                    }
                    //assert(i + nb_buckets > nb_events);
                    i += nb_buckets;
                }
            }

        };

        /////// !Event Manager

        static void __start__physical_thread(PhysicalCoreHandler &core) {
            if (core.init()) {
                std::vector<ActorProxy> _actor_to_remove;
                cube::io::cout() << "Init " << core << std::endl;
                while (likely(true)) {
                    //! not sure to implement this maybe it has overhead
                    for (auto &actor: core._shared_core_actor) {
                        if (unlikely(actor.second._this->main())) {
                            core.removeActor(actor.second._id);
                            _actor_to_remove.push_back(actor.second);
                        }
                    }
                    core._event_manager->flush();

                    //! next
                    if (unlikely(!_actor_to_remove.empty())) {
                        // remove dead actor
                        for (auto const &actor : _actor_to_remove) {
                            delete actor._this;
                            core._shared_core_actor.erase(actor._id);
                        }
                        _actor_to_remove.clear();
                        if (core._shared_core_actor.empty())
                            break;
                    }

                    core._event_manager->receive();
                }
            } else {
                cube::io::cout() << "Failed Init" << core << std::endl;
            }
        }

        //////// Members
        _ParentHandler *_parent = nullptr;
        EventManager *_event_manager = nullptr;

        std::unordered_map<uint64_t, ActorProxy> _shared_core_actor;
        std::thread _thread;
        //////// !Members

    public:
        constexpr static const std::size_t _index = _CoreIndex;

        PhysicalCoreHandler() = delete;

        PhysicalCoreHandler(PhysicalCoreHandler const &rhs) : _parent(rhs._parent) {}

        PhysicalCoreHandler(_ParentHandler *parent) : _parent(parent) {}

        bool init() {
            bool ret(true);
#ifdef UNIX
            cpu_set_t cpuset;

            CPU_ZERO(&cpuset);
            CPU_SET(_index, &cpuset);

            ret = !pthread_setaffinity_np(_thread.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
#ifdef WIN32
#ifdef _MSC_VER
            DWORD_PTR mask = (1 << (_index < 0 ? 0 : _index));
            ret = (SetThreadAffinityMask(GetCurrentThread(), mask));
#else
            //!Note Cannot set affinity on windows with GNU Compiler
#endif
#endif
            if (!ret)
                return false;

            // Init StaticActors
            for (auto &it : _shared_core_actor) {
                if (it.second._this->init())
                    return false;
            }

            return true;
        }

        ~PhysicalCoreHandler() {
            for (auto &it : _shared_core_actor) {
                delete it.second._this;
            }

            delete _event_manager;
        }

        template<typename _Actor, typename ..._Init>
        _Actor *addReferencedActor(_Init const &...init) {
            auto actor = new _Actor(init...);
            actor->_index = _index;
            actor->_handler = this;

            if (unlikely(actor->init())) {
                delete actor;
                return nullptr;
            }
            _shared_core_actor.insert({actor->id(), actor->proxy()});
            _parent->addActor(actor->proxy());
            return actor;
        };

        template<std::size_t _CoreIndex_, template<typename _Handler> typename _Actor, typename ..._Init>
        ActorId addActor(_Init const &...init) {
            if constexpr (_CoreIndex_ == _index) {
                auto actor = new _Actor<PhysicalCoreHandler>(init...);
                actor->_index = _index;
                actor->_handler = this;
                _shared_core_actor.insert({actor->id(), actor->proxy()});
                _parent->addActor(actor->proxy());
                return actor->id();
            }
            return ActorId::NotFound;
        }

        void removeActor(ActorId const &id) {
            _parent->removeActor(id);
        }

    public:

        template<typename T>
        T &push(T const &event) {
            return _event_manager->getPipe(event.dest._index).template push<T>(event);
        }

        void send(CacheLine const *data, uint32_t const index, uint32_t const size) {
            _parent->send(data, index, size);
        }


//        template<typename _Data, typename ..._Init>
//        void send(ActorId const &dest, ActorId const &source, _Init ...init) {
//            _parent->template send<_Data, _Init...>(dest, source, std::forward<_Init>(init)...);
//        }


        void receive(CacheLine const *data, uint32_t const size) {
            _event_manager->_spsc_buffer.enqueue(data, size);
        }

        // Start Sequence Usage
        void start() {
            _event_manager = new EventManager(*this);
            _thread = std::thread(__start__physical_thread, std::ref(*this));
        }

        void join() {
            _thread.join();
        }
    };

    template<typename _ParentHandler, std::size_t _CoreIndex>
    std::ostream &operator<<(std::ostream &os, PhysicalCoreHandler<_ParentHandler, _CoreIndex> const &core) {
        os << "PhysicalCore(" << core._index << ").id(" << std::this_thread::get_id() << ")";
        return os;
    };

    template<typename _ParentHandler, typename ..._Core>
    class LinkedCoreHandler
            : public TComposition<typename _Core::template type<LinkedCoreHandler<_ParentHandler, _Core...>>...> {
        _ParentHandler *_parent;
        using base_t = TComposition<typename _Core::template type<LinkedCoreHandler>...>;

    public:
        LinkedCoreHandler() = delete;

        LinkedCoreHandler(LinkedCoreHandler const &rhs)
                : base_t(typename _Core::template type<LinkedCoreHandler>(this)...), _parent(rhs._parent) {}

        LinkedCoreHandler(_ParentHandler *parent)
                : base_t(typename _Core::template type<LinkedCoreHandler>(this)...), _parent(parent) {}

        void addActor(ActorProxy const &actor) {
            _parent->addActor(actor);
        }

        void removeActor(ActorId const &id) {
            _parent->removeActor(id);
        }

        template<std::size_t _CoreIndex, template<typename _Handler> typename _Actor, typename ..._Init>
        ActorId addActor(_Init const &...init) {
            ActorId id = ActorId::NotFound;
            this->each([this, &id, &init...](auto &item) -> int {
                id = item.template addActor<_CoreIndex, _Actor, _Init...>(init...);
                if (id)
                    return 0;
                return 1;
            });
            return id;
        }

//        template<typename _Data, typename ..._Init>
//        void send(ActorId const &dest, ActorId const &source, _Init ...init) {
//            using event_t = TEvent<_Data, _Init...>;
//
//            auto event = event_t(init...);
//            event.size = sizeof(event_t) / CUBE_LOCKFREE_CACHELINE_BYTES;
//            event.id = type_id<_Data>();
//            event.dest = dest;
//            event.source = source;
//
//            this->each([&event, &dest](auto &item) -> bool {
//                if (item._index == dest._index) {
//                    item.receive(event);
//                    return false;
//                }
//                return true;
//            });
//        }
//

        void send(CacheLine const *data, uint32_t const index, uint32_t const size) {
            this->each([&data, index, size](auto &item) -> bool {
                if (item._index == index) {
                    item.receive(data, size);
                    return false;
                }
                return true;
            });
        }

        // Start Sequence Usage
        void start() {
            this->each([](auto &item) -> bool {
                item.start();
                return true;
            });
        }

        void join() {
            this->each([](auto &item) -> bool {
                item.join();
                return true;
            });
        }

    };

    template<typename ..._Core>
    class Main : TComposition<typename _Core::template type<Main<_Core...>>...> {
        using base_t = TComposition<typename _Core::template type<Main>...>;
        std::unordered_map<uint64_t, ActorProxy> _all_actor;

    public:
        Main() : base_t(typename _Core::template type<Main>(this)...) {
            cube::io::cout() << "Main Handler THIS(" << this << ")" << std::endl;
        }

        //Todo : no thread safe need a lock or lockfree list
        // should be not accessible to users
        void addActor(ActorProxy const &actor) {
            //_all_actor.insert({actor._id, actor});
        }

        void removeActor(ActorId const &id) {
            //_all_actor.erase(id);
        }
        /////////////////////////////////////////////////////

        // Start Sequence Usage

        template<std::size_t _CoreIndex, template<typename _Handler> typename _Actor, typename ..._Init>
        ActorId addActor(_Init const &...init) {
            ActorId id = ActorId::NotFound;
            this->each([this, &id, &init...](auto &item) -> int {
                id = item.template addActor<_CoreIndex, _Actor>(init...);
                if (id)
                    return 0;
                return 1;
            });
            return id;
        }

        void start() {
            this->each([](auto &item) -> bool {
                item.start();
                return true;
            });
        }

        void join() {
            this->each([](auto &item) -> bool {
                item.join();
                return true;
            });
        }

    };
}

template<typename ..._Builder>
struct LinkedCore {
    template<typename _Parent>
    using type = cube::LinkedCoreHandler<_Parent, _Builder...>;
};

template<std::size_t _CoreIndex>
struct PhysicalCore {
    template<typename _Parent>
    using type = cube::PhysicalCoreHandler<_Parent, _CoreIndex>;

};

#endif //CUBE_PHYSICALCORE_H
