//
// Created by isndev on 12/4/18.
//

#include <cube/engine/Core.h>

namespace cube {
    Core::Core(uint8_t const id, Main &engine)
            : _index(id)
            , _engine(engine)
            , _mail_box(engine.getMailBox(id))
            , _nano_timer(Timestamp::nano()) {
        _ids.reserve(std::numeric_limits<uint16_t>::max() - SERVICE_ACTOR_INDEX);
        for (auto i = SERVICE_ACTOR_INDEX + 1; i <= std::numeric_limits<uint16_t>::max(); ++i) {
            _ids.insert(static_cast<uint16_t >(i));
        }
    }

    ActorId Core::__generate_id__() {
        if (!_ids.size())
            return ActorId::NotFound;
        return ActorId(_ids.extract(_ids.begin()).value(), _index);
    }

    // Event Management
    Pipe &Core::__getPipe__(uint32_t core) {
        return _pipes[core];
    }

    void Core::__receive_events__(CacheLine *buffer, std::size_t const nb_events) {
        if (!nb_events)
            return;
        std::size_t i = 0;
        while (i < nb_events) {
            auto event = reinterpret_cast<Event *>(buffer + i);
            auto actor = _actors.find(event->dest);
            if (likely(actor != std::end(_actors))) {
                event->state[0] = 0;
                actor->second->on(event);
                LOG_DEBUG << "Sucess Event" << *this
                          << " [Source](" << event->source << ")"
                          << " [Dest](" << event->dest << ") Size=" << event->bucket_size;
            } else {
                LOG_WARN << "Failed Event" << *this
                         << " [Source](" << event->source << ")"
                         << " [Dest](" << event->dest << ") NOT FOUND";
            }

            i += event->bucket_size;
        }
    }

    void Core::__receive__() {
        // global_core_events
        _mail_box.dequeue([this](CacheLine *buffer, std::size_t const nb_events){
            __receive_events__(buffer, nb_events);
        }, _event_buffer.data(), MaxRingEvents);
    }

    void Core::__flush__() {
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

    bool Core::__flush_all__() {
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
    bool Core::__init__actors__() const {
        // Init StaticActors
        for (const auto &it : _actors) {
            if (!it.second->onInit()) {
                LOG_WARN << "Actor at " << *this << " failed to init";
                return false;
            }
        }
        return true;
    }

    bool Core::__init__() {
        bool ret(true);
#if defined(unix) || defined(__unix) || defined(__unix__)
        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        CPU_SET(_index, &cpuset);

        pthread_t current_thread = pthread_self();
        ret = !pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
#elif defined(_WIN32) || defined(_WIN64)
        #ifdef _MSC_VER
            DWORD_PTR mask = static_cast<uint32_t>(1 << (_index < 0 ? 0 : _index));
            ret = (SetThreadAffinityMask(GetCurrentThread(), mask));
#else
#warning "Cannot set affinity on windows with GNU Compiler"
#endif
#endif
        _actor_to_remove.reserve(_actors.size());
        return ret;
    }

    void Core::__wait__all__cores__ready() {
        const auto total_core = _engine.getNbCore();
        Main::sync_start.fetch_add(1, std::memory_order_acq_rel);
        LOG_INFO << "[READY]" << *this;
        while (Main::sync_start.load(std::memory_order_acquire) < total_core)
            std::this_thread::yield();
    }

    void Core::__updateTime__() {
        const auto now = Timestamp::nano();
        _nano_timer = now;
    }

    void Core::__spawn__() {
        try {
            if (__init__()) {
                __init__actors__();
                __wait__all__cores__ready();

                LOG_INFO << "StartSequence Init " << *this << " Success";
                while (likely(Main::is_running)) {
                    __updateTime__();
                    __receive__();

                    for (const auto &callback : _actor_callbacks)
                        callback.second->onCallback();

                    __flush__();

                    if (unlikely(!_actor_to_remove.empty())) {
                        // remove dead actors
                        for (auto const &actor : _actor_to_remove)
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
    void Core::addActor(Actor *actor) {
        _actors.insert({actor->id(), actor});
        LOG_DEBUG << "New " << *actor;
    }

    void Core::removeActor(ActorId const &id) {
        const auto it = _actors.find(id);
        LOG_DEBUG << "Delete Actor(" << id.index() << "," << id.sid() << ")";
        delete it->second;
        _actors.erase(it);
        unregisterCallback(id);
        if (id._id > SERVICE_ACTOR_INDEX)
            _ids.insert(id._id);
    }

    //!Actor Management

    void Core::start() {
        _thread = std::thread(&Core::__spawn__, this);
        if (_thread.get_id() == std::thread::id())
            std::runtime_error("failed to start a PhysicalCore");
    }

    void Core::join() {
        if (_thread.get_id() != std::thread::id{})
            _thread.join();
    }

    void Core::killActor(ActorId const &id) {
        _actor_to_remove.push_back(id);
    }

    void Core::unregisterCallback(ActorId const &id) {
        auto it =_actor_callbacks.find(id);
        if (it != _actor_callbacks.end())
            _actor_callbacks.erase(it);
    }

    //Event Api
    ProxyPipe Core::getPipeProxy(ActorId const dest, ActorId const source) {
        return {__getPipe__(dest._index), dest, source};
    }

    bool Core::try_send(Event const &event) const {
        // Todo: Fix MonoThread Optimization
        // if (event.dest._index == _index) {
        //     const_cast<Event &>(event).state[0] = 0;
        //     _actors.find(event.dest)->second->on(const_cast<Event *>(&event));
        //     return true;
        // }
        return _engine.send(event);
    }

    void Core::send(Event const &event) {
        if (unlikely(!try_send(event))) {
            auto &pipe = __getPipe__(event.dest._index);
            pipe.recycle(event, event.bucket_size);
        }
    }

    Event &Core::push(Event const &event) {
        auto &pipe = __getPipe__(event.dest._index);
        return pipe.recycle_back(event, event.bucket_size);
    }

    void Core::reply(Event &event) {
        std::swap(event.dest, event.source);
        event.state[0] = 1;
        send(event);
    }

    void Core::forward(ActorId const dest, Event &event) {
        event.source = event.dest;
        event.dest = dest;
        event.state[0] = 1;
        send(event);
    }
    //!Event Api

    uint16_t Core::getIndex() const { return _index; }
    uint64_t Core::time() const { return _nano_timer; }

}

cube::io::stream &operator<<(cube::io::stream &os, cube::Core const &core) {
    std::stringstream ss;
    ss << "Core(" << core.getIndex() << ").id(" << std::this_thread::get_id() << ")";
    os << ss.str();
    return os;
};