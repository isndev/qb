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

#include <qb/core/VirtualCore.h>

namespace qb {
    VirtualCore::VirtualCore(uint8_t const id, Main &engine)
            : _index(id)
            , _engine(engine)
            , _mail_box(engine.getMailBox(id))
            , _nano_timer(Timestamp::nano()) {
        _ids.reserve(std::numeric_limits<uint16_t>::max() - _nb_service);
        _event_map.reserve(128);
        for (auto i = _nb_service + 1; i <= std::numeric_limits<uint16_t>::max(); ++i) {
            _ids.insert(static_cast<uint16_t >(i));
        }
    }

    ActorId VirtualCore::__generate_id__() {
        if (!_ids.size())
            return ActorId::NotFound;
        return ActorId(_ids.extract(_ids.begin()).value(), _index);
    }

    // Event Management
    void VirtualCore::unregisterEvents(ActorId const id) {
        for (auto handler : _event_map)
            handler.second->unregisterEvent(id);

    }

    Pipe &VirtualCore::__getPipe__(uint32_t core) {
        return _pipes[core];
    }

    void VirtualCore::__receive_events__(CacheLine *buffer, std::size_t const nb_events) {
        if (!nb_events)
            return;
        std::size_t i = 0;
        while (i < nb_events) {
            auto event = reinterpret_cast<Event *>(buffer + i);
            // Todo : secure this
            _event_map.at(event->id)->invoke(event);
            i += event->bucket_size;
        }
    }

    void VirtualCore::__receive__() {
        // global_core_events
        _mail_box.dequeue([this](CacheLine *buffer, std::size_t const nb_events){
            __receive_events__(buffer, nb_events);
        }, _event_buffer.data(), MaxRingEvents);
    }

    void VirtualCore::__flush__() {
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

    bool VirtualCore::__flush_all__() {
        bool ret = false;
        for (auto &it : _pipes) {
            auto &pipe = it.second;
            if (pipe.end()) {
                ret = true;
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
        return ret;
    }
    //!Event Management

    // Workflow
    void VirtualCore::__init__actors__() const {
        // Init StaticActors
        if (std::any_of(_actors.begin(), _actors.end(), [](auto &it) { return !it.second->onInit(); }))
        {
            LOG_CRIT << "Actor at " << *this << " failed to init";
            Main::sync_start.store(Error::BadActorInit, std::memory_order_release);
        }
    }

    void VirtualCore::__init__() {
        bool ret(true);
#if defined(unix) || defined(__unix) || defined(__unix__)
        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        CPU_SET(_index, &cpuset);

        pthread_t current_thread = pthread_self();
        ret = !pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
#elif defined(_WIN32) || defined(_WIN64)
        #ifdef _MSC_VER
            DWORD_PTR mask = static_cast<uint32_t>(1 << _index);
            ret = (SetThreadAffinityMask(GetCurrentThread(), mask));
#else
#warning "Cannot set affinity on windows with GNU Compiler"
#endif
#endif
        _actor_to_remove.reserve(_actors.size());

        if (!ret) {
            LOG_CRIT << "" << *this << " Init Failed";
            Main::sync_start.store(Error::BadInit, std::memory_order_release);
        } else if (!_actors.size()) {
            LOG_CRIT << "" << *this << " Started with 0 Actor";
            Main::sync_start.store(Error::NoActor, std::memory_order_release);
            return;
        }
    }

    bool VirtualCore::__wait__all__cores__ready() {
        const auto total_core = _engine.getNbCore();
        Main::sync_start.fetch_add(1, std::memory_order_acq_rel);
        uint64_t ret = 0;
        do {
            std::this_thread::yield();
            ret = Main::sync_start.load(std::memory_order_acquire);
        }
        while (ret < total_core);
        return ret < Error::BadInit;
    }

    void VirtualCore::__updateTime__() {
        const auto now = Timestamp::nano();
        _nano_timer = now;
    }

    void VirtualCore::__spawn__() {
        try {
            __init__();
            __init__actors__();
            if (!__wait__all__cores__ready())
                return;

            LOG_INFO << "" << *this << " Init Success " << _actors.size() << " actor(s)";

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

            if (!Main::is_running)
                LOG_INFO << "" << *this << " Stopped by user leave " << _actors.size() << " actor(s)";
            else
                LOG_INFO << "" << *this << " Stopped normally";
        } catch (std::exception &e) {
            LOG_CRIT << "Exception thrown on " << *this << " what:" << e.what();
            Main::sync_start.store(Error::ExceptionThrown, std::memory_order_release);
            Main::is_running = false;
        }
    }
    //!Workflow

    // Actor Management
    void VirtualCore::addActor(Actor *actor) {
        actor->registerEvent<KillEvent>(*actor);
        _actors.insert({actor->id(), actor});
        LOG_DEBUG << "New " << *actor;
    }

    void VirtualCore::removeActor(ActorId const id) {
        const auto it = _actors.find(id);
        delete it->second;
        _actors.erase(it);
        unregisterCallback(id);
        unregisterEvents(id);
        if (id._id > _nb_service)
            _ids.insert(id._id);
        LOG_DEBUG << "Delete Actor(" << id.index() << "," << id.sid() << ")";
    }

    //!Actor Management

    void VirtualCore::start() {
        _thread = std::thread(&VirtualCore::__spawn__, this);
        if (_thread.get_id() == std::thread::id())
            std::runtime_error("failed to start a PhysicalCore");
    }

    void VirtualCore::join() {
        if (_thread.get_id() != std::thread::id{})
            _thread.join();
    }

    void VirtualCore::killActor(ActorId const id) {
        _actor_to_remove.insert(id);
    }

    void VirtualCore::unregisterCallback(ActorId const id) {
        auto it =_actor_callbacks.find(id);
        if (it != _actor_callbacks.end())
            _actor_callbacks.erase(it);
    }

    //Event Api
    ProxyPipe VirtualCore::getProxyPipe(ActorId const dest, ActorId const source) {
        return {__getPipe__(dest._index), dest, source};
    }

    bool VirtualCore::try_send(Event const &event) const {
        // Todo: Fix MonoThread Optimization
        // if (event.dest._index == _index) {
        //     const_cast<Event &>(event).state[0] = 0;
        //     _actors.find(event.dest)->second->on(const_cast<Event *>(&event));
        //     return true;
        // }
        return _engine.send(event);
    }

    void VirtualCore::send(Event const &event) {
        if (unlikely(!try_send(event))) {
            auto &pipe = __getPipe__(event.dest._index);
            pipe.recycle(event, event.bucket_size);
        }
    }

    Event &VirtualCore::push(Event const &event) {
        auto &pipe = __getPipe__(event.dest._index);
        return pipe.recycle_back(event, event.bucket_size);
    }

    void VirtualCore::reply(Event &event) {
        std::swap(event.dest, event.source);
        event.state[0] = 1;
        send(event);
    }

    void VirtualCore::forward(ActorId const dest, Event &event) {
        event.source = event.dest;
        event.dest = dest;
        event.state[0] = 1;
        send(event);
    }
    //!Event Api

    uint16_t VirtualCore::getIndex() const { return _index; }
    uint64_t VirtualCore::time() const { return _nano_timer; }
	uint16_t VirtualCore::_nb_service = 0;

}

qb::io::stream &operator<<(qb::io::stream &os, qb::VirtualCore const &core) {
    std::stringstream ss;
    ss << "VirtualCore(" << core.getIndex() << ").id(" << std::this_thread::get_id() << ")";
    os << ss.str();
    return os;
};