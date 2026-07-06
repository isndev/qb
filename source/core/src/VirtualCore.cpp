/**
 * @file qb/core/src/VirtualCore.cpp
 * @brief Implementation of the VirtualCore class for the QB Actor Framework
 *
 * This file contains the implementation of the VirtualCore class which manages
 * actor execution within a single thread. It handles event routing, actor lifecycle,
 * and inter-core communication within the QB Actor Framework.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#include <climits>
#include <ostream>
#include <qb/core/VirtualCore.h>
#include <qb/event.h>
#include <qb/io/async/listener.h>
#include <qb/system/cpu.h>
#include <qb/system/time.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/types.h>

// Modern C++: use using alias instead of typedef struct
using cpu_set_t = struct cpu_set {
    uint32_t count;
};

constexpr int CPU_SETSIZE = static_cast<int>(sizeof(uint32_t) * CHAR_BIT);

static inline void
CPU_ZERO(cpu_set_t *cs) {
    cs->count = 0;
}

static inline void
CPU_SET(int num, cpu_set_t *cs) {
    if (num < 0 || num >= CPU_SETSIZE) {
        return;
    }
    cs->count |= (1 << num);
}

static inline int
CPU_ISSET(int num, cpu_set_t *cs) {
    if (num < 0 || num >= CPU_SETSIZE) {
        return 0;
    }
    return (cs->count & (1 << num));
}

// NB: a macOS shim for pthread_getaffinity_np() used to live here, but
// __init__ no longer calls getaffinity (it would clobber the requested cpuset),
// so it has been removed as dead code.

static int
pthread_setaffinity_np(pthread_t thread, size_t cpu_size, cpu_set_t *cpu_set) {
    // Modern C++: use std::cmp_less for safe mixed-signed comparisons (or use unsigned types)
    // Here we use size_t to match the unsigned nature of cpu_size and hardware_concurrency
    size_t core = 0;

    for (; core < 8 * cpu_size; ++core) {
        if (CPU_ISSET(static_cast<int>(core), cpu_set))
            break;
    }
    if (core >= std::thread::hardware_concurrency())
        return -1;
    thread_affinity_policy_data_t policy      = {static_cast<integer_t>(core)};
    thread_port_t                 mach_thread = pthread_mach_thread_np(thread);
    const auto                    ret = thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy), 1);
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
    _ids.init(static_cast<ServiceId>(_nb_service.load(std::memory_order_relaxed) + 1));
}

VirtualCore::~VirtualCore() noexcept = default;

void
VirtualCore::__set_stop_token__(qb::stop_token token) noexcept {
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
VirtualCore::unregisterEvents(ActorId const id) const noexcept {
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
        // Activation gate: while the destination actor is still Activating (an
        // `onInit()` performed a `co_await`), defer its inbound *unicast business*
        // events into the actor's FIFO stash — replayed in order once it becomes
        // active. Broadcasts (incl. Kill/Signal) pass straight through so a kill during
        // init still unwinds the coroutine. The empty() fast-path keeps the common case
        // (nothing activating) free.
        if (unlikely(!_activating.empty())) {
            const ActorId dest = event->getDestination();
            // A `KillEvent` always passes the gate so an Activating actor stays killable
            // (its `on(KillEvent)` → `kill()` cancels the scope and unwinds the in-flight
            // onInit). Everything else unicast to an Activating actor is either delivered
            // (if it is the reply to an `ask` this actor issued from inside `onInit` — that
            // must NOT be stashed or its init would deadlock on its own reply) or stashed
            // and replayed FIFO once the actor becomes active.
            if (!dest.is_broadcast() && __is_activating__(dest) && event->getID() != qb::Event::type_to_id<qb::KillEvent>()) {
                if (!qb::detail::ask_try_deliver_reply(*event, dest)) {
                    // Stash for FIFO replay on activation. If the cap overflowed the event is
                    // dropped here, so dispose its payload (the byte-copy never happened) to
                    // avoid leaking a non-trivial std::string/std::vector member.
                    if (!__stash_event__(dest, event))
                        _router.dispose(*event);
                }
                ++_metrics._nb_event_received;
                _metrics._nb_bucket_received += event->bucket_size;
                i += event->bucket_size;
                continue;
            }
        }
        event->state.alive = 0;
        _router.route(*event, [this](auto &event) {
            if (!event.getDestination().is_broadcast())
                LOG_WARN(*this << " failed to send event[" << event.getID() << "] sent from " << event.getSource());
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
        [this](EventBucket *buffer, std::size_t const nb_events) { __receive_events__(std::span<EventBucket>{buffer, nb_events}); },
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
static_assert(kFlushSpinAttempts < kFlushYieldAttempts, "spin phase must precede yield phase");
} // namespace

bool
VirtualCore::__flush_all__() noexcept {
    bool        any_work = false;
    std::size_t pipe_idx = 0;
    for (auto &pipe : _pipes) {
        // Skip the self-core pipe (local delivery bypasses the mailbox layer)
        // and any empty outbound pipe.
        if (pipe_idx == _resolved_index || !pipe.size()) {
            ++pipe_idx;
            continue;
        }
        any_work = true;

        auto *const base    = pipe.data();
        auto       *cur     = pipe.begin();
        auto *const end     = pipe.end();
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
            for (std::uint32_t attempt = 1; attempt <= kFlushYieldAttempts; ++attempt) {
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
    if (!affinity_cores.empty() && std::any_of(affinity_cores.begin(), affinity_cores.end(), is_real_core)) {
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
        constexpr auto kAffinityBits = static_cast<CoreId>(sizeof(DWORD_PTR) * 8u);
        DWORD_PTR      mask          = 0u;
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
VirtualCore::__init__actors__() {
    // Snapshot the actor pointers first: driving an `onInit()` may itself create
    // referenced actors (`addRefActor`), mutating `_actors` mid-iteration.
    std::vector<Actor *> actors_to_init;
    actors_to_init.reserve(_actors.size());
    for (const auto &actor : _actors | std::views::values)
        actors_to_init.push_back(actor.get());
    for (auto *actor : actors_to_init) {
        qb::io::async::task<bool> init = actor->onInit();
        switch (__drive_init__(*actor, init)) {
            case InitOutcome::ReadyTrue:
                break; // completed synchronously → already active, frame freed
            case InitOutcome::ReadyFalse:
                LOG_CRIT(*actor << " failed to init");
                return false;
            case InitOutcome::Suspended:
                // `onInit()` performed a `co_await`; the suspended initial init cannot
                // complete pre-loop (the scheduler is not running yet). It completes in
                // `__workflow__` (its awaiters resume on `listener.run()`); the dispatch
                // gate stashes its inbound unicast until it becomes active.
                __begin_activation__(*actor, std::move(init));
                break;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// Asynchronous actor initialization — the *Activating* phase (driver + pump).
// -----------------------------------------------------------------------------

VirtualCore::InitOutcome
VirtualCore::__drive_init__(Actor &actor, qb::io::async::task<bool> &init) noexcept {
    auto h = init.handle();
    if (unlikely(!h))
        return InitOutcome::ReadyTrue; // defensive: a null task ⇒ trivially successful
    // `task`'s initial_suspend is `suspend_always`, so the body has not run yet: resume
    // once to reach the first `co_await` or the `co_return`. Drive the handle DIRECTLY
    // (never `scheduler.spawn()` it) so a synchronously-ready init keeps no scheduler
    // continuation and its frame is freed the instant the owning `task` is destroyed.
    h.resume();
    if (h.done()) {
        auto &p = h.promise();
        if (unlikely(p.has_exception())) {
            // Surface an uncaught throw as an init failure (preserves the BadActorInit
            // outcome) — but never let it escape into this noexcept owning-thread path.
            try {
                std::rethrow_exception(p.exception());
            } catch (const std::exception &e) {
                LOG_CRIT(actor << " onInit() threw: " << e.what());
            } catch (...) {
                LOG_CRIT(actor << " onInit() threw a non-standard exception");
            }
            return InitOutcome::ReadyFalse;
        }
        return p.value() ? InitOutcome::ReadyTrue : InitOutcome::ReadyFalse;
    }
    return InitOutcome::Suspended;
}

void
VirtualCore::__begin_activation__(Actor &actor, qb::io::async::task<bool> &&init) noexcept {
    // A suspended onInit is driven directly (not via scheduler.spawn), so this core may not
    // have a coroutine scheduler yet. Its awaiters resume via schedule_via_current (e.g.
    // qb::ask's reply/timeout delivery), which requires the TLS scheduler to exist — force
    // it now, before any reply/timer can fire on the next iteration.
    (void) qb::io::async::listener::current.coro_scheduler();
    // The actor is now Activating: gate its inbound unicast and keep its frame alive.
    actor._activated = false;
    const auto now   = static_cast<std::uint64_t>(qb::unix_nanos(qb::wall_now()));
    Activation act;
    act.init        = std::move(init);
    act.deadline_ns = activation_deadline_ns ? now + activation_deadline_ns : 0; // 0 ⇒ no deadline
    _activating.emplace(actor.id(), std::move(act));
    LOG_INFO(actor << " activating (async onInit in flight)");
}

bool
VirtualCore::__is_activating__(ActorId const id) const noexcept {
    return _activating.find(id) != _activating.end();
}

bool
VirtualCore::__stash_event__(ActorId const dest, Event *event) noexcept {
    auto it = _activating.find(dest);
    if (unlikely(it == _activating.end()))
        return false; // not actually activating — caller already filtered, defensive only
    auto &stash = it->second.stash;
    if (unlikely(stash.size() >= kActivationStashCap)) {
        // A wedged-in-init actor must not OOM the core: drop the overflow and fail the
        // activation on the next pump by forcing its deadline to expire now. Report `false`
        // so the caller disposes the dropped event's payload (it is not taken into the stash).
        LOG_WARN(*this << " activation stash full for actor(" << dest.index() << "." << dest.sid() << "); dropping event[" << event->getID()
                       << "] and failing activation");
        it->second.deadline_ns = 1; // already in the past ⇒ pump cancels + fails it
        return false;
    }
    // Byte-copy the event's buckets out of the transient receive buffer into owned
    // storage; replayed verbatim (FIFO) once the actor becomes active. Ownership of any
    // non-trivial payload moves to the stash copy (the original is not disposed by the
    // caller); the copy is disposed either on replay (route) or on drop (__pump_activations__).
    auto *buckets = reinterpret_cast<EventBucket *>(event);
    stash.emplace_back(buckets, buckets + event->bucket_size);
    return true;
}

void
VirtualCore::__pump_activations__() noexcept {
    if (likely(_activating.empty()))
        return;
    const auto now = static_cast<std::uint64_t>(qb::unix_nanos(qb::wall_now()));

    // Collect ids to finalize/expire first; finalizing mutates `_activating`.
    thread_local std::vector<ActorId> done_ids;
    done_ids.clear();
    for (auto &[id, act] : _activating) {
        if (act.init.done()) {
            done_ids.push_back(id);
        } else if (!act.cancelling && act.deadline_ns && now >= act.deadline_ns) {
            // Deadline reached: cancel the actor's coro scope so its `onInit()` unwinds
            // (its cancellation-aware awaiters throw `cancelled_error`); it then reports
            // `done()` on a later pump and is finalized as a failure below.
            act.cancelling = true;
            if (const auto ait = _actors.find(id); ait != _actors.end()) {
                LOG_WARN(*ait->second << " activation deadline expired — cancelling onInit");
                ait->second->__cancel_coro_scope__();
            }
        }
    }

    for (auto const id : done_ids) {
        auto it = _activating.find(id);
        if (it == _activating.end())
            continue;
        Activation act = std::move(it->second);
        _activating.erase(it);

        const bool dying = _dying_with_frame.erase(id) != 0;
        // Read the init verdict (frame is done): a clean `co_return false`, a thrown
        // exception, or a deadline/kill cancellation all resolve to "not successful".
        bool ok = false;
        if (auto h = act.init.handle(); h && h.done()) {
            auto &p = h.promise();
            ok      = !p.has_exception() && p.value();
        }
        // Free the onInit frame now that it has fully unwound (no awaiter references it).
        act.init = qb::io::async::task<bool>{};

        const auto ait = _actors.find(id);
        if (dying || !ok || ait == _actors.end()) {
            // Killed during init, failed init, or already gone → complete teardown now
            // (the deferred-destroy: the actor outlived its own coroutine frame).
            // Dispose the never-replayed stash so any non-trivial event payload (std::string /
            // std::vector in a `push`'d event) is destroyed instead of leaked: the stash holds
            // byte-copied events whose destructors only ever run via route() (success path) or
            // here (drop path) — the raw `vector<EventBucket>` teardown would free bytes only.
            for (auto &buckets : act.stash) {
                auto *ev = reinterpret_cast<Event *>(buckets.data());
                _router.dispose(*ev);
            }
            if (ait != _actors.end()) {
                if (!dying && !ok)
                    LOG_CRIT(*ait->second << " async onInit failed — removing");
                removeActor(id);
            }
            continue;
        }
        // Success: flip Active, then replay the stashed inbound unicast FIFO.
        ait->second->_activated = true;
        LOG_INFO(*ait->second << " activated");
        for (auto &buckets : act.stash) {
            auto *ev        = reinterpret_cast<Event *>(buckets.data());
            ev->state.alive = 0; // mark consumed, exactly as __receive_events__ does pre-route
            _router.route(*ev, [this](auto &e) {
                if (!e.getDestination().is_broadcast())
                    LOG_WARN(*this << " failed to deliver stashed event[" << e.getID() << "]");
            });
        }
    }
}

void
VirtualCore::__workflow__() {
    LOG_INFO(*this << " Init Success " << static_cast<uint32_t>(_actors.size()) << " actor(s)");
    while (likely(true)) {
        _metrics._nanotimer = static_cast<uint64_t>(qb::unix_nanos(qb::wall_now()));
        ++_loop_count; // 1-based loop-pass index surfaced to callbacks via qb::LoopEvent

        // Poll for pending signal (signal-handler-safe lock-free atomic read)
        // OR for a C++20 cooperative cancellation request coming from the
        // engine's `std::stop_source` (finding 2.17 — jthread/stop_token).
        // The stop_token path is checked only when no signal is already pending
        // so the existing signum semantics are preserved: we synthesise a
        // virtual SIGINT to reuse the same shutdown plumbing (SignalEvent
        // broadcast + actors' `onSignal` / `kill()` chain).
        // Steady state carries no pending signal, so keep the hot path at exactly one relaxed load +
        // the stop-token check (the pre-existing cost) and pull the generation load into the cold
        // shutdown branch below — an atomic load per loop pass is not free on this hot path.
        const auto pending_signal = Main::_signal_pending.load(std::memory_order_relaxed);
        const bool stop_requested = _stop_token.stop_possible() && _stop_token.stop_requested();
        if (unlikely(pending_signal != 0 || stop_requested)) {
            // Cold (shutdown only). A single per-core "consumed" latch used to drop every signal after
            // the first — leaving the engine unstoppable after e.g. a SIGHUP reload, a double Ctrl-C,
            // or Main::stop() after an earlier signal. Re-synthesize on every newly-raised signal
            // (Main::_signal_generation advanced) and once for the cooperative stop_token latch.
            // Signals COALESCE to the latest generation: the single _signal_pending slot holds only the
            // most recent signum, so two signals between loop passes deliver one event carrying the latest
            // — sufficient for the lifecycle/shutdown contract (no per-signum fan-out guarantee).
            // Acquire the generation, then re-load the signum under it so (generation, signum) stay
            // coherent — pairs with the release bump in onSignal()/stop().
            const auto signal_generation = Main::_signal_generation.load(std::memory_order_acquire);
            const auto signum            = Main::_signal_pending.load(std::memory_order_relaxed);
            const bool new_signal        = (signum != 0) && (signal_generation != _last_signal_generation);
            if (new_signal || (stop_requested && !_stop_delivered)) {
                if (new_signal)
                    _last_signal_generation = signal_generation;
                if (stop_requested)
                    _stop_delivered = true;
                SignalEvent sig_event;
                fill_event<SignalEvent>(sig_event, BroadcastId(_index), BroadcastId(_index));
                sig_event.signum = (signum != 0) ? signum : SIGINT;
                auto &pipe       = __getPipe__(_index);
                pipe.recycle(sig_event, sig_event.bucket_size);
            }
        }

        if (io::async::listener::current.has_coro_scheduler() || io::async::listener::current.size()) {
            // Hot path: call `listener::run` directly — no `async::run` wrapper
            // (avoids redundant checks; metrics match `nb_invoked_event()` contract).
            io::async::listener::current.run(EVRUN_NOWAIT);
            _metrics._nb_event_io = io::async::listener::current.nb_invoked_event();
        }

        // Complete any async `onInit()` resumed above (replay stashes / enforce
        // deadlines / finish deferred destroys). empty()-guarded: free when idle.
        if (unlikely(!_activating.empty())) {
            __pump_activations__();
            if (unlikely(_actors.empty()))
                break; // the last actor was an activating-then-dying one
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
        // snapshot is still required because the tick handler `on(LoopEvent&)` may
        // register or unregister actors during dispatch (e.g. via `addRefActor`).
        {
            // One LoopEvent for the whole pass — same `now`/`iteration` for every callback,
            // consistent with `Actor::time()`. Delivered by a direct virtual call (not routed).
            const qb::LoopEvent                     loop_ev{_metrics._nanotimer, _loop_count};
            thread_local std::vector<CallbackEntry> cb_snapshot;
            cb_snapshot = _callback_list;
            for (auto const &entry : cb_snapshot) {
                // Skip the callback of an actor killed earlier in this same
                // dispatch pass (e.g. by an earlier actor's tick). The
                // object is still alive — destruction is deferred to the
                // removeActors phase below — so this is purely a semantics fix:
                // a killed actor must not get another tick, matching the
                // event-kill path which skips the whole callback phase. The
                // empty() fast-path keeps the common (nothing killed) case free.
                if (likely(_actor_to_remove.empty()) || !_actor_to_remove.count(entry.id))
                    entry.cb->on(loop_ev);
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
    // Receive and flush residual events, guaranteed to terminate without dropping anything
    // a live peer can still accept.
    //
    // The hot-loop invariant — __flush_all__ partial-bails on backpressure, then the peer's
    // __receive__ frees mailbox space so the next pass makes progress — does NOT hold at
    // shutdown: cores leave __workflow__ independently (there is no shutdown barrier), so a
    // peer can exit and stop draining its mailbox while this core still has QoS-guaranteed
    // events queued for it; the original unbounded `while (__flush_all__())` then spins
    // forever (try_send can never succeed against a full, no-longer-drained mailbox).
    //
    // We keep draining: every pass __receive__ frees OUR mailbox so live peers can deliver to
    // us, and __flush_all__ pushes our outbound. We only give up on a pipe once its
    // destination core has published its "stopped" flag — it has left its own workflow and
    // will never drain its mailbox again, so that residue can never be delivered; we dispose
    // it (the events were never sent, so neither this core nor any peer would otherwise free
    // a non-trivial QoS-2 payload) and drop it. A pipe to a still-live, merely backpressured
    // peer is retried, never dropped — that peer's __receive__ keeps making room, so the loop
    // makes progress and ends when our outbound is empty or every remaining target has gone.
    for (;;) {
        __receive__();
        if (!__flush_all__())
            break; // every outbound pipe drained — clean finish
        if (!__dispose_residual_to_stopped_cores__())
            break; // nothing left targets a still-live core — done
    }
    // Publish AFTER the final __receive__/__flush_all__ above: from here this core no longer
    // drains its mailbox, so peers must stop sending to it and dispose anything left for it.
    _engine.mark_core_stopped(_resolved_index);

    LOG_INFO(*this << " Stopped normally");
}

bool
VirtualCore::__dispose_residual_to_stopped_cores__() noexcept {
    bool        any_live_pending = false;
    std::size_t pipe_idx         = 0;
    for (auto &pipe : _pipes) {
        // The self-core pipe is delivered locally (never via the mailbox) and is already
        // drained by __receive__; an empty pipe has nothing pending.
        if (pipe_idx == _resolved_index || !pipe.size()) {
            ++pipe_idx;
            continue;
        }
        if (!_engine.is_core_stopped(static_cast<CoreId>(pipe_idx))) {
            // Destination still alive (its __receive__ keeps freeing mailbox room): keep its
            // events and retry next pass — never drop to a live peer.
            any_live_pending = true;
            ++pipe_idx;
            continue;
        }
        // Destination has left its workflow and will never drain its mailbox again: its queued
        // events can never be delivered. Free their non-trivial QoS-2 payloads via the global
        // disposer registry (no-op for trivially-destructible events) and drop them — this
        // mirrors the stash-drop dispose in __receive_events__.
        auto       *cur = pipe.begin();
        auto *const end = pipe.end();
        while (cur < end) {
            auto      &event = *reinterpret_cast<Event *>(cur);
            const auto bsz   = event.bucket_size;
            // Defensive: a zero bucket_size (only reachable via a malformed event) would spin
            // forever — stop draining this pipe instead (mirrors __receive_events__).
            if (unlikely(bsz == 0))
                break;
            _router.dispose(event);
            cur += bsz;
        }
        pipe.reset();
        ++pipe_idx;
    }
    return any_live_pending;
}

//! Workflow
// Actor Management
ActorId
VirtualCore::initActor(Actor &actor, bool const doInit) noexcept {
    if (doInit) {
        qb::io::async::task<bool> init = actor.onInit();
        switch (__drive_init__(actor, init)) {
            case InitOutcome::ReadyTrue:
                break; // completed synchronously → already active (identical to before)
            case InitOutcome::ReadyFalse:
                removeActor(actor.id());
                return ActorId::NotFound;
            case InitOutcome::Suspended:
                // Dynamic (`addRefActor`) async init: the actor exists and is addressable
                // now, but is not yet active. Its id is returned as VALID; inbound unicast
                // is stashed until it activates (gate), the deadline bounds the window.
                __begin_activation__(actor, std::move(init));
                break;
        }
    }

    return actor.id();
}

ActorId
VirtualCore::appendActor(std::unique_ptr<Actor> actor_ptr, bool const doInit) noexcept {
    Actor        &actor = *actor_ptr;
    const ActorId id    = actor.id();
    // Reject duplicates *before* driving `onInit()`: a suspended (async) init must never
    // coexist with an append failure, otherwise its still-live frame would be orphaned.
    if (unlikely(_actors.find(id) != _actors.end())) {
        LOG_CRIT("Error Cannot add Service Actor multiple times" << actor);
        return ActorId::NotFound;
    }
    if (initActor(actor, doInit).is_valid()) {
        _actors.emplace(id, std::move(actor_ptr));
        LOG_INFO("New " << actor);
        return id;
    }
    return ActorId::NotFound;
}

void
VirtualCore::removeActor(ActorId const id) noexcept {
    // Deferred destroy (the actor must outlive its own coroutine frame): if its
    // `onInit()` frame is still suspended, cancel the scope so the frame unwinds, mark
    // the actor dying, and let `__pump_activations__` complete the teardown once the
    // frame reports `done()`. Re-entry from the pump (after the frame unwound and the
    // activation was dropped) falls straight through to the normal teardown below.
    if (const auto ait = _activating.find(id); unlikely(ait != _activating.end())) {
        if (const auto act = _actors.find(id); act != _actors.end())
            act->second->__cancel_coro_scope__();
        if (!ait->second.init.done()) {
            _dying_with_frame.insert(id);
            return;
        }
        _activating.erase(ait);
        _dying_with_frame.erase(id);
    }
    __unregisterCallback(id);
    unregisterEvents(id);
    const auto it = _actors.find(id);
    if (it != _actors.end()) {
        auto &actor = it->second;
        // Catch-all cancel-on-destroy: every destruction path funnels through here
        // (kill, onInit failure, engine shutdown). Cancelling the scope wakes scoped
        // coroutines so they unwind cleanly; idempotent with kill()'s cancel.
        actor->__cancel_coro_scope__();
        if (actor->has_active_coroutines()) {
            if (actor->has_coro_scope())
                // Scoped coroutines were just cancelled — they unwind on the next loop
                // iteration. A non-zero count here is expected and safe.
                LOG_INFO(*actor << " destroyed with " << actor->active_coroutine_count() << " scoped coroutine(s) pending cancellation");
            else
                LOG_WARN(*actor << " destroyed with " << actor->active_coroutine_count()
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
        auto vit = std::ranges::find_if(_callback_list, [id](CallbackEntry const &e) { return e.id == id; });
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
    // Pass THIS core's resolved index as the MPSC producer slot: the physical
    // sending thread owns exactly one single-producer ring per destination
    // mailbox. `event.source` can name a different core after `forward()`, so it
    // must NOT drive slot selection (that would be a cross-core two-writer race).
    return _engine.send(_resolved_index, event);
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
thread_local VirtualCore *VirtualCore::_handler               = nullptr;
std::uint64_t             VirtualCore::activation_deadline_ns = 5ull * 1000u * 1000u * 1000u; // 5 s
} // namespace qb
#ifdef QB_WITH_LOGGING
qb::io::log::stream &
qb::operator<<(qb::io::log::stream &os, qb::VirtualCore const &core) {
    os << "VirtualCore(" << core.getIndex() << ").id(" << std::this_thread::get_id() << ")";
    return os;
}
#endif

std::ostream &
qb::operator<<(std::ostream &os, qb::VirtualCore const &core) {
    os << "VirtualCore(" << core.getIndex() << ").id(" << std::this_thread::get_id() << ")";
    return os;
}
