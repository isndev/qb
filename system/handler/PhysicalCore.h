
#ifndef CUBE_PHYSICALCORE_H
# define CUBE_PHYSICALCORE_H
# include "Types.h"

namespace cube {
    using namespace std::chrono;

    template<typename _ParentHandler, std::size_t _CoreIndex, typename _SharedData>
    class PhysicalCoreHandler {
    public:
        //////// Types
        constexpr static std::uint64_t MaxEvents = ((std::numeric_limits<uint16_t>::max)());
        using SPSCBuffer = lockfree::ringbuffer<CacheLine, MaxEvents>;
        using EventBuffer = std::array<CacheLine, MaxEvents>;
    private:
        friend _ParentHandler;
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
				T &ret = *(new (reinterpret_cast<T *>(_buffer.data() + _index)) T(init...));
				ret.bucket_size = (sizeof(T) / CUBE_LOCKFREE_CACHELINE_BYTES + sizeof(T) % CUBE_LOCKFREE_CACHELINE_BYTES);
				return ret;
			}

			template<typename T>
            T &recycle(T const &data) {
                T &ret = *reinterpret_cast<T *>(_buffer.data() + _index);
                std::memcpy(&ret, &data, sizeof(T));
                std::swap(ret.dest, ret.source);
                const_cast<T &>(data).alive = true;
                return push(ret);
            }

            template<typename T>
            T &push(T &event) {
                _index += event.bucket_size;
//                if (unlikely(_index >= MaxEvents))
//                    throw std::runtime_error();
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
                        LOG_WARN << "Failed Event" << _core << " [Source](" << event->source <<
                                 ") [Dest](" << event->dest << ") NOT FOUND" << _core._shared_core_actor.size();
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
                LOG_INFO << "StartSequence" << core << " Init Success";
                while (likely(true)) {
                    //! not sure to implement this maybe it has overhead
                    for (auto &actor: core._shared_core_actor) {
                        if (unlikely(actor.second._this->main())) {
                            core.removeActor(actor.second._id);
                            _actor_to_remove.push_back(actor.second);
                        }
                    }
                    core._eventManager->flush();

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

                    core._eventManager->receive();
                }
            } else {
                LOG_CRIT << "StartSequence" << core << " Init Failed";
            }
        }

        //////// Members
        _ParentHandler *_parent = nullptr;
        EventManager *_eventManager = nullptr;
        _SharedData *_sharedData = nullptr;

        std::unordered_map<uint64_t, ActorProxy> _shared_core_actor;
        std::thread _thread;
        //////// !Members

        void removeActor(ActorId const &id) {
            _parent->removeActor(id);
        }

        // Start Sequence Usage
        void __alloc__event() {
            if constexpr (!std::is_void<_SharedData>::value) {
                _sharedData = new _SharedData();
            }
            _eventManager = new EventManager(*this);
        }

        void __start() {
            _thread = std::thread(__start__physical_thread, std::ref(*this));
        }

        void __join() {
            _thread.join();
        }

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

        void receive(CacheLine const *data, uint32_t const size) {
            _eventManager->_spsc_buffer.enqueue(data, size);
        }

    public:
        constexpr static const std::size_t _index = _CoreIndex;

        PhysicalCoreHandler() = delete;
        PhysicalCoreHandler(PhysicalCoreHandler const &rhs) : _parent(rhs._parent) {}
        PhysicalCoreHandler(_ParentHandler *parent) : _parent(parent) {}
        ~PhysicalCoreHandler() {
            for (auto &it : _shared_core_actor) {
                delete it.second._this;
            }

            delete _eventManager;
            if constexpr (!std::is_void<_SharedData>::value)
                delete _sharedData;
        }

        template<typename _Actor, typename ..._Init>
        _Actor *addReferencedActor(_Init const &...init) {
            auto actor = new _Actor(init...);
            actor->__set_id(__generate_id());
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
                actor->__set_id(__generate_id());
                actor->_handler = this;
                _shared_core_actor.insert({actor->id(), actor->proxy()});
                _parent->addActor(actor->proxy());
                return actor->id();
            }
            return ActorId::NotFound{};
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
            return _eventManager->getPipe(event.source._index).template recycle<T>(event);
        }


        void send(CacheLine const *data, uint32_t const index, uint32_t const size) {
            _parent->send(data, index, size);
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
