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
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
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

// NB: a macOS shim for pthread_getaffinity_np() used to live here, but
// __init__ no longer calls getaffinity (it would clobber the requested cpuset),
// so it has been removed as dead code.

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
    // Seed the pool after the last statically-registered service id. The
    // atomic load is relaxed because every writer publishes through the
    // magic-static acquire edge of `Actor::registerIndex<Tag>()` (2.3).
    _ids.init(static_cast<ServiceId>(
        _nb_service.load(std::memory_order_relaxed) + 1));
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

        // Defensive: a well-formed event always spans at least one bucket. A
        // zero bucket_size (only reachable via a malformed / oversized
        // allocated_push whose uint16 size field wrapped to 0) would make
        // `i += 0` spin forever, re-routing the same event and hanging the
        // core. Stop draining this batch instead of looping indefinitely.
        if (unlikely(event->bucket_size == 0)) {
            LOG_CRIT(*this << " received event with bucket_size==0 (malformed or "
                              "oversized event); aborting batch");
            break;
        }
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

// -----------------------------------------------------------------------------
// __flush_all__ — structured, deadlock-free outbound pipe drain (finding 2.4).
// -----------------------------------------------------------------------------
//
// Scenario: when core A and core B *simultaneously* hold full outbound pipes
// for each other **and** their respective ingress mailboxes are full, neither
// can progress without first reading from its own mailbox. Unbounded retry in
// `try_send` would therefore deadlock.
//
// Invariant re-established by this implementation: **every pass of
// `__flush_all__` terminates in bounded time**. Once a QoS-guaranteed event
// exhausts its retry budget, we perform a *partial flush* (the unsent tail is
// kept in the local pipe) and yield control to the caller. The caller
// (`__workflow__`) then drains the local mailbox via `__receive__`, which
// frees space for peers and lets the next `__flush_all__` pass make progress.
//
// Backoff policy (monotonic, cache-friendly):
//   [0, SPIN_THRESHOLD)   pure spin + `qb::spin_loop_pause()` (CPU hint — no
//                         scheduler involvement, minimal latency)
//   [SPIN_THRESHOLD, YIELD_THRESHOLD) `std::this_thread::yield()` — give the
//                         OS a chance to run the peer consumer
//   >= YIELD_THRESHOLD    partial bail: wake the destination's mailbox and
//                         return to the workflow loop.
//
// Non-QoS events (`event.state.qos == 0`) preserve their original
// best-effort semantics: a single `try_send` attempt, then drop on failure.
// -----------------------------------------------------------------------------

namespace {
// Tunables — deliberately kept out of the public header to allow empirical
// tuning without forcing recompiles of downstream code.
constexpr std::uint32_t kFlushSpinAttempts  = 64;  // `spin_loop_pause` phase.
constexpr std::uint32_t kFlushYieldAttempts = 512; // total budget per event.
static_assert(kFlushSpinAttempts < kFlushYieldAttempts,
              "spin phase must precede yield phase");
} // namespace

bool
VirtualCore::__flush_all__() noexcept {
    bool         any_work  = false;
    std::size_t  pipe_idx  = 0;
    for (auto &pipe : _pipes) {
        // Skip the self-core pipe (local delivery bypasses the mailbox layer)
        // and any empty outbound pipe.
        if (pipe_idx == _resolved_index || !pipe.size()) {
            ++pipe_idx;
            continue;
        }
        any_work = true;

        auto *const base = pipe.data();
        auto       *cur  = pipe.begin();
        auto *const end  = pipe.end();
        bool        partial = false;

        while (cur < end) {
            const auto &event = *reinterpret_cast<const Event *>(cur);
            ++_metrics._nb_event_sent_try;

            if (try_send(event)) {
                ++_metrics._nb_event_sent;
                _metrics._nb_bucket_sent += event.bucket_size;
                cur += event.bucket_size;
                continue;
            }

            if (!event.state.qos) {
                // Best-effort event: dropped on backpressure (preserves the
                // original fire-and-forget semantics for QoS-0 events such as
                // metrics or heartbeats). The "sent" counter is advanced to
                // remain consistent with the previous behaviour.
                ++_metrics._nb_event_sent;
                _metrics._nb_bucket_sent += event.bucket_size;
                cur += event.bucket_size;
                continue;
            }

            // QoS-guaranteed event: bounded backoff.
            bool sent = false;
            for (std::uint32_t attempt = 1; attempt <= kFlushYieldAttempts;
                 ++attempt) {
                ++_metrics._nb_event_sent_try;
                if (try_send(event)) {
                    sent = true;
                    break;
                }
                if (attempt < kFlushSpinAttempts) {
                    qb::spin_loop_pause();
                } else {
                    std::this_thread::yield();
                }
            }

            if (sent) {
                ++_metrics._nb_event_sent;
                _metrics._nb_bucket_sent += event.bucket_size;
                cur += event.bucket_size;
                continue;
            }

            // Budget exhausted — surrender cleanly. The destination's consumer
            // is woken so it runs immediately (no-op when its mailbox is in
            // zero-latency spin mode).
            _engine.getMailBox(event.dest.index()).notify();
            pipe.reset(static_cast<std::size_t>(cur - base));
            partial = true;
            break;
        }

        if (!partial)
            pipe.reset();

        ++pipe_idx;
    }
    return any_work;
}
//! Event Management

// Workflow
bool
VirtualCore::__init__(CoreIdSet const &affinity_cores) {
    bool ret(true);
    // Filter out the public `qb::NoAffinity` sentinel (== CoreId::max()) and
    // any out-of-range CoreId so users can pass `CoreIdSet{qb::NoAffinity}`
    // without triggering UB in the OS-level pinning APIs.
    auto is_real_core = [](CoreId c) noexcept {
        return c < static_cast<CoreId>(qb::MaxCores);
    };
    if (!affinity_cores.empty() &&
        std::any_of(affinity_cores.begin(), affinity_cores.end(), is_real_core)) {
#if defined(unix) || defined(__unix) || defined(__unix__) || defined(__APPLE__)
        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        for (const auto core : affinity_cores)
            if (is_real_core(core))
                CPU_SET(core, &cpuset);

        pthread_t current_thread = pthread_self();
        // NB: do NOT call pthread_getaffinity_np() on `cpuset` here. It writes the
        // thread's *current* affinity mask into `cpuset`, overwriting the requested
        // set built above — so the subsequent pthread_setaffinity_np() would just
        // re-apply the current affinity (a no-op on Linux) and the requested
        // per-core pinning would be silently discarded. Apply the requested set
        // directly.
        //
        // Affinity is best-effort: a logical QB CoreId need not map to a physical
        // CPU (e.g. core 255 on an 8-core host), so a failed pin must NOT fail the
        // VirtualCore init — it only loses a placement optimisation. Warn and
        // continue. (Previously the pthread_getaffinity_np clobber masked this by
        // always pinning to CPU 0, which trivially succeeds.)
        if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0)
            LOG_WARN("set thread affinity failed: " << strerror(errno));
#elif defined(_WIN32) || defined(_WIN64)
#ifdef _MSC_VER
        constexpr auto kAffinityBits =
            static_cast<CoreId>(sizeof(DWORD_PTR) * 8u);
        DWORD_PTR mask = 0u;
        for (const auto core : affinity_cores)
            if (is_real_core(core) && core < kAffinityBits)
                mask |= static_cast<DWORD_PTR>(1u) << core;
        // QB CoreIds are logical ids; they may exceed the affinity width of a
        // single Windows processor group. In that case, skip OS pinning rather
        // than failing VirtualCore init for an otherwise legal QB core id.
        // Affinity is best-effort: never fail init on a pin failure (warn only).
        if (mask != 0u && SetThreadAffinityMask(GetCurrentThread(), mask) == 0)
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
        _metrics._nanotimer = static_cast<uint64_t>(qb::unix_nanos(qb::wall_now()));

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
            // Hot path: call `listener::run` directly — no `async::run` wrapper
            // (avoids redundant checks; metrics match `nb_invoked_event()` contract).
            io::async::listener::current.run(EVRUN_NOWAIT);
            _metrics._nb_event_io = io::async::listener::current.nb_invoked_event();
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
            thread_local std::vector<CallbackEntry> cb_snapshot;
            cb_snapshot = _callback_list;
            for (auto const &entry : cb_snapshot) {
                // Skip the callback of an actor killed earlier in this same
                // dispatch pass (e.g. by an earlier actor's onCallback). The
                // object is still alive — destruction is deferred to the
                // removeActors phase below — so this is purely a semantics fix:
                // a killed actor must not get another tick, matching the
                // event-kill path which skips the whole callback phase. The
                // empty() fast-path keeps the common (nothing killed) case free.
                if (likely(_actor_to_remove.empty()) ||
                    !_actor_to_remove.count(entry.id))
                    entry.cb->onCallback();
            }
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
        if (_mail_box.getLatency() > qb::duration::zero()) {
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
        // Only non-service ids are recycled into the pool: a ServiceActor's
        // id is assigned at static init (see 2.3) and must remain reserved
        // for the lifetime of the process to keep `ServiceIndex` stable.
        if (id._service_id > _nb_service.load(std::memory_order_relaxed))
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
        _actor_callbacks.erase(it);
        // Keep the flat callback snapshot in sync (2.6).
        auto vit = std::find_if(_callback_list.begin(), _callback_list.end(),
                                [id](CallbackEntry const &e) { return e.id == id; });
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

std::atomic<ServiceId>    VirtualCore::_nb_service{0};
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
