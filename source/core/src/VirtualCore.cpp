/**
 * @file qb/core/src/VirtualCore.cpp
 * @brief Implementation of the VirtualCore class for the QB Actor Framework
 *
 * This file contains the implementation of the VirtualCore class which manages
 * actor execution within a single thread. It handles event routing, actor lifecycle,
 * and inter-core communication within the QB Actor Framework.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 * @ingroup Core
 */

#include <qb/core/VirtualCore.h>
#include <qb/event.h>
#include <qb/io/async/listener.h>
#include <qb/system/cpu.h>
#include <qb/system/timestamp.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>

typedef struct cpu_set {
    uint32_t count;
} cpu_set_t;

static inline void
CPU_ZERO(cpu_set_t *cs) {
    cs->count = 0;
}

static inline void
CPU_SET(int num, cpu_set_t *cs) {
    cs->count |= (1 << num);
}

static inline int
CPU_ISSET(int num, cpu_set_t *cs) {
    return (cs->count & (1 << num));
}

static int
pthread_getaffinity_np(pthread_t thread, size_t cpusetsize, cpu_set_t *cpuset) {
    if (!cpuset)
        return false;

    // Only logical cores count on macOS
    int    num_cores;
    size_t len = sizeof(num_cores);
    if (sysctlbyname("hw.logicalcpu", &num_cores, &len, nullptr, 0))
        return 1;

    CPU_ZERO(cpuset);

    // On macOS, no api to set affinity,
    for (int i = 0; i < num_cores; i++) {
        CPU_SET(i, cpuset);
    }

    return 0;
}

static int
pthread_setaffinity_np(pthread_t thread, size_t cpu_size, cpu_set_t *cpu_set) {
    thread_port_t mach_thread;
    int           core = 0;

    for (core = 0; core < 8 * cpu_size; core++) {
        if (CPU_ISSET(core, cpu_set))
            break;
    }
    if (core >= std::thread::hardware_concurrency())
        return -1;
    thread_affinity_policy_data_t policy = {core};
    mach_thread                          = pthread_mach_thread_np(thread);
    const auto ret = thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY,
                                       (thread_policy_t) &policy, 1);
    return !(ret == KERN_SUCCESS || ret == KERN_NOT_SUPPORTED);
}
#endif

namespace qb {
VirtualCore::VirtualCore(CoreId const id, SharedCoreCommunication &engine) noexcept
    : _index(id)
    , _resolved_index(engine._core_set.resolve(id))
    , _engine(engine)
    , _mail_box(engine.getMailBox(id))
    , _event_buffer(std::make_unique<EventBuffer>())
    , _pipes(engine.getNbCore())
    , _mono_pipe_swap(_pipes[_resolved_index])
    , _mono_pipe(std::make_unique<VirtualPipe>()) {
    for (auto i = _nb_service + 1; i < ActorId::BroadcastSid; ++i) {
        _ids.insert(static_cast<ServiceId>(i));
    }
}

VirtualCore::~VirtualCore() noexcept {
    for (auto [_, actor] : _actors)
        delete actor;
    for (auto [_, callback] : _actor_callbacks)
        delete callback;
}

ActorId
VirtualCore::__generate_id__() noexcept {
    return _ids.empty() ? ActorId(ActorId::NotFound)
                        : ActorId(_ids.extract(_ids.begin()).value(), _index);
}

// Event Management
void
VirtualCore::unregisterEvents(ActorId const id) noexcept {
    _router.unsubscribe(id);
}

VirtualPipe &
VirtualCore::__getPipe__(CoreId const core) noexcept {
    return _pipes[_engine._core_set.resolve(core)];
}

void
VirtualCore::__receive_events__(EventBucket *buffer, std::size_t const nb_events) {
    std::size_t i = 0;
    while (i < nb_events) {
        auto event = reinterpret_cast<Event *>(buffer + i);

        event->state.alive = 0;
        _router.route(*event, [this](auto &event) {
            if (!event.getDestination().is_broadcast())
                LOG_WARN(*this << " failed to send event[" << event.getID()
                               << "] sent from " << event.getSource());
        });
        ++_metrics._nb_event_received;
        _metrics._nb_bucket_received += event->bucket_size;
        i += event->bucket_size;
    }
}

void
VirtualCore::__receive__() {
    // from same core
    _mono_pipe->swap(_mono_pipe_swap);
    __receive_events__(_mono_pipe->begin(), _mono_pipe->size());
    _mono_pipe->reset();
    // global_core_events
    _mail_box.dequeue(
        [this](EventBucket *buffer, std::size_t const nb_events) {
            __receive_events__(buffer, nb_events);
        },
        _event_buffer->data(), MaxRingEvents);
}

//    void VirtualCore::__receive_from__(CoreId const index) noexcept {
//        _mail_box.ringOf(index).dequeue([this](EventBucket *buffer, std::size_t const
//        nb_events) {
//            __receive_events__(buffer, nb_events);
//        }, _event_buffer.data(), MaxRingEvents);
//    }

bool
VirtualCore::__flush_all__() noexcept {
    bool ret = false;
    auto in  = 0u;
    for (auto &pipe : _pipes) {
        if (in != _resolved_index && pipe.size()) {
            ret    = true;
            auto i = pipe.begin();
            while (i < pipe.end()) {
                const auto &event = *reinterpret_cast<const Event *>(i);
                ++_metrics._nb_event_sent_try;
                if (!try_send(event) && event.state.qos) {
                    ++_metrics._nb_event_sent_try;
                    static thread_local auto &current_lock =
                        _engine._event_safe_deadlock[_resolved_index];
                    // current locked by event set to true
                    current_lock.store(true, std::memory_order_release);
                    while (!try_send(event)) {
                        ++_metrics._nb_event_sent_try;
                        // entering in deadlock
                        if (current_lock.load(std::memory_order_acquire)) {
                            // notify to unlock dest core
                            _engine
                                ._event_safe_deadlock[_engine._core_set.resolve(
                                    event.dest.index())]
                                .store(false, std::memory_order_release);
                        } else {
                            // partial send another core is maybe in deadlock
                            pipe.reset(i - pipe.data());
                            goto end;
                        }
                    }
                }
                ++_metrics._nb_event_sent;
                _metrics._nb_bucket_sent += event.bucket_size;
                i += event.bucket_size;
            }
            pipe.reset();
        }
    end:;
        ++in;
    }
    return ret;
}
//! Event Management

// Workflow
bool
VirtualCore::__init__(CoreIdSet const &affinity_cores) {
    bool ret(true);
    if (!affinity_cores.empty()) {
#if defined(unix) || defined(__unix) || defined(__unix__) || defined(__APPLE__)
        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        for (const auto core : affinity_cores)
            CPU_SET(core, &cpuset);

        pthread_t current_thread = pthread_self();
        ret = !pthread_getaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (!ret)
            LOG_WARN("get thread affinity failed: " << strerror(errno));
        ret = !pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (!ret)
            LOG_WARN("set thread affinity failed: " << strerror(errno));
#elif defined(_WIN32) || defined(_WIN64)
#ifdef _MSC_VER
        DWORD_PTR mask = 0u;
        for (const auto core : affinity_cores)
            mask |= static_cast<DWORD_PTR>(1u) << core;
        ret = (SetThreadAffinityMask(GetCurrentThread(), mask));
        if (!ret)
            LOG_WARN("set thread affinity failed");
#else
#warning "Cannot set affinity on windows with GNU Compiler"
#endif
#endif
    }
    _actor_to_remove.reserve(_actors.size());
    return ret;
}

bool
VirtualCore::__init__actors__() const {
    // Create a vector of pairs of ActorId and Actor*
    // This is done to avoid modifying the _actors map while iterating over it
    // This is safe because we are not modifying the map while iterating over it
    std::vector<std::pair<ActorId, Actor *>> actors_to_init;
    for (auto &actor : _actors) {
        actors_to_init.push_back(actor);
    }
    // Init Static Actors
    return !std::any_of(actors_to_init.begin(), actors_to_init.end(), [](auto &pair) {
        auto ret = !pair.second->onInit();
        if (ret)
            LOG_CRIT(*pair.second << " failed to init");
        return ret;
    });
}

void
VirtualCore::__workflow__() {
    LOG_INFO(*this << " Init Success " << static_cast<uint32_t>(_actors.size())
                   << " actor(s)");
    while (likely(true)) {
        _metrics._nanotimer = Timestamp::nano();
        // core has io
        if (io::async::listener::current.size())
            _metrics._nb_event_io = io::async::run(EVRUN_NOWAIT);
        // send core events
        __flush_all__();
        // receive core events
        __receive__();
        // check if reception killed actors
        if (unlikely(!_actor_to_remove.empty()))
            goto removeActors;
        // call registered actor callbacks
        for (const auto &callback : _actor_callbacks)
            callback.second->onCallback();
        // check if callbacks killed actors
        if (unlikely(!_actor_to_remove.empty())) {
        removeActors:
            // remove dead actors
            for (auto const &actor : _actor_to_remove)
                removeActor(actor);
            _actor_to_remove.clear();
            if (_actors.empty()) {
                break;
            }
        }
        // reset metrics
        _metrics.reset();
        if (_mail_box.getLatency()) {
            if (likely(_metrics._sleep_count))
                --_metrics._sleep_count;
            else
                _mail_box.wait();
        }
    }
    // receive and flush residual events
    do {
        __receive__();
    } while (__flush_all__());

    LOG_INFO(*this << " Stopped normally");
}

//! Workflow
// Actor Management
ActorId
VirtualCore::initActor(Actor &actor, bool const doInit) noexcept {
    if (doInit && unlikely(!actor.onInit())) {
        removeActor(actor.id());

        return ActorId::NotFound;
    }

    return actor.id();
}

ActorId
VirtualCore::appendActor(Actor &actor, bool const doInit) noexcept {
    if (initActor(actor, doInit).is_valid()) {
        if (_actors.find(actor.id()) == _actors.end()) {
            _actors.insert({actor.id(), &actor});
            LOG_INFO("New " << actor);
        } else {
            LOG_CRIT("Error Cannot add Service Actor multiple times" << actor);
            return ActorId::NotFound;
        }
        return actor.id();
    }
    return ActorId::NotFound;
}

void
VirtualCore::removeActor(ActorId const id) noexcept {
    __unregisterCallback(id);
    unregisterEvents(id);
    const auto it = _actors.find(id);
    if (it != _actors.end()) {
        LOG_INFO("Delete " << *it->second);
        delete it->second;
        _actors.erase(it);
        if (id._service_id > _nb_service)
            _ids.insert(id._service_id);
    }
}

//! Actor Management

void
VirtualCore::killActor(ActorId const id) noexcept {
    _actor_to_remove.insert(id);
}
void
VirtualCore::__unregisterCallback(ActorId const id) noexcept {
    auto it = _actor_callbacks.find(id);
    if (it != _actor_callbacks.end())
        _actor_callbacks.erase(it);
}

void
VirtualCore::unregisterCallback(ActorId const id) noexcept {
    push<UnregisterCallbackEvent>(id, id);
}

// Event Api
Pipe
VirtualCore::getProxyPipe(ActorId const dest, ActorId const source) noexcept {
    return {__getPipe__(dest._core_id), dest, source};
}

bool
VirtualCore::try_send(Event const &event) const noexcept {
    return _engine.send(event);
}

void
VirtualCore::send(Event const &event) noexcept {
    if (event.dest._core_id == _index || !try_send(event)) {
        auto &pipe = __getPipe__(event.dest._core_id);
        pipe.recycle(event, event.bucket_size);
    }
}

Event &
VirtualCore::push(Event const &event) noexcept {
    auto &pipe = __getPipe__(event.dest._core_id);
    return pipe.recycle_back(event, event.bucket_size);
}

void
VirtualCore::reply(Event &event) noexcept {
    std::swap(event.dest, event.source);
    event.state.alive = 1;
    send(event);
}

void
VirtualCore::forward(ActorId const dest, Event &event) noexcept {
    event.dest        = dest;
    event.state.alive = 1;
    send(event);
}
//! Event Api

CoreId
VirtualCore::getIndex() const noexcept {
    return _index;
}

const CoreIdSet &
VirtualCore::getCoreSet() const noexcept {
    return _engine._core_set.raw();
}

uint64_t
VirtualCore::time() const noexcept {
    return _metrics._nanotimer;
}

ServiceId                 VirtualCore::_nb_service = 0;
thread_local VirtualCore *VirtualCore::_handler    = nullptr;
} // namespace qb
#ifdef QB_LOGGER
qb::io::log::stream &
qb::operator<<(qb::io::log::stream &os, qb::VirtualCore const &core) {
    os << "VirtualCore(" << core.getIndex() << ").id(" << std::this_thread::get_id()
       << ")";
    return os;
}
#endif

std::ostream &
qb::operator<<(std::ostream &os, qb::VirtualCore const &core) {
    os << "VirtualCore(" << core.getIndex() << ").id(" << std::this_thread::get_id()
       << ")";
    return os;
}