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

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <unordered_set>
#include <qb/core/Actor.h>
#include <qb/core/VirtualCore.h> // also carries Actor's template bodies (was qb/core/Actor.tpp)
#include <qb/io/async/listener.h>

namespace qb {

// ---------------------------------------------------------------------------
// ask() pattern — per-worker-thread correlation registry (Layer 3).
// Strictly mono-thread per VirtualCore: a plain thread_local table, no locks.
// Slots are owned by the awaiter living in the asking coroutine's frame; the
// registry only stores raw pointers, in a slot table the correlation id indexes.
// ---------------------------------------------------------------------------
namespace detail {
namespace {
/**
 * @brief The pending-ask registry: a slot table the correlation id INDEXES, one per worker thread.
 * @details Every `qb::ask` takes an entry before it sends and gives it back when it resumes, so
 *          this is the one container on the ask hot path. It used to be a hash table keyed by a
 *          per-core counter — a multiply and a probe per insert, per lookup and per erase, plus
 *          the backward shift an erase pays to leave no tombstone — and `perf` on
 *          savina/bank-transaction (1c, g++-14) put it at 10–12 % of the core. Nothing about the
 *          id needed hashing: the registry hands the id out itself, so it can make the id SAY
 *          where the entry is. A correlation id is
 *
 *              [ core index : 16 ][ generation : 26 ][ slot index : 22 ]
 *
 *          — the low bits index this table directly, the generation is the entry's reuse count
 *          at the time it was taken (a stale id, one whose entry has since been released, misses
 *          on the compare), and the high 16 bits name the owning core, as before, so an id can
 *          never resolve an entry of another core's table. Take = pop the free list, look-up =
 *          index + compare, release = push; no multiply, no probe, no allocation once warm (the
 *          table doubles when the free list runs dry and never shrinks). The free list is FIFO,
 *          not LIFO, on purpose: it spreads reuse across every free entry, so a generation
 *          advances once per `free_count` asks rather than once per ask and a 26-bit generation
 *          wraps after ~2^26 × 63 asks on a warm table — and a wrapped stale id would still have
 *          to name its own owner. Id 0 stays reserved ("not an ask"): generations start at 1.
 *          Strictly mono-thread, like everything else in here.
 */
class ask_table {
public:
    static constexpr unsigned      slot_bits = 22;
    static constexpr unsigned      gen_bits  = 26;
    static constexpr std::uint64_t slot_mask = (std::uint64_t{1} << slot_bits) - 1;
    static constexpr std::uint32_t gen_max   = (std::uint32_t{1} << gen_bits) - 1;

    ask_table()                             = default;
    ask_table(const ask_table &)            = delete;
    ask_table &operator=(const ask_table &) = delete;
    ~ask_table() {
        std::free(_tab);
    }

    /// Take a free entry for an ask owned by an actor of core `core`; returns its correlation id.
    [[nodiscard]] std::uint64_t
    take(std::uint16_t const core) {
        if (_free_head == none)
            grow();
        const std::uint32_t i = _free_head;
        entry              &e = _tab[i];
        _free_head            = e.next;
        if (_free_head == none)
            _free_tail = none;
        e.next = busy;
        e.slot = nullptr;
        return (static_cast<std::uint64_t>(core) << 48) | (static_cast<std::uint64_t>(e.gen) << slot_bits) | i;
    }

    /// Bind the awaiter's slot to the entry `id` names. The id must be taken and not released.
    void
    bind(std::uint64_t const id, ask_slot *slot) noexcept {
        entry *const e = live(id);
        assert(e && "ask_register: id not taken from this core's registry (or already released)");
        if (e)
            e->slot = slot;
    }

    [[nodiscard]] ask_slot *
    find(std::uint64_t const id) const noexcept {
        const entry *const e = live(id);
        return e ? e->slot : nullptr;
    }

    /// Give the entry back. Idempotent: a released (or never taken) id is a no-op.
    void
    release(std::uint64_t const id) noexcept {
        entry *const e = live(id);
        if (!e)
            return;
        e->slot = nullptr;
        e->gen  = e->gen == gen_max ? 1 : e->gen + 1; // a stale id misses from this point on
        push_free(static_cast<std::uint32_t>(e - _tab));
    }

private:
    struct entry {
        ask_slot     *slot = nullptr;
        std::uint32_t gen  = 1;
        std::uint32_t next = 0; ///< free-list link while free, `busy` while taken
    };
    static constexpr std::uint32_t none = ~std::uint32_t{0};
    static constexpr std::uint32_t busy = none - 1;

    [[nodiscard]] entry *
    live(std::uint64_t const id) const noexcept {
        const std::uint64_t i = id & slot_mask;
        if (i >= _cap)
            return nullptr;
        entry *const e = _tab + i;
        if (e->next != busy || e->gen != static_cast<std::uint32_t>((id >> slot_bits) & gen_max))
            return nullptr;
        return e;
    }

    void
    push_free(std::uint32_t const i) noexcept {
        _tab[i].next = none;
        if (_free_tail == none)
            _free_head = i;
        else
            _tab[_free_tail].next = i;
        _free_tail = i;
    }

    void
    grow() {
        // 2^22 entries taken on ONE core is 2^22 suspended asks; the registry cannot
        // degrade past that (an id that names no entry is a lost reply), so it stops.
        if (_cap > slot_mask)
            std::abort();
        const std::uint32_t old_cap = _cap;
        _cap                        = _cap ? _cap * 2 : 64;
        auto *const t               = static_cast<entry *>(std::realloc(_tab, _cap * sizeof(entry)));
        if (!t)
            std::abort();
        _tab = t;
        for (std::uint32_t i = old_cap; i < _cap; ++i) {
            _tab[i] = entry{};
            push_free(i);
        }
    }

    entry        *_tab       = nullptr;
    std::uint32_t _cap       = 0;
    std::uint32_t _free_head = none;
    std::uint32_t _free_tail = none;
};

thread_local ask_table tls_ask_slots;
// Set of event type-ids known to derive from AskEvent (i.e. carry `correlation_id`),
// populated lazily by `qb::ask<E>`. Lets the activation gate recognise an ask reply
// without RTTI and read `correlation_id` at the AskEvent base offset safely.
thread_local std::unordered_set<Event::id_type> tls_ask_types;
} // namespace

std::uint64_t
ask_next_id(qb::ActorId const owner) noexcept {
    // The id names the OWNER actor's core index in its high 16 bits, so correlation
    // ids are globally unique across VirtualCores: two cores' independent registries
    // would otherwise hand out identical (generation, slot) pairs, and `ask_deliver`
    // matches on (id, owner) only — a cross-core request carrying a colliding id
    // would resolve the receiver's OWN pending slot and hand its `ask_awaiter<E>` an
    // event of the wrong type (type confusion). `ask_deliver` refuses an id whose
    // core is not the delivering actor's before it even indexes the table. The low
    // 48 bits locate the entry (see ask_table); 0 stays reserved for "not an ask".
    // The entry is TAKEN here, before the request is sent: `ask_register` binds the
    // awaiter to it once the coroutine suspends, `ask_unregister` gives it back.
    return tls_ask_slots.take(owner.index());
}

void
ask_register(std::uint64_t const id, ask_slot *slot) noexcept {
    tls_ask_slots.bind(id, slot);
}

void
ask_unregister(std::uint64_t const id) noexcept {
    tls_ask_slots.release(id);
}

bool
ask_deliver(std::uint64_t const id, ActorId const owner, Event &resp) noexcept {
    // The core in the id is the table the entry lives in; anything else is a miss by
    // construction (as is id 0, "not an ask", whose entry can never be taken).
    if ((id >> 48) != owner.index())
        return false;
    ask_slot *slot = tls_ask_slots.find(id);
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

// is_alive() is defined in-class: the router's dispatch trampoline calls it on every event
// from the user's TU, where an out-of-line definition is a call into the archive.

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
    // Idempotent: the second kill() of one actor has nothing left to do, and returning here
    // is what lets the core keep its kill queue as a plain vector -- one entry per actor,
    // no set to deduplicate against. `_alive` has no other writer.
    if (!_alive)
        return;
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
// announce a failed core init (`Main.cpp:471`). Nothing here is on a hot path: reaching this
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