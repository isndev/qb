/**
 * @file qb/core/VirtualCore.cpp
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

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <ostream>
#include <thread>
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
    // `1u`, not `1`: CPU_SETSIZE is 32, so `num` reaches 31 and `1 << 31` shifts into an int's
    // sign bit. C++20 defines that modulo 2^32, but the value then round-trips through
    // uint32_t and back to int in CPU_ISSET, which is out-of-range and implementation-defined.
    // An unsigned literal keeps the whole path well-defined on every standard qb targets (20/23).
    cs->count |= (1u << num);
}

static inline int
CPU_ISSET(int num, cpu_set_t *cs) {
    if (num < 0 || num >= CPU_SETSIZE) {
        return 0;
    }
    return static_cast<int>((cs->count & (1u << num)) != 0u);
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
    , _pipes(__make_pipes__(_pipe_pool, engine.getNbCore()))
    , _self_pipe(_pipes[_resolved_index]) {
    // Every logical core id, member of the set or not, maps to the pipe `resolve()` names for it;
    // `_pipes` never grows after this, so the pointers stay good for the core's lifetime.
    for (std::size_t core = 0; core < MaxCores; ++core)
        _pipe_of_core[core] = &_pipes[engine._core_set.resolve(core)];
    _peer_pipes.reserve(_pipes.size() - 1);
    for (std::size_t i = 0; i < _pipes.size(); ++i)
        if (i != _resolved_index)
            _peer_pipes.push_back(&_pipes[i]);
    // Seed the pool after the last statically-registered service id. The
    // atomic load is relaxed because every writer publishes through the
    // magic-static acquire edge of `Actor::registerIndex<Tag>()` (2.3).
    _ids.init(static_cast<ServiceId>(_nb_service.load(std::memory_order_relaxed) + 1));
    // The five default events resolve through the actor registry, not a handler table: install
    // their resolvers now, before any actor can register one (`memh::install` must precede the
    // first `subscribe<E>` for that type, and `registerEvent<E>` never subscribes a default one).
    // `*this` is a stack object on the worker thread (Main.cpp, `VirtualCore core(...)`) and is
    // never moved, so the reference each resolver keeps is good for the router's lifetime.
    [this]<typename... E>(std::tuple<E...> *) {
        (_router.install<E>(std::make_unique<DefaultEventResolver<E>>(*this)), ...);
    }(static_cast<default_events_t *>(nullptr));
    // The death watch's control events (Huly QB-51) are this core's to resolve, never an actor's.
    [this]<typename... C>(std::tuple<C...> *) {
        (_router.install<C>(std::make_unique<WatchControlResolver<C>>(*this)), ...);
    }(static_cast<std::tuple<detail::WatchRequest, detail::UnwatchRequest, detail::WatchDown, detail::CoreStopping> *>(nullptr));
}

VirtualCore::~VirtualCore() noexcept {
    _tearing_down = true;
    // This runs while Main still keeps the owning-thread TLS context, router,
    // callback/kill/deadline tables and pipes alive. Member destruction alone
    // runs in the wrong order for an actor destructor that uses the core.
    // Cancellation hooks run synchronously and may add referenced actors,
    // growing _actors. Keep no vector iterator or slot reference over cancel().
    for (std::size_t sid = 0; sid < _actors.size(); ++sid)
        if (Actor *const actor = _actors[sid].get())
            actor->__cancel_coro_scope__();

    // A suspended onInit frame must die before its actor. Its stash owns raw
    // event bytes, whose payloads need the router's type-erased disposer.
    // A payload destructor may itself add a child with suspended onInit,
    // inserting into _activating. Extract one activation before running user
    // destruction, then return to the map for the next one.
    auto drain_activations = [this] {
        while (!_activating.empty()) {
            auto       it         = _activating.begin();
            Activation activation = std::move(it->second);
            _activating.erase(it);
            for (auto &buckets : activation.stash)
                _router.dispose(*reinterpret_cast<Event *>(buckets.data()));
        }
        _dying_with_frame.clear();
    };
    drain_activations();

    // Cancellation may have queued coroutine resumes. Destroy the frames and
    // withdraw loop watchers while their actors are still alive. In particular,
    // start(false) leaves the thread-local listener in the caller's thread.
    auto &listener = io::async::listener::current;
    listener.reset_coro_scheduler();
    listener.clear();

    // Destructors can kill peers or create a referenced child. Re-scan until
    // every actor has passed through removeActor's cancellation and watch path.
    while (_actor_count != 0 || !_activating.empty()) {
        // A listener or actor destructor can create a suspended child after
        // the first activation drain. Remove its frame before its actor.
        drain_activations();
        if (_actor_count == 0)
            break;
        for (std::size_t sid = 0; sid < _actors.size(); ++sid) {
            if (Actor *const actor = _actors[sid].get()) {
                const ActorId id = actor->id();
                removeActor(id, DownReason::core_stopped);
                break; // restart after user teardown, which may resize _actors
            }
        }
    }

    // Pipes contain placement-constructed events. Freeing their segments alone
    // loses non-trivial payloads (including a self-push from failed onInit).
    for (auto &pipe : _pipes) {
        for (auto segment = pipe.front(); !segment.empty(); segment = pipe.front()) {
            auto       *cur = segment.data();
            auto *const end = cur + segment.size();
            while (cur < end) {
                auto      &event = *reinterpret_cast<Event *>(cur);
                const auto width = event.bucket_size;
                if (unlikely(width == 0))
                    break; // malformed width: no safe way to find following events
                _router.dispose(event);
                cur += width;
            }
            pipe.pop_front();
        }
    }
}

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
    const ActorId id(sid, _index);
    if (_constructing_actor_id_out != nullptr)
        *_constructing_actor_id_out = id;
    return id;
}

// Event Management
void
VirtualCore::unregisterEvents(ActorId const id) const noexcept {
    _router.unsubscribe(id);
}

void
VirtualCore::__rollback_failed_admission__(ActorId const id) noexcept {
    // An actor may have published registrations and killed itself before its constructor
    // threw or onInit failed. No earlier actor owns this ID while an admission is active.
    __unregisterCallback(id);
    unregisterEvents(id);
    if (unlikely(!_watchers_of.empty() || !_watching.empty()))
        __on_actor_down__(id, DownReason::init_failed);
    std::erase(_actor_to_remove, id);
}

// __getPipe__ is defined inline at the tail of VirtualCore.h: it is called on every push
// from the user's TU (see the note there).

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
            QB_LOG_CRIT(*this << " received event with bucket_size==0 (malformed or "
                                 "oversized event); aborting batch");
            break;
        }
        // Read the width ONCE, before the handler runs. A handler's reply is a byte copy of
        // this very event into the outbound pipe; re-reading `bucket_size` after the call is
        // a load trailing that store by a few instructions, and when the two pipes' segments
        // share their low twelve address bits (carved on a fixed stride out of aligned slabs,
        // they all did) it lands on the 4 KB offset the reply was just written to and memory
        // disambiguation stalls it behind the store (4K aliasing): 12% of savina/big at one
        // core, measured. `segment_pool` staggers its segments too; this removes the reload.
        const std::size_t width = event->bucket_size;
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
                if (qb::detail::ask_try_deliver_reply(*event, dest)) {
                    // Consumed by a pending continuation — and this is the END of that event's
                    // life, so it must be disposed exactly like every other terminal path
                    // (route() disposes after the handler; the stash disposes on overflow and
                    // on a failed init; the replay routes and disposes). The awaiter took what
                    // it needed BY VALUE — `ask` moves the event into its `result`, `ask_stream`
                    // moves it into the chunk deque, `ping`/`require` copy two scalars — so
                    // nothing references these bytes afterwards. Skipping the destructor leaked
                    // whatever the thunk did not move out: the whole payload for a thunk that
                    // moves nothing (`ask_stream`'s end-of-stream marker, discovery replies),
                    // and the original's heap for any exchange event whose move degrades to a
                    // copy (a const member, or a user-declared copy ctor suppressing the move).
                    _router.dispose(*event);
                } else {
                    // Stash for FIFO replay on activation. If the cap overflowed the event is
                    // dropped here, so dispose its payload (the byte-copy never happened) to
                    // avoid leaking a non-trivial std::string/std::vector member.
                    if (!__stash_event__(dest, event))
                        _router.dispose(*event);
                }
                ++_metrics._nb_event_received;
                _metrics._nb_bucket_received += width;
                i += width;
                continue;
            }
        }
        try {
            _router.route(*event, [this](auto &event) {
                // No actor on this core registered the type. A broadcast reaching such a core is
                // normal; a unicast is a dead letter (Huly QB-163) -- the destination is alive and
                // handles no such event, or it is not there at all.
                if (!event.getDestination().is_broadcast())
                    __dead_letter__(event, __undelivered_reason__(event.getDestination()));
            });
        } catch (...) {
            // The type resolver disposed the faulting event, including on a
            // handler throw. Dispose only later events in this copied-out batch;
            // their bytes would otherwise be dropped when the callback unwinds.
            for (i += width; i < nb_events;) {
                auto &pending = *reinterpret_cast<Event *>(events.data() + i);
                if (unlikely(pending.bucket_size == 0))
                    break;
                if (!pending.is_alive())
                    _router.dispose(pending);
                i += pending.bucket_size;
            }
            throw;
        }
        ++_metrics._nb_event_received;
        _metrics._nb_bucket_received += width;
        i += width;
    }
}

VirtualCore::PipeMap
VirtualCore::__make_pipes__(VirtualPipe::pool_type &pool, std::size_t const nb_core) {
    PipeMap pipes;
    pipes.reserve(nb_core);
    for (std::size_t i = 0; i < nb_core; ++i)
        pipes.emplace_back(pool);
    return pipes;
}

void
VirtualCore::__receive__() {
    // from same core. The self pipe is walked IN PLACE, a segment at a time, up to a fence taken
    // here: a handler that pushes to this same core appends behind the fence, and those events
    // are the next pass's -- exactly what the old two-pipe swap guaranteed, at the price of six
    // loads and six stores per pass plus the handler's first push waiting on the `_wcur` the
    // swap had just written (the longest dependency chain of a one-event pass, QB-182). Each
    // segment ahead of the fence goes back to the core's pool as soon as its events are routed,
    // and a drained pipe rewinds its one resident segment, so a one-event pass -- a ping-pong --
    // reads and writes the same 64 bytes every time. An event's reference is valid for exactly
    // the handler it is routed to, which is the contract Actor::push documents. `front(fence)`
    // is empty exactly when the walk is over, so it is the loop's only test; the walk itself
    // is gated on the pipe holding anything, two pointer compares on every pass of a core whose
    // events all come from other cores -- or from nowhere, which is every idle pass.
    if (!_self_pipe.empty()) {
        auto fence = _self_pipe.mark();
        for (auto run = _self_pipe.front(fence); !run.empty(); run = _self_pipe.front(fence)) {
            try {
                __receive_events__(run);
            } catch (...) {
                _self_pipe.pop_front(fence); // the faulting run was disposed by __receive_events__
                throw;
            }
            _self_pipe.pop_front(fence);
        }
    }
    // global_core_events. `consume_all(func, scratch, chunk)`, not `dequeue(T*, n)`: the third
    // argument is a PER-PRODUCER batch limit, so every peer core's ring is drained on every
    // turn. A shared budget would let one saturated producer consume it and starve the rest.
    _mail_box.consume_all(
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
// __flush_all__ / __flush_pipes__ — structured, deadlock-free outbound pipe drain (finding 2.4).
// `__flush_all__` (inline, VirtualCore.h) is the pass-time scan of the peer pipes; this is the
// drain it enters once one of them holds events.
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
// Non-QoS events (`event.state.bits.qos == 0`) preserve their original
// best-effort semantics: a single `try_send` attempt, then drop on failure.
// -----------------------------------------------------------------------------

namespace {
// Tunables — deliberately kept out of the public header to allow empirical
// tuning without forcing recompiles of downstream code.
constexpr std::uint32_t kFlushSpinAttempts  = 64;  // `spin_loop_pause` phase.
constexpr std::uint32_t kFlushYieldAttempts = 512; // total budget per event.
static_assert(kFlushSpinAttempts < kFlushYieldAttempts, "spin phase must precede yield phase");
// Longest run of whole events `__flush_all__` publishes with ONE ring write (buckets, i.e.
// cache lines). The run is what amortises the per-publish costs — the release store of the
// ring's write index, the consumer's re-read of that line, the notify fence — and its cap is
// what bounds how long the run's FIRST event waits for its last to be copied: 256 buckets is a
// quarter of a 1023-bucket mailbox ring, ~16 KiB, a few hundred nanoseconds of memcpy. A pipe
// holding one event (a ping-pong) forms a run of one and pays what `try_send` paid. See
// `SharedCoreCommunication::send_run` for the measurement.
constexpr std::size_t kFlushRunBuckets = 256;
} // namespace

// Widest event a destination mailbox ring can EVER accept — `kMaxDeliverableBuckets`,
// declared with the class in VirtualCore.h. Single source of truth:
// `SharedCoreCommunication::MaxRingEvents` sizes every per-producer SPSC ring, and
// `spsc::enqueue<_All = true>` is all-or-nothing, so an event wider than this is not
// "backpressured" — it is permanently unsendable however much the consumer drains.
// Separating the two cases is what keeps `__flush_all__` terminating (see below).

bool
VirtualCore::__flush_pipes__() noexcept {
    static_assert(kFlushRunBuckets <= kMaxDeliverableBuckets, "a run must fit an empty mailbox ring");
    bool        any_work = false;
    std::size_t pipe_idx = 0;
    for (auto &pipe : _pipes) {
        // Skip the self-core pipe (local delivery bypasses the mailbox layer)
        // and any empty outbound pipe.
        if (pipe_idx == _resolved_index || pipe.empty()) {
            ++pipe_idx;
            continue;
        }
        any_work = true;

        // One segment at a time: `front()` is the head segment's live range, and no event
        // straddles two segments, so a run never has to. A segment fully published is popped
        // (back to the pool); a partial bail consumes up to the event that would not go.
        bool partial = false, discard = false;
        while (!partial && !discard && !pipe.empty()) {
            auto const  seg = pipe.front();
            auto       *cur = seg.data();
            auto *const end = cur + seg.size();

            while (cur < end) {
                // --- Fast path: a run of whole events, one ring write -----------------------
                // Gather consecutive deliverable events from the head, up to `kFlushRunBuckets`.
                // The run stops SHORT of a zero-width or oversize event so the slow path below
                // meets it at the head and disposes it; a head wider than the cap forms a run of
                // its own. `send_run` cuts the run to what the ring can take right now, at an event
                // boundary, so the fast path only yields to the slow one when not even the head
                // fits — which is the backpressure regime the backoff below exists for.
                {
                    std::size_t run = 0, run_events = 0;
                    for (auto const *p = cur; p < end;) {
                        const std::size_t width = reinterpret_cast<Event const *>(p)->bucket_size;
                        if (unlikely(width == 0 || width > kMaxDeliverableBuckets) || (run && run + width > kFlushRunBuckets))
                            break;
                        run += width;
                        ++run_events;
                        p += width;
                    }
                    if (likely(run)) {
                        const auto sent = _engine.send_run(_resolved_index, static_cast<CoreId>(pipe_idx), cur, run, run_events);
                        if (likely(sent.buckets)) {
                            _metrics._nb_event_sent_try += sent.events;
                            _metrics._nb_event_sent += sent.events;
                            _metrics._nb_bucket_sent += sent.buckets;
                            cur += sent.buckets;
                            continue;
                        }
                    }
                }

                // --- Slow path: the head event alone --------------------------------------
                // Non-const: an undeliverable event is disposed here (its payload must be freed
                // exactly like every other terminal path), so we need a mutable reference.
                auto &event = *reinterpret_cast<Event *>(cur);
                ++_metrics._nb_event_sent_try;

                if (try_send(event)) {
                    ++_metrics._nb_event_sent;
                    _metrics._nb_bucket_sent += event.bucket_size;
                    cur += event.bucket_size;
                    continue;
                }

                // --- Permanently unsendable, NOT backpressure -----------------------------
                // Everything below this point assumes `try_send` failed because the peer's ring
                // is momentarily full and will drain. Two shapes break that assumption, and for
                // them the retry never converges: the bounded backoff expires, we partial-bail,
                // and the next pass re-queues the exact same event. The pipe is FIFO, so the
                // whole outbound stream to that core is held hostage behind it (head-of-line),
                // including whatever would have killed the destination actor; the sender then
                // leaves its main loop and spins in the shutdown residual drain forever because
                // the destination is still "live". Net effect: `qb::Main::join()` never returns
                // and two cores burn 100% CPU with no diagnostic. Pinned by
                // `OversizeEvent.OversizedEventDoesNotWedgeTheEngine`.
                //
                // Both shapes are cold (they cost one compare against a constant on a path that
                // already failed a send), and dropping is the only terminating action available:
                // the event is undeliverable by construction, not by timing.
                if (unlikely(event.bucket_size == 0)) {
                    // Malformed: a zero-width event leaves `cur` standing still, so the pipe can
                    // no longer be walked and the remaining events cannot even be identified to
                    // dispose them. Only reachable by overflowing `bucket_size`'s uint16 via a
                    // >= 65536-bucket `allocated_push`. Discard what is left of this pipe
                    // (`discard` makes the trailing `pipe.reset()` free every segment).
                    QB_LOG_CRIT(*this << " outbound pipe to core(" << pipe_idx << ") holds a zero-width event (bucket_size overflowed); "
                                      << "discarding the rest of the pipe");
                    discard = true;
                    break;
                }
                if (unlikely(event.bucket_size > kMaxDeliverableBuckets)) {
                    QB_LOG_CRIT(*this << " dropping event[" << qb::event_type_name(event.getID()) << '#' << event.getID() << "] from "
                                      << event.getSource() << " to " << event.getDestination() << ": " << event.bucket_size
                                      << " buckets exceeds the " << kMaxDeliverableBuckets
                                      << "-bucket mailbox ring, so it can never be delivered cross-core. Keep events small and move bulk "
                                         "data behind a pointer member (see Pipe::allocated_push).");
                    __dead_letter__(event, DeadLetterReason::oversize);
                    _router.dispose(event);
                    cur += event.bucket_size;
                    continue;
                }

                // From here the destination's mailbox is full: backpressure, counted for
                // `Actor::getCoreStats()` once per failed attempt (Huly QB-162).
                ++_nb_send_blocked;
                if (!event.state.bits.qos) {
                    // Best-effort event: dropped on backpressure (preserves the
                    // original fire-and-forget semantics for QoS-0 events such as
                    // metrics or heartbeats). Counted as dropped, not as sent: it
                    // was counted "sent" until 3.3, which made the two read the same.
                    ++_nb_event_dropped;
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
                    ++_nb_send_blocked;
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
                // is woken so it runs immediately (at zero latency only the fence
                // runs — see `Mailbox::notify()`).
                _engine.getMailBox(event.dest.index()).notify();
                partial = true;
                break;
            }

            // Everything before `cur` in this segment is published or disposed. Consuming exactly
            // that much pops the segment when the walk reached its end, and otherwise leaves the
            // head at the event the next pass retries.
            pipe.consume_front(static_cast<std::size_t>(cur - seg.data()));
        }

        if (discard)
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
            QB_LOG_WARN("set thread affinity failed: " << strerror(errno));
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
            QB_LOG_WARN("set thread affinity failed");
#else
#warning "Cannot set affinity on windows with GNU Compiler"
#endif
#endif
    }
    _actor_to_remove.reserve(_actor_count);
    // Publish this thread's io loop to the mailbox so a producer can end a park taken INSIDE
    // it (`Mailbox::wait(listener &)`, below in `__workflow__`). Before the start barrier,
    // hence before any peer can enqueue — and withdrawn by `Main::start_thread`'s exit guard
    // on every exit path, before the thread's `listener::current` is destroyed.
    _mail_box.attach_loop(&io::async::listener::current);
    return ret;
}

bool
VirtualCore::__init__actors__() {
    // Snapshot the actor pointers first: driving an `onInit()` may itself create
    // referenced actors (`addRefActor`), mutating `_actors` mid-iteration.
    std::vector<Actor *> actors_to_init;
    actors_to_init.reserve(_actor_count);
    for (const auto &slot : _actors)
        if (slot)
            actors_to_init.push_back(slot.get());
    for (auto *actor : actors_to_init) {
        qb::io::async::task<bool> init = actor->onInit();
        switch (__drive_init__(*actor, init)) {
            case InitOutcome::ReadyTrue:
                break; // completed synchronously → already active, frame freed
            case InitOutcome::ReadyFalse:
                QB_LOG_CRIT(*actor << " failed to init");
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
        auto &p = qb::io::async::detail::promise_of(h);
        if (unlikely(p.has_exception())) {
            // Surface an uncaught throw as an init failure (preserves the BadActorInit
            // outcome) — but never let it escape into this noexcept owning-thread path.
            try {
                std::rethrow_exception(p.exception());
            } catch (const std::exception &e) {
                QB_LOG_CRIT(actor << " onInit() threw: " << e.what());
            } catch (...) {
                QB_LOG_CRIT(actor << " onInit() threw a non-standard exception");
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
    QB_LOG_VERB(actor << " activating (async onInit in flight)");
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
        __dead_letter__(*event, DeadLetterReason::stash_overflow);
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
VirtualCore::__pump_activations__() {
    if (likely(_activating.empty()))
        return;
    const auto now = static_cast<std::uint64_t>(qb::unix_nanos(qb::wall_now()));

    // Collect ids before invoking cancellation hooks: a hook may add an actor
    // whose onInit suspends, rehashing _activating. No map iterator or reference
    // may span that callback. New entries are considered on the next pump.
    thread_local std::vector<ActorId> scan_ids;
    thread_local std::vector<ActorId> done_ids;
    scan_ids.clear();
    done_ids.clear();
    scan_ids.reserve(_activating.size());
    for (auto const &entry : _activating)
        scan_ids.push_back(entry.first);
    for (auto const id : scan_ids) {
        auto it = _activating.find(id);
        if (it == _activating.end())
            continue; // a prior hook removed it
        auto &act = it->second;
        if (act.init.done()) {
            done_ids.push_back(id);
        } else if (!act.cancelling && act.deadline_ns && now >= act.deadline_ns) {
            // Deadline reached: cancel the actor's coro scope so its `onInit()` unwinds
            // (its cancellation-aware awaiters throw `cancelled_error`); it then reports
            // `done()` on a later pump and is finalized as a failure below.
            act.cancelling = true;
            if (Actor *const actor = __actor_slot__(id)) {
                QB_LOG_WARN(*actor << " activation deadline expired — cancelling onInit");
                actor->__cancel_coro_scope__();
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
        bool ok    = false;
        bool threw = false;
        if (auto h = act.init.handle(); h && h.done()) {
            auto &p = qb::io::async::detail::promise_of(h);
            threw   = p.has_exception();
            ok      = !threw && p.value();
        }
        // Free the onInit frame now that it has fully unwound (no awaiter references it).
        act.init = qb::io::async::task<bool>{};

        Actor *const actor = __actor_slot__(id);
        if (dying || !ok || actor == nullptr) {
            // Killed during init, failed init, or already gone → complete teardown now
            // (the deferred-destroy: the actor outlived its own coroutine frame).
            // Dispose the never-replayed stash so any non-trivial event payload (std::string /
            // std::vector in a `push`'d event) is destroyed instead of leaked: the stash holds
            // byte-copied events whose destructors only ever run via route() (success path) or
            // here (drop path) — the raw `vector<EventBucket>` teardown would free bytes only.
            for (auto &buckets : act.stash) {
                auto *ev = reinterpret_cast<Event *>(buckets.data());
                __dead_letter__(*ev, DeadLetterReason::init_failed);
                _router.dispose(*ev);
            }
            if (actor != nullptr) {
                if (!dying && !ok)
                    QB_LOG_CRIT(*actor << " async onInit failed — removing");
                // What its watchers are told (Huly QB-51): killed during its init; failed -- a
                // `co_return false`, or the deadline's cancellation, which unwinds the frame through
                // an exception of its own; or threw.
                removeActor(id, dying            ? DownReason::killed
                                : act.cancelling ? DownReason::init_failed
                                : threw          ? DownReason::init_threw
                                                 : DownReason::init_failed);
            }
            // Whoever awaited this activation learns now that it will never happen (QB-62),
            // where the poll it replaced only ever learned it from its own timeout.
            __fire_activation_waiters__(act, false);
            continue;
        }
        // Success: flip Active, then replay the stashed inbound unicast FIFO.
        actor->_activated = true;
        QB_LOG_VERB(*actor << " activated");
        for (std::size_t i = 0; i < act.stash.size(); ++i) {
            auto &buckets = act.stash[i];
            auto *ev      = reinterpret_cast<Event *>(buckets.data());
            try {
                _router.route(*ev, [this](auto &e) {
                    if (!e.getDestination().is_broadcast())
                        __dead_letter__(e, __undelivered_reason__(e.getDestination()));
                });
            } catch (...) {
                // The resolver disposed the faulting event before rethrowing.
                for (++i; i < act.stash.size(); ++i)
                    _router.dispose(*reinterpret_cast<Event *>(act.stash[i].data()));
                __fire_activation_waiters__(act, false);
                throw;
            }
        }
        // After the replay: a `ready_async` waiter resumes (next pass, through the scheduler)
        // to an actor that has already seen everything queued for it while it was Activating.
        __fire_activation_waiters__(act, true);
    }
}

template <bool _Timed>
void
VirtualCore::__workflow_loop__() {
    QB_LOG_INFO(*this << " Init Success " << static_cast<uint32_t>(_actor_count) << " actor(s)");
    // This thread's io loop, reached ONCE: `listener::current` is an inline thread_local with a
    // non-trivial constructor, so g++ routes every access through its TLS wrapper (the init guard,
    // `__tls_init`) -- 2.2 % of savina/ping-pong 1c for the three accesses a pass made (Huly
    // QB-199). The object lives for the thread, and this loop is the thread.
    auto &loop = io::async::listener::current;
    // The signal generation this core last scanned, as a LOCAL: the pass compares it against the
    // global one in a register. Compared with the member, the check paid a memory operand on every
    // pass (+0.25 ns on the qb-vs-others pass-cost probe at k = 1, WSL2 g++-14, Huly QB-65).
    unsigned int scanned_generation = _last_signal_generation;
    // `_metrics.activity()` at the end of the previous pass, a LOCAL for the same reason: the
    // counters accumulate (Huly QB-162), and a pass is idle when their sum did not move. What it
    // replaced cleared six per-pass counters at the end of every pass.
    std::uint64_t activity_mark = _metrics.activity(loop.total_events_processed());
    while (likely(true)) {
        ++_loop_count; // 1-based loop-pass index surfaced to callbacks via qb::LoopEvent; also keys the `time()` sample
        // A timed core (Huly QB-165) stamps its pass here and closes it before the park policy: the
        // pass is timed, never the park. Absent from the untimed instantiation.
        [[maybe_unused]] qb::mono_time pass_start{};
        if constexpr (_Timed)
            pass_start = qb::mono_now();

        // Signals, Main::stop() and the C++20 cooperative stop (finding 2.17 -- jthread/stop_token)
        // all advance ONE global generation: the signal handler and stop() after bumping their
        // signal's slot, ~Main after request_stop(). So the steady-state pass reads ONE relaxed load
        // and compares it with a register -- cheaper than before Huly QB-65, which read a pending-signal
        // slot AND polled the stop token's state on every pass. A moved generation sends the core to
        // the cold scan (out of line), which delivers one SignalEvent per signal raised since and the
        // cooperative stop's virtual SIGINT; a scanned generation costs nothing on later passes, where
        // the old slot, never cleared, sent every pass after a first SIGHUP into the cold branch.
        if (unlikely(Main::_signal_generation.load(std::memory_order_relaxed) != scanned_generation))
            scanned_generation = __deliver_signals__();

        // Pump qb-io only when its loop has something to deliver: a referenced active
        // watcher, a pending event, a deferred callback or a ready coroutine. The gate
        // used to be `has_coro_scheduler() || size() || has_deferred()`, and the first
        // term is true for the rest of a core's life once any actor has spawned a
        // coroutine — a `NOWAIT` libev pass on every tick, ~300–380 ns of poll + clock
        // reads per pass with nothing to poll for (axis E of the qb-vs-others audit).
        // Request deadlines (`ask`, `ask_stream::next()`, `ping`, `require` with a timeout) live in
        // the core's own clock, not in libev (Huly QB-189): a member load a pass; when a deadline
        // is armed, a coarse clock read (~3-5 ns) and, only within a scheduler tick of the earliest
        // one, the precise read that fires it. Before the io pump, so a fired deadline's coroutine
        // (queued to the scheduler) resumes in THIS pass's `run()`.
        if (_deadlines.armed())
            detail::deadlines_check(_deadlines, 0);

        if (loop.has_work()) {
            // Hot path: call `listener::run` directly — no `async::run` wrapper
            // (avoids redundant checks). It counts the callbacks it runs; the park policy
            // reads that count at the end of the pass (`total_events_processed()`).
            loop.run(EVRUN_NOWAIT);
        }

        // Complete any async `onInit()` resumed above (replay stashes / enforce
        // deadlines / finish deferred destroys). empty()-guarded: free when idle.
        if (unlikely(!_activating.empty())) {
            __pump_activations__();
            if (unlikely(_actor_count == 0))
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
        // The `empty()` guard is what makes the per-pass clock lazy IN PRACTICE: the
        // `LoopEvent` below is built from `time()`, and until the guard that was the
        // one unconditional clock read left in a pass — `985cbb3a` moved the read from
        // the top of the loop into this block and wrote that a pass nobody asks costs
        // nothing, but a `LoopEvent` built for zero callbacks still asked. A core with
        // no registered callback — every benchmark, every server that drives itself
        // from io and events — paid one `clock_gettime` per pass for an event nobody
        // received: 39.6 % of a one-core `ping-pong` profile (`perf record -e cpu-clock`,
        // WSL2 / g++-14), ~13 ns of a ~33 ns pass. The snapshot copy sits inside the
        // guard for the same reason. No branch hint: the predicate is a per-core
        // constant on every pass but the one that registers or unregisters the
        // first / last callback.
        if (!_callback_list.empty()) {
            // One LoopEvent for the whole pass — same `now`/`iteration` for every callback,
            // consistent with `Actor::time()` (the same per-pass sample; a pass with no
            // registered callback and no caller never reads the clock). Delivered by a direct
            // virtual call (not routed).
            const qb::LoopEvent                     loop_ev{time(), _loop_count};
            thread_local std::vector<CallbackEntry> cb_snapshot;
            cb_snapshot = _callback_list;
            for (auto const &entry : cb_snapshot) {
                // Skip the callback of an actor killed earlier in this same
                // dispatch pass (e.g. by an earlier actor's tick). The
                // object is still alive — destruction is deferred to the
                // removeActors phase below — so this is purely a semantics fix:
                // a killed actor must not get another tick, matching the
                // event-kill path which skips the whole callback phase. The
                // empty() fast-path keeps the common (nothing killed) case free;
                // `is_alive()` is the kill flag itself (`Actor::kill()` is its only
                // writer), read off an object the reap below has not reached yet.
                if (likely(_actor_to_remove.empty()) || entry.actor->is_alive())
                    entry.cb->on(loop_ev);
            }
        }
        // check if callbacks killed actors
        if (unlikely(!_actor_to_remove.empty())) {
        removeActors:
            // Reap dead actors. `removeActor()` destroys the actor, running user code that may `kill()`
            // ANOTHER actor and so re-enter `killActor()` → `_actor_to_remove.push_back()`. The scratch
            // buffer keeps that re-entrant push off the container being iterated (a growth
            // reallocation of the vector moves its elements and invalidates a live iterator) and keeps
            // late kills from being discarded by the clear (which stranded a `!is_alive()` actor in
            // `_actors`, so the live count never reached zero and the core never terminated). Terminates:
            // only that user code refills the queue and it runs at most once per actor — once an id has
            // left `_actors`, `removeActor()` destroys nothing — so a pass that destroys nothing ends it.
            // Pinned by `KillDuringReap.ActorKilledFromAnotherDestructorIsStillReaped`.
            while (!_actor_to_remove.empty()) {
                _actor_remove_batch.clear();
                _actor_remove_batch.swap(_actor_to_remove);
                for (auto const actor : _actor_remove_batch)
                    removeActor(actor);
            }
            _actor_remove_batch.clear(); // drop the last batch's ids (capacity is kept for reuse)
            if (_actor_count == 0) {
                break;
            }
        }
        // Park policy — a TIME floor, not an event-count credit. A pass is idle when it
        // moved no event (received, io, a flush attempt: the `activity()` sum stood still)
        // and left nothing for the next pass in the self-core pipe (an actor's callback may
        // push to itself with no counted activity; parking over that would delay a local
        // event by up to `latency`). The first idle
        // pass stamps `_idle_since`; the core keeps polling until the mailbox's idle-spin
        // floor has elapsed, then parks — which returns on data, on a producer's notify, or
        // after `latency`. A wait that returns with nothing to do keeps the old stamp and
        // parks again on the next pass, so an idle core does not re-spin a whole floor
        // between two timeouts. The clock is read on idle passes only — a busy pass pays one
        // store — and it is read in EVERY latency mode, a latency-0 core included, which
        // never parks and so never uses the stamp. That read is not waste: it is what
        // paces the idle poll. With the tick phase above no longer reading the wall clock,
        // a spinning core's idle pass had become ~20 ns of unserialized code, and its
        // cross-core exchange got SLOWER for it — savina/ping-pong and thread-ring 2c-spin
        // +25 % on i9-12900K / WSL2 g++-14, ten interleaved launches each, fully separated
        // distributions — while the one-core cells took the full gain. The idle poll needs
        // ~10–15 ns of serialized work between two reads of the peer's ring index: an
        // `lfence` alone (3 ns) left +7 / +14 %, `spin_loop_pause()` (33 ns here) +9 / +20 %,
        // a bounded tight `has_data()` poll was worse than no pacing at all (+30 %), and the
        // monotonic clock read (13 ns, `lfence; rdtsc` under the vDSO) put both cells back
        // on the control's figure while the one-core cells stayed at −56 %. Measured, not
        // reasoned; the census is `qb-vs-others` results `qb-branch-perf-loop-clock-on-demand/`.
        //
        // WHERE it parks depends on whether the io loop has anything to deliver. With io
        // watchers active the park is taken INSIDE the loop (`Mailbox::wait(listener &)`:
        // `ev_run(EVRUN_ONCE)` capped at `latency`, ended by the producer through the
        // loop's async watcher), so a socket that becomes readable wakes the core at poll
        // latency instead of at the timeout — a cv-parked core answered io only when
        // `latency` expired (p50 0.9–1.2 ms at 100 µs, 15.6 ms at 10 ms, against 19 µs polling;
        // qb-vs-others audit, axis N). Io delivered by that park counts as activity: the
        // stamp is cleared so the reply, and the request after it, are met at polling
        // latency. A core with no io work keeps the condition-variable park, whose cost
        // and handshake are the measured ones.
        [[maybe_unused]] qb::mono_time pass_end{};
        if constexpr (_Timed) {
            pass_end = qb::mono_now();
            _pass_recorder.record(pass_start, pass_end);
        }
        const std::uint64_t activity = _metrics.activity(loop.total_events_processed());
        if (likely(activity != activity_mark) || !_self_pipe.empty()) {
            _idle_since = qb::mono_time{};
        } else {
            const auto now = _Timed ? pass_end : qb::mono_now(); // a timed pass has just read it
            // The idle pass's reading, handed to the deadline list for free: an idle core with a
            // pending request fires its deadline on the precise clock, no coarse pre-check.
            if (_deadlines.armed())
                detail::deadlines_check(_deadlines, static_cast<std::uint64_t>(now.time_since_epoch().count()));
            if (_idle_since == qb::mono_time{}) {
                _idle_since = now;
            } else if (_mail_box.getLatency() > qb::duration::zero() && now - _idle_since >= _mail_box.getIdleSpin()) {
                // A pending request deadline bounds the park (Huly QB-189): the loop holds no
                // timer for it any more, so neither park may outlast it -- the cap is the smaller
                // of `latency` and the time to the earliest deadline, on the reading made above.
                auto cap = _mail_box.getLatency();
                if (_deadlines.armed()) {
                    const auto left = detail::deadlines_next_in(_deadlines, static_cast<std::uint64_t>(now.time_since_epoch().count()));
                    if (left < static_cast<std::uint64_t>(cap.count()))
                        cap = qb::duration{static_cast<qb::duration::rep>(left)};
                }
                if (loop.has_work()) {
                    if (_mail_box.wait(loop, cap))
                        _idle_since = qb::mono_time{};
                } else {
                    _mail_box.wait(cap);
                }
            }
        }
        activity_mark = activity;
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

    QB_LOG_INFO(*this << " Stopped normally");
}

void
VirtualCore::__workflow__() {
    // Pass timing is opt-in, and decided once (Huly QB-165): two instantiations of one loop, so an
    // untimed core runs the code it always ran -- the timing is not a branch in its pass, it is
    // absent from it.
    if (unlikely(_pass_timing))
        __workflow_loop__<true>();
    else
        __workflow_loop__<false>();
}

bool
VirtualCore::__dispose_residual_to_stopped_cores__() noexcept {
    bool        any_live_pending = false;
    std::size_t pipe_idx         = 0;
    for (auto &pipe : _pipes) {
        // The self-core pipe is delivered locally (never via the mailbox) and is already
        // drained by __receive__; an empty pipe has nothing pending.
        if (pipe_idx == _resolved_index || pipe.empty()) {
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
        for (auto seg = pipe.front(); !seg.empty(); seg = pipe.front()) {
            auto       *cur = seg.data();
            auto *const end = cur + seg.size();
            while (cur < end) {
                auto      &event = *reinterpret_cast<Event *>(cur);
                const auto bsz   = event.bucket_size;
                // Defensive: a zero bucket_size (only reachable via a malformed event) would spin
                // forever — stop draining this pipe instead (mirrors __receive_events__).
                if (unlikely(bsz == 0))
                    break;
                __dead_letter__(event, DeadLetterReason::peer_stopped);
                _router.dispose(event);
                cur += bsz;
            }
            pipe.pop_front();
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
            case InitOutcome::ReadyFalse: {
                const auto h     = init.handle();
                const bool threw = h && qb::io::async::detail::promise_of(h).has_exception();
                removeActor(actor.id(), threw ? DownReason::init_threw : DownReason::init_failed);
                return ActorId::NotFound;
            }
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
    if (unlikely(_tearing_down))
        return ActorId::NotFound;
    Actor        &actor = *actor_ptr;
    const ActorId id    = actor.id();
    // Reject duplicates *before* driving `onInit()`: a suspended (async) init must never
    // coexist with an append failure, otherwise its still-live frame would be orphaned.
    if (unlikely(__actor_slot__(id) != nullptr)) {
        QB_LOG_CRIT("Error Cannot add Service Actor multiple times" << actor);
        return ActorId::NotFound;
    }
    if (initActor(actor, doInit).is_valid()) {
        // Direct-mapped by sid; grow the slot table geometrically (never past the 16-bit id
        // space) so a burst of fresh ids costs O(1) amortised, not one reallocation each.
        const std::size_t sid = id._service_id;
        if (sid >= _actors.size())
            _actors.resize(std::min<std::size_t>(std::max<std::size_t>(sid + 1, _actors.size() * 2), ServiceIdPool::kBits + 1));
        _actors[sid] = std::move(actor_ptr);
        ++_actor_count;
        QB_LOG_VERB("New " << actor);
        return id;
    }
    return ActorId::NotFound;
}

void
VirtualCore::removeActor(ActorId const id, DownReason const reason) noexcept {
    // Deferred destroy (the actor must outlive its own coroutine frame): if its
    // `onInit()` frame is still suspended, cancel the scope so the frame unwinds, mark
    // the actor dying, and let `__pump_activations__` complete the teardown once the
    // frame reports `done()`. Re-entry from the pump (after the frame unwound and the
    // activation was dropped) falls straight through to the normal teardown below.
    if (unlikely(_activating.contains(id))) {
        if (Actor *const act = __actor_slot__(id))
            act->__cancel_coro_scope__();
        // cancel() runs user hooks synchronously. They may add an activating
        // child and rehash this table, so find the parent's entry again.
        const auto ait = _activating.find(id);
        if (ait != _activating.end()) {
            if (!ait->second.init.done()) {
                _dying_with_frame.insert(id);
                return;
            }
            // Dropped here rather than by the pump: its waiters learn the outcome here (QB-62), and its
            // stash dies here -- disposed as on the pump's failure path (until 3.3 this erase freed the
            // stashed bytes without running a single event destructor), and reported (QB-163).
            Activation activation = std::move(ait->second);
            _activating.erase(ait);
            activation.init = qb::io::async::task<bool>{}; // free completed frame before user teardown
            __fire_activation_waiters__(activation, false);
            for (auto &buckets : activation.stash) {
                auto *ev = reinterpret_cast<Event *>(buckets.data());
                __dead_letter__(*ev, DeadLetterReason::init_failed);
                _router.dispose(*ev);
            }
            _dying_with_frame.erase(id);
        }
    }
    __unregisterCallback(id);
    unregisterEvents(id);
    if (Actor *const actor = __actor_slot__(id)) {
        // Catch-all cancel-on-destroy: every destruction path funnels through here
        // (kill, onInit failure, engine shutdown). Cancelling the scope wakes scoped
        // coroutines so they unwind cleanly; idempotent with kill()'s cancel.
        actor->__cancel_coro_scope__();
        if (__actor_slot__(id) != actor)
            return; // a cancellation hook already completed this removal
        if (actor->has_active_coroutines()) {
            if (actor->has_coro_scope())
                // Scoped coroutines were just cancelled — they unwind on the next loop
                // iteration. A non-zero count here is expected and safe.
                QB_LOG_VERB(*actor << " destroyed with " << actor->active_coroutine_count() << " scoped coroutine(s) pending cancellation");
            else
                QB_LOG_WARN(*actor << " destroyed with " << actor->active_coroutine_count()
                                   << " active coroutines - coroutines must not access actor state!");
        }
        QB_LOG_VERB("Delete " << *actor);
        // Detach the owning unique_ptr before calling user ~Actor(). A destructor
        // may add a referenced child and resize _actors; a reference to its slot
        // would dangle across that call.
        std::unique_ptr<Actor> dying = std::move(_actors[id._service_id]);
        --_actor_count;
        dying.reset(); // ~Actor() runs here; the slot stays empty for the id's next owner
        // Only non-service ids are recycled into the pool: a ServiceActor's
        // id is assigned at static init (see 2.3) and must remain reserved
        // for the lifetime of the process to keep `ServiceIndex` stable.
        if (id._service_id > _nb_service.load(std::memory_order_relaxed))
            _ids.release(id._service_id);
        // Death watch (Huly QB-51), after the destructor: a watcher learns the actor is gone, not
        // going. Both tables are empty on a core where nobody watches, which is the whole cost.
        if (unlikely(!_watchers_of.empty() || !_watching.empty()))
            __on_actor_down__(id, reason);
    }
}

//! Actor Management

bool
VirtualCore::isActorAlive(ActorId const id) const noexcept {
    if (!id.is_valid())
        return false;
    Actor const *const actor = __actor_slot__(id);
    // Same phase oracle as findActor<T>(): an actor whose async onInit() is still in flight is
    // addressable but not yet active, and one that has been killed is skipped even though its
    // destruction is deferred to the reap phase.
    return actor != nullptr && actor->is_active();
}

void
VirtualCore::killActor(ActorId const id) noexcept {
    _actor_to_remove.push_back(id);
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

// Same-core copies go through detail::event_wire::copy: the 16-byte header as one load (it
// forwards from the one store reply()/forward() just made), `alive` cleared in the copy, the
// tail moved without a libc call for a one-bucket event. See event_wire's doc block in Event.h.
// The cross-core mailbox is a raw memcpy of the original, so an original that still says
// `alive` (the module-facing `Actor::send(Event const &)` on an event already replied or
// forwarded — reply()/forward() themselves clear it in their header rewrite) takes the local
// pipe, whose copy clears it; the batched flush relocates it later exactly like a try_send miss.
void
VirtualCore::send(Event const &event) noexcept {
    if (event.dest._core_id == _index || unlikely(event.state.bits.alive) || !try_send(event)) {
        auto &pipe = __getPipe__(event.dest._core_id);
        detail::event_wire::copy(pipe.allocate_back(event.bucket_size), event, event.bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES);
        return;
    }
    // Published straight into the peer's ring: the flush will never see it, so it is counted
    // here (Huly QB-162) -- every reply of a cross-core exchange takes this path.
    ++_metrics._nb_event_sent;
    _metrics._nb_bucket_sent += event.bucket_size;
}

Event &
VirtualCore::push(Event const &event) noexcept {
    auto &pipe = __getPipe__(event.dest._core_id);
    // the copy clears `alive` itself, so re-pushing an already replied/forwarded event is fine
    return detail::event_wire::copy(pipe.allocate_back(event.bucket_size), event, event.bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES);
}

void
VirtualCore::reply(Event &event) noexcept {
    detail::event_wire::swap_dest_source(event); // one 16-byte store the copy loads whole, alive 0
    send(event);
    event.state.bits.alive = 1; // AFTER the copy: the copy — on either transport — carries 0
}

void
VirtualCore::forward(ActorId const dest, Event &event) noexcept {
    detail::event_wire::set_dest(event, dest);
    send(event);
    event.state.bits.alive = 1;
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
    // One clock read per pass at most, taken on the first call of the pass (see the field).
    if (unlikely(_nanotimer_pass != _loop_count)) {
        _nanotimer      = static_cast<uint64_t>(qb::unix_nanos(qb::wall_now()));
        _nanotimer_pass = _loop_count;
    }
    return _nanotimer;
}

// `_nb_service`, `_handler` and `activation_deadline_ns` used to be defined HERE. All three are
// process-wide state -- the ServiceActor id counter, the per-thread current core, and a public
// knob a consumer sets before Main::start() -- and an out-of-line definition makes each of them
// one per *image*: a host and a plugin that each statically link libqb-core.a got two, silently.
// They are now `inline` + QB_ABI_ANCHOR in VirtualCore.h. See qb/utility/abi.h.
// Cold, and defined AFTER the pass it is called from: placed before it, this body shifted every
// address of the pass and read +2 % on savina/ping-pong 1c (WSL2 g++-14, 12 rounds, quartiles
// separated) for a function the ping-pong never calls -- a layout effect, and one this placement
// does not produce (measured, level).
void
VirtualCore::__fire_activation_waiters__(Activation &act, bool const ok) noexcept {
    // Detach the whole list first, then fire: a `fire` resumes through the scheduler, never
    // inline, but it must find its node unlinked whatever it does -- `finish()` on the awaiter
    // reads `linked()` and would otherwise walk into a list that is being consumed.
    detail::activation_waiter *w = act.waiters;
    act.waiters                  = nullptr;
    while (w) {
        detail::activation_waiter *const next = w->next;
        auto *const                      fire = w->fire;
        void *const                      ctx  = w->ctx;
        w->prev = w->next = nullptr;
        w->fire           = nullptr;
        w->ctx            = nullptr;
        if (fire)
            fire(ctx, ok);
        w = next;
    }
}

// Cold, and defined at the end of the file for the same layout reason as the function above: the
// pass calls it only when a signal was raised or a stop requested.
unsigned int
VirtualCore::__deliver_signals__() noexcept {
    auto const deliver = [this](int const signum) noexcept {
        SignalEvent sig_event;
        fill_event<SignalEvent>(sig_event, BroadcastId(_index), BroadcastId(_index));
        sig_event.signum = signum;
        __getPipe__(_index).recycle_back(sig_event, sig_event.bucket_size);
    };
    bool delivered_sigint = false;
    // Acquire pairs with the release bump in Main::onSignal() / Main::stop() / ~Main: every slot
    // raised, and the stop request made, before the generation read here are visible below. A slot
    // raised after that read is seen now or on the next pass (the generation will have moved again)
    // -- never lost, and never delivered twice, because `_signal_seen` records what was delivered.
    const auto generation     = Main::_signal_generation.load(std::memory_order_acquire);
    const bool stop_requested = _stop_token.stop_possible() && _stop_token.stop_requested();
    if (generation != _last_signal_generation) {
        _last_signal_generation = generation;
        for (std::size_t signum = 1; signum < Main::SignalSlots; ++signum) {
            const auto raised = Main::_signal_raised[signum].load(std::memory_order_relaxed);
            if (raised == _signal_seen[signum])
                continue;
            _signal_seen[signum] = raised;
            deliver(static_cast<int>(signum));
            delivered_sigint = delivered_sigint || signum == static_cast<std::size_t>(SIGINT);
        }
    }
    // The cooperative stop (~Main's request_stop()) is a virtual SIGINT, delivered once -- unless this
    // very scan has just delivered a real one. Returned: the generation scanned, for the pass's copy.
    if (stop_requested && !_stop_delivered) {
        _stop_delivered = true;
        if (!delivered_sigint)
            deliver(SIGINT);
    }
    return _last_signal_generation;
}

// The dead-letter path (Huly QB-163): cold, out of line, at the end of the file for the same layout
// reason -- reached only when an event finds no actor, never on a pass that delivers.
void
VirtualCore::__dead_letter__(Event const &event, DeadLetterReason const reason) noexcept {
    if (unlikely(__is_watch_control__(event.getID())))
        return; // the death watch's own bookkeeping, which its protocol answers (Huly QB-51): never a letter
    const auto n = ++_nb_dead_letters;
    ++_nb_dead_letters_by_reason[static_cast<std::size_t>(reason)];
    // The first 16 of the core, then one line at each power of two: a flood of stale ids leaves a
    // bounded trace with the running count, and no clock is read. `oversize` has its own CRIT at
    // the flush, with the remedy.
    if (reason != DeadLetterReason::oversize && (n <= kDeadLetterLogFirst || std::has_single_bit(n))) {
        QB_LOG_WARN(*this << " dead letter #" << n << " (" << dead_letter_reason_name(reason) << "): event["
                          << qb::event_type_name(event.getID()) << '#' << event.getID() << "] from " << event.getSource() << " to "
                          << event.getDestination()
                          << (n == kDeadLetterLogFirst ? " -- further dead letters are logged at each power of two" : ""));
    }
    if (_dead_letter_handler) {
        const DeadLetter letter{event.getID(), event.getSource(), event.getDestination(), _index, reason};
        try {
            _dead_letter_handler(letter);
        } catch ([[maybe_unused]] std::exception const &e) {
            QB_LOG_CRIT(*this << " dead-letter handler threw: " << e.what() << " -- contained, the core carries on");
        } catch (...) {
            QB_LOG_CRIT(*this << " dead-letter handler threw a non-standard exception -- contained, the core carries on");
        }
    }
}

DeadLetterReason
VirtualCore::__undelivered_reason__(ActorId const dest) const noexcept {
    Actor const *const actor = __actor_slot__(dest);
    return actor != nullptr && actor->is_alive() ? DeadLetterReason::unhandled : DeadLetterReason::not_found;
}

// Cold (asked, never driven by the pass) and at the end of the file for the same layout reason.
CoreStats
VirtualCore::getCoreStats() const noexcept {
    CoreStats s;
    s.loop_passes            = _loop_count;
    s.events_received        = _metrics._nb_event_received;
    s.buckets_received       = _metrics._nb_bucket_received;
    s.events_sent            = _metrics._nb_event_sent;
    s.buckets_sent           = _metrics._nb_bucket_sent;
    s.sends_blocked          = _nb_send_blocked;
    s.events_dropped         = _nb_event_dropped;
    s.dead_letters           = _nb_dead_letters;
    s.dead_letters_by_reason = _nb_dead_letters_by_reason;
    // The loop's own cumulative count, the one the park policy reads: it includes the callbacks an
    // idle core's park ran.
    s.io_events = io::async::listener::current.total_events_processed();
    // Timed cores only (Huly QB-165): the sliding worst case is judged against the clock of the ask.
    if (_pass_timing)
        s.pass_time = _pass_recorder.snapshot(qb::mono_now());
    return s;
}

// The router's report of a unicast it could not deliver (`router::internal::reports_undelivered`).
// A broadcast that skipped a dead handler delivered to the others: nothing to report. A thread
// that runs no core (a router used outside the engine) reports nowhere.
void
Event::__undelivered__(Event &event) noexcept {
    VirtualCore *const core = VirtualCore::_handler;
    if (core == nullptr || event.getDestination().is_broadcast())
        return;
    core->__dead_letter__(event, core->__undelivered_reason__(event.getDestination()));
}

// Death watch (Huly QB-51): cold, at the end of the file -- reached by `watch()` / `unwatch()`, by the
// control events and by the removal of an actor that watches or is watched, never by a pass that
// does none of these.
//
// One rule carries it: a watch is a record on its watcher's core, opened once and closed once -- by
// its one answer (`__on_watch_down__`), by `unwatch()` or by the watcher's removal. Every answer --
// the target's core's (`killed`, `init_failed`, `init_threw`, `unknown`) or the watcher's own core's
// (`core_stopped`, `unknown`) -- travels as a `detail::WatchDown` carrying the watch's ticket, and
// becomes a `DownEvent` only where it closes that record: two answers to one watch deliver one,
// none follows `unwatch()`, and none closes a later watch of the same, reused, id.
void
VirtualCore::__watch__(ActorId const watcher, ActorId const target) noexcept {
    if (target == watcher)
        return; // watching oneself would announce a death to the dead
    auto &open = _watching[watcher];
    if (std::ranges::any_of(open, [target](WatchEntry const &w) { return w.peer == target; }))
        return; // open already: its one answer is on its way, or will be
    const std::uint64_t ticket = ++_watch_tickets;
    open.push_back({target, ticket});
    const CoreId core = target._core_id;
    if (core == _index) {
        __register_watch__(target, watcher, ticket);
        return;
    }
    if (!_engine._core_set.raw().contains(core)) {
        push<detail::WatchDown>(watcher, target, DownReason::unknown, ticket); // no such core: no actor holds the id
        return;
    }
    // Pairs with the fence of a core that has stopped (`__announce_stop__`): either this load sees
    // that core stopped, or that core sees the flag and announces its stop to this one, which
    // answers the watch (`__on_core_stopping__`) -- never neither. The flag is stored once:
    // its line holds the engine's mailbox table, read by every cross-core send. Release/acquire
    // makes the store that set it happen before this fence either way.
    if (!_engine._cross_core_watch.load(std::memory_order_acquire))
        _engine._cross_core_watch.store(true, std::memory_order_release);
    SharedCoreCommunication::seq_cst_fence();
    if (_engine.is_core_stopped(_engine._core_set.resolve(core)))
        push<detail::WatchDown>(watcher, target, DownReason::core_stopped, ticket); // nobody there to ask
    else
        push<detail::WatchRequest>(BroadcastId(core), watcher, target, ticket);
}

void
VirtualCore::__unwatch__(ActorId const watcher, ActorId const target) noexcept {
    const auto it = _watching.find(watcher);
    if (it == _watching.end() || std::erase_if(it->second, [target](WatchEntry const &w) { return w.peer == target; }) == 0)
        return; // not open: never watched, or answered already
    if (it->second.empty())
        _watching.erase(it);
    const CoreId core = target._core_id;
    if (core == _index)
        __unregister_watch__(target, watcher);
    else if (_engine._core_set.raw().contains(core) && !_engine.is_core_stopped(_engine._core_set.resolve(core)))
        push<detail::UnwatchRequest>(BroadcastId(core), watcher, target);
}

void
VirtualCore::__register_watch__(ActorId const target, ActorId const watcher, std::uint64_t const ticket) noexcept {
    // An actor killed but not yet reaped still holds its slot: registering then is right, its
    // removal at the end of this pass answers. No actor at all: it never existed or is gone.
    if (__actor_slot__(target) == nullptr) {
        push<detail::WatchDown>(watcher, target, DownReason::unknown, ticket);
        return;
    }
    // One registration per watcher: its core opens one watch of an actor at a time, and a withdrawal
    // reaches this core before the next request (one ordered pipe).
    auto &watchers = _watchers_of[target];
    if (const auto at = std::ranges::find(watchers, watcher, &WatchEntry::peer); at != watchers.end())
        at->ticket = ticket;
    else
        watchers.push_back({watcher, ticket});
}

void
VirtualCore::__unregister_watch__(ActorId const target, ActorId const watcher) noexcept {
    const auto it = _watchers_of.find(target);
    if (it == _watchers_of.end())
        return;
    std::erase_if(it->second, [watcher](WatchEntry const &w) { return w.peer == watcher; });
    if (it->second.empty())
        _watchers_of.erase(it);
}

void
VirtualCore::__on_watch_down__(ActorId const watcher, ActorId const target, DownReason const reason, std::uint64_t const ticket) {
    const auto it = _watching.find(watcher);
    if (it == _watching.end() || std::erase_if(it->second, [&](WatchEntry const &w) { return w.peer == target && w.ticket == ticket; }) == 0)
        return; // withdrawn by unwatch(), answered already, or its watcher is gone
    if (it->second.empty())
        _watching.erase(it);
    // Delivered now, in the step that closed the watch: a handler that watches the id again opens
    // a new watch. Built as `push` builds an event in a pipe slot -- the storage a pipe slot spans,
    // prepared, then the header -- so a `reply()` or `forward()` of it copies defined bytes.
    constexpr std::size_t        bytes = allocator::getItemSize<DownEvent, EventBucket>() * sizeof(EventBucket);
    alignas(DownEvent) std::byte storage[bytes];
    detail::prepare_event_storage(storage, bytes);
    auto &down = *new (storage) DownEvent(target, reason);
    fill_event(down, watcher, target);
    _router.route(down, [this](auto &event) {
        // No actor of this core registered `DownEvent`: the watcher's dead letter, like any event.
        __dead_letter__(event, __undelivered_reason__(event.getDestination()));
    });
}

void
VirtualCore::__on_core_stopping__(CoreId const core) noexcept {
    // Every answer `core` sent was received before this notice (one ordered pipe): a watch still
    // open on it is one it never received. Answered through the same `WatchDown` as any other, so
    // a watch answered already but not yet delivered (stashed for an activating watcher, or seen
    // stopped by `__watch__`) still delivers one `DownEvent`.
    for (auto const &[watcher, open] : _watching)
        for (WatchEntry const &w : open)
            if (w.peer._core_id == core)
                push<detail::WatchDown>(watcher, w.peer, DownReason::core_stopped, w.ticket);
}

void
VirtualCore::__announce_stop__(SharedCoreCommunication &engine, CoreId const index) noexcept {
    // A watch that reaches this core from now on is never answered here. The fence pairs with the
    // one in `__watch__`: a watcher's core either sees this core stopped and answers at once, or
    // this load sees its flag and the notice goes out -- never neither.
    SharedCoreCommunication::seq_cst_fence();
    if (likely(!engine._cross_core_watch.load(std::memory_order_relaxed)))
        return;
    const CoreId                            resolved = engine._core_set.resolve(index);
    constexpr std::size_t                   bytes    = allocator::getItemSize<detail::CoreStopping, EventBucket>() * sizeof(EventBucket);
    alignas(detail::CoreStopping) std::byte storage[bytes];
    detail::prepare_event_storage(storage, bytes);
    auto &notice = *new (storage) detail::CoreStopping();
    for (const auto core : engine._core_set.raw()) {
        if (core == index)
            continue;
        fill_event(notice, BroadcastId(core), BroadcastId(index));
        // Straight into that core's mailbox, behind all this core sent it before: after a normal
        // stop its pipes to running cores are empty; after an exception, what they held was never
        // sent and is not sent now -- the watches it would have answered are answered by this.
        // A full ring empties as its core receives; a core that stops meanwhile needs no notice.
        while (!engine.is_core_stopped(engine._core_set.resolve(core)) && !engine.send(resolved, notice))
            std::this_thread::yield();
    }
}

bool
VirtualCore::__is_watch_control__(EventId const id) noexcept {
    return id == Event::type_to_id<detail::WatchRequest>() || id == Event::type_to_id<detail::UnwatchRequest>()
           || id == Event::type_to_id<detail::WatchDown>() || id == Event::type_to_id<detail::CoreStopping>();
}

void
VirtualCore::__on_actor_down__(ActorId const id, DownReason const reason) noexcept {
    // Its watchers: one answer each. A watcher of this core already killed, not yet reaped, is
    // skipped -- its own removal closes its watches.
    if (const auto it = _watchers_of.find(id); it != _watchers_of.end()) {
        const std::vector<WatchEntry> watchers = std::move(it->second);
        _watchers_of.erase(it);
        for (WatchEntry const &w : watchers) {
            if (w.peer._core_id == _index) {
                Actor const *const actor = __actor_slot__(w.peer);
                if (actor == nullptr || !actor->is_alive())
                    continue;
            }
            push<detail::WatchDown>(w.peer, id, reason, w.ticket);
        }
    }
    // Its own watches: closed and withdrawn, so a long-lived target never accumulates dead watchers.
    if (const auto it = _watching.find(id); it != _watching.end()) {
        const std::vector<WatchEntry> open = std::move(it->second);
        _watching.erase(it);
        for (WatchEntry const &w : open) {
            const CoreId core = w.peer._core_id;
            if (core == _index)
                __unregister_watch__(w.peer, id);
            else if (_engine._core_set.raw().contains(core) && !_engine.is_core_stopped(_engine._core_set.resolve(core)))
                push<detail::UnwatchRequest>(BroadcastId(core), id, w.peer);
        }
    }
}

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
