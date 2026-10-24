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
#include <system_error>
#include <thread>
#include <type_traits>
#include <qb/core/VirtualCore.h>
#include <qb/event.h>
#include <qb/io/async/listener.h>
#include <qb/system/cpu.h>
#include <qb/system/time.h>

namespace qb {
VirtualCore::VirtualCore(CoreId const id, SharedCoreCommunication &engine) noexcept
    : _index(id)
    , _resolved_index(engine._core_set.resolve(id))
    , _engine(engine)
    , _mail_box(engine.getMailBox(id))
    , _event_buffer(allocator::make_region<EventBuffer>())
    , _pipes(__make_pipes__(_pipe_pool, engine.getNbCore()))
    , _self_pipe(_pipes[_resolved_index])
    , _ids(id) {
    // What this thread's io loop has run so far is not this core's: under `start(false)` the loop is the calling
    // thread's, and it ran the engines before this one (Huly QB-357).
    _io_events_base = io::async::listener::current.total_events_processed();
    // Every logical core id, member of the set or not, maps to the pipe `resolve()` names for it;
    // `_pipes` never grows after this, so the pointers stay good for the core's lifetime.
    for (std::size_t core = 0; core < MaxCores; ++core)
        _pipe_of_core[core] = &_pipes[engine._core_set.resolve(core)];
    _peer_pipes.reserve(_pipes.size() - 1);
    for (std::size_t i = 0; i < _pipes.size(); ++i)
        if (i != _resolved_index)
            _peer_pipes.push_back(&_pipes[i]);
    _peer_seen_read.assign(_pipes.size(), SIZE_MAX);
    _peer_stalls.assign(_pipes.size(), 0);
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
    for (auto *bank : {&_service_actors, &_actors})
        for (std::size_t slot = 0; slot < bank->size(); ++slot)
            if (Actor *const actor = (*bank)[slot].get())
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
    // start(false) leaves the thread-local listener in the caller's thread. Recycle, not reset:
    // the frames die with this engine, the scheduler's storage stays warm for the next one.
    auto &listener = io::async::listener::current;
    listener.recycle_coro_scheduler();
    listener.clear();

    // Destructors can kill peers or create a referenced child. Re-scan until
    // every actor has passed through removeActor's cancellation and watch path.
    while (_actor_count != 0 || !_activating.empty()) {
        // A listener or actor destructor can create a suspended child after
        // the first activation drain. Remove its frame before its actor.
        drain_activations();
        if (_actor_count == 0)
            break;
        ActorId next = ActorId::NotFound;
        for (auto *bank : {&_service_actors, &_actors}) {
            for (auto const &slot : *bank)
                if (slot) {
                    next = slot->id();
                    break;
                }
            if (next.is_instance())
                break;
        }
        if (next.is_instance())
            removeActor(next, DownReason::core_stopped);
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

    // A handler threw while a mailbox batch was being routed: its events still sit in `_event_buffer`.
    // Every event the batch already got rid of -- routed and disposed, replied or forwarded, stashed --
    // carries `alive` (router::internal::mark_disposed); the rest are destroyed here, once. The self
    // pipe needs nothing of the kind: an interrupted run is never popped, so the walk above saw it.
    if (unlikely(_rx_len != 0)) {
        auto       *cur  = _event_buffer->data();
        auto *const last = cur + _rx_len;
        while (cur < last) {
            auto      &event = *reinterpret_cast<Event *>(cur);
            const auto width = event.bucket_size;
            if (unlikely(width == 0))
                break;
            _router.dispose(event);
            cur += width;
        }
        _rx_len = 0;
    }

    // A continuation queued by the teardown above (an actor or payload destructor that wakes a
    // coroutine) lands in this loop's scheduler (bound lazily if none was). Destroy it with this
    // core: under start(false) the thread outlives the engine, and the next engine on it must
    // not inherit a frame of this one.
    listener.recycle_coro_scheduler();
    // Main owns the initializer until after its worker joins. Return all generations only
    // after actor/frame/user destruction; startup failures also retire abandoned promises.
    _ids.clear_reservations();
    if (_ids_owner != nullptr)
        *_ids_owner = std::move(_ids);
}

void
VirtualCore::__set_stop_token__(qb::stop_token token) noexcept {
    _stop_token = std::move(token);
}

void
VirtualCore::__install_id_pool__(detail::ActorIdPool &pool) {
    _ids       = pool.take();
    _ids_owner = &pool;
    if (_ids.ordinary_slot_count() != 0)
        allocator::detail::region_resize(_actors, _ids.ordinary_slot_count(), ActorId::MaxOrdinarySlot + 1u);
    if (_ids.service_slot_count() != 0)
        allocator::detail::region_resize(_service_actors, _ids.service_slot_count(), std::numeric_limits<ServiceId>::max());
}

void
VirtualCore::__prepare_actor_slot__(ActorId const id) {
    auto             &bank  = id.is_service() ? _service_actors : _actors;
    const std::size_t slot  = id.is_service() ? id.service_tag() : id.slot();
    const std::size_t limit = id.is_service() ? std::numeric_limits<ServiceId>::max() : ActorId::MaxOrdinarySlot + 1u;
    if (slot >= bank.size())
        // Grow the logical registry geometrically as well as its allocation. Fresh
        // admissions then initialise/check the extended pointer range once per doubling,
        // rather than invoking vector growth for every successive ordinary slot.
        allocator::detail::region_resize(bank, std::max(slot + 1u, bank.size() * 2u), limit);
}

ActorId
VirtualCore::__generate_id__() noexcept {
    // Both startup factories and dynamic admissions reserve before invoking user allocation.
    // Nested addReferencedActor calls save and restore this promise. Its owner alone can roll
    // it back, after construction returns or unwinds, so consuming it needs no pool recheck.
    const ActorId id  = _factory_actor_id;
    _factory_actor_id = {};
    if (id.is_valid())
        // A nonempty promise is a reserved instance: slot and generation are already nonzero.
        // Still refuse a service promise consumed by an ordinary Actor base constructor.
        return id.slot() < ActorId::ServiceBand ? id : ActorId{};
    return _ids.reserve_ordinary();
}

ActorId
VirtualCore::__generate_service_id__(ServiceId const tag) noexcept {
    const ActorId id  = _factory_actor_id;
    _factory_actor_id = {};
    if (id.is_valid())
        return id.is_service_instance() && id.service_tag() == tag ? id : ActorId{};
    return _ids.reserve_service(tag);
}

ActorId
VirtualCore::__resolve_destination__(ActorId const id) const noexcept {
    if (id.is_service_address())
        if (Actor *const service = __service_slot__(id))
            return service->id();
    return id;
}

// Event Management
void
VirtualCore::__initialize_default_events__(Actor &actor) noexcept {
    // Both base constructors call this before any derived registration can run. The identity
    // does not change here, so one failed-allocation check covers all five pointer stores.
    if (unlikely(!actor.id().is_valid()))
        return;

    // Keep each verbose log immediately before its store, in constructor registration order.
#ifdef QB_WITH_LOGGING
    if (nanolog::is_logged(nanolog::LogLevel::VERBOSE))
        __write_subscription_log__<KillEvent>(actor, __FILE__, __func__, __LINE__);
#else
    QB_LOG_VERB("Actor(" << actor.id() << ") subscribed to " << ActorProxy::getName<KillEvent>());
#endif
    actor._default_on[static_cast<std::size_t>(default_event_index<KillEvent>)] = &default_trampoline<Actor, KillEvent>;
#ifdef QB_WITH_LOGGING
    if (nanolog::is_logged(nanolog::LogLevel::VERBOSE))
        __write_subscription_log__<SignalEvent>(actor, __FILE__, __func__, __LINE__);
#else
    QB_LOG_VERB("Actor(" << actor.id() << ") subscribed to " << ActorProxy::getName<SignalEvent>());
#endif
    actor._default_on[static_cast<std::size_t>(default_event_index<SignalEvent>)] = &default_trampoline<Actor, SignalEvent>;
#ifdef QB_WITH_LOGGING
    if (nanolog::is_logged(nanolog::LogLevel::VERBOSE))
        __write_subscription_log__<UnregisterCallbackEvent>(actor, __FILE__, __func__, __LINE__);
#else
    QB_LOG_VERB("Actor(" << actor.id() << ") subscribed to " << ActorProxy::getName<UnregisterCallbackEvent>());
#endif
    actor._default_on[static_cast<std::size_t>(default_event_index<UnregisterCallbackEvent>)] =
        &default_trampoline<Actor, UnregisterCallbackEvent>;
#ifdef QB_WITH_LOGGING
    if (nanolog::is_logged(nanolog::LogLevel::VERBOSE))
        __write_subscription_log__<PingEvent>(actor, __FILE__, __func__, __LINE__);
#else
    QB_LOG_VERB("Actor(" << actor.id() << ") subscribed to " << ActorProxy::getName<PingEvent>());
#endif
    actor._default_on[static_cast<std::size_t>(default_event_index<PingEvent>)] = &default_trampoline<Actor, PingEvent>;
#ifdef QB_WITH_LOGGING
    if (nanolog::is_logged(nanolog::LogLevel::VERBOSE))
        __write_subscription_log__<RequireEvent>(actor, __FILE__, __func__, __LINE__);
#else
    QB_LOG_VERB("Actor(" << actor.id() << ") subscribed to " << ActorProxy::getName<RequireEvent>());
#endif
    actor._default_on[static_cast<std::size_t>(default_event_index<RequireEvent>)] = &default_trampoline<Actor, RequireEvent>;
}

void
VirtualCore::unregisterEvents(ActorId const id) const noexcept {
    _router.unsubscribe(id);
}

void
VirtualCore::__rollback_failed_admission__(ActorId const id) noexcept {
    // An outer guard may observe an admission already removed by the async-init path.
    // Only the reservation still owned by this exact incarnation can be rolled back.
    if (!_ids.owns_reservation(id))
        return;
    __unregisterCallback(id);
    unregisterEvents(id);
    if (unlikely(!_watchers_of.empty() || !_watching.empty()))
        __on_actor_down__(id, DownReason::init_failed);
    std::erase(_actor_to_remove, id);
    _ids.release(id);
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
        // Bind a logical service once. An activation stash retains this captured incarnation.
        if (unlikely(event->dest.is_service_address()))
            event->dest = __resolve_destination__(event->dest);
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
                    // Consumed by a live operation or discarded as obsolete — the END of that event's
                    // life, so it must be disposed exactly like every other terminal path
                    // (route() disposes after the handler; the stash disposes on overflow and
                    // on a failed init; the replay routes and disposes). A live awaiter took what
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
        _router.route(*event, [this](auto &event) {
            // No actor on this core registered the type. A broadcast reaching such a core is
            // normal -- unless it names a core the engine does not run (Huly QB-268); a unicast is a
            // dead letter (Huly QB-163) -- the destination is alive and handles no such event, or it
            // is not there at all.
            if (!event.getDestination().is_broadcast())
                __dead_letter__(event, __undelivered_reason__(event.getDestination()));
            else
                (void) Event::__broadcast_here__(event);
        });
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

bool
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
    // events all come from other cores -- or from nowhere, which is every idle pass. That test
    // and the mailbox's count are the pass's evidence that it received (Huly QB-257).
    const bool self = !_self_pipe.empty();
    if (self) {
        auto fence = _self_pipe.mark();
        for (auto run = _self_pipe.front(fence); !run.empty(); run = _self_pipe.front(fence)) {
            __receive_events__(run);
            _self_pipe.pop_front(fence);
        }
    }
    // global_core_events. `consume_all(func, scratch, chunk)`, not `dequeue(T*, n)`: the third
    // argument is a PER-PRODUCER batch limit, so every peer core's ring is drained on every
    // turn. A shared budget would let one saturated producer consume it and starve the rest.
    const std::size_t consumed = _mail_box.consume_all(
        [this](EventBucket *buffer, std::size_t const nb_events) {
            _rx_len = nb_events; // ~VirtualCore's walk if a handler throws mid-batch
            __receive_events__(std::span<EventBucket>{buffer, nb_events});
            _rx_len = 0;
        },
        _event_buffer->data(), MaxRingEvents);
    return self | (consumed != 0);
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
// Backoff policy (monotonic, cache-friendly) -- entered only by a core with no inbound work of
// its own; a core that has some bails at once and runs it (Huly QB-979, in the body):
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
// Refusals in a row, each finding the peer's consumer index where the previous one left it, before the
// flush calls the peer STALLED and gives the pass back to inbound work instead of retrying (Huly
// QB-979). Each refusal before that runs the whole budget above: two budgets (~250 us on g++, the
// yields' price) are what a peer that merely drains slowly -- savina/nqueens' master, in a kernel of its
// own core's workers between two drains -- is given to show it is alive, and what a peer held for
// milliseconds in one handler (savina/barber's factory) costs this core once per such episode.
constexpr std::uint8_t kFlushStallRefusals = 2;
// The idle poll's pacing floor (Huly QB-251): the least interval, in nanoseconds, between two ordered
// counter reads of a spinning core's idle passes (`VirtualCore::__pace__`). 0, the value QB-251 measured on
// both hosts, is one ordered read per idle pass and no re-read, ~11 ns on an i9-12900K P-core: the bare
// serialized gap the raw-ring probe measured as the shape that reaches the low regime of a cross-core hop
// (qb-vs-others TUNING.md section 16.2). A constant of this file, not a setting.
constexpr std::uint64_t kIdlePollFloorNs = 0;
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

                // The ring is full: backpressure. The cold half of this function -- the count, the QoS-0
                // drop, the stall test, the bounded backoff and the two partial bails -- is `__refused__`,
                // out of line at the end of this file (Huly QB-979). What that does NOT buy is the
                // compiler's schedule of the run-gathering loop above as it was before the rule: g++
                // and MSVC redo it whenever this function changes, however cold the change, and
                // savina/big at two cores -- where none of the rule's code runs -- reads ~1.5 % slower
                // for it on g++ (forty interleaved launches; the same with the branch inline, and worse,
                // +6.5 %, with the loop in a function of its own, which pays a call per run). Size,
                // member layout and alignment were excluded by their own controls; Windows is level
                // (qb-vs-others docs/TUNING.md 21.8). `true` gives the pass back with the event still
                // at the head of its pipe.
                if (__refused__(event, pipe_idx)) {
                    partial = true;
                    break;
                }
                cur += event.bucket_size;
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
VirtualCore::__init__(std::span<unsigned const> const cpus) {
    bool ret(true);
    // The CPUs `Main::start_thread` placed this core on (`qb::detail::place_thread`, Huly QB-1081): its position in the
    // mask its thread inherited by default, an explicit request inside that mask, or nothing -- the thread then keeps
    // the mask it inherited. Affinity is best-effort: a refused pin loses a placement, never the core, so it only warns.
    if (const int error = detail::pin_current_thread(cpus); error != 0)
        QB_LOG_WARN("set thread affinity failed: " << std::system_category().message(error));
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
    // Snapshot identities, not pointers: a previous onInit may destroy another
    // startup actor or grow the registry through addRefActor.
    std::vector<ActorId> actors_to_init;
    actors_to_init.reserve(_actor_count);
    // Services first, as when they held the lowest slots of one bank.
    for (auto const *bank : {&_service_actors, &_actors})
        for (auto const &slot : *bank)
            if (slot)
                actors_to_init.push_back(slot->id());
    for (const ActorId id : actors_to_init) {
        Actor *const actor = __actor_slot__(id);
        if (actor == nullptr || actor->_init_started)
            continue;
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

namespace {
/// The actor whose `onInit()` `__drive_init__` is running on this thread, or null: until that first resume
/// returns, the actor is not yet in `_activating` (Huly QB-1100). Saved and restored, as an init may add an actor.
constinit thread_local const Actor *tls_init_running = nullptr;
} // namespace

VirtualCore::InitOutcome
VirtualCore::__drive_init__(Actor &actor, qb::io::async::task<bool> &init) noexcept {
    actor._init_started = true;
    auto h              = init.handle();
    if (unlikely(!h))
        return InitOutcome::ReadyTrue; // defensive: a null task ⇒ trivially successful
    // `task` starts suspended: resume once to reach the first `co_await` or the `co_return`.
    // No scheduler is bound here on purpose. A zero sleep or an inline completion that queues
    // during this resume reaches `CoroutineScheduler::current()` / `schedule_via_current()`,
    // whose cold path binds THIS loop's scheduler (`detail::bind_thread_scheduler`), so the
    // continuation is drained by the owning loop; a synchronous init creates no scheduler, and
    // a core whose actors never suspend keeps a loop pass that asks no scheduler for work.
    const Actor *const outer = tls_init_running;
    tls_init_running         = &actor;
    h.resume();
    tls_init_running = outer;
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
    // Any immediate completion during __drive_init__'s resume was queued on this loop's
    // scheduler (bound lazily by the queueing path), so listener::run() drains it. Keep the frame
    // alive here until that loop drains the continuation and the activation pump
    // observes its verdict; never resume it inline from this activation path.
    // In particular, a ready continuation still owns the suspended init frame.
    // The actor is now Activating: gate its inbound unicast and keep its frame alive.
    actor._activated = false;
    const auto now   = static_cast<std::uint64_t>(qb::unix_nanos(qb::wall_now()));
    Activation act;
    act.init        = std::move(init);
    act.deadline_ns = activation_deadline_ns ? now + activation_deadline_ns : 0; // 0 ⇒ no deadline
    _activating.emplace(actor.id(), std::move(act));
    QB_LOG_VERB(actor << " activating (async onInit in flight)");
}

qb::duration
VirtualCore::__activation_park_cap__(qb::duration cap) const noexcept {
    // Activation uses wall time; request deadlines below remain monotonic.
    std::uint64_t nearest = 0;
    for (auto const &entry : _activating) {
        auto const &act = entry.second;
        if (act.cancelling || act.deadline_ns == 0)
            continue;
        if (nearest == 0 || act.deadline_ns < nearest)
            nearest = act.deadline_ns;
    }
    if (nearest == 0)
        return cap;
    const auto wall = static_cast<std::uint64_t>(qb::unix_nanos(qb::wall_now()));
    const auto left = nearest > wall ? nearest - wall : 0;
    if (left < static_cast<std::uint64_t>(cap.count()))
        cap = qb::duration{static_cast<qb::duration::rep>(left)};
    return cap;
}

bool
VirtualCore::__is_activating__(ActorId const id) const noexcept {
    return _activating.find(id) != _activating.end();
}

bool
VirtualCore::__in_init__(ActorId const id) const noexcept {
    return (tls_init_running != nullptr && tls_init_running->id() == id) || __is_activating__(id);
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
    event->__retire__(); // AFTER the copy (which keeps alive == 0): a teardown walk of the batch skips it
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
    thread_local allocator::region_vector<ActorId> scan_ids;
    thread_local allocator::region_vector<ActorId> done_ids;
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
        if (dying || act.cancelling || !ok || actor == nullptr) {
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

// The idle pass's pacing read (Huly QB-251). An idle pass of a SPINNING core is a poll of the peer
// rings: `__receive__` read each ring's published index and found it where this consumer left it.
// What sits between two of those reads decides the cross-core exchange -- measured, not reasoned
// (i9-12900K / WSL2 g++-14, ten interleaved launches a variant, Huly QB-180): ~20 ns of ordinary
// code, the pass with no clock read left in it, cost the 2c-spin cells of savina/ping-pong and
// thread-ring +25 %; a bounded tight `has_data()` poll was worse (+30 %); `lfence` alone (3 ns)
// left +7 / +14 %, `spin_loop_pause()` (33 ns on Golden Cove) +9 / +20 %; an ORDERED counter read
// (`lfence; rdtsc`, 11 ns) and the monotonic clock (13 ns: the same instructions under the vDSO)
// put both cells back on the control's figure. The raw-ring probe says why (qb-vs-others
// TUNING.md section 16.2): with ordinary work between two polls the next poll's load issues
// speculatively at the top of the gap and keeps the producer's line snooped while it publishes;
// a read that waits for the earlier loads to complete issues the next poll late, and once.
// Until 3.3 that read was the clock read the park policy needed anyway, and the poll's cadence was
// whatever the pass's content plus that clock came to: on Windows the clock is a call
// (`QueryPerformanceCounter`, ordered as the kernel chooses), and a pass made lighter by an
// unrelated change (one load less, Huly QB-65) moved the Windows 2c-spin cells +2.5-3 %
// (Huly QB-251). The read is now the counter itself, ordered the same way on both compilers and
// on arm64 (`qb::detail::tsc_ticks_ordered`), and the interval between two of them has an explicit floor,
// `_poll_floor_ticks` (`kIdlePollFloorNs` in the listener's calibration), held by re-reading the
// counter: the cadence is a number this file states, not the sum of what the pass happens to do,
// and a pass that gets lighter leaves it where it is. The park policy keeps its stamp in the same
// unit, so an idle pass reads no clock at all until it parks.
std::uint64_t
VirtualCore::__pace__() noexcept {
    std::uint64_t t = qb::detail::tsc_ticks_ordered();
    while (t - _poll_mark < _poll_floor_ticks)
        t = qb::detail::tsc_ticks_ordered();
    _poll_mark = t;
    return t;
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
    // The idle floors, in the counter's ticks (Huly QB-251): the listener calibrated the rate once,
    // for its io cadence; a counter that does not move (rate 0) is taken at one tick a nanosecond
    // -- the floors are bounds, never a clock. A core that never parks keeps `_idle_spin_ticks` at
    // all ones, so its idle pass never loads the mailbox's latency to learn that.
    {
        double rate = io::async::detail::tsc_per_ns(loop);
        if (rate <= 0.)
            rate = 1.;
        _poll_floor_ticks = static_cast<std::uint64_t>(rate * static_cast<double>(kIdlePollFloorNs));
        if (_mail_box.getLatency() > qb::duration::zero())
            _idle_spin_ticks = static_cast<std::uint64_t>(rate * static_cast<double>(_mail_box.getIdleSpin().count()));
    }
    // The signal generation this core last scanned, as a LOCAL: the pass compares it against the
    // global one in a register. Compared with the member, the check paid a memory operand on every
    // pass (+0.25 ns on the qb-vs-others pass-cost probe at k = 1, WSL2 g++-14, Huly QB-65).
    unsigned int scanned_generation = _last_signal_generation;
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

        // The pass's evidence of work, one flag from each phase (Huly QB-257): the io turn's count,
        // the flush's result, the receive's. What it replaced summed three cumulative counters at
        // the end of every pass and kept the previous sum live across the loop -- one more register
        // the pass could not spare on MSVC, where the signal generation had gone to the stack for it.
        bool active = false;
        if (loop.has_work()) {
            // Hot path: call `listener::run` directly -- no `async::run` wrapper
            // (avoids redundant checks). It counts the callbacks it runs: this turn's count is
            // the io phase's evidence.
            loop.run(EVRUN_NOWAIT);
            active = loop.nb_invoked_event() != 0;
        }

        // Complete any async `onInit()` resumed above (replay stashes / enforce
        // deadlines / finish deferred destroys). empty()-guarded: free when idle.
        if (unlikely(!_activating.empty())) {
            __pump_activations__();
            if (unlikely(_actor_count == 0))
                break; // the last actor was an activating-then-dying one
        }

        // send core events: a flush attempted is work, published or refused (a refused event is
        // retried by the next pass, which must not be a park)
        active |= __flush_all__();
        // receive core events
        active |= __receive__();
        // The tick phase and the reap, out of the loop (Huly QB-257): a core with no registered
        // callback -- every benchmark, every server driven by io and events -- carried the tick
        // phase's `LoopEvent`, snapshot assignment and thread-local guard in its loop body, and the
        // reap's vector swaps after them: code the hot pass jumped over, stack slots in its frame,
        // and MSVC, which lays blocks out in source order, had spilled the signal generation around
        // them. Kills during the drain skip the tick phase: a killed actor must not get another tick,
        // and its whole callback phase is skipped, as it always was (`__tick__` skips the ones a tick
        // kills). The `empty()` guard is what keeps the per-pass clock lazy IN PRACTICE: the
        // `LoopEvent` is built from `time()`, and until the guard that was the one unconditional
        // clock read left in a pass -- `985cbb3a` moved the read from the top of the loop into the
        // tick phase and wrote that a pass nobody asks costs nothing, but a `LoopEvent` built for
        // zero callbacks still asked: one `clock_gettime` per pass, 39.6 % of a one-core `ping-pong`
        // profile (`perf record -e cpu-clock`, WSL2 / g++-14), ~13 ns of a ~33 ns pass. No branch
        // hint on it: the predicate is a per-core constant on every pass but the one that registers
        // or unregisters the first / last callback.
        bool killed = !_actor_to_remove.empty();
        if (!killed && !_callback_list.empty())
            killed = __tick__();
        if (killed) [[unlikely]] {
            if (__reap__())
                break; // the last actor is gone
        }
        // Park policy -- a TIME floor, not an event-count credit. A pass is idle when it moved no
        // event (no io callback, no flush attempt, nothing received: the flags above) and left
        // nothing for the next pass in the self-core pipe (an actor's callback may push to itself
        // with no counted activity; parking over that would delay a local event by up to `latency`).
        // The first idle pass stamps `_idle_since`; the core keeps polling until the mailbox's
        // idle-spin floor has elapsed, then parks -- which returns on data, on a producer's notify,
        // or after `latency`. A wait that returns with nothing to do keeps the old stamp and parks
        // again on the next pass, so an idle core does not re-spin a whole floor between two
        // timeouts. The stamp and the floor are in the ordered counter's ticks: the counter is read
        // on idle passes only -- a busy pass pays one store -- and it is read in EVERY latency
        // mode, a latency-0 core included, which never parks and so never uses the stamp. That read
        // is not waste: it is what paces the idle poll, with its floor, as `__pace__` says. The
        // clock is read only to cap a park by a pending request deadline (Huly QB-189), a
        // microsecond-scale event; until 3.3 every idle pass read it and handed the reading to the
        // deadline list -- a precise fire the coarse pre-check at the top of the pass gives within
        // a scheduler tick anyway (Huly QB-251).
        //
        // WHERE it parks depends on whether the io loop has anything to deliver. With io
        // watchers active the park is taken INSIDE the loop (`Mailbox::wait(listener &)`:
        // `ev_run(EVRUN_ONCE)` capped at `latency`, ended by the producer through the
        // loop's async watcher), so a socket that becomes readable wakes the core at poll
        // latency instead of at the timeout -- a cv-parked core answered io only when
        // `latency` expired (p50 0.9-1.2 ms at 100 us, 15.6 ms at 10 ms, against 19 us polling;
        // qb-vs-others audit, axis N). Io delivered by that park counts as activity: the
        // stamp is cleared so the reply, and the request after it, are met at polling
        // latency. A core with no io work keeps the condition-variable park, whose cost
        // and handshake are the measured ones.
        if constexpr (_Timed)
            _pass_recorder.record(pass_start, qb::mono_now()); // the pass is timed, never the park
        if (likely(active) || !_self_pipe.empty()) {
            _idle_since = 0;
        } else {
            const std::uint64_t t = __pace__();
            if (_idle_since == 0) {
                _idle_since = t;
            } else if (t - _idle_since >= _idle_spin_ticks) {
                // A pending request deadline bounds the park (Huly QB-189): the loop holds no
                // timer for it any more, so neither park may outlast it -- the cap is the smaller
                // of `latency` and the time to the earliest deadline, on a clock reading made here.
                auto cap = _mail_box.getLatency();
                if (unlikely(!_activating.empty()))
                    cap = __activation_park_cap__(cap);
                if (_deadlines.armed()) {
                    const auto now  = static_cast<std::uint64_t>(qb::mono_now().time_since_epoch().count());
                    const auto left = detail::deadlines_next_in(_deadlines, now);
                    if (left < static_cast<std::uint64_t>(cap.count()))
                        cap = qb::duration{static_cast<qb::duration::rep>(left)};
                }
                if (loop.has_work()) {
                    if (_mail_box.wait(loop, cap))
                        _idle_since = 0;
                } else {
                    _mail_box.wait(cap);
                }
            }
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
bool
VirtualCore::initActor(Actor &actor, bool const doInit) noexcept {
    if (doInit) {
        qb::io::async::task<bool> init = actor.onInit();
        switch (__drive_init__(actor, init)) {
            case InitOutcome::ReadyTrue:
                break; // completed synchronously → already active (identical to before)
            case InitOutcome::ReadyFalse: {
                const auto h     = init.handle();
                const bool threw = h && qb::io::async::detail::promise_of(h).has_exception();
                actor.__cancel_coro_scope__();
                removeActor(actor.id(), threw ? DownReason::init_threw : DownReason::init_failed);
                return false;
            }
            case InitOutcome::Suspended:
                // Dynamic (`addRefActor`) async init: the actor exists and is addressable
                // now, but is not yet active. Admission succeeds; inbound unicast
                // is stashed until it activates (gate), the deadline bounds the window.
                __begin_activation__(actor, std::move(init));
                break;
        }
    }

    return true;
}

bool
VirtualCore::appendActor(std::unique_ptr<Actor> actor_ptr, ActorId const reserved_id, bool const doInit) noexcept {
    if (unlikely(_tearing_down))
        return false;
    Actor        &actor = *actor_ptr;
    const ActorId id    = actor.id();
    // Both private callers keep a guard owning reserved_id until append returns or unwinds.
    // User allocation/constructor hooks may grow the banks but cannot release that reservation:
    // it has no registry actor yet. Matching the complete identity carries that proof here.
    // Preserve the checked path for an allocator that returns a different reserved actor.
    if (unlikely(id != reserved_id)) {
        if (unlikely(!_ids.owns_reservation(id))) {
            QB_LOG_CRIT("Error Cannot add " << actor << ": its reservation is unavailable");
            return false;
        }
    }
    assert(_ids.owns_reservation(id));
    // Factory stores are prepared when installed; dynamic admission prepares before construction.
    // An instance's slot is above the band marker exactly when it is a service (instances never hold the broadcast slot).
    const bool        service = id.slot() > ActorId::ServiceBand;
    auto             &bank    = service ? _service_actors : _actors;
    const std::size_t slot    = service ? id.service_tag() : id.slot();
    if (unlikely(slot >= bank.size() || bank[slot] != nullptr))
        return false;
    if (initActor(actor, doInit)) {
        // The reservation stays owned by this admission until append returns: the actor
        // is not in the registry yet, so even a reentrant removal cannot release it.
        // onInit can grow both banks; reacquire the metadata row by slot, never by pointer.
        _ids.commit_reserved(service, slot);
        bank[slot] = std::move(actor_ptr);
        ++_actor_count;
        QB_LOG_VERB("New " << actor);
        return true;
    }
    return false;
}

#ifdef QB_WITH_LOGGING
namespace {
// Formatting only: the original removal sites own level checks and metadata.
QB_NOINLINE void
write_scoped_removal_log(Actor &actor, char const *file, char const *function, std::uint32_t line) noexcept {
    (void) (nanolog::NanoLog()
            == (nanolog::NanoLogLine(nanolog::LogLevel::VERBOSE, file, function, line)
                << actor << " destroyed with " << actor.active_coroutine_count() << " scoped coroutine(s) pending cancellation"));
}

QB_NOINLINE void
write_unscoped_removal_log(Actor &actor, char const *file, char const *function, std::uint32_t line) noexcept {
    (void) (nanolog::NanoLog()
            == (nanolog::NanoLogLine(nanolog::LogLevel::WARN, file, function, line)
                << actor << " destroyed with " << actor.active_coroutine_count()
                << " active coroutines - coroutines must not access actor state!"));
}

QB_NOINLINE void
write_delete_actor_log(Actor &actor, char const *file, char const *function, std::uint32_t line) noexcept {
    (void) (nanolog::NanoLog() == (nanolog::NanoLogLine(nanolog::LogLevel::VERBOSE, file, function, line) << "Delete " << actor));
}

} // namespace
#endif

bool
VirtualCore::__prepare_activating_removal__(ActorId const id) noexcept {
    if (Actor *const act = __actor_slot__(id))
        act->__cancel_coro_scope__();
    // cancel() runs user hooks synchronously. They may add an activating
    // child and rehash this table, so find the parent's entry again.
    const auto ait = _activating.find(id);
    if (ait != _activating.end()) {
        if (!ait->second.init.done()) {
            _dying_with_frame.insert(id);
            return false;
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
    return true;
}

void
VirtualCore::removeActor(ActorId const id, DownReason const reason) noexcept {
    // Deferred destroy (the actor must outlive its own coroutine frame): if its
    // `onInit()` frame is still suspended, cancel the scope so the frame unwinds, mark
    // the actor dying, and let `__pump_activations__` complete the teardown once the
    // frame reports `done()`. Re-entry from the pump (after the frame unwound and the
    // activation was dropped) falls straight through to the normal teardown below.
    if (unlikely(_activating.contains(id))) {
        if (!__prepare_activating_removal__(id))
            return;
    }
    // Callback-free cores need no hash lookup.
    if (!_actor_callbacks.empty())
        __unregisterCallback(id);
    unregisterEvents(id);
    if (Actor *const actor = __actor_slot__(id)) {
        // Catch-all cancel-on-destroy: every destruction path funnels through here
        // (kill, onInit failure, engine shutdown). Cancelling the scope wakes scoped
        // coroutines so they unwind cleanly; idempotent with kill()'s cancel.
        if (actor->has_coro_scope()) {
            actor->__cancel_coro_scope__();
            if (__actor_slot__(id) != actor)
                return; // a cancellation hook already completed this removal
        }
        if (actor->has_active_coroutines()) {
            if (actor->has_coro_scope())
            // Scoped coroutines were just cancelled — they unwind on the next loop
            // iteration. A non-zero count here is expected and safe.
#ifdef QB_WITH_LOGGING
                do {
                    if (nanolog::is_logged(nanolog::LogLevel::VERBOSE))
                        write_scoped_removal_log(*actor, __FILE__, __func__, __LINE__);
                } while (false);
#else
                QB_LOG_VERB(*actor << " destroyed with " << actor->active_coroutine_count() << " scoped coroutine(s) pending cancellation");
#endif
            else
#ifdef QB_WITH_LOGGING
                do {
                    if (nanolog::is_logged(nanolog::LogLevel::WARN))
                        write_unscoped_removal_log(*actor, __FILE__, __func__, __LINE__);
                } while (false);
#else
                QB_LOG_WARN(*actor << " destroyed with " << actor->active_coroutine_count()
                                   << " active coroutines - coroutines must not access actor state!");
#endif
        }
#ifdef QB_WITH_LOGGING
        do {
            if (nanolog::is_logged(nanolog::LogLevel::VERBOSE))
                write_delete_actor_log(*actor, __FILE__, __func__, __LINE__);
        } while (false);
#else
        QB_LOG_VERB("Delete " << *actor);
#endif
        // Detach the owning unique_ptr before calling user ~Actor(). A destructor
        // may add a referenced child and resize _actors; a reference to its slot
        // would dangle across that call.
        // `id` named a live actor just above: an instance, a service exactly when its slot is above the band marker
        const bool        service = id.slot() > ActorId::ServiceBand;
        auto             &bank    = service ? _service_actors : _actors;
        const std::size_t slot    = service ? id.service_tag() : id.slot();
        // The full-id registry lookup (repeated after a cancellation hook) proved this
        // live owner. No user code runs between retiring it and detaching its registry slot.
        _ids.begin_retirement(service, slot);
        std::unique_ptr<Actor> dying = std::move(bank[slot]);
        --_actor_count;
        dying.reset(); // re-entrant admission cannot take this Retiring slot
        // The detached Retiring slot cannot be reused or released by a nested removal.
        // Its metadata may have moved while the destructor admitted other actors.
        _ids.finish_retirement(id, service, slot);
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
    Actor const *actor = __actor_slot__(id);
    // A logical service address deliberately follows the current incarnation. Keep this
    // fallback off the successful exact-instance path; stale instance IDs never resolve here.
    if (unlikely(actor == nullptr))
        actor = __service_slot__(id);
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
// VirtualCore::getProxyPipe is inline in VirtualCore.h. Actor::getPipe is inline in Actor.h,
// where it constructs its Pipe from the private pipe_buffer_for's scalar buffer reference.
// The lines are kept, which documents cite.
//

bool
VirtualCore::try_send(Event const &event) const noexcept {
    // Pass THIS core's resolved index as the MPSC producer slot: the physical
    // sending thread owns exactly one single-producer ring per destination
    // mailbox. `event.source` can name a different core after `forward()`, so it
    // must NOT drive slot selection (that would be a cross-core two-writer race).
    return _engine.send(_resolved_index, event);
}

// Same-core copies go through detail::event_wire::copy: the first 16-byte chunk clears
// `alive`, and the remaining chunks preserve the full source identity and payload. A one-bucket
// event needs no libc call. See event_wire's doc block in Event.h.
// The cross-core mailbox is a raw memcpy of the original, so an original that still says
// `alive` (the module-facing `Actor::send(Event const &)` on an event already replied or
// forwarded — reply()/forward() themselves clear it in their header rewrite) takes the local
// pipe, whose copy clears it; the batched flush relocates it later exactly like a try_send miss.
void
VirtualCore::send(Event const &event) noexcept {
    // The local destination is already known: reserve directly in its stable pipe,
    // without decoding the destination again to discover the same cursor address.
    if (event.dest.index() == _index) {
        detail::event_wire::copy(_self_pipe.allocate_back(event.bucket_size), event, event.bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES);
        return;
    }
    if (unlikely(event.state.bits.alive) || !try_send(event)) {
        __send_unpublished__(event);
        return;
    }
    // Published straight into the peer's ring: the flush will never see it, so it is counted
    // here (Huly QB-162) -- every reply of a cross-core exchange takes this path.
    ++_metrics._nb_event_sent;
    _metrics._nb_bucket_sent += event.bucket_size;
}

Event &
VirtualCore::push(Event const &event) noexcept {
    auto &pipe = __getPipe__(event.dest.index());
    // the copy clears `alive` itself, so re-pushing an already replied/forwarded event is fine
    return detail::event_wire::copy(pipe.allocate_back(event.bucket_size), event, event.bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES);
}

void
VirtualCore::reply(Event &event) noexcept {
    if (event.source.index() == _index) {
        reply_local(event);
        return;
    }
    reply_remote(event);
}

void
VirtualCore::reply_local(Event &event) noexcept {
    auto *const       raw   = _self_pipe.allocate_back(event.bucket_size);
    const std::size_t bytes = event.bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES;
    if (bytes > QB_LOCKFREE_EVENT_BUCKET_BYTES) { // more than one bucket (a literal 64 sent every arm64 event here)
        // Copy the contiguous range before composing its final reply header.
        // Use memcpy's returned destination; capture the header only after the call.
        auto *const   d           = static_cast<unsigned char *>(std::memcpy(raw, &event, bytes));
        const ActorId destination = event.source;
        const ActorId previous    = event.dest;
        std::uint64_t metadata;
        std::memcpy(&metadata, &event, 8);
        metadata &= ~static_cast<std::uint64_t>(detail::event_wire::alive_bit);
        std::memcpy(d, &metadata, 8);
        std::memcpy(d + 8, &destination, 8);
        std::memcpy(d + 16, &previous, 8);
        event.dest             = destination;
        event.source           = previous;
        event.state.bits.alive = 1;
        return;
    }
    const ActorId     destination = event.source;
    const ActorId     previous    = event.dest;
    auto *const       d           = reinterpret_cast<unsigned char *>(raw);
    auto const *const s           = reinterpret_cast<unsigned char const *>(&event);
    std::uint64_t     metadata;
    std::memcpy(&metadata, s, 8);
    metadata &= ~static_cast<std::uint64_t>(detail::event_wire::alive_bit);
    std::memcpy(d, &metadata, 8);
    std::memcpy(d + 8, &destination, 8);
    std::memcpy(d + 16, &previous, 8);
    event.dest   = destination;
    event.source = previous;
#if defined(QB_EVENT_WIRE_SSE2)
    if constexpr (QB_LOCKFREE_EVENT_BUCKET_BYTES >= 64) {
        _mm_storel_epi64(reinterpret_cast<__m128i *>(d + 24), _mm_loadl_epi64(reinterpret_cast<__m128i const *>(s + 24)));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 32), _mm_loadu_si128(reinterpret_cast<__m128i const *>(s + 32)));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 48), _mm_loadu_si128(reinterpret_cast<__m128i const *>(s + 48)));
        if (bytes > 64)
            std::memcpy(d + 64, s + 64, bytes - 64);
    } else {
        std::memcpy(d + 24, s + 24, bytes - 24);
    }
#else
    // One bucket: a constant-size copy the compiler expands inline. `bytes - 24` alone is a library
    // call to memmove on every event of a target without SSE2 -- arm64 takes this path, 40 bytes an event.
    if (likely(bytes == QB_LOCKFREE_EVENT_BUCKET_BYTES))
        std::memcpy(d + 24, s + 24, QB_LOCKFREE_EVENT_BUCKET_BYTES - 24);
    else
        std::memcpy(d + 24, s + 24, bytes - 24);
#endif
    event.state.bits.alive = 1;
    return;
}

void
VirtualCore::reply_remote(Event &event) noexcept {
    if (_engine.send_reply(_resolved_index, event)) {
        ++_metrics._nb_event_sent;
        _metrics._nb_bucket_sent += event.bucket_size;
        return;
    }
    // Full ring: the transaction did not touch the event. Queue exactly once, with
    // the same blocked-send accounting and deferred publication as ordinary reply.
    detail::event_wire::swap_dest_source(event);
    __send_unpublished__(event);
    event.state.bits.alive = 1;
}

void
VirtualCore::forward(ActorId const dest, Event &event) noexcept {
    if (dest.index() == _index) {
        // Compose the queued header directly, preserving the logical source. The stable
        // segmented pipe cannot move the original while reserving the copied event.
        auto *const       raw   = _self_pipe.allocate_back(event.bucket_size);
        const std::size_t bytes = event.bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES;
        auto *const       d     = reinterpret_cast<unsigned char *>(raw);
        auto const *const s     = reinterpret_cast<unsigned char const *>(&event);
        std::uint64_t     metadata;
        std::memcpy(&metadata, s, 8);
        metadata &= ~static_cast<std::uint64_t>(detail::event_wire::alive_bit);
        std::memcpy(d, &metadata, 8);
        std::memcpy(d + 8, &dest, 8);
        std::memcpy(d + 16, &event.source, 8);
#if defined(QB_EVENT_WIRE_SSE2)
        if constexpr (QB_LOCKFREE_EVENT_BUCKET_BYTES >= 64) {
            _mm_storel_epi64(reinterpret_cast<__m128i *>(d + 24), _mm_loadl_epi64(reinterpret_cast<__m128i const *>(s + 24)));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 32), _mm_loadu_si128(reinterpret_cast<__m128i const *>(s + 32)));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 48), _mm_loadu_si128(reinterpret_cast<__m128i const *>(s + 48)));
            if (bytes > 64)
                std::memcpy(d + 64, s + 64, bytes - 64);
        } else {
            std::memcpy(d + 24, s + 24, bytes - 24);
        }
#else
        // One bucket: a constant-size copy the compiler expands inline. `bytes - 24` alone is a library
        // call to memmove on every event of a target without SSE2 -- arm64 takes this path, 40 bytes an event.
        if (likely(bytes == QB_LOCKFREE_EVENT_BUCKET_BYTES))
            std::memcpy(d + 24, s + 24, QB_LOCKFREE_EVENT_BUCKET_BYTES - 24);
        else
            std::memcpy(d + 24, s + 24, bytes - 24);
#endif
        event.dest             = dest;
        event.state.bits.alive = 1;
        return;
    }
    forward_remote(dest, event);
}

void
VirtualCore::forward_remote(ActorId const dest, Event &event) noexcept {
    // forward already established a remote destination; set_dest clears alive before
    // the same physical-producer publication that send would attempt.
    detail::event_wire::set_dest(event, dest);
    if (!try_send(event)) {
        __send_unpublished__(event);
    } else {
        ++_metrics._nb_event_sent;
        _metrics._nb_bucket_sent += event.bucket_size;
    }
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
    ++_nb_dead_letters_by_reason[static_cast<std::size_t>(reason)];
    const auto n = __dead_letter_count__();
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

std::uint64_t
VirtualCore::__dead_letter_count__() const noexcept {
    std::uint64_t n = 0;
    for (auto const by_reason : _nb_dead_letters_by_reason)
        n += by_reason;
    return n;
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
    s.dead_letters           = __dead_letter_count__();
    s.dead_letters_by_reason = _nb_dead_letters_by_reason;
    // The loop's own count, the one the park policy reads -- it includes the callbacks an idle core's park ran --
    // since this core was built: under `start(false)` the loop is the calling thread's and outlives the engine
    // (Huly QB-357).
    s.io_events = io::async::listener::current.total_events_processed() - _io_events_base;
    // Timed cores only (Huly QB-165): the sliding worst case is judged against the clock of the ask.
    if (_pass_timing)
        s.pass_time = _pass_recorder.snapshot(qb::mono_now());
    return s;
}

// The cold half of the router's question before a broadcast is fanned out (`Event::__broadcast_here__`,
// `router::internal::scopes_broadcast`, Huly QB-268): the broadcast names another core than the one this thread
// runs -- a `BroadcastId` of a core the engine does not run aliased this core's queues (`CoreSet::resolve`'s index
// 0) and is owed to no actor here. Reported once, `not_found`; the router disposes it. Reached only on a thread
// bound to a core (`__bind_thread__`: the inline compare answers for every other); the test is kept for a caller
// that would not be.
void
Event::__broadcast_elsewhere__(Event &event) noexcept {
    if (VirtualCore *const core = VirtualCore::_handler; core != nullptr)
        core->__dead_letter__(event, DeadLetterReason::not_found);
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
    const CoreId core = target.index();
    if (core == _index) {
        __register_watch__(target, watcher, ticket);
        return;
    }
    if (!_engine._core_set.raw().contains(core)) {
        push<detail::WatchDown>(watcher, target, DownReason::unknown, ticket); // no such core: no actor holds the id
        return;
    }
    // Pairs with the fence of a core that has stopped (`__announce_stop__`): either this load sees that
    // core stopped, or that core finds this core's bit among its watchers and tells it of its stop, which
    // answers the watch (`__on_core_stopping__`) -- never neither. One bit per pair of cores, set once;
    // release/acquire orders the store before this fence. Only a watcher hears of a stop (Huly QB-203).
    auto &mine = _engine.watchers_of(_engine._core_set.resolve(core))[_resolved_index / 64]; // this core's word in its set
    if (std::uint64_t const bit = std::uint64_t{1} << (_resolved_index % 64); !(mine.load(std::memory_order_acquire) & bit))
        mine.fetch_or(bit, std::memory_order_release);
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
    const CoreId core = target.index();
    if (core == _index)
        __unregister_watch__(target, watcher);
    else if (_engine._core_set.raw().contains(core) && !_engine.is_core_stopped(_engine._core_set.resolve(core)))
        push<detail::UnwatchRequest>(BroadcastId(core), watcher, target);
}

void
VirtualCore::__register_watch__(ActorId const target, ActorId const watcher, std::uint64_t const ticket) noexcept {
    // An actor killed but not yet reaped still holds its slot: registering then is right, its
    // removal at the end of this pass answers. No actor at all: it never existed or is gone.
    if (__actor_slot__(__resolve_destination__(target)) == nullptr) {
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
            if (w.peer.index() == core)
                push<detail::WatchDown>(watcher, w.peer, DownReason::core_stopped, w.ticket);
}

void
VirtualCore::__announce_stop__(SharedCoreCommunication &engine, CoreId const index) noexcept {
    // A watch that reaches this core from now on is never answered here. The fence pairs with the
    // one in `__watch__`: a watcher's core either sees this core stopped and answers at once, or this
    // load sees its bit and the notice goes out -- to the cores that watched this one, and no other.
    SharedCoreCommunication::seq_cst_fence();
    const CoreId resolved = engine._core_set.resolve(index);
    auto const   watchers = engine.watchers_of(resolved);
    if (likely(std::ranges::all_of(watchers, [](auto const &bits) { return bits.load(std::memory_order_relaxed) == 0; })))
        return;
    constexpr std::size_t                   bytes = allocator::getItemSize<detail::CoreStopping, EventBucket>() * sizeof(EventBucket);
    alignas(detail::CoreStopping) std::byte storage[bytes];
    detail::prepare_event_storage(storage, bytes);
    auto &notice = *new (storage) detail::CoreStopping();
    for (const auto core : engine._core_set.raw()) {
        const CoreId watcher = engine._core_set.resolve(core);
        if (core == index || !((watchers[watcher / 64].load(std::memory_order_relaxed) >> (watcher % 64)) & 1u))
            continue; // a core with no watch here: nothing to answer, nor, on Windows, a closed ring to open (QB-203)
        fill_event(notice, BroadcastId(core), BroadcastId(index));
        // Straight into its mailbox, behind all this core sent it: after a normal stop its pipes to running cores are
        // empty, after an exception what they held is not sent and this answers its watches; a full ring empties.
        while (!engine.is_core_stopped(watcher) && !engine.send(resolved, notice))
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
    // Logical service watches retain the requested address as their ticket key, while
    // instance watches retain the exact generation. Retiring blocks replacement until teardown.
    auto notify = [this, reason](ActorId const target) {
        if (const auto it = _watchers_of.find(target); it != _watchers_of.end()) {
            const std::vector<WatchEntry> watchers = std::move(it->second);
            _watchers_of.erase(it);
            for (WatchEntry const &w : watchers) {
                if (w.peer.index() == _index) {
                    Actor const *const actor = __actor_slot__(w.peer);
                    if (actor == nullptr || !actor->is_alive())
                        continue;
                }
                push<detail::WatchDown>(w.peer, target, reason, w.ticket);
            }
        }
    };
    notify(id);
    if (id.is_service_instance())
        notify(ActorId::service_address(id.service_tag(), id.index()));
    // Its own watches: closed and withdrawn, so a long-lived target never accumulates dead watchers.
    if (const auto it = _watching.find(id); it != _watching.end()) {
        const std::vector<WatchEntry> open = std::move(it->second);
        _watching.erase(it);
        for (WatchEntry const &w : open) {
            const CoreId core = w.peer.index();
            if (core == _index)
                __unregister_watch__(w.peer, id);
            else if (_engine._core_set.raw().contains(core) && !_engine.is_core_stopped(_engine._core_set.resolve(core)))
                push<detail::UnwatchRequest>(BroadcastId(core), id, w.peer);
        }
    }
}

// The service registry (Huly QB-1007). Cold, and at the END of the file like the other cold bodies here: a tag
// registers once per process, a core is counted twice in its life.

namespace detail {
service_registry_seal::service_registry_seal() noexcept {
    // Keep the existing first-registration policy sealed for the whole core lifetime.
    // Ordinary slots no longer depend on this registry's tag count.
    std::lock_guard<std::mutex> const lock(VirtualCore::servicesMutex());
    ++VirtualCore::_live_cores;
}

service_registry_seal::~service_registry_seal() noexcept {
    std::lock_guard<std::mutex> const lock(VirtualCore::servicesMutex());
    --VirtualCore::_live_cores;
}
} // namespace detail

static_assert(std::is_empty_v<detail::service_registry_seal>,
              "VirtualCore's registry seal must stay empty: as an empty base it leaves VirtualCore's layout unchanged");

ServiceId
VirtualCore::__register_service__(std::atomic<ServiceId> &slot, TypeId const tag) noexcept {
    std::lock_guard<std::mutex> const lock(servicesMutex());
    // Another thread of this image registered the tag while this one waited for the lock.
    if (ServiceId const sid = slot.load(std::memory_order_relaxed); sid != 0)
        return sid;
    // Another image registered it (its own slot, the same type id): one id per tag per process, not a second one.
    auto &services = getServices();
    if (auto const it = services.find(tag); it != services.end()) {
        slot.store(it->second, std::memory_order_release);
        return it->second;
    }
    auto const registered = _nb_service.load(std::memory_order_relaxed);
    if (unlikely(_live_cores != 0)) {
        // The prototype retains the established policy: load first-time service tags before start.
        // Not stored in `slot`: registration may be retried once every core is gone.
        QB_LOG_CRIT("Service tag " << qb::event_type_name(tag) << " first registered while " << _live_cores
                                   << " VirtualCore(s) are alive: refused, its service id is NotFound -- load it before Main::start");
        assert(false && "qb: a service tag registered while a VirtualCore is alive is refused (Huly QB-1007) -- see the CRIT log");
        return 0;
    }
    if (unlikely(registered + 1 >= std::numeric_limits<ServiceId>::max())) {
        // 65534 tags: encoding tag 65535 in the service band would collide with the broadcast slot.
        QB_LOG_CRIT("Service tag " << qb::event_type_name(tag) << " refused: the service id space is spent (" << registered
                                   << " tags registered)");
        assert(false && "qb: the service id space is exhausted (Huly QB-1007) -- see the CRIT log");
        return 0;
    }
    auto const sid = static_cast<ServiceId>(registered + 1);
    services.emplace(tag, sid);
    _nb_service.store(sid, std::memory_order_relaxed);
    slot.store(sid, std::memory_order_release);
    return sid;
}

bool
VirtualCore::__predicted_ids_hold__(CoreInitializer const &initializer) const noexcept {
    return std::ranges::all_of(initializer._actor_factories,
                               [this](auto const &factory) { return _ids.owns_reservation(factory->reserved_id()); });
}

// The flush's question on backpressure (Huly QB-979): does this core hold work of its own that
// `__receive__` or the io pump would run if the flush gave the pass back now? Cold -- asked only
// once a peer's ring is found full -- and at the end of the file for the layout reason above.
bool
VirtualCore::__has_inbound_work__() const noexcept {
    return !_self_pipe.empty() || _mail_box.has_data() || io::async::listener::current.has_work();
}

// The flush's refused event (Huly QB-979): the cold half of `__flush_pipes__`, out of line and at the end
// of the file for the layout reason above (what that does and does not buy the hot loop is said at its
// call site). `event` is the head of
// the pipe to `pipe_idx`'s core, whose ring `try_send` has just refused; the two undeliverable shapes
// were taken out by the caller. Returns true when the pass must be given back with the event still at
// the head of its pipe (a partial flush), false when the event was published or dropped and the walk
// moves past it.
bool
VirtualCore::__refused__(Event &event, std::size_t const pipe_idx) noexcept {
    // From here the destination's mailbox is full: backpressure, counted for
    // `Actor::getCoreStats()` once per failed attempt (Huly QB-162).
    ++_nb_send_blocked;
    if (!event.state.bits.qos) {
        // Best-effort event: dropped on backpressure (preserves the
        // original fire-and-forget semantics for QoS-0 events such as
        // metrics or heartbeats). Counted as dropped, not as sent: it
        // was counted "sent" until 3.3, which made the two read the same.
        ++_nb_event_dropped;
        return false;
    }

    // QoS-guaranteed event. Only the peer's own receive pass frees its ring, and what
    // this core does meanwhile depends on two things (Huly QB-979): whether the peer is
    // STALLED -- it consumed nothing since this core last found its ring full, which the
    // producer ring's snapshot of the consumer's index says without a cross-line read --
    // and whether this core has INBOUND work of its own waiting: a peer's ring into it
    // holds events, the self pipe holds the same-core pushes of the last pass, or the io
    // loop has something to deliver.
    //
    //   * stalled peer AND inbound work -- bail NOW, exactly as the exhausted budget
    //     below bails: the event stays at the head of the pipe for the next pass, the
    //     peer is notified, and `__receive__` runs. That work may be what the peer is
    //     waiting on (the mutual-full-rings scenario at the top of this block is
    //     resolved by this very drain), and it is throughput this core would otherwise
    //     burn in the backoff: savina/barber at two cores spent its barber's passes
    //     here -- up to 64 pauses and 448 yields per pass -- while its factory held the
    //     peer core in one handler for the whole production (94 285 blocked attempts
    //     per run, benchmarks/savina/barber.md), and the second core bought 5 % of the
    //     one-core time.
    //   * a peer that IS draining -- then the bounded backoff below, even with inbound
    //     work waiting: savina/nqueens' master drains its ring every pass and relays
    //     what the workers send it, so a worker that ran its own kernels instead of
    //     waiting a few tens of microseconds hoarded the children the master was about to
    //     relay, and the search fell into its slower schedule more often (72 % of the
    //     repetitions against 28 %, measured, with a rule that bailed on inbound work
    //     alone; 46 % with a stall called on the first unmoved refusal). A stall is
    //     called on the `kFlushStallRefusals`-th refusal in a row that finds the index
    //     unmoved; the refusals before it run the whole budget, the first of an episode
    //     always (there is nothing to compare with yet).
    //   * nothing else to do -- the bounded backoff: a spin that re-tries at the ring's
    //     latency, then yields that give the CPU to a peer that may share it. A core with
    //     nothing to do has nothing better to spend the time on, and the yield phase is
    //     what keeps an oversubscribed pair out of a livelock.
    //
    // Ticks (`_callback_list`) are not inbound work, on purpose: a ticking producer that
    // floods a slower peer keeps the throttle the backoff gives it.
    auto &peer_ring = _engine.getMailBox(event.dest.index()).ringOf(_resolved_index);
    {
        const std::size_t seen    = peer_ring.producer_seen_read_index();
        const bool        unmoved = seen == _peer_seen_read[pipe_idx];
        _peer_seen_read[pipe_idx] = seen;
        std::uint8_t &stalls      = _peer_stalls[pipe_idx];
        stalls                    = unmoved ? static_cast<std::uint8_t>(stalls == 255 ? 255 : stalls + 1) : 0;
        if (stalls >= kFlushStallRefusals && __has_inbound_work__()) {
            _engine.getMailBox(event.dest.index()).notify();
            return true;
        }
    }

    // Bounded backoff.
    for (std::uint32_t attempt = 1; attempt <= kFlushYieldAttempts; ++attempt) {
        if (try_send(event)) {
            ++_metrics._nb_event_sent;
            _metrics._nb_bucket_sent += event.bucket_size;
            return false;
        }
        ++_nb_send_blocked;
        if (attempt < kFlushSpinAttempts) {
            qb::spin_loop_pause();
        } else {
            std::this_thread::yield();
        }
    }

    // Budget exhausted — surrender cleanly. The destination's consumer
    // is woken so it runs immediately (at zero latency only the fence
    // runs — see `Mailbox::notify()`). What the peer consumed DURING the budget is
    // not a stall's evidence: the snapshot the next refusal compares with is the one
    // the last attempt refreshed.
    _peer_seen_read[pipe_idx] = peer_ring.producer_seen_read_index();
    _engine.getMailBox(event.dest.index()).notify();
    return true;
}

// The push-family answers (Huly QB-976). reply() and forward() publish straight into another core's ring
// -- the eager path, send's, unordered -- so an answer can reach its destination before an event the same
// handler pushed to it earlier. These two take the pipe instead: the copy is the one push(Event const&)
// makes, at the tail behind everything this core queued, delivered by the pass-end flush in order (same
// core through the self pipe, cross-core through the outbound pipe). The header rewrite and the `alive`
// raise after the copy are reply()'s and forward()'s. At the end of the file: nothing the pass drives, and
// no existing path changes -- the ordering was measured as a filter on reply() itself (the ticket's A/B:
// the hub shapes paid it, chameneos +9 %, big +6 % on g++-14) and is offered as a spelling instead.
void
VirtualCore::push_reply(Event &event) noexcept {
    detail::event_wire::swap_dest_source(event); // swap complete identities, clear alive before copying
    push(event);
    event.state.bits.alive = 1; // AFTER the copy, which carries 0
}

void
VirtualCore::push_forward(ActorId const dest, Event &event) noexcept {
    detail::event_wire::set_dest(event, dest);
    push(event);
    event.state.bits.alive = 1;
}

// The cross-core `send(Event const &)` that was not published straight into the peer's ring (Huly QB-356): the
// ring refused it -- a full mailbox, one blocked attempt, as a refused flush is -- or the original is `alive` and
// was not offered (see `send`). Either way it waits in the outbound pipe, whose copy clears `alive`, and the flush
// delivers it. Cold, out of line and at the end of the file for the layout reason above: inline, its counter kept
// `this` live across the copy and cost the published reply one instruction (27 Ir a call against 26, callgrind).
void
VirtualCore::__send_unpublished__(Event const &event) noexcept {
    _nb_send_blocked += !event.state.bits.alive;
    auto &pipe = __getPipe__(event.dest.index());
    detail::event_wire::copy(pipe.allocate_back(event.bucket_size), event, event.bucket_size * QB_LOCKFREE_EVENT_BUCKET_BYTES);
}

// The direct publication of `send<E>` that a full mailbox refused (Huly QB-356): one blocked attempt, the event
// left at the tail of the outbound pipe for the flush. Cold and out of line for the same reason: inline, MSVC
// laid the increment between the published path and the epilogue, which every published cross-core `send<E>`
// jumped over (91 instructions against 89, and the published path a taken jump longer).
void
VirtualCore::__send_blocked__() noexcept {
    ++_nb_send_blocked;
}

// The tick phase (Huly QB-257): out of the pass's loop, and after it in the file for the layout reason
// above -- a ticking core calls it every pass, every other core never, and the call costs the ticking
// core less than the snapshot assignment it makes anyway. Not inlined, which is the point: inlined,
// the `LoopEvent` and the snapshot's thread-local guard were a stack frame and a TLS derivation in
// the hot loop's body. The `empty()` guard is at the call site.
bool
VirtualCore::__tick__() {
    // One LoopEvent for the whole pass -- same `now`/`iteration` for every callback, consistent with
    // `Actor::time()` (the same per-pass sample; a pass with no registered callback and no caller
    // never reads the clock). Delivered by a direct virtual call (not routed), from a snapshot: a
    // tick handler may register or unregister actors during the dispatch (e.g. via `addRefActor`).
    const qb::LoopEvent loop_ev{time(), _loop_count};
    _callback_snapshot = _callback_list;
    for (auto const &entry : _callback_snapshot) {
        // Skip the callback of an actor killed earlier in this same dispatch pass (e.g. by an
        // earlier actor's tick). The object is still alive -- destruction is deferred to the reap
        // -- so this is purely a semantics fix: a killed actor must not get another tick, matching
        // the event-kill path which skips the whole callback phase. The empty() fast-path keeps the
        // common (nothing killed) case free; `is_alive()` is the kill flag itself (`Actor::kill()`
        // is its only writer), read off an object the reap has not reached yet.
        if (likely(_actor_to_remove.empty()) || entry.actor->is_alive())
            entry.cb->on(loop_ev);
    }
    return !_actor_to_remove.empty();
}

// The reap (Huly QB-257): out of the pass's loop, cold, at the end of the file for the same layout
// reason. `removeActor()` destroys the actor, running user code that may `kill()` ANOTHER actor and
// so re-enter `killActor()` -> `_actor_to_remove.push_back()`. The scratch buffer keeps that
// re-entrant push off the container being iterated (a growth reallocation of the vector moves its
// elements and invalidates a live iterator) and keeps late kills from being discarded by the clear
// (which stranded a `!is_alive()` actor in `_actors`, so the live count never reached zero and the
// core never terminated). Terminates: only that user code refills the queue and it runs at most
// once per actor -- once an id has left `_actors`, `removeActor()` destroys nothing -- so a pass that
// destroys nothing ends it. Pinned by `KillDuringReap.ActorKilledFromAnotherDestructorIsStillReaped`.
bool
VirtualCore::__reap__() noexcept {
    while (!_actor_to_remove.empty()) {
        _actor_remove_batch.clear();
        _actor_remove_batch.swap(_actor_to_remove);
        for (auto const actor : _actor_remove_batch)
            removeActor(actor);
    }
    _actor_remove_batch.clear(); // drop the last batch's ids (capacity is kept for reuse)
    return _actor_count == 0;
}

// Cold incarnation allocation; per-event resolution reads only the actor registry.
detail::ActorIdPool::ActorIdPool(CoreId const core, ActorSlotId const ordinary_limit, std::uint32_t const generation_limit) noexcept
    : _core(core)
    , _ordinary_limit(std::min(ordinary_limit, ActorId::MaxOrdinarySlot))
    , _generation_limit(generation_limit == 0 ? 1 : generation_limit)
    // a core the codec cannot carry gets no row at all (empty() refuses every reservation), so these bits never meet one
    , _core_bits(static_cast<std::uint64_t>(core) << 24u) {}

detail::ActorIdPool
detail::ActorIdPool::take() noexcept {
    static_assert(std::is_nothrow_move_constructible_v<ActorIdPool>);
    static_assert(std::is_nothrow_move_assignable_v<ActorIdPool>);
    ActorIdPool result = std::move(*this);
    *this              = ActorIdPool(_core, _ordinary_limit, _generation_limit);
    return result;
}

ActorId
detail::ActorIdPool::reserve_ordinary_grow() {
    // Only reserve_ordinary() calls this half, after observing an empty free list.
    // This is the only operation that changes the bank's size; take() moves the count
    // with the bank and resets both. The count therefore also names the next new slot.
    assert(_free_head == 0 && _ordinary_rows == _ordinary.size());
    const std::size_t slot = _ordinary_rows == 0 ? 1u : _ordinary_rows;
    if (_core >= MaxCores || slot > _ordinary_limit)
        return {};
    if (slot + 1u > _ordinary.capacity()) [[unlikely]]
        allocator::detail::region_reserve(_ordinary, slot + 1u, static_cast<std::size_t>(_ordinary_limit) + 1u);
    if (_ordinary_rows == 0)
        _ordinary.emplace_back(); // invalid row zero, never returned as an actor identity
    _ordinary.emplace_back(Entry{Reserved, 1u});
    _ordinary_rows = slot + 1u;
    // The core passed its bound; 1 <= slot <= _ordinary_limit <= MaxOrdinarySlot.
    // Generation 1 is valid even under the smallest supported generation limit.
    return std::bit_cast<ActorId>((std::uint64_t{1} << 32u) | _core_bits | static_cast<std::uint64_t>(slot));
}

ActorId
detail::ActorIdPool::reserve_service(ServiceId const tag) {
    if (_core >= MaxCores || tag == 0 || tag == std::numeric_limits<ServiceId>::max())
        return ActorId::NotFound;
    if (tag >= _services.size())
        allocator::detail::region_resize(_services, static_cast<std::size_t>(tag) + 1u, std::numeric_limits<ServiceId>::max());
    Entry &item = _services[tag];
    if (item.state >= Retired)
        return ActorId::NotFound;
    item.state = Reserved;
    return ActorId::service_instance(tag, _core, item.generation);
}

detail::ActorIdPool::Entry *
detail::ActorIdPool::service_entry(ActorId const id) noexcept {
    // entry()'s out-of-line half: the full decode, for the service band and anything above the ordinary range.
    // The instance domain is every nonzero slot except the service-band marker and broadcast slot, with a nonzero
    // generation. Retain core, bounds and generation checks for callers supplying stale or foreign identities.
    const ActorSlotId slot       = id.slot();
    const auto        generation = id.generation();
    if (generation == 0 || id.index() != _core || slot == 0 || slot == ActorId::ServiceBand || slot == ActorId::BroadcastSid)
        return nullptr;
    const bool        service = slot > ActorId::ServiceBand;
    auto             &bank    = service ? _services : _ordinary;
    const std::size_t index   = service ? static_cast<ServiceId>(slot) : slot;
    if (index >= bank.size())
        return nullptr;
    Entry &item = bank[index];
    return item.generation == generation ? &item : nullptr;
}

void
detail::ActorIdPool::release_slow(ActorId const id) noexcept {
    // release()'s out-of-line half: a service tag, or an ordinary id this pool holds no row for (a no-op then).
    Entry *const item = entry(id);
    if (item == nullptr || (item->state != Reserved && item->state != Live && item->state != Retiring))
        return;
    if (item->generation >= _generation_limit) {
        item->state = Retired;
        return;
    }
    if (id.is_service()) {
        ++item->generation;
        item->state = 0;
    } else {
        set_free_link(*item, _free_head);
        _free_head = id.raw64() + NextGeneration;
    }
}

void
detail::ActorIdPool::clear_reservations() noexcept {
    for (std::size_t slot = 1; slot < _ordinary.size(); ++slot)
        if (_ordinary[slot].state == Reserved)
            release(ActorId::ordinary(static_cast<ActorSlotId>(slot), _core, _ordinary[slot].generation));
    for (std::size_t tag = 1; tag < _services.size(); ++tag)
        if (_services[tag].state == Reserved)
            release(ActorId::service_instance(static_cast<ServiceId>(tag), _core, _services[tag].generation));
}

bool
detail::ActorIdPool::empty() const noexcept {
    if (_core >= MaxCores || _ordinary_limit == 0)
        return true;
    return _free_head == 0 && !_ordinary.empty() && _ordinary.size() > _ordinary_limit;
}

std::size_t
detail::ActorIdPool::metadata_bytes() const noexcept {
    return (_ordinary.capacity() + _services.capacity()) * sizeof(Entry);
}

std::size_t
detail::ActorIdPool::slot_count() const noexcept {
    return (_ordinary.empty() ? 0 : _ordinary.size() - 1u) + (_services.empty() ? 0 : _services.size() - 1u);
}

std::size_t
detail::ActorIdPool::ordinary_slot_count() const noexcept {
    return _ordinary.size();
}

std::size_t
detail::ActorIdPool::service_slot_count() const noexcept {
    return _services.size();
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
