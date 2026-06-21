/**
 * @file qb/core/src/Actor.cpp
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
#include <qb/core/Actor.h>
#include <qb/core/Actor.tpp>
#include <qb/core/VirtualCore.h>
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
} // namespace

std::uint64_t
ask_next_id() noexcept {
    // 0 is reserved for "not an ask"; skip it on the (astronomically rare) wrap.
    return ++tls_ask_counter ? tls_ask_counter : ++tls_ask_counter;
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
}

Actor::Actor(no_default_events_t) noexcept
    : _id((assert(VirtualCore::_handler != nullptr && "Actor must be constructed from within a VirtualCore worker thread."),
           VirtualCore::_handler->__generate_id__())) {
    // Intentionally no default event registrations — derived class owns its wiring.
    // Register at minimum KillEvent in onInit() for graceful shutdown.
}

void
Actor::on(PingEvent const &event) noexcept {
    if (event.type == id_type)
        send<RequireEvent>(event.source, event.type, ActorStatus::Alive);
}

void
Actor::on(KillEvent const &) noexcept {
    kill();
}

void
Actor::on(SignalEvent const &event) noexcept {
    if (event.signum == SIGINT)
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

bool
Actor::is_alive() const noexcept {
    return _alive;
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
        LOG_WARN(*this << " failed to reply broadcast event");
        return;
    }
    VirtualCore::_handler->reply(event);
}

void
Actor::forward(ActorId const dest, Event &event) const noexcept {
    // Do not overwrite event.source: reply() routes via swap(dest, source); the
    // original sender must remain the logical client (matches Actor.h contract).
    if (unlikely(event.dest.is_broadcast())) {
        LOG_WARN(*this << " failed to forward broadcast event");
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