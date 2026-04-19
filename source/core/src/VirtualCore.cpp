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

#include <ostream>
#include <qb/core/VirtualCore.h>
#include <qb/event.h>
#include <qb/io/async/listener.h>
#include <qb/system/cpu.h>
#include <qb/system/timestamp.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>

// C++23: Use using alias instead of typedef struct
using cpu_set_t = struct cpu_set {
    uint32_t count;
};

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
    // C++23: Using auto for type deduction
    for (auto i = 0; i < num_cores; ++i) {
        CPU_SET(i, cpuset);
    }

    return 0;
}

static int
pthread_setaffinity_np(pthread_t thread, size_t cpu_size, cpu_set_t *cpu_set) {
    thread_port_t mach_thread;
    // C++23: Use std::cmp_less for safe mixed-signed comparisons (or use unsigned types)
    // Here we use size_t to match the unsigned nature of cpu_size and hardware_concurrency
    size_t core = 0;

    for (; core < 8 * cpu_size; ++core) {
        if (CPU_ISSET(static_cast<int>(core), cpu_set))
            break;
    }
    if (core >= std::thread::hardware_concurrency())
        return -1;
    thread_affinity_policy_data_t policy = {static_cast<integer_t>(core)};
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
    _ids.init(static_cast<ServiceId>(_nb_service + 1));
}

VirtualCore::~VirtualCore() noexcept = default;

void
VirtualCore::__set_stop_token__(std::stop_token token) noexcept {
    _stop_token = std::move(token);
}

ActorId
VirtualCore::__generate_id__() noexcept {
    if (_ids.empty())
        return ActorId(ActorId::NotFound);
    const auto sid = _ids.acquire();
    if (sid == ActorId::BroadcastSid)
        return ActorId(ActorId::NotFound);
    return ActorId(sid, _index);
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
VirtualCore::__receive_events__(std::span<EventBucket> events) {
    const std::size_t nb_events = events.size();
    std::size_t       i         = 0;
    while (i < nb_events) {
        // Safe reinterpret_cast: `events` is a contiguous view over EventBucket
        // storage, and Event objects are placement-constructed within it.
        auto event = reinterpret_cast<Event *>(events.data() + i);

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
    __receive_events__(std::span<EventBucket>{_mono_pipe->begin(), _mono_pipe->size()});
    _mono_pipe->reset();
    // global_core_events
    _mail_box.dequeue(
        [this](EventBucket *buffer, std::size_t const nb_events) {
            __receive_events__(std::span<EventBucket>{buffer, nb_events});
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
    std::vector<Actor *> actors_to_init;
    actors_to_init.reserve(_actors.size());
    for (const auto &[_, actor] : _actors)
        actors_to_init.push_back(actor.get());
    return !std::any_of(actors_to_init.begin(), actors_to_init.end(), [](auto *actor) {
        auto ret = !actor->onInit();
        if (ret)
            LOG_CRIT(*actor << " failed to init");
        return ret;
    });
}

void
VirtualCore::__workflow__() {
    LOG_INFO(*this << " Init Success " << static_cast<uint32_t>(_actors.size())
                   << " actor(s)");
    while (likely(true)) {
        _metrics._nanotimer = Timestamp::nano();

        // Poll for pending signal (async-signal-safe: volatile sig_atomic_t read)
        // OR for a C++20 cooperative cancellation request coming from the
        // engine's `std::stop_source` (finding 2.17 — jthread/stop_token).
        // The stop_token path is checked only when no signal is already pending
        // so the existing signum semantics are preserved: we synthesise a
        // virtual SIGINT to reuse the same shutdown plumbing (SignalEvent
        // broadcast + actors' `onSignal` / `kill()` chain).
        const bool signal_pending = (Main::_signal_pending != 0);
        const bool stop_requested =
            _stop_token.stop_possible() && _stop_token.stop_requested();
        if ((signal_pending || stop_requested) && !_signal_consumed) {
            _signal_consumed = true;
            SignalEvent sig_event;
            fill_event<SignalEvent>(sig_event, BroadcastId(_index),
                                   BroadcastId(_index));
            sig_event.signum = signal_pending ? Main::_signal_pending : SIGINT;
            auto &pipe = __getPipe__(_index);
            pipe.recycle(sig_event, sig_event.bucket_size);
        }

        if (io::async::listener::current.has_coro_scheduler() || 
            io::async::listener::current.size()) {
            _metrics._nb_event_io = io::async::run(EVRUN_NOWAIT);
        }
        
        // send core events
        __flush_all__();
        // receive core events
        __receive__();
        // check if reception killed actors
        if (unlikely(!_actor_to_remove.empty()))
            goto removeActors;
        // Dispatch callbacks from a flat, cache-friendly snapshot (2.6). The
        // master list `_callback_list` is kept in sync with the hashmap on
        // register / unregister, so building the per-iteration snapshot is now a
        // single contiguous copy instead of an `unordered_map` walk. A local
        // snapshot is still required because `onCallback()` may register or
        // unregister actors during dispatch (e.g. via `addRefActor`).
        {
            thread_local std::vector<ICallback *> cb_snapshot;
            cb_snapshot = _callback_list;
            for (auto *cb : cb_snapshot)
                cb->onCallback();
        }
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
        // Adaptive backoff: a busy iteration refills the spin credit; otherwise we
        // burn through the remaining credit before blocking on the mailbox (2.15).
        _metrics.carry_over();
        if (_mail_box.getLatency()) {
            if (likely(_metrics._spin_credit))
                --_metrics._spin_credit;
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
VirtualCore::appendActor(std::unique_ptr<Actor> actor_ptr, bool const doInit) noexcept {
    Actor &actor = *actor_ptr;
    if (initActor(actor, doInit).is_valid()) {
        ActorId id = actor.id();
        if (_actors.find(id) == _actors.end()) {
            _actors.emplace(id, std::move(actor_ptr));
            LOG_INFO("New " << actor);
        } else {
            LOG_CRIT("Error Cannot add Service Actor multiple times" << actor);
            return ActorId::NotFound;
        }
        return id;
    }
    return ActorId::NotFound;
}

void
VirtualCore::removeActor(ActorId const id) noexcept {
    __unregisterCallback(id);
    unregisterEvents(id);
    const auto it = _actors.find(id);
    if (it != _actors.end()) {
        auto &actor = it->second;
        if (actor->has_active_coroutines()) {
            LOG_WARN("Actor " << id << " destroyed with "
                     << actor->active_coroutine_count()
                     << " active coroutines - coroutines must not access actor state!");
        }
        LOG_INFO("Delete " << *actor);
        _actors.erase(it);
        if (id._service_id > _nb_service)
            _ids.release(id._service_id);
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
    if (it != _actor_callbacks.end()) {
        auto *cb = it->second;
        _actor_callbacks.erase(it);
        // Keep the flat callback snapshot in sync (2.6).
        auto vit = std::find(_callback_list.begin(), _callback_list.end(), cb);
        if (vit != _callback_list.end()) {
            *vit = _callback_list.back();
            _callback_list.pop_back();
        }
    }
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
#ifdef QB_WITH_LOGGING
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