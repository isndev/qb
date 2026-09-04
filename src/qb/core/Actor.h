/**
 * @file qb/core/Actor.h
 * @brief Actor base class and core actor model implementation
 *
 * This file defines the core Actor class which serves as the base class for all actors
 * in the QB Actor Framework. It implements the fundamental actor model concepts
 * including message passing via events, lifecycle management, and actor identification.
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

#ifndef QB_ACTOR_H
#define QB_ACTOR_H
#include <algorithm>
#include <any>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>
// include from qb
#include <qb/system/container/unordered_map.h>
#include <qb/utility/abi.h> /* QB_ABI_ANCHOR */
#include <qb/utility/nocopy.h>
#include <qb/utility/type_traits.h>
#include <qb/io/async/coroutine.h>
#include "Event.h"
#include "ICallback.h"
#include "Pipe.h"

#ifdef __GNUG__
#include <cstdlib>
#include <cxxabi.h>
#include <memory>
#endif

namespace qb {

class VirtualCore;
class ActorProxy;
class Service;
class Event;
class ICallback;
class Actor;             // Forward declaration for concepts
class ScopedCoroContext; // Forward for Actor::context() (defined after the Actor body)
template <typename _Actor>
class ActorHandle; // Forward for Actor::addRefActor (RefActorHandle is an alias of this)

/**
 * @brief Tag type used to opt out of registering the five default event handlers
 *        (KillEvent, SignalEvent, UnregisterCallbackEvent, PingEvent, RequireEvent) on construction.
 * @ingroup Actor
 * @details
 * Pass `qb::no_default_events` to the protected `Actor` constructor for a lightweight actor that registers no system
 * events — meant for pools of short-lived actors where the router bookkeeping is a measurable overhead.
 * @warning **Register `qb::SignalEvent`, not `qb::KillEvent`.** `Main::stop()`, SIGINT and SIGTERM reach an actor ONLY
 * as a `SignalEvent` (synthesised per core, `VirtualCore.cpp:677`); nothing in the engine ever sends a `KillEvent`.
 * MEASURED: registering only `KillEvent` — what this note used to advise — leaves `Main::join()` hanging forever.
 */
struct no_default_events_t {
    explicit constexpr no_default_events_t() noexcept = default;
};

/** @brief Inline constexpr tag value; see `qb::no_default_events_t`. @ingroup Actor */
QB_ABI_ANCHOR inline constexpr no_default_events_t no_default_events{};

/**
 * @brief Concept for event types that derive from qb::Event
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept event_type = std::is_base_of_v<Event, T>;

/**
 * @brief Concept for actor types that derive from qb::Actor
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept actor_type = std::is_base_of_v<Actor, T>;

/**
 * @brief Concept for service types that derive from qb::Service
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept service_type = std::is_base_of_v<Service, T>;

/**
 * @brief Concept for types that implement ICallback interface
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept callback_type = std::is_base_of_v<ICallback, T>;

/**
 * @brief Concept for event types that are trivially destructible (for unordered send)
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept trivial_event = event_type<T> && std::is_trivially_destructible_v<T>;

/**
 * @brief Concept for QOS0 event types (fire-and-forget, unordered delivery)
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept event_qos0_type = std::is_base_of_v<EventQOS0, T>;

// `service_event_type` was declared HERE through 2.6.0; in 3.0 it moved to Event.h (see the
// banner at the tail of that header). Pipe.h, which this header includes at :50 -- long
// before this point -- needs the concept for its own template bodies and cannot reach a
// declaration made down here. It still arrives through the `#include "Event.h"` at :48,
// unchanged, so no consumer has to write anything differently. `event_qos0_type` above stays
// put: only VirtualCore's bodies use it, and they see a complete Actor.h. This block holds the
// LINE COUNT -- 87 `Actor.h:NNN` citations anchor below it and a plain deletion moves them all.

/**
 * @class Actor
 * @brief Base class for all actors in the qb framework
 * @ingroup Actor
 *
 * The Actor class is the fundamental unit of computation in the qb framework.
 * Actors communicate exclusively by passing messages (events) to each other,
 * which are processed by event handlers. This messaging pattern ensures
 * isolation and prevents shared mutable state, making the system more robust
 * for concurrent and distributed applications.
 *
 * Each actor:
 * - Has a unique identity (ActorId)
 * - Processes events asynchronously
 * - Can send events to other actors
 * - Manages its own internal state
 * - Has a well-defined lifecycle
 *
 * Example usage:
 * @code
 * class MyActor : public qb::Actor {
 * private:
 *     int counter = 0;
 *
 * public:
 *     // Custom event type
 *     struct IncrementEvent : qb::Event {
 *         int amount;
 *         IncrementEvent(int amt) : amount(amt) {}
 *     };
 *
 *     qb::io::async::task<bool> onInit() override {
 *         // Register event handlers (an async onInit may also co_await — see onInit() docs)
 *         registerEvent<IncrementEvent>(*this);
 *         registerEvent<qb::KillEvent>(*this);
 *         QB_LOG_INFO("MyActor initialized with ID: " << id());
 *         co_return true;
 *     }
 *
 *     void on(const IncrementEvent& event) {
 *         counter += event.amount;
 *         QB_LOG_INFO("Counter updated to: " << counter);
 *     }
 *
 *     void on(const qb::KillEvent& event) {
 *         QB_LOG_INFO("MyActor shutting down...");
 *         kill();
 *     }
 * };
 *
 * // In a VirtualCore or Main context:
 * auto actor_id = addActor<MyActor>();
 * to(actor_id).push<MyActor::IncrementEvent>(5);
 * @endcode
 */
class Actor : nocopy {
    friend class VirtualCore;
    friend class ActorProxy;
    friend class Service;
    friend struct std::default_delete<Actor>;

    const char *name = "unnamed";
    ActorId     _id;
    /**
     * @brief Liveness flag — drives actor destruction in the workflow loop.
     *
     * @note Thread-ownership model (finding 2.9)
     *       - This flag is **single-writer / single-reader**: both sides
     *         always run on the exact same `VirtualCore` worker thread (the
     *         one that hosts this actor). `VirtualCore` is strictly
     *         thread-affine — an actor never migrates between cores, and
     *         remote senders only enqueue `KillEvent`s into the core's
     *         mailbox; they never flip this flag directly.
     *       - Therefore no atomicity, no memory fence and no lock is
     *         required: the owning core reads/writes `_alive` under the
     *         sequential semantics of a single thread, exactly like any
     *         other member of `Actor`.
     *       - `mutable` lets `kill() const` flip it while preserving a
     *         *const-correct* public API (handlers commonly receive events
     *         by value/ref under a const `this`).
     *       - External observers **must not** read this flag. They should
     *         use `qb::RefActorHandle<T>` (finding 2.9) which resolves
     *         liveness through `VirtualCore::findActor<T>()` on the
     *         owning core.
     */
    mutable bool _alive = true;
    /**
     * @brief Activation flag — `false` only while a *suspended* `onInit()` is in flight.
     *
     * @details An actor is *active* once its `onInit()` coroutine has completed
     *          successfully. The common case (no `co_await` in `onInit`) completes
     *          synchronously, so `_activated` is never observed `false`. It is flipped
     *          `false` by the owning core only when an `onInit()` actually suspends, and
     *          back `true` when it resumes to completion. Same single-writer /
     *          single-reader thread-affinity contract as `_alive`. `is_active()` ==
     *          `_alive && _activated` is the phase oracle, and it has exactly TWO
     *          consumers: `VirtualCore::findActor<T>()` (hence every `ActorHandle`
     *          accessor) and `VirtualCore::isActorAlive()` (hence `is_actor_alive()`).
     *          `getService<T>()` is deliberately NOT one of them, and the inbound-event
     *          dispatch gate keys off `VirtualCore::_activating` membership rather than
     *          this flag. The full inventory — who is withheld during the Activating
     *          window, who is not, and why — is the table on `is_active()` below.
     */
    mutable bool  _activated = true;
    std::uint32_t id_type    = 0u;

    /**
     * @brief Check if this actor is of a specific type
     *
     * @tparam _Type The type to check against
     * @return true if the actor is of the specified type
     * @return false otherwise
     * @private
     */
    template <typename _Type>
    bool require_type() const noexcept;

    /**
     * @brief Constructor with specified actor ID
     *
     * @param id The actor ID to assign
     * @private
     */
    explicit Actor(ActorId id) noexcept;

protected:
    /**
     * @brief Register a service index
     *
     * @tparam Tag Type tag for the service; must be a **complete** type (reaches `typeid(Tag)`)
     * @return ServiceId The service ID
     * @private
     */
    template <typename Tag>
    static ServiceId registerIndex() noexcept;

    /**
     * @name Construction/Destruction
     * @{
     */

    /**
     * @brief Default constructor
     *
     * Creates an actor with a default (invalid) ID. The actual ID will be
     * assigned when the actor is registered with a VirtualCore.
     */
    Actor() noexcept;

    /**
     * @brief Lightweight constructor that skips the default event registrations.
     * @param tag `qb::no_default_events` — selects this overload.
     * @details
     * Unlike the default constructor, this overload does **not** register handlers for
     * `KillEvent`, `SignalEvent`, `UnregisterCallbackEvent`, `PingEvent`, or `RequireEvent`.
     * The derived class registers, in `onInit()`, every system event it expects. For shutdown that means
     * **`qb::SignalEvent`** — `Main::stop()` and the terminal signals arrive only as one — plus `qb::KillEvent` if a
     * peer will kill this actor by pushing one. See the @warning on `qb::no_default_events_t`.
     *
     * Intended for high-throughput scenarios where actors are short-lived and the
     * five default subscriptions per actor become measurable overhead.
     */
    explicit Actor(no_default_events_t tag) noexcept;

    /**
     * @brief Virtual destructor
     *
     * Ensures proper cleanup of derived actor classes.
     * @note Called after the actor logic has completed and it has been removed from the engine.
     */
    virtual ~Actor() noexcept = default;

    /**
     * @brief Asynchronous initialization callback, called once after construction and ID assignment.
     *
     * Invoked when the actor is added to the system, before it processes any business
     * events. Override it to register event handlers and set up actor state. Because it
     * is a coroutine (`qb::io::async::task<bool>`) it **may `co_await`** — sleep, await
     * I/O, or query a peer — and the owning core keeps serving its other actors while this
     * one's init is in flight.
     *
     * While `onInit()` is suspended the actor is *Activating*: inbound **unicast business
     * events are stashed** and replayed (FIFO) once it becomes active; broadcasts (incl.
     * `KillEvent`) still pass through, and a `co_await` that never completes is bounded by
     * the activation deadline. Use `context()` to obtain a cancellation-aware context
     * (`co_await context().sleep(...)`), so a kill during init unwinds the coroutine cleanly.
     *
     * @return `co_return true`  → initialization succeeded; the actor becomes active.
     * @return `co_return false` → initialization failed; the actor is killed and removed
     *         from the engine. An uncaught exception is also treated as failure.
     * @details Crucial for `registerEvent<EventType>(*this)` calls. An init that needs no
     *          `co_await` simply `co_return`s — it completes synchronously on the first
     *          resume and never pays the suspended-activation machinery.
     * Example:
     * @code
     * qb::io::async::task<bool> onInit() override {
     *   registerEvent<CustomEvent>(*this);
     *   registerEvent<qb::KillEvent>(*this);     // graceful shutdown
     *
     *   _my_resource = std::make_unique<MyResource>();
     *   if (!_my_resource) {
     *     QB_LOG_CRIT(*this << " failed to allocate MyResource");
     *     co_return false;                       // initialization failed
     *   }
     *
     *   co_await context().sleep(std::chrono::milliseconds{1}); // optional async work
     *   co_return true;                          // initialization successful
     * }
     * @endcode
     */
    virtual qb::io::async::task<bool>
    onInit() {
        co_return true;
    }

public:
    /**
     * @brief Terminate this actor and mark it for removal from the system.
     *
     * Marks the actor for removal from the system. After calling this method,
     * the actor will no longer receive new events (though it may process events already in its queue)
     * and will be cleaned up by the framework during the next appropriate cycle.
     *
     * This method is typically called from within an event handler (e.g., `on(qb::KillEvent&)`)
     * when the actor decides to terminate itself, or it can be triggered by sending a `KillEvent`
     * to the actor.
     * @note This method only flags the actor for termination; the actual destruction
     *       and `~Actor()` call occur later, managed by the `VirtualCore`.
     */
    void kill() const noexcept;

    /**
     * @}
     */

public:
    /**
     * @name Built-in Event Handlers
     * @{
     */

    /**
     * @brief Handler for KillEvent.
     *
     * Default handler for the KillEvent which terminates the actor by calling `this->kill()`.
     * Derived classes can override this handler to perform cleanup actions
     * before termination, but should typically call `Actor::kill()` or `this->kill()`
     * at the end of their implementation to ensure proper termination.
     *
     * @param event The received kill event (often unused in overrides, but available).
     * @note **Do not write `override`.** This handler is not virtual — event dispatch here is
     *       static, resolved by the router against the most-derived overload set, so a derived
     *       `on(qb::KillEvent const &)` *hides* this one rather than overriding it. Spelling
     *       `override` is `error: only virtual member functions can be marked 'override'`.
     *       The same applies to every other built-in handler below.
     * Example of overriding:
     * @code
     * void on(qb::KillEvent const &event) {
     *   QB_LOG_INFO("Actor " << id() << " cleaning up before termination...");
     *   // Perform cleanup: close connections, release resources not handled by RAII, etc.
     *   closeConnections();
     *   releaseResources();
     *
     *   // Finally, ensure the actor is marked for termination
     *   Actor::kill(); // Or just kill();
     * }
     * @endcode
     */
    void on(KillEvent const &event) noexcept;

    /**
     * @brief Handler for SignalEvent.
     *
     * `SIGINT` and `SIGTERM` are terminal: for either one the default handler calls `kill()`,
     * so a Ctrl-C or a container stop unwinds every actor. `Main::start()` installs both.
     *
     * Every other signal is delivered but is **not** terminal — the default handler ignores
     * it. A signal you add with `qb::Main::registerSignal()` (`SIGHUP`, `SIGUSR1`, ...) arrives
     * here for a config reload or a stats dump; override this handler to act on it, and call
     * `kill()` there if that signal should stop the actor too.
     *
     * @param event The received signal event, containing `event.signum`.
     * Example of overriding:
     * @code
     * void on(qb::SignalEvent const &event) {
     *   if (event.signum == SIGUSR1) {
     *     QB_LOG_INFO("Actor " << id() << " received SIGUSR1, reloading configuration.");
     *     reloadConfig(); // stays alive: SIGUSR1 is not terminal
     *   } else {
     *     QB_LOG_INFO("Actor " << id() << " shutting down on signal " << event.signum);
     *     flushPendingWork();
     *     Actor::on(event); // kills the actor on SIGINT / SIGTERM, no-op otherwise
     *   }
     * }
     * @endcode
     */
    void on(SignalEvent const &event) noexcept;

    /**
     * @brief Handler for UnregisterCallbackEvent.
     *
     * This handler unregisters a previously registered callback for this actor.
     * It should generally not be overridden by derived classes as its behavior is fixed.
     *
     * @param event The received unregister callback event.
     * @note This event is usually sent internally when `unregisterCallback()` is called.
     */
    void on(UnregisterCallbackEvent const &event) noexcept;

    /**
     * @brief Handler for PingEvent.
     *
     * Responds to ping requests, primarily used for actor alive checks, diagnostics,
     * and by the `require<T>()` mechanism for actor discovery. The default implementation
     * sends a `RequireEvent` back to the source of the `PingEvent` if the ping type matches.
     *
     * @param event The received ping event, containing `event.type` to match against.
     * @note Derived classes typically do not need to override this unless they have
     *       very specific custom ping/discovery logic.
     */
    void on(PingEvent const &event) noexcept;

    /**
     * @brief Default handler for `RequireEvent` (auto-registered, like `KillEvent`/`PingEvent`).
     * @details Routes a discovery/liveness reply to its pending `co_await qb::ping` / `qb::require`
     *          via `resolve_require` — so those work with **no boilerplate**. Override only for the
     *          legacy fire-and-forget `require<...>()` + `is<T>()` dance; if you do and also use the
     *          coroutine form, call `resolve_require(e)` first (mirrors `resolve_ask`).
     */
    void on(RequireEvent &event) noexcept;

    /**
     * @brief Route a `RequireEvent` reply to its pending coroutine discovery.
     * @param e The received reply.
     * @return `true` if it resolved a pending `ping`/`require` of this actor; `false` otherwise
     *         (legacy `correlation_id == 0`, or not ours).
     */
    [[nodiscard]] bool resolve_require(RequireEvent &e) const noexcept;

    /**
     * @}
     */

public:
    /**
     * @class EventBuilder
     * @brief Helper class for building and sending events to actors
     * @ingroup EventCore
     *
     * This class simplifies the process of sending multiple events to a target
     * actor. It provides a fluent interface for chaining event sends, ensuring
     * that events are delivered in the order they are pushed.
     */
    class EventBuilder {
        friend class Actor;
        Pipe dest_pipe;

        /**
         * @brief Construct a new EventBuilder for the given pipe
         *
         * @param pipe The destination pipe to send events to
         */
        explicit EventBuilder(Pipe const &pipe) noexcept;

    public:
        EventBuilder()                                 = delete;
        EventBuilder(EventBuilder const &rhs) noexcept = default;

        /**
         * @brief Send a new event to the target actor
         *
         * Creates and sends an event of the specified type to the target actor.
         * The event is constructed with the provided arguments and will be
         * delivered in the order it was pushed.
         *
         * Example:
         * @code
         * // Send multiple events to an actor
         * to(targetActorId)
         *   .push<ReadyEvent>()
         *   .push<DataEvent>(buffer, size)
         *   .push<CompleteEvent>(status);
         * @endcode
         *
         * @tparam _Event The type of event to create and send
         * @tparam Args Types of arguments to forward to the event constructor
         * @param args Arguments to forward to the event constructor
         * @return Reference to this EventBuilder for method chaining
         */
        template <event_type _Event, typename... _Args>
        EventBuilder &push(_Args &&...args) noexcept;
    };

    /**
     * @name Public Accessors
     * @{
     */

    /**
     * Get ActorId
     * @return ActorId of this actor. This ID is unique within the QB system.
     */
    [[nodiscard]] ActorId
    id() const noexcept {
        return _id;
    }

    /**
     * Get core index
     * @return CoreId (unsigned short) indicating the VirtualCore where this actor is running.
     */
    [[nodiscard]] CoreId getIndex() const noexcept;

    /**
     * Get derived class name.
     * @return A `std::string_view` of this actor's demangled class name.
     * @note The name is determined at compile time via `typeid`.
     */
    [[nodiscard]] std::string_view getName() const noexcept;

    /**
     * @brief Get the set of cores that this actor's VirtualCore can communicate with.
     * @return Const reference to a `CoreIdSet` representing connected cores.
     * @details This reflects the `CoreSet` the `VirtualCore` was initialized with.
     */
    [[nodiscard]] const CoreIdSet &getCoreSet() const noexcept;

    /**
     * @brief Get current time from the VirtualCore's perspective (nanoseconds since epoch).
     * @return `uint64_t` timestamp in nanoseconds.
     * @details This value is optimized and cached/updated by the `VirtualCore` at the beginning of each processing loop. Thus,
     * multiple calls in one event handler or `on(qb::LoopEvent const&)` invocation return the *same* timestamp (equal to
     * `qb::LoopEvent::now`).
     * @code
     * // ...
     * auto t1 = time();
     * // ... some heavy calculation ...
     * assert(t1 == time()); // true - will not assert within the same event handler execution
     * @endcode
     * @note This time is primarily for relative measurements or logging within an actor's turn.
     * @note For a continuously updating, high-precision timestamp, use `qb::unix_nanos(qb::wall_now())` from `<qb/system/time.h>`.
     * @note Inside `onInit()` there is no pass yet, so the value is the instant the owning `VirtualCore` was CONSTRUCTED
     * (`VirtualCore.h:377` seeds it); through 3.0.0 that field was still 0 there, so `onInit()` read a zero timestamp.
     */
    [[nodiscard]] uint64_t time() const noexcept;

    /**
     * @brief Typed wall-clock instant for the current turn — the `std::chrono` view of `time()`.
     * @return `qb::wall_time` (a `system_clock` time point) equal to `time()` nanoseconds since epoch.
     * @details Cached per loop iteration just like `time()` (stable within one handler). Prefer this
     *          over the raw `uint64_t` when working with the `std::chrono` time vocabulary.
     */
    [[nodiscard]] qb::wall_time now() const noexcept;

    /**
     * @private
     */
    template <typename T>
    [[nodiscard]] static ActorId getServiceId(CoreId index) noexcept;

    /**
     * @brief Get direct access to a `ServiceActor` on the **same** core.
     * @return Pointer to the service, or `nullptr` if no service of that type is registered
     *         on this core.
     * @details **Not phase-gated, and that is deliberate.** Unlike `ActorHandle::get()` /
     *          `VirtualCore::findActor()`, this hands back the pointer even while the
     *          service's own async `onInit()` is still in flight (*Activating*), and even
     *          after it has been `kill()`ed but not yet reaped — it consults neither
     *          `is_active()` nor `is_alive()`. That is what makes the common bootstrap
     *          pattern work: a service legitimately looks itself, or a peer service, up
     *          from inside its own `onInit()`, where it is mid-init by definition.
     *          The cost is on you: the service you get back may not have finished
     *          initializing. If you need that guarantee, ask it (`push` an event, or
     *          `co_await qb::ask(...)`) instead of touching its state. See the inventory
     *          table on `is_active()`.
     */
    template <typename _ServiceActor>
    [[nodiscard]] _ServiceActor *getService() const noexcept;

    // void setCoreLowLatency(bool state) const noexcept;

    /**
     * @brief Check if Actor is alive and processing events.
     * @return true if Actor is alive (i.e., `kill()` has not been effectively processed yet), false otherwise.
     * @details An actor is considered alive until its `kill()` method has been called AND the
     *          `VirtualCore` has processed its removal. It might still process events in its queue
     *          after `kill()` is called but before it's fully removed.
     */
    [[nodiscard]] bool
    is_alive() const noexcept {
        return _alive;
    }

    /**
     * @brief Check if the actor is alive **and** fully activated.
     * @return `true` iff the actor is alive and its `onInit()` has completed successfully.
     * @details Equivalent to `is_alive() && ` *activated*. Differs from `is_alive()` only
     *          during the brief window in which an `onInit()` that performed a `co_await`
     *          is still suspended (the *Activating* phase).
     *
     * ### The Activating window — who is withheld, and who is not
     *
     * One mechanism, several surfaces, and they deliberately do **not** all behave the same.
     * This table is the inventory; read it before assuming a lookup is phase-aware.
     *
     * | Surface | Consults | Behaviour while the target is *Activating* |
     * |---|---|---|
     * | `VirtualCore::findActor<T>(id)` | `is_active()` | **withheld** — `nullptr` |
     * | `ActorHandle<T>::get()` / `operator->` / `operator*` / `ready()` / `operator bool` | `findActor` | **withheld** — `nullptr` / `false` |
     * | `is_actor_alive(id)` (→ `VirtualCore::isActorAlive`) | `is_active()` | **withheld** — `false` |
     * | `is_active()` | `_alive && _activated` | `false` |
     * | `is_alive()` | `_alive` only | `true` — it is not a phase check |
     * | `getService<T>()` (→ `VirtualCore::getService`) | *nothing* | **handed out**, by design |
     * | inbound-event dispatch gate (`VirtualCore::__receive_events__`) | `VirtualCore::_activating` membership | **deferred**, not withheld |
     * | `ActorHandle<T>::id()`, `push` / `send` / `broadcast` / `to()` | *nothing* | usable immediately |
     *
     * Three consequences worth stating outright:
     *
     * - **"Can I look this up from inside `onInit()`?"** `getService<T>()` — yes; that
     *   bootstrap pattern is exactly why it is not gated. `ActorHandle::get()` — no: it
     *   returns `nullptr` for a child that is itself still Activating. Either wait, with
     *   `co_await handle.ready_async(context())`, or just `push` to `handle.id()` and let
     *   the dispatch gate hold the event for you.
     * - **`getService<T>()` is not gated on `is_alive()` either**, so it can hand back a
     *   service that has been `kill()`ed but not yet reaped. `findActor` / `isActorAlive`
     *   are the only lookups that filter either condition, and they filter on `is_active()`
     *   — never on `is_alive()` alone.
     * - **The dispatch gate defers; it does not drop.** A unicast business event addressed
     *   to an Activating actor is byte-copied into that actor's FIFO stash and replayed in
     *   order once it activates. Three things bypass the gate: broadcasts, any `KillEvent`
     *   (so an Activating actor stays killable), and the reply to a `qb::ask` the actor
     *   itself issued from inside `onInit()` — stashing that would deadlock the init on its
     *   own reply.
     *
     * A `false` from any of these is therefore not, on its own, evidence that the actor is
     * gone; over a `co_await` in a peer's `onInit()` it may simply be early.
     */
    [[nodiscard]] bool is_active() const noexcept;

    /**
     * @}
     */

    /**
     * @name Public Member Functions
     * This part describes how to manage Actor loop callback, events registration,
     * several ways to send events and create referenced actors.
     * @{
     */

    /**
     * @brief Register a looped callback for this actor.
     * @tparam _Actor The derived actor type, which must inherit from `qb::ICallback`.
     * @param actor A reference to the derived actor instance (usually `*this`).
     * @details
     * The registered `on(qb::LoopEvent const&)` method (from `qb::ICallback`) will be called
     * by the `VirtualCore` during each of its processing loop iterations, after event processing.
     * This allows the actor to perform periodic tasks or background operations; the `LoopEvent`
     * carries per-pass context (`now`, `iteration`).
     * The callback remains active until explicitly unregistered or the actor is terminated.
     * @note Ensure the tick handler is fast and non-blocking.
     * @see qb::ICallback, qb::LoopEvent
     * @code
     * class MyPollingActor
     *   : public qb::Actor
     *   , public qb::ICallback // Must inherit from ICallback
     * {
     * public:
     *   qb::io::async::task<bool> onInit() override {
     *     registerCallback(*this); // Register self for periodic callbacks
     *     co_return true;
     *   }
     *
     *   void on(qb::LoopEvent const &loop) override {
     *     // This code will be executed periodically by the VirtualCore
     *     // pollExternalSystem();
     *     // if (checkCondition()) {
     *     //   unregisterCallback(); // Stop further callbacks
     *     // }
     *   }
     *   // ... other methods and event handlers ...
     * };
     * @endcode
     */
    template <callback_type _Actor>
    void registerCallback(_Actor &actor) const noexcept;

    /**
     * @brief Unregister a previously registered looped callback for this actor.
     * @tparam _Actor The derived actor type.
     * @param actor A reference to the derived actor instance (usually `*this`).
     * @details
     * Stops the periodic invocation of the actor's `on(qb::LoopEvent const&)` tick handler.
     * It is safe to call this even if no callback is currently registered.
     * @note Can be called from within the tick handler to self-terminate the callback cycle,
     *       or from any event handler.
     * @code
     * // void on(MyStopEvent& event) {
     * //   unregisterCallback(*this); // Stop the periodic callback
     * // }
     * @endcode
     */
    template <callback_type _Actor>
    void unregisterCallback(_Actor &actor) const noexcept;

    /**
     * @private
     * @brief Internal method to unregister callback without type information.
     * @details This is typically called by the framework or the typed `unregisterCallback`.
     */
    void unregisterCallback() const noexcept;

    /**
     * @brief Subscribe this actor to listen for a specific event type.
     * @tparam _Event The type of event to listen for (must derive from `qb::Event`).
     * @tparam _Actor The derived actor type that implements the `on(_Event&)` handler.
     * @param actor A reference to the derived actor instance (usually `*this`).
     * @details
     * After registration, if an event of type `_Event` is sent to this actor's ID,
     * its corresponding `void on(_Event& event)` or `void on(const _Event& event)`
     * method will be invoked by the `VirtualCore`.
     * This is typically called within the actor's `onInit()` method.
     * @note The actor must have a public member function `void on(const _Event& event)` or `void on(_Event& event)`.
     * @code
     * // qb::io::async::task<bool> onInit() override {
     * //   registerEvent<MyCustomEvent>(*this);
     * //   registerEvent<AnotherEvent>(*this);
     * //   co_return true;
     * // }
     * //
     * // void on(const MyCustomEvent& event) {  handle MyCustomEvent... }
     * // void on(AnotherEvent& event) { handle AnotherEvent, can reply/forward }
     * @endcode
     */
    template <event_type _Event, actor_type _Actor>
    void registerEvent(_Actor &actor) const noexcept;

    /**
     * @brief Unsubscribe this actor from listening to a specific event type.
     * @tparam _Event The type of event to stop listening for.
     * @tparam _Actor The derived actor type.
     * @param actor A reference to the derived actor instance (usually `*this`).
     * @details
     * After this call, the actor will no longer receive new events of type `_Event`.
     * It is safe to call this for event types the actor was not subscribed to.
     * @code
     * // void on(StopListeningEvent& event) {
     * //   unregisterEvent<MyCustomEvent>(*this);
     * // }
     * @endcode
     */
    template <event_type _Event, actor_type _Actor>
    void unregisterEvent(_Actor &actor) const noexcept;

    /**
     * @private
     * @brief Internal method to unregister from an event type.
     * @tparam _Event The event type to unregister.
     * @details Typically called by the framework or the typed `unregisterEvent`.
     */
    template <typename _Event>
    void unregisterEvent() const noexcept;

    /**
     * @brief Get an EventBuilder for sending chained events to a destination actor.
     * @param dest The `ActorId` of the destination actor.
     * @return An `Actor::EventBuilder` instance associated with the destination actor.
     * @details
     * This provides a fluent interface for sending multiple events to the same actor:
     * @code
     * // ActorId target_id = GetSomeActorId();
     * // to(target_id)
     * //   .push<MyEvent1>()
     * //   .push<MyEvent2>(param1, param2)
     * //   .push<MyEvent3>(data_ptr);
     * @endcode
     * All events pushed via the builder are sent in an ordered fashion, similar to `push()`.
     * @attention
     * Multiple calls to `to(same_id)` will yield `EventBuilder` instances that operate on the
     * same underlying communication pipe to that destination. Event ordering is maintained per pipe.
     * @see Actor::EventBuilder
     */
    [[nodiscard]] EventBuilder to(ActorId dest) const noexcept;

    /**
     * @brief Send a new event in an ordered fashion to a destination actor, returning a reference to it.
     * @tparam _Event The type of event to create and send (must derive from `qb::Event`).
     * @tparam _Args Types of arguments to forward to the `_Event` constructor.
     * @param dest The `ActorId` of the destination actor.
     * @param args Arguments to forward to the constructor of `_Event`.
     * @return A mutable reference to the constructed `_Event` object before it is sent.
     *         This allows modification of the event's members after construction but before sending.
     * @details
     * This is the primary and recommended method for sending events. Events sent using `push()`
     * to the same destination actor from the same source actor are guaranteed to be received
     * in the order they were pushed.
     * The event is queued and sent by the `VirtualCore` at an appropriate time (usually at the end of the current processing loop).
     * Supports events with non-trivially destructible members (`std::vector`, smart pointers,
     * `qb::string<N>`): the framework runs the event's destructor exactly once, after the handler.
     * @warning **The member must be trivially RELOCATABLE — it must not hold a pointer into
     *          itself.** Cross-core delivery moves an event by raw `memcpy` (sender pipe → peer
     *          mailbox ring → receive buffer) and never runs the source destructor, so a
     *          self-referential member still addresses the *sender's* buffer after the hop.
     *          A **short `std::string` by value is exactly that on libstdc++** (its `_M_p` points
     *          at its own inline `_M_local_buf`): the handler reads reused memory and
     *          `~basic_string()` then calls `operator delete` on a pointer that never came from
     *          the heap. libc++ recomputes `data()` from `this`, so macOS never shows it and only
     *          Linux corrupts. Use `qb::string<N>` for inline text, or keep the data on the heap
     *          behind a `std::shared_ptr` / `std::unique_ptr` member.
     * @note    **Debug builds catch this for you.** Before relocating an event cross-core the
     *          engine scans it for a pointer into its own storage and aborts with a diagnostic
     *          instead of corrupting the peer (`SharedCoreCommunication::send`). It is compiled
     *          out entirely under `NDEBUG`, so a release build pays nothing — the point is to make
     *          the defect visible on the development platform, where the standard library hides
     *          it. Pinned by `RelocatablePayload.*` / `RelocatablePayloadDeathTest.*` in
     *          `system/messaging/relocatable-payload.cpp`.
     * @code
     * // ActorId target_id = GetSomeActorId();
     * // auto& my_evt = push<MyDataEvent>(target_id, initial_value);
     * // my_evt.data_field = 42; // Modify event before it's sent
     * // my_evt.message = "Hello";
     * //
     * // push<AnotherEvent>(target_id); // This will be processed by target_id after my_evt
     * @endcode
     * @note If the event type has a non-trivial destructor, the framework ensures it is called appropriately.
     * @attention **The returned reference dies at the very next event queued to the same
     *            destination core** — not merely at the end of the enclosing scope. The pipe is a
     *            growable buffer: the next `push`/`send`/`broadcast` whose destination resolves to
     *            that core may reallocate it (invalidating the reference) or compact it in place
     *            (leaving the reference aliasing a *different* event inside a still-live
     *            allocation — which no allocator debugger can detect). Populate the event fully
     *            **before** queueing anything else, exactly as the example above does; never hold
     *            the reference across another send, a helper call that may send, or a loop
     *            iteration. Pinned by `PipeAllocatorContract.*` in
     *            `tests/io/unit/core/pipe-allocator.cpp`.
     * @warning `noexcept`: an allocation failure while growing the pipe buffer or
     *          constructing the event (e.g. under OOM) cannot be reported and calls
     *          `std::terminate()`. Keep events small / allocation-light. See
     *          `qb::Pipe::push` for the full contract (applies to `send`/`broadcast` too).
     */
    template <typename _Event, typename... _Args>
    _Event &push(ActorId const &dest, _Args &&...args) const noexcept;

    /**
     * @brief Send a new event in an unordered fashion to a destination actor.
     * @tparam _Event The type of event to create and send (must derive from `qb::Event`).
     * @tparam _Args Types of arguments to forward to the `_Event` constructor.
     * @param dest The `ActorId` of the destination actor.
     * @param args Arguments to forward to the constructor of `_Event`.
     * @details
     * Events sent using `send()` are not guaranteed to be received in the order they were sent,
     * even if sent to the same destination from the same source. This method may offer slightly
     * lower latency for same-core communication in specific scenarios but sacrifices ordering.
     * @note Trivial destructibility is a guideline here and a COMPILER-ENFORCED rule only for `qb::EventQOS0` — the one kind
     *       the flush may DROP undisposed. Prefer POD members or `qb::string`; a delivered event is disposed exactly once.
     * @code
     * // ActorId critical_service_id = GetSomeActorId();
     * // // Fire-and-forget status update, order not critical
     * // send<StatusUpdateEvent>(critical_service_id, current_status);
     * @endcode
     * @attention Use with caution. Prefer `push()` for most use cases.
     *            Misuse can lead to difficult-to-debug race conditions or logical errors if order matters.
     */
    template <typename _Event, typename... _Args>
    void send(ActorId const &dest, _Args &&...args) const noexcept;

    /**
     * @brief Construct an event locally, intended for immediate self-processing or direct calls.
     * @tparam _Event The type of event to build (must derive from `qb::Event`).
     * @tparam _Args Types of arguments to forward to the `_Event` constructor.
     * @param source The `ActorId` to be set as the source of this event (usually `this->id()`).
     * @param args Arguments to forward to the constructor of `_Event`.
     * @return A locally constructed `_Event` object.
     * @details
     * This method creates an event object but does not send it through the actor system's
     * messaging queues. It's typically used to prepare an event that will be passed directly
     * to one of the actor's own `on()` handlers or to a referenced actor's methods.
     * The `dest` field of the event will be set to `this->id()`.
     * @code
     * // // ... inside an actor method ...
     * // MyInternalEvent local_evt = build_event<MyInternalEvent>(id(), event_data);
     * // local_evt.some_flag = true;
     * // this->on(local_evt); // Directly call the event handler
     * @endcode
     * @note The lifetime of the returned event is managed by the caller.
     *       This does not involve the actor framework's event queue.
     */
    template <typename _Event, typename... _Args>
    [[nodiscard]] _Event build_event(qb::ActorId const source, _Args &&...args) const noexcept;

    /**
     * @brief Is `id` an actor that is still alive and active on **this** VirtualCore?
     * @param id The actor identifier to test.
     * @return `true` iff an actor with that id exists on this core and its `onInit()` has
     *         completed; `false` for an invalid id, an actor on another core, one whose async
     *         `onInit()` is still in flight, and one that has been killed.
     * @details
     * The untyped counterpart of `qb::ActorHandle<T>::ready()`: one hash lookup, no
     * `dynamic_cast`, no knowledge of the concrete type. It exists for bookkeeping that stores
     * bare `ActorId`s — a subscriber list, a routing table, a worker registry — and must drop
     * entries whose actor has gone. The framework prunes its **own** such map when an actor dies
     * (`VirtualCore::removeActor` → `unregisterEvents`), so any user-space mirror of it needs
     * this to stay bounded; `qb::PubSub<Topic>` uses it exactly so.
     * @attention Same-core only, by construction: an actor map belongs to its own VirtualCore and
     *            is not synchronized. A `false` for a *remote* id is therefore not evidence that
     *            the actor is gone — for cross-core liveness, ask the remote actor
     *            (`co_await qb::ping(...)`).
     * @code
     * // std::erase_if(_subscribers, [this](qb::ActorId s) { return !is_actor_alive(s); });
     * @endcode
     */
    [[nodiscard]] bool is_actor_alive(ActorId id) const noexcept;

    /**
     * @brief Check if a given ID matches the type ID of `_Type`.
     * @tparam _Type The type to check against.
     * @param id The type ID (usually from an event or actor) to compare.
     * @return `true` if `id` is the type ID of `_Type`, `false` otherwise.
     */
    template <typename _Type>
    [[nodiscard]] inline bool
    is(uint32_t const id) const noexcept {
        return id == type_id<_Type>();
    }

    /**
     * @brief Check if a `RequireEvent` is for a specific actor type.
     * @tparam _Type The actor type to check against.
     * @param event The `RequireEvent` to inspect.
     * @return `true` if `event.type` matches the type ID of `_Type`, `false` otherwise.
     * @see qb::RequireEvent
     */
    template <typename _Type>
    [[nodiscard]] inline bool
    is(RequireEvent const &event) const noexcept {
        return event.type == type_id<_Type>();
    }

    /**
     * @brief Legacy fire-and-forget discovery of other actors of specified types.
     * @tparam _Actors Variadic template pack of actor types to discover.
     * @return `true` if the discovery ping was successfully broadcasted for all types.
     * @details
     * For each type in `_Actors`, broadcasts a `PingEvent`; live actors of that type reply with a
     * `RequireEvent`. Override `on(RequireEvent&)` and use `is<_ActorType>(event)` to identify
     * responses (a reply means alive — presence is the status).
     *
     * @note Prefer the coroutine form **`co_await qb::require<T>(ctx, timeout)`** (returns the
     *       discovered `ActorId`s directly, works inside `onInit`, no `on(RequireEvent)` boilerplate).
     * @code
     * // qb::io::async::task<bool> onInit() override {
     * //   registerEvent<qb::RequireEvent>(*this);
     * //   require<ServiceA, ServiceB>(); // discover; replies arrive after activation
     * //   co_return true;
     * // }
     * // void on(qb::RequireEvent& event) {                 // override for the legacy path
     * //   if (qb::is<ServiceA>(event)) _service_a_id = event.getSource();
     * // }
     * @endcode
     * @see qb::require, qb::ping, qb::PingEvent, qb::RequireEvent
     */
    template <typename... _Actors>
    bool require() const noexcept;

    /**
     * @brief Broadcast an event to all actors on all cores.
     * @tparam _Event The type of event to broadcast (must derive from `qb::Event`).
     * @tparam _Args Types of arguments to forward to the `_Event` constructor.
     * @param args Arguments to forward to the constructor of `_Event`.
     * @details
     * The event will be sent to every actor currently running in the system across all `VirtualCore`s.
     * The source of the event will be this actor's ID.
     * Use `push<MyEvent>(qb::BroadcastId(core_id), ...)` to broadcast only to a specific core.
     * @code
     * // broadcast<SystemShutdownNoticeEvent>("System shutting down in 5 minutes");
     * @endcode
     * @note Ensure the event type `_Event` is appropriate for system-wide broadcast and that
     *       all potential recipient actors are registered to handle it or will ignore it safely.
     */
    template <typename _Event, typename... _Args>
    void broadcast(_Args &&...args) const noexcept;

    /**
     * @brief Reply to the source of a received event, reusing the event object.
     * @param event The event object that was received. This event will be modified
     *              (its `dest` and `source` will be swapped) and sent back to its original source.
     * @details
     * This is the most efficient way to send a response back to the sender of an event.
     * The original event object is reused, minimizing allocations and copies.
     * The `on()` handler receiving the event must take it by non-const reference (`MyEvent& event`)
     * to allow `reply()` to modify and effectively consume it.
     * @code
     * // void on(MyRequestEvent& request) { // Note: non-const reference
     * //   request.result_data = process(request.input_data);
     * //   request.status_code = 200;
     * //   reply(request); // Sends the modified MyRequestEvent back to its original source
     * // }
     * @endcode
     * @attention After calling `reply(event)`, the `event` object in the current handler
     *            should be considered consumed and no longer valid for further use or modification.
     */
    void reply(Event &event) const noexcept;

    /**
     * @brief Forward a received event to a new destination, reusing the event object.
     * @param dest The `ActorId` of the new destination actor.
     * @param event The event object that was received. This event will be modified
     *              (its `dest` will be updated to `dest`) and sent.
     *              The original `source` of the event is preserved.
     * @details
     * This is an efficient way to delegate an event to another actor without creating a new event.
     * The `on()` handler receiving the event must take it by non-const reference (`MyEvent& event`)
     * to allow `forward()` to modify and effectively consume it.
     * @code
     * // void on(WorkItemEvent& item) { // Note: non-const reference
     * //   if (item.type == WorkType::TypeA)
     * //     forward(_worker_a_id, item); // Forward to Worker A
     * //   else
     * //     forward(_worker_b_id, item); // Forward to Worker B
     * // }
     * @endcode
     * @attention After calling `forward(dest, event)`, the `event` object in the current handler
     *            should be considered consumed and no longer valid for further use or modification.
     */
    void forward(ActorId dest, Event &event) const noexcept;

    // OpenApi : used for module
    /** @private */
    void send(Event const &event) const noexcept;
    /** @private */
    void push(Event const &event) const noexcept;
    /** @private */
    bool try_send(Event const &event) const noexcept;

    /**
     * @brief Get direct access to the underlying communication pipe for a destination actor.
     * @param dest The `ActorId` of the destination actor.
     * @return A `qb::Pipe` object representing the unidirectional communication channel to `dest`.
     * @details
     * This provides lower-level access to the event sending mechanism. It can be useful for
     * performance-critical scenarios, especially when sending multiple events to the same
     * destination or when needing to pre-allocate buffer space for large events using
     * `Pipe::allocated_push()`.
     * @code
     * // ActorId target_id = GetSomeActorId();
     * // qb::Pipe comm_pipe = getPipe(target_id);
     * // auto& ev1 = comm_pipe.push<MyEvent1>();
     * // // allocated_push's first argument is the TRAILING bytes reserved after the event
     * // // (sizeof(LargeEvent) is added internally) — pass 0 unless you write raw bytes past it.
     * // auto& ev2 = comm_pipe.allocated_push<LargeEvent>(0, constructor_args_for_large_event);
     * @endcode
     * @see qb::Pipe
     * @see Pipe::push
     * @see Pipe::allocated_push
     */
    [[nodiscard]] Pipe getPipe(ActorId dest) const noexcept;

    /**
     * @brief Create a new referenced actor on the same VirtualCore and return a handle to it.
     * @tparam _Actor The concrete derived actor type to create (must inherit from `qb::Actor`).
     * @tparam _Args Types of arguments to forward to the `_Actor`'s constructor.
     * @param args Arguments to forward to the constructor of `_Actor`.
     * @return A phase-aware `qb::ActorHandle<_Actor>`. The handle's `id()` is valid immediately
     *         (even if the child's async `onInit()` is still in flight); `get()`/`operator->`
     *         resolve the actor only once it is **active** (`onInit` completed), and are
     *         `nullptr` while Activating, after a failed init, or after it died. An empty
     *         handle (`!valid()`) means creation failed.
     * @details
     * Referenced actors are created on the same `VirtualCore` as the calling (parent) actor and
     * manage their own lifecycle (the parent does **not** own them). Send to `handle.id()` at
     * any time — events to a still-Activating child are stashed and replayed FIFO once it
     * activates. To touch the child directly, gate on readiness:
     * @code
     * auto helper = addRefActor<HelperActor>(cfg);   // qb::ActorHandle<HelperActor>
     * push<TaskEvent>(helper.id(), task_data);        // always safe (stashed if Activating)
     * if (helper.ready())                             // sync-init child: ready at once
     *     helper->doSomething();
     * // async-init child: co_await helper.ready_async(context()); then helper->doSomething();
     * @endcode
     * @attention Direct method calls bypass the child's event queue; prefer `push(...)` to
     *            `handle.id()`. Never call `operator->` on a non-`ready()` handle.
     * @note Not `[[nodiscard]]`: fire-and-forget creation of a self-managing child (one you
     *       only talk to by events, or that needs no further reference) is a first-class use —
     *       discard the handle freely. Use `addRefHandle` when you specifically want the handle.
     */
    template <typename _Actor, typename... _Args>
    ActorHandle<_Actor> addRefActor(_Args &&...args) const;

    /**
     * @brief Alias of `addRefActor<T>()` — both return a phase-aware `qb::ActorHandle<T>`.
     * @tparam _Actor Actor type to create.
     * @tparam _Args  Constructor argument types.
     * @param args Constructor arguments.
     * @return `ActorHandle<_Actor>` — empty if creation failed.
     * @details Retained for source compatibility; new code can just call `addRefActor`.
     */
    template <typename _Actor, typename... _Args>
    [[nodiscard]] ActorHandle<_Actor>
    addRefHandle(_Args &&...args) const {
        return addRefActor<_Actor>(std::forward<_Args>(args)...);
    }

    /**
     * @name Coroutine Support
     * C++20 coroutine integration for async I/O operations.
     * @{
     */

    /**
     * @brief Launch a **detached** async coroutine in an isolated context (low-level).
     *
     * @note Prefer `spawn()` for anything bound to this actor: it is cancelled
     *       automatically when the actor is killed. Use `spawn_detached` only for
     *       fire-and-forget work that must intentionally **outlive** the actor (it is
     *       NOT cancelled on kill — it runs to completion, orphaned).
     *
     * The coroutine runs in an isolated context and cannot directly
     * access Actor state. Communication must happen via push<Event>().
     *
     * ⚠️ CRITICAL SAFETY REQUIREMENTS:
     * ================================
     * 1. **NEVER access actor member variables after co_await**
     *    - Actor may be destroyed while coroutine is suspended
     *    - Accessing `this->_member` after suspension = UNDEFINED BEHAVIOR
     *
     * 2. **Capture all data by VALUE before first co_await**
     *    - Copy everything you need from actor state BEFORE any co_await
     *    - Never capture `this` or references to actor members
     *
     * 3. **Use ONLY CoroContext after suspension**
     *    - ctx.push<Event>() is safe (events to dead actors are ignored)
     *    - ctx.id() and ctx.time() are safe
     *
     * 4. **Keep coroutines SHORT-LIVED**
     *    - Long-running coroutines increase risk of actor destruction
     *    - Prefer multiple short coroutines over one long one
     *
     * @tparam Func Coroutine function type (returns task<void>)
     * @param func Coroutine to execute
     *
     * @example ✅ SAFE Pattern
     * void on(RequestEvent& ev) {
     *     // Copy ALL needed data BEFORE spawning
     *     std::string key = ev.key;
     *     ActorId sender = ev.sender;
     *
     *     spawn_detached([key, sender](auto ctx) -> qb::io::async::task<void> {
     *         // NO access to 'this' after this point!
     *         auto reply = co_await fetch(key);  // Actor may die here
     *
     *         // ONLY use ctx after co_await
     *         ctx.template push<ResultEvent>(reply);
     *     });
     * }
     *
     * @example ❌ DANGEROUS Pattern (DO NOT USE)
     * void on(RequestEvent& ev) {
     *     spawn_detached([this](auto ctx) -> qb::io::async::task<void> {
     *         co_await sleep(100ms);  // Actor may die here
     *
     *         // ❌ CRASH: accessing this->_member after suspension!
     *         this->_member = value;  // UNDEFINED BEHAVIOR
     *     });
     * }
     */
    template <typename Func>
    void spawn_detached(Func &&func) const;

    /**
     * @brief Launch a coroutine **scoped to this actor's lifetime** (recommended).
     *
     * Like `spawn_detached`, but the coroutine is bound to a per-actor cancellation
     * scope. When the actor is killed/destroyed, the scope is cancelled: any
     * coroutine awaiting a *cancellation-aware* operation provided by
     * `ScopedCoroContext` (`ctx.sleep(...)`, `ctx.cancellation_point()`,
     * `ctx.cancellable(...)`) wakes within the next loop iteration, throws
     * `qb::io::async::cancelled_error`, and unwinds cleanly (RAII + catch run).
     *
     * This makes actor coroutines **safe and bounded by construction**: a killed
     * actor no longer leaves a coroutine blocked on a long timeout/I/O. The lambda
     * receives a `qb::ScopedCoroContext` (a superset of `CoroContext` that also
     * carries the scope token).
     *
     * The same capture discipline as `spawn_detached` still applies — **capture by
     * value, never `this`** (the scope bounds the lifetime, it does not make
     * member access after suspension legal).
     *
     * @tparam Func Coroutine function type taking a `ScopedCoroContext`, returning `task<void>`.
     * @param func Coroutine to execute.
     *
     * @code
     * void on(FetchEvent &e) {
     *     auto key = e.key;
     *     auto who = e.getSource();
     *     spawn([key, who](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
     *         co_await ctx.sleep(50ms);        // cancelled if the actor is killed
     *         ctx.push_to<ResultEvent>(who);
     *     });
     * }
     * @endcode
     * @see spawn_detached, ScopedCoroContext, qb::io::async::cancellation_token
     */
    template <typename Func>
    void spawn(Func &&func) const;

    /**
     * @brief Obtain a cancellation-aware coroutine context bound to **this** actor.
     * @return A `ScopedCoroContext` carrying this actor's id and its per-actor
     *         cancellation scope (the scope is lazily allocated on first use).
     * @details The context whose `sleep` / `cancellation_point` / `cancellable` helpers
     *          (and `qb::ask(context(), ...)`) are cancelled when the actor is
     *          killed/destroyed — exactly the token a `spawn()` body receives, but
     *          available wherever you hold the actor, most notably **inside `onInit()`**:
     * @code
     * qb::io::async::task<bool> onInit() override {
     *   co_await context().sleep(std::chrono::milliseconds{5});
     *   co_return true;
     * }
     * @endcode
     * @note The same capture discipline applies: never capture `this` past a `co_await`.
     */
    [[nodiscard]] ScopedCoroContext context() const;

    /**
     * @brief Check if actor has active coroutines
     * @return true if any coroutines are still running
     */
    [[nodiscard]] bool
    has_active_coroutines() const {
        return active_coroutines_ && active_coroutines_->load(std::memory_order_relaxed) > 0;
    }

    /**
     * @brief Whether this actor has lazily created its coroutine cancellation scope.
     * @return true once `spawn()` has been called at least once.
     */
    [[nodiscard]] bool
    has_coro_scope() const noexcept {
        return static_cast<bool>(_coro_scope);
    }

    /**
     * @brief Resolve a pending `ask` from a response event (call from `on(E&)`).
     * @tparam E A `qb::AskEvent` subtype used as the request/response envelope.
     * @param e The received event (a response carries the stamped `correlation_id`).
     * @return `true` if `e` matched a pending ask of this actor and was delivered to the
     *         waiting coroutine; `false` if it is an unrelated/unsolicited event — handle
     *         it normally in that case.
     * @details
     * The `ask` pattern round-trips a single event type: the asker sends it via
     * `qb::ask(ctx, ...)`, the responder fills its response fields and `reply()`s it back
     * (which preserves `correlation_id`), and the asker's `on(E&)` handler routes it here.
     * @code
     * void on(MyExchange &e) { if (resolve_ask(e)) return; // ... unsolicited handling ... }
     * @endcode
     */
    template <typename E>
    bool resolve_ask(E &e) const noexcept;

    /**
     * @brief Get number of active coroutines
     * @return Count of running coroutines
     */
    [[nodiscard]] std::size_t
    active_coroutine_count() const {
        return active_coroutines_ ? active_coroutines_->load(std::memory_order_relaxed) : 0;
    }

    /**
     * @brief Get the coroutine scheduler for this actor
     * @return Pointer to scheduler, or nullptr if not initialized
     * @private Internal use by VirtualCore for workflow integration
     */
    [[nodiscard]] qb::io::async::CoroutineScheduler *
    get_coro_scheduler() const {
        return coro_scheduler_;
    }

    /**
     * @}
     */

private:
    /**
     * @brief Coroutine scheduler pointer (shared with listener)
     *
     * Points to the listener's scheduler (not owned by actor).
     * All actors on the same VirtualCore share the scheduler for efficiency.
     *
     * SAFETY: When actor is destroyed, active_coroutines_ is checked.
     * Coroutines must NOT access actor state after suspension - they should
     * only use CoroContext to send events back to the actor.
     */
    mutable qb::io::async::CoroutineScheduler *coro_scheduler_ = nullptr;

    /**
     * @brief Shared count of active coroutines.
     *
     * Uses `shared_ptr` so coroutine RAII guards can safely decrement even
     * after the actor is destroyed (the shared_ptr keeps the counter alive
     * until the last orphaned coroutine frame is destroyed).
     *
     * @note Eagerly allocated in the Actor constructor (finding 2.12) — this
     *       trades one heap allocation per actor construction for the removal
     *       of a `nullptr` check on every `spawn_detached()` hot path. A single
     *       `make_shared` is a negligible cost compared to the rest of actor
     *       construction and guarantees branch-predicted coroutine spawning.
     */
    mutable std::shared_ptr<std::atomic<std::size_t>> active_coroutines_ = std::make_shared<std::atomic<std::size_t>>(0);

    /**
     * @brief Per-actor coroutine cancellation scope (lazy, empty by default).
     *
     * Created on the first `spawn()` call only, so actors that never spawn a
     * scoped coroutine pay **zero** cost (an empty token allocates nothing). Cancelled
     * when the actor is killed/destroyed so that coroutine awaiting a cancellation-aware
     * `ScopedCoroContext` operation is unwound promptly. Captured by value into each
     * scoped coroutine frame, so its shared state safely outlives the actor.
     */
    mutable qb::io::async::cancellation_token _coro_scope{qb::io::async::null_token};

    /**
     * @brief Resolve / revalidate the cached coroutine scheduler against the TLS one.
     * @details Shared hot path of `spawn_detached` / `spawn` (finding 2.D.4).
     *          `current_ptr()` is a plain thread_local load; the comparison is one cmp+jne.
     */
    void __resolve_coro_scheduler__() const noexcept;

    /** @brief Lazily allocate the real cancellation scope on first scoped spawn. */
    void __ensure_coro_scope__() const;

    /**
     * @brief Cancel the coroutine scope if it exists (cancel-on-kill / on-destroy).
     * @details Idempotent. Called by `kill()` (promptness) and by
     *          `VirtualCore::removeActor` (catch-all for every destruction path).
     */
    void __cancel_coro_scope__() const noexcept;
};

/**
 * @class CoroContext
 * @brief Safe context for coroutines spawned via spawn_detached()
 * @ingroup Actor
 *
 * Provides a restricted interface for coroutines to interact with
 * the actor system safely after co_await points. Captures ActorId
 * by value to avoid dangling pointer if the actor is destroyed
 * while the coroutine is suspended.
 */
class CoroContext {
    ActorId actor_id_;

public:
    /**
     * @brief Construct from actor, capturing its ID by value
     * @param actor The parent actor (only used to capture ID)
     */
    explicit CoroContext(Actor const *actor)
        : actor_id_(actor->id()) {}

    /**
     * @brief Send an event to self (the spawning actor's ID)
     * @tparam _Event The event type
     * @tparam Args Event constructor arguments
     * @param args Arguments to forward to event constructor
     */
    template <typename _Event, typename... Args>
    void push(Args &&...args) const;

    /**
     * @brief Send an event to a specific destination actor
     * @tparam _Event The event type
     * @tparam Args Event constructor arguments
     * @param dest Destination actor ID
     * @param args Arguments to forward to event constructor
     */
    template <typename _Event, typename... Args>
    void push_to(ActorId dest, Args &&...args) const;

    /**
     * @brief Broadcast an event to every actor on all cores (source = the spawning actor).
     * @tparam _Event The event type
     * @tparam Args Event constructor arguments
     * @details Mirrors `Actor::broadcast`; used e.g. by `qb::require` to fan out a discovery ping.
     */
    template <typename _Event, typename... Args>
    void broadcast(Args &&...args) const;

    /**
     * @brief Get the spawning actor's ID (captured at construction)
     * @return ActorId
     */
    [[nodiscard]] ActorId
    id() const noexcept {
        return actor_id_;
    }

    /**
     * @brief Get current time from the VirtualCore
     * @return Timestamp in nanoseconds
     */
    [[nodiscard]] uint64_t time() const noexcept;
};

/**
 * @struct AskEvent
 * @ingroup Actor
 * @brief Base event for the request/response `ask` pattern.
 * @details
 * The `ask` pattern round-trips a **single** event type: `qb::ask(ctx, target, E{...}, t)`
 * sends it (the framework stamps `correlation_id`), the responder fills its response fields
 * and `reply()`s it back (which reuses the event object, preserving `correlation_id`), and
 * the asker's `on(E&)` handler routes it via `resolve_ask(e)` to resume the awaiting
 * coroutine. Derive your exchange event from `qb::AskEvent` (alias `qb::ask_event`).
 */
struct AskEvent : qb::CorrelatedEvent {
    // `correlation_id` is inherited from CorrelatedEvent (stamped by `ask()`, preserved by
    // `reply()`; do not set manually). AskEvent stays a distinct type for the request/response API.
};

namespace detail {

/**
 * @brief Type-erased pending-ask slot, stored by address in the per-core registry.
 * @details Function-pointer dispatch (no vtable) keeps it POD/allocation-friendly. Lives
 *          inside the `ask_awaiter` (which lives in the ask() coroutine frame).
 */
struct ask_slot {
    qb::ActorId owner;
    bool        done                                      = false;
    void       *self                                      = nullptr;
    void (*deliver)(void *self, qb::Event &resp) noexcept = nullptr;
};

[[nodiscard]] std::uint64_t ask_next_id(qb::ActorId owner) noexcept;
void                        ask_register(std::uint64_t id, ask_slot *slot) noexcept;
void                        ask_unregister(std::uint64_t id) noexcept;
bool                        ask_deliver(std::uint64_t id, qb::ActorId owner, qb::Event &resp) noexcept;
[[nodiscard]] ev::loop_ref  ask_loop() noexcept;

/**
 * @brief RAII rollback for a freshly-registered slot.
 * @details Helpers that `ask_register` a slot and *then* send the request (`ask_stream`, `ping`,
 *          `require`) have a window where the send (`push_to`/`broadcast`, not `noexcept`) could
 *          throw before the slot's RAII owner (the `stream`/awaiter) exists — which would strand a
 *          dangling slot in the registry. Arm this guard right after `ask_register`; `release()` it
 *          once the owner has taken over. If the send throws, the guard deregisters on unwind.
 */
struct ask_slot_guard {
    std::uint64_t id;
    bool          armed = true;
    explicit ask_slot_guard(std::uint64_t i) noexcept
        : id(i) {}
    ask_slot_guard(const ask_slot_guard &)            = delete;
    ask_slot_guard &operator=(const ask_slot_guard &) = delete;
    void
    release() noexcept {
        armed = false;
    }
    ~ask_slot_guard() {
        if (armed)
            ask_unregister(id);
    }
};

/**
 * @brief Register an event type as ask-correlated (carries `AskEvent::correlation_id`).
 * @details Called once per exchange type by `qb::ask<E>`, on the asker's worker thread. The
 *          activation dispatch gate consults this set so an in-flight ask **reply** can reach
 *          an actor that is still *Activating* (a `co_await qb::ask(...)` inside `onInit`)
 *          instead of being stashed — which would deadlock the init on its own reply.
 */
void ask_register_type(qb::Event::id_type type) noexcept;

/**
 * @brief If `ev` is an ask reply resolving a pending ask owned by `dest`, deliver it.
 * @return `true` if `ev` was an ask-correlated event that resolved one of `dest`'s pending
 *         asks (consumed); `false` otherwise (the caller stashes / routes normally).
 * @details The type-id check guarantees `ev` derives from `AskEvent`, so reading
 *          `correlation_id` at the base subobject offset is safe. Used only by the gate.
 */
[[nodiscard]] bool ask_try_deliver_reply(qb::Event &ev, qb::ActorId dest) noexcept;

/**
 * @brief Single awaiter backing `qb::ask` — three wake sources
 *        (response / timeout / actor-scope cancel) guarded by one `done` flag.
 *
 * @details
 * Deliberately a custom awaiter rather than a composition of `with_deadline` /
 * `when_any` + `cancellable_sleep`. Those now reclaim their detached timeout/branch
 * tasks the instant a winner is decided (so they no longer leave a zombie timer per
 * in-flight ask), but each still spawns and tears down several helper coroutine frames
 * per call. On a hot request/response path this awaiter is leaner: it arms a single
 * `ev_timer` and **stops it immediately** on response — no spawned helper at all. It
 * lives in the ask() coroutine frame (address-stable) and is non-movable (the registry
 * holds it by address).
 */
template <typename E>
struct ask_awaiter {
    ask_slot                          slot{};
    std::uint64_t                     id;
    qb::duration                      timeout;
    qb::io::async::cancellation_token token;
    std::optional<E>                  result;
    std::coroutine_handle<>           cont;
    ev_timer                          timer{};
    bool                              timer_started               = false;
    enum class kind { pending, ok, timed_out, cancelled } outcome = kind::pending;
    std::shared_ptr<bool>                      alive              = std::make_shared<bool>(true);
    qb::io::async::cancellation_token::id_type cancel_id          = 0; ///< scope on_cancel reg; removed in finish().

    ask_awaiter(std::uint64_t aid, qb::ActorId owner, qb::duration t, qb::io::async::cancellation_token tok)
        : id(aid)
        , timeout(t)
        , token(std::move(tok)) {
        slot.owner   = owner;
        slot.self    = this;
        slot.deliver = &ask_awaiter::deliver_thunk;
        // Make this exchange type recognisable to the activation gate (asker's core), so an
        // in-onInit ask's reply is delivered here instead of stashed (which would deadlock).
        ask_register_type(qb::Event::type_to_id<E>());
    }
    ask_awaiter(const ask_awaiter &)            = delete;
    ask_awaiter(ask_awaiter &&)                 = delete;
    ask_awaiter &operator=(const ask_awaiter &) = delete;

    [[nodiscard]] bool
    await_ready() const noexcept {
        return token.is_cancelled();
    }

    void
    await_suspend(std::coroutine_handle<> h) {
        cont = h;
        if (token.is_cancelled()) { // outcome stays `pending` → await_resume throws cancelled.
            qb::io::async::schedule_via_current(h);
            return;
        }
        ask_register(id, &slot);
        if (timeout.count() > 0) {
            ev_timer_init(&timer, &ask_awaiter::on_timeout, qb::detail::to_ev_seconds(timeout), 0.0);
            timer.data = this;
            auto loop  = ask_loop();
            ev_now_update(static_cast<struct ev_loop *>(loop));
            ev_timer_start(loop, &timer);
            timer_started = true;
        }
        auto a    = alive;
        cancel_id = token.on_cancel([this, a]() {
            if (*a && !slot.done) {
                slot.done = true;
                outcome   = kind::cancelled;
                qb::io::async::schedule_via_current(cont);
            }
        });
    }

    E
    await_resume() {
        finish();
        switch (outcome) {
            case kind::ok:
                return std::move(*result);
            case kind::timed_out:
                throw qb::io::async::timeout_error();
            default: // pending (entry-cancelled) or cancelled
                throw qb::io::async::cancelled_error();
        }
    }

    ~ask_awaiter() {
        if (alive)
            *alive = false;
        finish();
    }

private:
    void
    finish() noexcept {
        ask_unregister(id);
        // Deregister the scope cancel hook so a long-lived actor scope token does not retain
        // one dead callback per `qb::ask` for the actor's whole life (idempotent).
        token.remove_on_cancel(cancel_id);
        cancel_id = 0;
        if (timer_started) {
            // The deadline is a one-shot ev_timer embedded directly in this
            // awaiter (which lives in the ask() coroutine frame). libev auto-stops
            // a one-shot the instant it expires, BEFORE invoking deliver/on_timeout
            // — leaving it inactive but still pending in `pendings[]` with
            // `timer.data` → this (about-to-be-freed) frame. Gating the stop on
            // `ev_is_active` would skip `clear_pending` in that window, so a later
            // ev_invoke_pending() would dispatch into freed memory. `ev_timer_stop`
            // always clears pending first, then no-ops if inactive — so gate on
            // `timer_started` only. See qb/io/async/coroutine/awaiter.h for details.
            ev_timer_stop(ask_loop(), &timer);
            timer_started = false;
        }
    }

    static void
    deliver_thunk(void *self, qb::Event &resp) noexcept {
        auto *me = static_cast<ask_awaiter *>(self);
        if (me->slot.done)
            return;
        me->slot.done = true;
        me->outcome   = kind::ok;
        me->result.emplace(std::move(static_cast<E &>(resp)));
        qb::io::async::schedule_via_current(me->cont);
    }

    static void
    on_timeout(struct ev_loop *, ev_timer *w, int) noexcept {
        auto *me = static_cast<ask_awaiter *>(w->data);
        if (me && !me->slot.done) {
            me->slot.done = true;
            me->outcome   = kind::timed_out;
            qb::io::async::schedule_via_current(me->cont);
        }
    }
};

} // namespace detail

/**
 * @class ScopedCoroContext
 * @brief Context for coroutines spawned via `Actor::spawn()`.
 * @ingroup Actor
 *
 * A superset of `CoroContext`: it carries the spawning actor's `ActorId` **and** its
 * per-actor cancellation scope (`qb::io::async::cancellation_token`). Its `sleep`,
 * `cancellation_point` and `cancellable` helpers route that token, so a coroutine that
 * only awaits these is **automatically cancelled when the actor is killed/destroyed**
 * (it throws `qb::io::async::cancelled_error` and unwinds). Inherits the safe
 * `push` / `push_to` / `id` / `time` surface from `CoroContext`.
 *
 * @note Still **never capture `this`** — the scope bounds the coroutine's lifetime but
 *       does not legalize actor-member access after a `co_await`.
 */
class ScopedCoroContext : public CoroContext {
    qb::io::async::cancellation_token _scope;

public:
    /**
     * @brief Construct from actor + its cancellation scope.
     * @param actor The spawning actor (only its id is captured, via CoroContext).
     * @param scope The actor's coroutine cancellation token (copied by value).
     */
    ScopedCoroContext(Actor const *actor, qb::io::async::cancellation_token scope)
        : CoroContext(actor)
        , _scope(std::move(scope)) {}

    /** @brief The actor's cancellation scope token (cancelled on kill/destroy). */
    [[nodiscard]] const qb::io::async::cancellation_token &
    token() const noexcept {
        return _scope;
    }

    /** @brief True once the actor scope has been cancelled (e.g. the actor was killed). */
    [[nodiscard]] bool
    cancelled() const noexcept {
        return _scope.is_cancelled();
    }

    /**
     * @brief Derive a child token linked to the actor scope.
     * @return A fresh token that is cancelled whenever the actor scope is cancelled.
     * @details Use it to scope a sub-operation that can also be cancelled independently
     *          (cancel the child without affecting the actor scope).
     */
    [[nodiscard]] qb::io::async::cancellation_token
    child_token() const {
        qb::io::async::cancellation_token child;
        _scope.on_cancel([child]() mutable { child.cancel(); });
        return child;
    }

    /**
     * @brief Cancellation-aware sleep: wakes immediately if the actor scope is cancelled.
     * @param d Duration to sleep.
     * @return An awaitable `task<void>` throwing `cancelled_error` on cancellation.
     */
    [[nodiscard]] qb::io::async::task<void>
    sleep(qb::duration d) const {
        return qb::io::async::cancellable_sleep(d, _scope);
    }

    /**
     * @brief Cooperative cancellation point: yields to the scheduler, then throws
     *        `cancelled_error` if the actor scope was cancelled meanwhile.
     * @return A yield awaiter; sprinkle `co_await ctx.cancellation_point();` inside a
     *         compute loop so a killed actor's coroutine bails between iterations.
     */
    [[nodiscard]] qb::io::async::yield_awaiter
    cancellation_point() const {
        return qb::io::async::yield_or_cancel(_scope);
    }

    /**
     * @brief Suspend until the actor scope is cancelled, then throw `cancelled_error`.
     * @return An awaiter; `co_await ctx.until_cancelled();` parks a coroutine that has no
     *         other work to do until its actor is killed (no timer/helper allocated).
     */
    [[nodiscard]] qb::io::async::cancellation_awaiter
    until_cancelled() const {
        return qb::io::async::check_cancelled(_scope);
    }

    /**
     * @brief Wrap any `task<T>` so it is cancelled together with the actor scope.
     * @tparam T Result type of the wrapped task.
     * @param t The task to make cancellable.
     * @return A cancellable operation awaitable.
     */
    template <typename T>
    [[nodiscard]] auto
    cancellable(qb::io::async::task<T> &&t) const {
        return qb::io::async::make_cancellable(std::move(t), _scope);
    }
};

/**
 * @brief Definition of `Actor::context()` — deferred until `ScopedCoroContext` is complete.
 * @details Lazily allocates the per-actor cancellation scope (so a kill during an
 *          in-flight `onInit()` / `spawn()` unwinds it) and returns a context bound to
 *          this actor's id + scope. Mirrors what `spawn()` builds for its coroutine body.
 */
inline ScopedCoroContext
Actor::context() const {
    __ensure_coro_scope__();
    return ScopedCoroContext(this, _coro_scope);
}

/**
 * @class Service
 * @brief Internal base class for services.
 * @ingroup Actor
 * @details Services are special actors, often used as singletons within a core.
 */
class Service : public Actor {
public:
    explicit Service(ServiceId const sid) noexcept;
};

/**
 * @class ServiceActor
 * @ingroup Actor
 * @brief SingletonActor base class, ensuring one instance per VirtualCore per Tag.
 * @tparam Tag A unique, **complete** struct Tag identifying the service type. Write
 *             `struct MyTag {};` first: `ServiceActor<struct MyTag>` only *declares* the tag,
 *             and since 3.0 the service index reaches `typeid(Tag)`, so an incomplete tag is a
 *             compile error in every build mode.
 * @details
 * ServiceActor is a special actor where DerivedActor
 * must define a unique service index by Tag.\n
 * Inherited Service Actors are unique per VirtualCore.
 */
template <typename Tag>
class ServiceActor : public Service {
    friend class Main;
    friend class CoreInitializer;
    friend class VirtualCore;
    static const ServiceId ServiceIndex;

public:
    ServiceActor()
        : Service(ServiceIndex) {}
};

/**
 * @class ActorHandle
 * @ingroup Actor
 * @brief Type-safe, phase-aware handle to a referenced actor hosted on the **same** VirtualCore.
 *
 * @tparam _Actor Concrete actor type this handle resolves to.
 *
 * @details
 * `addRefActor<T>()` returns an `ActorHandle<T>` (the alias `RefActorHandle<T>` is retained for
 * source compatibility). The handle captures the `ActorId` at creation and resolves the live
 * pointer **on demand** through `VirtualCore::findActor<T>()`, so it never hands back a dangling
 * pointer: `get()` is *phase-aware* and returns `nullptr` while the actor is still **Activating**
 * (its async `onInit()` is in flight), after a failed init, or once it has been destroyed.
 *
 * Callers can:
 * - Cheaply send events to the referenced actor via `id()` — valid the instant `addRefActor`
 *   returns, even while the actor is still Activating (events to it are stashed and replayed
 *   FIFO once active; events to ids whose actors have died are dropped by the router).
 * - Safely dereference via `get()` / `operator->()` / `operator*()`, which re-query
 *   `VirtualCore::_handler` and return the pointer only for an **active** actor (`is_active()`).
 * - Wait for an async-init child with `ready()` (sync) or `co_await ready_async(ctx)` (async).
 *
 * This is the *gated* side of the lookup surface; `Actor::getService<T>()` is the ungated one
 * (it hands out an Activating service on purpose). The table on `qb::Actor::is_active()` is the
 * inventory of which is which.
 *
 * Thread-model: must only be dereferenced from the owning VirtualCore's worker
 * thread (the same thread that created the referenced actor). Cross-thread use
 * is a logic error and is asserted on in debug builds.
 */
template <typename _Actor>
class ActorHandle {
    ActorId _id;
    _Actor *_cached = nullptr;

public:
    ActorHandle() noexcept = default;

    /**
     * @brief Wrap a pointer returned by `VirtualCore::addReferencedActor<_Actor>(...)`.
     * @param actor Pointer to an actor on the current VirtualCore (may be nullptr, and may
     *        be one whose async `onInit()` is still in flight — see `ready()`).
     */
    explicit ActorHandle(_Actor *actor) noexcept
        : _id(actor ? actor->id() : ActorId{})
        , _cached(actor) {}

    /**
     * @brief The ActorId of the referenced actor (may be invalid).
     * @details Valid the instant `addRefActor` returns — **even while the actor is still
     *          Activating** — so it is always safe to `push()` to `id()` (the dispatch gate
     *          stashes the event until the actor becomes active and replays it FIFO).
     */
    [[nodiscard]] ActorId
    id() const noexcept {
        return _id;
    }

    /** @brief True iff constructed with a non-null actor (allocation/append succeeded). */
    [[nodiscard]] bool
    valid() const noexcept {
        return _id.is_valid();
    }

    /**
     * @brief Phase-aware pointer accessor.
     * @return The actor pointer iff it is resolved **and** `is_active()` (its `onInit()` has
     *         completed) on the current VirtualCore; otherwise `nullptr` — i.e. nullptr while
     *         the actor is still Activating, and after it has Failed / been destroyed.
     */
    [[nodiscard]] _Actor *get() const noexcept;

    /** @brief True iff the actor is resolved and active (== `get() != nullptr`). */
    [[nodiscard]] bool
    ready() const noexcept {
        return get() != nullptr;
    }

    /**
     * @brief Suspend until the referenced actor becomes active (or the timeout elapses).
     * @param ctx A cancellation-aware context (e.g. the caller's `context()`), so a kill of
     *            the waiting actor unwinds this await cleanly.
     * @param timeout Maximum time to wait. Defaults to 5 s (the activation-deadline scale).
     * @return `true` once the actor is `ready()`; `false` if it did not become active in time
     *         (e.g. its async init failed). Safe to call when already active (returns at once).
     * @details Lets a parent block on an async-init child before using it:
     * @code
     * auto child = addRefActor<DbWorker>(dsn);
     * push(child.id(), Warmup{});           // safe now — stashed until active
     * if (co_await child.ready_async(context())) child->serve();
     * @endcode
     */
    [[nodiscard]] qb::io::async::task<bool>
    ready_async(ScopedCoroContext ctx, qb::duration timeout = std::chrono::seconds{5}) const {
        // Poll the phase oracle on the owning core; ctx.sleep is cancellation-aware so a kill
        // of the waiting actor throws out of here instead of spinning.
        auto           remaining = timeout;
        constexpr auto step      = qb::duration{std::chrono::milliseconds{1}};
        while (!ready() && remaining > qb::duration::zero()) {
            co_await ctx.sleep(step);
            remaining -= step;
        }
        co_return ready();
    }

    /** @brief `get()` asserted ready in debug builds (deref-when-ready). */
    [[nodiscard]] _Actor *
    operator->() const noexcept {
        auto *p = get();
        assert(p && "ActorHandle dereferenced while not active (Activating / Failed / destroyed)");
        return p;
    }

    /** @brief Dereference (undefined unless `ready()`). */
    [[nodiscard]] _Actor &
    operator*() const noexcept {
        auto *p = get();
        assert(p && "ActorHandle dereferenced while not active (Activating / Failed / destroyed)");
        return *p;
    }

    /** @brief `== ready()`. */
    explicit
    operator bool() const noexcept {
        return get() != nullptr;
    }
};

/**
 * @brief Backward-compatible alias for the pre-async-init name.
 * @details `qb::ActorHandle<T>` is the canonical name now that the handle is phase-aware
 *          (its `get()` resolves only an *active* actor). `RefActorHandle` is retained so
 *          existing code keeps compiling.
 */
template <typename _Actor>
using RefActorHandle = ActorHandle<_Actor>;

/**
 * @interface IActorFactory
 * @brief Interface for actor factory classes.
 * @ingroup Actor
 * @details Used internally by the framework to abstract actor construction.
 */
class IActorFactory {
public:
    virtual ~IActorFactory() = default;
    /**
     * @brief Creates an actor instance.
     * @return Pointer to the created Actor.
     */
    virtual Actor *create() = 0;
    /**
     * @brief Checks if the factory creates a service actor.
     * @return True if it creates a service actor, false otherwise.
     */
    [[nodiscard]] virtual bool isService() const = 0;
};

/**
 * @class ActorProxy
 * @brief Internal helper class for actor type and name management.
 * @ingroup Actor
 * @details Provides mechanisms for setting and retrieving type information for actors,
 *          primarily used by the actor factory system.
 */
class ActorProxy {
protected:
    ActorProxy() = default;
    template <typename _Type>
    static void
    setType(Actor &actor) {
        actor.id_type = ActorProxy::getType<_Type>();
    }
    template <typename _Type>
    static void
    setName(Actor &actor) {
        actor.name = ActorProxy::getName<_Type>();
    }

public:
    template <typename _Type>
    static auto
    getType() {
        return type_id<_Type>();
    }
    /**
     * @brief Demangled type name for `_Type`, valid for the whole program.
     * @tparam _Type The type to name.
     * @return A NUL-terminated string that is never freed — safe to store in a bare
     *         `const char *` and to read at any time, from any thread, including during
     *         static destruction.
     * @details
     * The returned pointer is what `setName`/`setTypeInfo` park in `Actor::name`, and
     * `Actor::getName()` builds a `std::string_view` straight from it. Both
     * `operator<<(…, Actor const&)` overloads read it — including the one
     * `VirtualCore::removeActor()` streams (`QB_LOG_INFO("Delete " << *actor)`), which is
     * live whenever logging is compiled in.
     *
     * That pointer therefore has to outlive every reader, and the engine offers no
     * guarantee that it does the reverse: `qb::Main` joins its workers in `~Main`, but a
     * `Main` that outlives `main()` — or one parked on a detached thread, the shape four
     * tests in this suite use — is still turning when the `__cxa_atexit` chain fires.
     *
     * So the cache is **deliberately immortal**: `abi::__cxa_demangle` returns heap, and
     * that heap is handed to a `static const char *const` that has no destructor, hence no
     * `__cxa_atexit` registration and no release at static-destruction time. Holding it in
     * a `static std::unique_ptr<char, void(*)(void*)>` instead — as this did before 3.0 —
     * registered a `free()` that ran on the main thread, at an exit-time position nothing
     * about the engine controls, turning every still-live reader into a use-after-free
     * (proven in `unit/core/actor-name-lifetime.cpp`). A mutex would not help: the storage
     * has to be there, not merely be accessed under a lock.
     *
     * Cost: one allocation per *type*, bounded by the number of actor types in the program
     * and never repeated. It is not a leak LeakSanitizer reports either — `res` is a static
     * root holding the block, so the block stays reachable. This is exactly the contract the
     * non-`__GNUC__` branch below has always had (`typeid(T).name()` is a link-time constant
     * address), and the one `Event.h` documents for its own type-name registry.
     */
    template <typename _Type>
    static const char *
    getName() {
#ifdef __GNUC__
        static const char *const res = [] {
            char *const demangled = abi::__cxa_demangle(typeid(_Type).name(), nullptr, nullptr, nullptr);
            // A demangle failure used to hand back nullptr, which `Actor::getName()` then
            // fed to a std::string_view (UB). The mangled name is a poorer label but a
            // valid, equally immortal one.
            return demangled ? static_cast<const char *>(demangled) : typeid(_Type).name();
        }();

        return res;
#else
        return typeid(_Type).name();
#endif
    }

    /**
     * @brief Set both the type id and demangled name of an actor in one call.
     * @tparam _Type The concrete actor type.
     * @param actor The actor instance whose metadata should be populated.
     * @details
     * Convenience public helper so non-friend code paths (e.g. `VirtualCore::addReferencedActor`)
     * can consistently tag actors with their concrete type without duplicating the
     * `setType`/`setName` access pattern. Grants both operations via a single call.
     */
    template <typename _Type>
    static void
    setTypeInfo(Actor &actor) {
        actor.id_type = ActorProxy::getType<_Type>();
        actor.name    = ActorProxy::getName<_Type>();
    }
};

// ======= Utility: detect std::reference_wrapper<T> =========

template <template <typename...> class Template, typename T>
struct is_specialization_of : std::false_type {};

template <template <typename...> class Template, typename... Args>
struct is_specialization_of<Template, Template<Args...>> : std::true_type {};

/**
 * @concept string_literal
 * @brief Concept for string literal types (const char[N])
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept string_literal = std::is_array_v<T> && std::is_same_v<std::remove_extent_t<T>, const char>;

/**
 * @concept reference_wrapper_type
 * @brief Concept for std::reference_wrapper types
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept reference_wrapper_type = is_specialization_of<std::reference_wrapper, std::decay_t<T>>::value;

/**
 * @struct actor_factory_param
 * @brief Utility struct for processing actor factory constructor arguments
 * @details
 * C++20: uses concepts and explicit specializations instead of nested std::conditional_t.
 * Handles reference wrappers, string literals, and other types for actor constructors.
 *
 * @tparam T The original parameter type to process
 * @ingroup Actor
 */
template <typename T>
struct actor_factory_param {
    /** @brief Type with references removed */
    using no_ref = std::remove_reference_t<T>;

    /** @brief Whether the type is a reference wrapper */
    static constexpr bool is_ref_wrapper = reference_wrapper_type<no_ref>;

    /** @brief The resulting type after transformation */
    // Modern C++: using constexpr if chain in the using declaration is not possible,
    // so we keep the std::conditional_t structure but with cleaner concept-based checks
    using type = std::conditional_t<is_ref_wrapper,
                                    no_ref, // Keep ref_wrapper untouched
                                    std::conditional_t<string_literal<T>,
                                                       std::string,    // string literals → std::string
                                                       std::decay_t<T> // fallback
                                                       >>;
};

/**
 * @brief Utility function for forwarding and transforming arguments to actor factory
 * @details
 * This function properly forwards arguments to the actor factory, handling special cases:
 * - String literals are converted to std::string
 * - Reference wrappers are preserved as-is
 * - Other types are forwarded with their original value categories
 *
 * @tparam T The type of the argument to forward
 * @param val The value to forward
 * @return The transformed and properly forwarded value
 * @ingroup Actor
 */
/**
 * @brief Utility function for forwarding and transforming arguments to actor factory
 * @details
 * C++20: uses string_literal concept for cleaner type checking.
 *
 * @tparam T The type of the argument to forward
 * @param val The value to forward
 * @return The transformed and properly forwarded value
 * @ingroup Actor
 */
template <typename T>
[[nodiscard]] inline auto
actor_factory_forward(T &&val) {
    // C++20: use concept for direct type checking instead of std::is_same_v
    if constexpr (string_literal<T>) {
        return std::string(std::forward<T>(val)); // copy literal to std::string
    } else {
        return std::forward<T>(val); // forward all others
    }
}

/**
 * @brief Customization point for actor allocation.
 * @ingroup Actor
 * @tparam _Actor The actor type to allocate.
 * @tparam _Args  The forwarded constructor argument types.
 * @param  args   The constructor arguments for `_Actor`.
 * @return Pointer to a newly constructed `_Actor`, owned by the caller.
 *
 * @details
 * The default implementation simply calls `new _Actor(args...)`. Users may provide
 * their own specialization (for a specific actor type) or replace this function via
 * an overload discoverable by ADL to plug in a memory pool, arena, or
 * `std::pmr::polymorphic_allocator` for hot-path actor factories.
 *
 * The returned pointer **must be destructible via `delete` using the matching deallocator**
 * for whatever allocation strategy the override uses, because `std::unique_ptr<Actor>`
 * with the default deleter is used downstream. If a custom allocator requires a specific
 * deleter, wrap it via an intrusive `operator delete` override or request framework-level
 * customization beyond this hook.
 *
 * @code
 * // Example: specialize for a specific actor to use a pool.
 * template <>
 * MyHotActor* qb::allocate_actor<MyHotActor>(MyHotActor::Args args) {
 *     return my_pool<MyHotActor>::acquire(std::move(args));
 * }
 * @endcode
 */
template <typename _Actor, typename... _Args>
[[nodiscard]] inline _Actor *
allocate_actor(_Args &&...args) {
    return new _Actor(std::forward<_Args>(args)...);
}

/**
 * @class TActorFactory
 * @brief Templated actor factory implementation.
 * @ingroup Actor
 * @tparam _Actor The concrete Actor type this factory will create.
 * @tparam _Args The argument types for the _Actor's constructor.
 * @details This class is used internally to create actor instances with their
 *          constructor arguments, managing type information via ActorProxy.
 */
template <typename _Actor, typename... _Args>
class TActorFactory
    : public IActorFactory
    , public ActorProxy {
    using Tuple = std::tuple<typename actor_factory_param<_Args>::type...>;

    ActorId _id;
    Tuple   _parameters;

public:
    explicit TActorFactory(ActorId const id, _Args &&...args)
        : _id(id)
        , _parameters(actor_factory_forward<_Args>(std::forward<_Args>(args))...) {}

    Actor *
    create() final {
        return create_impl(std::index_sequence_for<_Args...>{});
    }

    [[nodiscard]] bool
    isService() const final {
        // C++20: use concept instead of std::is_base_of_v
        return service_type<_Actor>;
    }

private:
    template <std::size_t... Is>
    Actor *
    create_impl(std::index_sequence<Is...>) {
        // Routes through the `qb::allocate_actor` customization point so users
        // can plug in pool / arena / PMR strategies without forking the framework.
        auto *actor = qb::allocate_actor<_Actor>(std::get<Is>(_parameters)...);
        ActorProxy::setTypeInfo<_Actor>(*actor);
        return actor;
    }
};

/**
 * @typedef actor
 * @brief Alias for the Actor class
 * @details Provided for naming consistency with other lowercase aliases in the framework
 * @ingroup Actor
 */
using actor = Actor;

/**
 * @typedef service_actor
 * @brief Alias for the ServiceActor template class
 * @details Provided for naming consistency with other lowercase aliases in the framework
 * @tparam Tag A unique, **complete** struct Tag identifying the service type (see `ServiceActor`)
 * @ingroup Actor
 */
template <typename Tag>
using service_actor = ServiceActor<Tag>;

/**
 * @typedef coro_context
 * @brief Alias for the CoroContext class
 * @details Provided for naming consistency with other lowercase aliases in the framework
 * @ingroup Actor
 */
using coro_context = CoroContext;

/**
 * @typedef scoped_coro_context
 * @brief Alias for the ScopedCoroContext class
 * @details Provided for naming consistency with other lowercase aliases in the framework
 * @ingroup Actor
 */
using scoped_coro_context = ScopedCoroContext;

/**
 * @typedef ask_event
 * @brief Alias for the AskEvent class
 * @details Provided for naming consistency with other lowercase aliases in the framework
 * @ingroup Actor
 */
using ask_event = AskEvent;

#ifdef QB_WITH_LOGGING
qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::Actor const &actor);
#endif

/**
 * @brief Stream output operator for Actor objects
 * @details Formats and outputs actor information to a stream
 *
 * @param os Output stream to write to
 * @param actor The Actor object to format and output
 * @return Reference to the output stream
 * @ingroup Actor
 */
std::ostream &operator<<(std::ostream &os, qb::Actor const &actor);

} // namespace qb

#endif // QB_ACTOR_H
