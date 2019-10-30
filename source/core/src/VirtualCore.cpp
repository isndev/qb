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
#include <qb/system/timestamp.h>

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>

typedef struct cpu_set {
  uint32_t    count;
} cpu_set_t;

static inline void
CPU_ZERO(cpu_set_t *cs) { cs->count = 0; }

static inline void
CPU_SET(int num, cpu_set_t *cs) { cs->count |= (1 << num); }

static inline int
CPU_ISSET(int num, cpu_set_t *cs) { return (cs->count & (1 << num)); }

static int pthread_setaffinity_np(pthread_t thread, size_t cpu_size,
                           cpu_set_t *cpu_set)
{
  thread_port_t mach_thread;
  int core = 0;

  for (core = 0; core < 8 * cpu_size; core++) {
    if (CPU_ISSET(core, cpu_set)) break;
  }
  if (core >= std::thread::hardware_concurrency())
    return -1;
  thread_affinity_policy_data_t policy = { core };
  mach_thread = pthread_mach_thread_np(thread);
  return thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY,
                    (thread_policy_t)&policy, 1) != KERN_SUCCESS;
}
#endif


namespace qb {
    VirtualCore::VirtualCore(CoreId const id, Main &engine) noexcept
            : _index(id)
            , _engine(engine)
            , _mail_box(engine.getMailBox(id)) {
        _ids.reserve(std::numeric_limits<ServiceId>::max() - _nb_service);
        _event_map.reserve(128);
        for (auto i = _nb_service + 1; i < ActorId::BroadcastSid; ++i) {
            _ids.insert(static_cast<ServiceId>(i));
        }
        for (auto cid : engine._core_set.raw())
            _pipes[cid];
    }

    VirtualCore::~VirtualCore() noexcept {
        for (auto it : _event_map)
            delete it.second;
        for (auto it : _actors)
            delete it.second;
    }

    ActorId VirtualCore::__generate_id__() noexcept {
        if (!_ids.size())
            return ActorId::NotFound;
        return ActorId(_ids.extract(_ids.begin()).value(), _index);
    }

    // Event Management
    void VirtualCore::unregisterEvents(ActorId const id) noexcept {
        for (auto handler : _event_map)
            handler.second->unregisterEvent(id);

    }

    Pipe &VirtualCore::__getPipe__(uint32_t core) noexcept {
        return _pipes.at(core);
    }

    void VirtualCore::__receive_events__(CacheLine *buffer, std::size_t const nb_events) {
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
        _mail_box.dequeue([this](CacheLine *buffer, std::size_t const nb_events) {
            __receive_events__(buffer, nb_events);
        }, _event_buffer.data(), MaxRingEvents);
    }

    void VirtualCore::__flush__() noexcept {
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

    bool VirtualCore::__flush_all__() noexcept {
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
    void VirtualCore::__init__() {
        bool ret(true);
#if defined(unix) || defined(__unix) || defined(__unix__) || defined(__APPLE__)
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
            LOG_CRIT("" << *this << " Init Failed");
            Main::sync_start.store(Error::BadInit, std::memory_order_release);
        }
    }

    void VirtualCore::__init__actors__() const {
        // Init StaticActors
        if (std::any_of(_actors.begin(), _actors.end(), [](auto &it) { return !it.second->onInit(); }))
        {
            LOG_CRIT("Actor at " << *this << " failed to init");
            Main::sync_start.store(Error::BadActorInit, std::memory_order_release);
        }
    }

    bool VirtualCore::__wait__all__cores__ready() noexcept {
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

    void VirtualCore::__workflow__() {
        LOG_INFO("" << *this << " Init Success " << static_cast<uint32_t>(_actors.size()) << " actor(s)");
        while (likely(Main::is_running)) {
            _nanotimer = 0;
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

        if (!Main::is_running) {
            LOG_INFO("" << *this << " Stopped by user leave " << static_cast<uint32_t>(_actors.size()) << " actor(s)");
        } else {
            LOG_INFO("" << *this << " Stopped normally");
        }
    }
    //!Workflow
    // Actor Management
    ActorId VirtualCore::initActor(Actor &actor, bool const is_service, bool const doInit) noexcept {
        if (is_service) {
            actor._index = _index;
            if (_actors.find(actor.id()) != _actors.end()) {
                delete &actor;
                return ActorId::NotFound;
            }
        } else {
            auto id = actor.id();
            if (id != ActorId::NotFound)
                _ids.extract(id.sid());
            else
                id = __generate_id__();

            actor.__set_id(id);
            // Number of actors attends to its limit in this core
            if (id == ActorId::NotFound) {
                _ids.insert(static_cast<ServiceId>(id.sid()));
                delete &actor;
                return ActorId::NotFound;
            }
        }

        registerEvent<KillEvent>(actor);
        registerEvent<PingEvent>(actor);

        if (doInit && unlikely(!actor.onInit())) {
            removeActor(actor.id());
            return ActorId::NotFound;
        }

        return actor.id();
    }

    ActorId VirtualCore::appendActor(Actor &actor, bool const is_service, bool const doInit) noexcept {
        if (initActor(actor, is_service, doInit) != ActorId::NotFound) {
            _actors.insert({actor.id(), &actor});
            LOG_DEBUG("New " << actor);
            return actor.id();
        }
        return ActorId::NotFound;
    }

    void VirtualCore::removeActor(ActorId const id) noexcept {
        unregisterCallback(id);
        unregisterEvents(id);
        const auto it = _actors.find(id);
        if (it != _actors.end()) {
            delete it->second;
            _actors.erase(it);
            if (id._id > _nb_service)
                _ids.insert(id._id);
        }
        LOG_DEBUG("Delete Actor(" << id.index() << "," << id.sid() << ")");
    }

    //!Actor Management

    void VirtualCore::killActor(ActorId const id) noexcept {
        _actor_to_remove.insert(id);
    }

    void VirtualCore::unregisterCallback(ActorId const id) noexcept {
        auto it =_actor_callbacks.find(id);
        if (it != _actor_callbacks.end())
            _actor_callbacks.erase(it);
    }

    //Event Api
    ProxyPipe VirtualCore::getProxyPipe(ActorId const dest, ActorId const source) noexcept{
        return {__getPipe__(dest._index), dest, source};
    }

    bool VirtualCore::try_send(Event const &event) const noexcept {
        return _engine.send(event);
    }

    void VirtualCore::send(Event const &event) noexcept {
        if (unlikely(!try_send(event))) {
            auto &pipe = __getPipe__(event.dest._index);
            pipe.recycle(event, event.bucket_size);
        }
    }

    Event &VirtualCore::push(Event const &event) noexcept {
        auto &pipe = __getPipe__(event.dest._index);
        return pipe.recycle_back(event, event.bucket_size);
    }

    void VirtualCore::reply(Event &event) noexcept {
        std::swap(event.dest, event.source);
        event.state[0] = 1;
        send(event);
    }

    void VirtualCore::forward(ActorId const dest, Event &event) noexcept {
        event.dest = dest;
        event.state[0] = 1;
        send(event);
    }
    //!Event Api

    CoreId VirtualCore::getIndex() const noexcept { return _index; }
    uint64_t VirtualCore::time() noexcept {
        _nanotimer = _nanotimer ? _nanotimer : Timestamp::nano();
        return _nanotimer;
    }
    ServiceId VirtualCore::_nb_service = 0;
    thread_local VirtualCore *VirtualCore::_handler = nullptr;
}

qb::io::stream &operator<<(qb::io::stream &os, qb::VirtualCore const &core) {
    std::stringstream ss;
    ss << "VirtualCore(" << core.getIndex() << ").id(" << std::this_thread::get_id() << ")";
    os << ss.str();
    return os;
};
