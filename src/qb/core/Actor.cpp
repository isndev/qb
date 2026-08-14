/**
 * @file qb/core/Actor.cpp
 * @brief Implementation of the Actor class for the QB framework
 *
 * This file contains the implementation of the Actor class which forms the foundation
 * of the actor model in the QB framework. It includes event handling, actor lifecycle
 * management, and inter-actor communication mechanisms.
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

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <qb/core/Actor.h>
#include <qb/core/VirtualCore.h> // also carries Actor's template bodies (was qb/core/Actor.tpp)
#include <qb/io/async/listener.h>

namespace qb {

// ---------------------------------------------------------------------------
// ask() pattern — per-worker-thread correlation registry (Layer 3).
// Strictly mono-thread per VirtualCore: a plain thread_local map, no locks.
// Slots are owned by the awaiter living in the asking coroutine's frame; the
// registry only stores raw pointers keyed by a per-core monotonic id.
// ---------------------------------------------------------------------------
namespace detail {
namespace {
thread_local std::unordered_map<std::uint64_t, ask_slot *> tls_ask_slots;
thread_local std::uint64_t                                 tls_ask_counter = 0;
// Set of event type-ids known to derive from AskEvent (i.e. carry `correlation_id`),
// populated lazily by `qb::ask<E>`. Lets the activation gate recognise an ask reply
// without RTTI and read `correlation_id` at the AskEvent base offset safely.
thread_local std::unordered_set<Event::id_type> tls_ask_types;
} // namespace

std::uint64_t
ask_next_id(qb::ActorId const owner) noexcept {
    // Salt the per-thread counter with the OWNER actor's core index (high 16
    // bits) so correlation ids are globally unique across VirtualCores. Two
    // cores' independent per-thread counters would otherwise produce identical
    // values, and `ask_deliver` matches on (id, owner) only — a cross-core
    // request carrying a colliding id would resolve the receiver's OWN pending
    // slot and hand its `ask_awaiter<E>` an event of the wrong type (type
    // confusion). Encoding the slot-owning core in the id makes a lookup on any
    // other core miss. Low 48 bits = counter (never realistically wrapped);
    // 0 stays reserved for "not an ask".
    const std::uint64_t n   = ++tls_ask_counter;
    const std::uint64_t seq = n ? n : ++tls_ask_counter;
    return (static_cast<std::uint64_t>(owner.index()) << 48) | seq;
}

void
ask_register(std::uint64_t const id, ask_slot *slot) noexcept {
    tls_ask_slots[id] = slot;
}

void
ask_unregister(std::uint64_t const id) noexcept {
    tls_ask_slots.erase(id);
}

bool
ask_deliver(std::uint64_t const id, ActorId const owner, Event &resp) noexcept {
    if (!id)
        return false;
    auto it = tls_ask_slots.find(id);
    if (it == tls_ask_slots.end())
        return false;
    ask_slot *slot = it->second;
    // Only the owning actor may resolve its slot, and only once.
    if (!slot || slot->done || !slot->deliver || !(slot->owner == owner))
        return false;
    slot->deliver(slot->self, resp);
    return true;
}

ev::loop_ref
ask_loop() noexcept {
    return qb::io::async::listener::current.loop();
}

void
ask_register_type(Event::id_type const type) noexcept {
    tls_ask_types.insert(type);
}

bool
ask_try_deliver_reply(Event &ev, ActorId const dest) noexcept {
    // Only correlated-reply types (registered by ask / ask_stream / require) carry a
    // `correlation_id`.
    if (tls_ask_types.find(ev.getID()) == tls_ask_types.end())
        return false;
    // The type-id match proves `ev` derives from CorrelatedEvent (it is the first base), so
    // `correlation_id` lives at the CorrelatedEvent base subobject offset.
    auto &ce = static_cast<CorrelatedEvent &>(ev);
    return ask_deliver(ce.correlation_id, dest, ev);
}
} // namespace detail

Actor::Actor() noexcept
    : _id((assert(VirtualCore::_handler != nullptr
                  && "Actor must be constructed from within a VirtualCore worker thread "
                     "(use Main::core(idx).addActor<T>(...) or addRefActor<T>()), never "
                     "from the main thread or an arbitrary user thread."),
           VirtualCore::_handler->__generate_id__())) {
    registerEvent<KillEvent>(*this);
    registerEvent<SignalEvent>(*this);
    registerEvent<UnregisterCallbackEvent>(*this);
    registerEvent<PingEvent>(*this);
    registerEvent<RequireEvent>(*this); // default: route coroutine discovery/liveness replies
}

Actor::Actor(ActorId const id) noexcept
    : _id(id) {
    assert(VirtualCore::_handler != nullptr
           && "Service actors must be constructed from within their owning VirtualCore "
              "worker thread.");
    registerEvent<KillEvent>(*this);
    registerEvent<SignalEvent>(*this);
    registerEvent<UnregisterCallbackEvent>(*this);
    registerEvent<PingEvent>(*this);
    registerEvent<RequireEvent>(*this); // default: route coroutine discovery/liveness replies
}

Actor::Actor(no_default_events_t) noexcept
    : _id((assert(VirtualCore::_handler != nullptr && "Actor must be constructed from within a VirtualCore worker thread."),
           VirtualCore::_handler->__generate_id__())) {
    // Intentionally no default event registrations — derived class owns its wiring. The minimum for
    // shutdown is SignalEvent, NOT KillEvent — see `qb::no_default_events_t` in Actor.h for why.
}

void
Actor::on(PingEvent const &event) noexcept {
    // type 0 is the wildcard liveness probe (any live actor replies — qb::ping); otherwise the
    // ping is a typed discovery (qb::require / legacy require<>()). Echo the correlation id so the
    // coroutine helpers can match the reply.
    if (event.type == 0 || event.type == id_type)
        send<RequireEvent>(event.source, event.type, event.correlation_id);
}

void
Actor::on(RequireEvent &event) noexcept {
    // Default: deliver the reply to a pending co_await qb::ping / qb::require. A correlation_id of 0
    // (legacy fire-and-forget require<>()) resolves nothing here — override on(RequireEvent&) to use
    // the legacy is<T>() dance.
    (void) resolve_require(event);
}

bool
Actor::resolve_require(RequireEvent &e) const noexcept {
    return qb::detail::ask_deliver(e.correlation_id, id(), e);
}

void
Actor::on(KillEvent const &) noexcept {
    kill();
}

void
Actor::on(SignalEvent const &event) noexcept {
    // Terminal signals only. SIGINT and SIGTERM both mean "stop": SIGTERM is what Docker,
    // Kubernetes and systemd send first, so swallowing it made a registered SIGTERM a no-op —
    // the process stayed alive and the supervisor had to escalate to SIGKILL, losing the very
    // graceful teardown `registerSignal`'s contract promises ("Registered signals will trigger
    // a graceful shutdown of all actors", core/Main.h).
    //
    // Everything else stays NON-terminal on purpose: SIGHUP / SIGUSR1 are the documented
    // "register your own signal" cases (config reload, stats dump). An actor that wants to act
    // on them overrides `on(SignalEvent&)`; killing every actor on a reload signal would be a
    // far worse regression than the bug this closes.
    // Pinned by `SignalShutdown.*` in system/engine/sigterm-shutdown.cpp.
    if (event.signum == SIGINT || event.signum == SIGTERM)
        kill();
}

void
Actor::on(UnregisterCallbackEvent const &) noexcept {
    VirtualCore::_handler->__unregisterCallback(id());
}

uint64_t
Actor::time() const noexcept {
    return VirtualCore::_handler->time();
}

qb::wall_time
Actor::now() const noexcept {
    return qb::wall_from_unix_nanos(static_cast<std::int64_t>(time()));
}

bool
Actor::is_alive() const noexcept {
    return _alive;
}

bool
Actor::is_active() const noexcept {
    return _alive && _activated;
}

Pipe
Actor::getPipe(ActorId const dest) const noexcept {
    return VirtualCore::_handler->getProxyPipe(dest, id());
}

CoreId
Actor::getIndex() const noexcept {
    return VirtualCore::_handler->getIndex();
}

std::string_view
Actor::getName() const noexcept {
    return name;
}

const CoreIdSet &
Actor::getCoreSet() const noexcept {
    return VirtualCore::_handler->getCoreSet();
}

void
Actor::unregisterCallback() const noexcept {
    VirtualCore::_handler->unregisterCallback(id());
}

void
Actor::__resolve_coro_scheduler__() const noexcept {
    // Finding 2.D.4: revalidate the cached scheduler pointer on every spawn by
    // comparing it against the current TLS scheduler. Caching alone is unsafe because
    // the listener-owned scheduler can be torn down and rebuilt (tests calling
    // `listener::reset_coro_scheduler()`, or a core destroyed and re-initialized).
    // Revalidation is essentially free: `current_ptr()` is a plain `thread_local*`
    // load and the comparison fits in one cmp+jne.
    auto *expected = qb::io::async::CoroutineScheduler::current_ptr();
    if (likely(expected != nullptr)) {
        if (unlikely(coro_scheduler_ != expected))
            coro_scheduler_ = expected;
    } else if (unlikely(!coro_scheduler_)) {
        // Extremely rare: spawn called before any TLS scheduler exists on this thread.
        coro_scheduler_ = &qb::io::async::listener::current.coro_scheduler();
    }
    // Finding 2.D.5: debug-only guard against cross-thread spawn. The actor system is
    // strictly mono-thread per VirtualCore; spawning from another thread is UB. We
    // cannot tell which VirtualCore owns this actor here, but we CAN verify a TLS
    // scheduler exists on the caller thread, excluding unrelated std::thread contexts.
    assert(qb::io::async::CoroutineScheduler::current_ptr() != nullptr
           && "Actor::spawn_detached/spawn called from a thread without a coroutine "
              "scheduler — are you calling this from outside the VirtualCore?");
}

void
Actor::__ensure_coro_scope__() const {
    if (!_coro_scope)
        _coro_scope = qb::io::async::cancellation_token{}; // allocate the real token once.
}

void
Actor::__cancel_coro_scope__() const noexcept {
    if (_coro_scope)
        _coro_scope.cancel();
}

bool
Actor::is_actor_alive(ActorId const id) const noexcept {
    return VirtualCore::_handler->isActorAlive(id);
}

void
Actor::kill() const noexcept {
    _alive = false;
    // Cancel-on-kill: wake any scoped coroutine awaiting a cancellation-aware op so it
    // unwinds promptly instead of blocking on a long timeout/I/O. Idempotent + no-op if
    // no scoped coroutine was ever spawned.
    __cancel_coro_scope__();
    VirtualCore::_handler->killActor(id());
}

Actor::EventBuilder::EventBuilder(Pipe const &pipe) noexcept
    : dest_pipe(pipe) {}

Actor::EventBuilder
Actor::to(ActorId const dest) const noexcept {
    return EventBuilder{getPipe(dest)};
}

void
Actor::reply(Event &event) const noexcept {
    if (unlikely(event.dest.is_broadcast())) {
        QB_LOG_WARN(*this << " failed to reply broadcast event");
        return;
    }
    VirtualCore::_handler->reply(event);
}

void
Actor::forward(ActorId const dest, Event &event) const noexcept {
    // Do not overwrite event.source: reply() routes via swap(dest, source); the
    // original sender must remain the logical client (matches Actor.h contract).
    if (unlikely(event.dest.is_broadcast())) {
        QB_LOG_WARN(*this << " failed to forward broadcast event");
        return;
    }
    VirtualCore::_handler->forward(dest, event);
}

// OpenApi : internal future use
void
Actor::send(Event const &event) const noexcept {
    VirtualCore::_handler->send(event);
}

void
Actor::push(Event const &event) const noexcept {
    VirtualCore::_handler->push(event);
}

bool
Actor::try_send(Event const &event) const noexcept {
    return VirtualCore::_handler->try_send(event);
}

uint64_t
CoroContext::time() const noexcept {
    return VirtualCore::_handler->time();
}

Service::Service(ServiceId const sid) noexcept
    : Actor(ActorId(sid, VirtualCore::_handler->getIndex())) {}

namespace detail {

// Declared in VirtualCore.h next to the two spawn wrappers that call it; see the note there for
// why a spawned coroutine's exception has nowhere to go and was being dropped in silence.
//
// TWO CHANNELS, AND THE SECOND IS NOT BELT-AND-BRACES. `QB_LOG_CRIT` is the structured one and
// it is a complete no-op unless the build defines QB_WITH_LOGGING or QB_STDOUT_LOGGING
// (`qb/io.h:262-265`) -- and even when it is live it writes to nanolog, which produces nothing
// until a program calls `qb::io::log::init()`. So on its own it would leave this report missing
// in ordinary builds, which is the very failure being fixed. `qb::io::cerr()` is qb's own
// mutex-guarded stderr (`qb/io.h:163`), always compiled, and is what the engine already uses to
// announce a failed core init (`Main.cpp:445`). Nothing here is on a hot path: reaching this
// function at all means a coroutine body threw.
void
report_unhandled_coroutine_exception(ActorId const owner, char const *const api, std::exception_ptr ep) noexcept {
    const auto emit = [&](char const *const what) {
        QB_LOG_CRIT("Actor(" << owner.index() << '.' << owner.sid() << ") " << api << "() coroutine body let an exception escape: " << what
                             << " -- the frame unwound and the exception was DISCARDED.");
        qb::io::cerr() << "CRITICAL: qb Actor(" << owner.index() << '.' << owner.sid() << ") " << api
                       << "() coroutine body let an exception escape, and it was DISCARDED: " << what
                       << " -- a spawned coroutine has no caller to receive it. Catch it in the body and report through an event." << std::endl;
    };
    // noexcept: this runs inside a catch handler while a coroutine frame unwinds, so it must not
    // itself throw. The outer try covers `emit` -- including a throw out of the inner HANDLER,
    // which the inner catch clauses cannot take.
    try {
        try {
            if (ep)
                std::rethrow_exception(ep);
            emit("<no exception object>");
        } catch (std::exception const &e) {
            // Emit from INSIDE the handler, and never hold `e.what()` past it. `rethrow_exception`
            // is not required to rethrow the same object: libstdc++ and libc++ bump a refcount, but
            // MSVC throws a fresh COPY, and `std::exception`'s copy constructor duplicates the
            // message buffer there (`__std_exception_copy`). So a `char const *` carried out of this
            // block would dangle on Windows only -- the platform with no CI (see verify-windows.ps1).
            emit(e.what());
        } catch (...) {
            emit("<exception not derived from std::exception>");
        }
    } catch (...) {
        // Reporting must never become the failure. There is nothing left to try.
    }
}

} // namespace detail
} // namespace qb

#ifdef QB_WITH_LOGGING
qb::io::log::stream &
qb::operator<<(qb::io::log::stream &os, qb::Actor const &actor) {
    os << "Actor[" << actor.getName() << "](" << actor.id().index() << "." << actor.id().sid() << ")";
    return os;
}
#endif

std::ostream &
qb::operator<<(std::ostream &os, qb::Actor const &actor) {
    os << "Actor[" << actor.getName() << "](" << actor.id().index() << "." << actor.id().sid() << ")";
    return os;
}