/**
 * @file qb/core/Actor.h
 * @brief Actor base class and core actor model implementation
 *
 * This file defines the core Actor class which serves as the base class for all actors
 * in the QB Actor Framework. It implements the fundamental actor model concepts
 * including message passing via events, lifecycle management, and actor identification.
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

#ifndef QB_ACTOR_H
#define QB_ACTOR_H
#include <atomic>
#include <cassert>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>
// include from qb
#include <qb/system/container/unordered_map.h>
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
class Actor;  // Forward declaration for concepts
template <typename _Actor> class RefActorHandle;  // Forward for Actor::addRefHandle

/**
 * @brief Tag type used to opt out of registering the four default event handlers
 *        (KillEvent, SignalEvent, UnregisterCallbackEvent, PingEvent) on construction.
 * @ingroup Actor
 * @details
 * Pass `qb::no_default_events` to the protected `Actor` constructor to create a
 * lightweight actor that registers no system events. This is useful for pools of
 * short-lived actors where the router bookkeeping is a measurable overhead.
 * Power users opting in are expected to register the events they actually need in
 * `onInit()` (typically at least `KillEvent` for graceful shutdown).
 */
struct no_default_events_t {
    explicit constexpr no_default_events_t() noexcept = default;
};

/** @brief Inline constexpr tag value; see `qb::no_default_events_t`. @ingroup Actor */
inline constexpr no_default_events_t no_default_events{};

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

/**
 * @brief Concept for service event types (forwardable events)
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept service_event_type = std::is_base_of_v<ServiceEvent, T>;

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
 *     bool onInit() override {
 *         // Register event handlers
 *         registerEvent<IncrementEvent>(*this);
 *         registerEvent<qb::KillEvent>(*this);
 *         LOG_INFO("MyActor initialized with ID: " << id());
 *         return true;
 *     }
 *
 *     void on(const IncrementEvent& event) {
 *         counter += event.amount;
 *         LOG_INFO("Counter updated to: " << counter);
 *     }
 *
 *     void on(const qb::KillEvent& event) {
 *         LOG_INFO("MyActor shutting down...");
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

    const char   *name = "unnamed";
    ActorId       _id;
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
    mutable bool  _alive  = true;
    std::uint32_t id_type = 0u;

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
     * @tparam Tag Type tag for the service
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
     * `KillEvent`, `SignalEvent`, `UnregisterCallbackEvent`, or `PingEvent`.
     * The derived class is fully responsible for registering in `onInit()` any
     * system events it expects to receive (it is strongly recommended to at least
     * register `KillEvent` to enable graceful shutdown via `Main::stop()`).
     *
     * Intended for high-throughput scenarios where actors are short-lived and the
     * four default subscriptions per actor become measurable overhead.
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
     * @brief Initialization callback, called once after construction and ID assignment.
     *
     * This method is called when the actor is added to the system, before it starts
     * processing any events. Override this method to perform initialization tasks such as
     * registering event handlers and setting up actor state.
     *
     * @return true if initialization was successful, which allows the actor to start.
     * @return false if initialization failed, which prevents the actor from
     *         being added to the engine and leads to its immediate destruction.
     * @details Crucial for `registerEvent<EventType>(*this)` calls.
     * Example:
     * @code
     * bool onInit() override {
     *   // Register events
     *   registerEvent<CustomEvent>(*this);
     *   registerEvent<qb::KillEvent>(*this); // Important for graceful shutdown
     *
     *   // Initialize resources or state
     *   _my_resource = std::make_unique<MyResource>();
     *   if (!_my_resource) {
     *     LOG_CRIT("Failed to allocate MyResource for Actor " << id());
     *     return false;  // Initialization failed
     *   }
     *
     *   LOG_INFO("Actor " << id() << " initialized successfully.");
     *   return true;     // Initialization successful
     * }
     * @endcode
     */
    virtual bool
    onInit() {
        return true;
    };

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
     * Example of overriding:
     * @code
     * void on(qb::KillEvent const &event) override {
     *   LOG_INFO("Actor " << id() << " cleaning up before termination...");
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
     * Default handler for system signals (e.g., SIGINT) that terminates the actor.
     * Derived classes can override this handler to perform custom signal handling.
     * If overridden, ensure proper state management or actor termination if needed.
     *
     * @param event The received signal event, containing `event.signum`.
     * Example of overriding:
     * @code
     * void on(qb::SignalEvent const &event) override {
     *   if (event.signum == SIGINT) {
     *     LOG_INFO("Actor " << id() << " received SIGINT, performing graceful shutdown.");
     *     // Custom shutdown logic here...
     *     kill(); // Terminate the actor
     *   } else if (event.signum == SIGUSR1) {
     *     LOG_INFO("Actor " << id() << " received SIGUSR1, reloading configuration.");
     *     reloadConfig();
     *   } else {
     *     LOG_WARN("Actor " << id() << " received unhandled signal: " << event.signum);
     *     // Default behavior for other signals might be to kill, or call base:
     *     // Actor::on(event); 
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
     * @details
     * This value is optimized and cached/updated by the `VirtualCore` at the beginning of each
     * of its processing loops. Thus, multiple calls to `time()` within the same event handler
     * or `onCallback()` invocation will return the *same* timestamp.
     * @code
     * // ...
     * auto t1 = time();
     * // ... some heavy calculation ...
     * assert(t1 == time()); // true - will not assert within the same event handler execution
     * @endcode
     * For a continuously updating, high-precision timestamp, use `qb::unix_nanos(qb::wall_now())` from `<qb/system/timestamp.h>`.
     * @note This time is primarily for relative measurements or logging within an actor's turn.
     */
    [[nodiscard]] uint64_t time() const noexcept;

    /**
     * @private
     */
    template <typename T>
    [[nodiscard]] static ActorId getServiceId(CoreId index) noexcept;

    /**
     * @brief Get direct access to ServiceActor* in same core
     * @return ptr to _ServiceActor else nullptr if not registered in core
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
    [[nodiscard]] bool is_alive() const noexcept;

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
     * The registered `onCallback()` method (from `qb::ICallback`) will be called
     * by the `VirtualCore` during each of its processing loop iterations, after event processing.
     * This allows the actor to perform periodic tasks or background operations.
     * The callback remains active until explicitly unregistered or the actor is terminated.
     * @note Ensure the `onCallback()` implementation is fast and non-blocking.
     * @see qb::ICallback
     * @code
     * class MyPollingActor
     *   : public qb::Actor
     *   , public qb::ICallback // Must inherit from ICallback
     * {
     * public:
     *   bool onInit() override {
     *     registerCallback(*this); // Register self for periodic callbacks
     *     return true;
     *   }
     *   
     *   void onCallback() override {
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
     * Stops the periodic invocation of the actor's `onCallback()` method.
     * It is safe to call this even if no callback is currently registered.
     * @note Can be called from within `onCallback()` to self-terminate the callback cycle,
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
     * // bool onInit() override {
     * //   registerEvent<MyCustomEvent>(*this);
     * //   registerEvent<AnotherEvent>(*this);
     * //   return true;
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
     * Supports events with non-trivially destructible members (e.g., `std::string`, `std::vector`).
     * @code
     * // ActorId target_id = GetSomeActorId();
     * // auto& my_evt = push<MyDataEvent>(target_id, initial_value);
     * // my_evt.data_field = 42; // Modify event before it's sent
     * // my_evt.message = "Hello";
     * //
     * // push<AnotherEvent>(target_id); // This will be processed by target_id after my_evt
     * @endcode
     * @note If the event type has a non-trivial destructor, the framework ensures it is called appropriately.
     * @attention Do not store the returned reference beyond the current scope, as the event object's lifetime
     *            is managed by the framework after it's sent.
     * @warning `noexcept`: an allocation failure while growing the pipe buffer or
     *          constructing the event (e.g. under OOM) cannot be reported and calls
     *          `std::terminate()`. Keep events small / allocation-light. See
     *          `qb::Pipe::push` for the full contract (applies to `send`/`broadcast` too).
     */
    template <typename _Event, typename... _Args>
    _Event &push(ActorId const &dest, _Args &&...args) const noexcept;

    /**
     * @brief Send a new event in an unordered fashion to a destination actor.
     * @tparam _Event The type of event to create and send (must derive from `qb::Event` and be trivially destructible).
     * @tparam _Args Types of arguments to forward to the `_Event` constructor.
     * @param dest The `ActorId` of the destination actor.
     * @param args Arguments to forward to the constructor of `_Event`.
     * @details
     * Events sent using `send()` are not guaranteed to be received in the order they were sent,
     * even if sent to the same destination from the same source. This method may offer slightly
     * lower latency for same-core communication in specific scenarios but sacrifices ordering.
     * @note The `_Event` type **must be trivially destructible** (e.g., contain only POD types or `qb::string`).
     *       `std::string`, `std::vector`, etc., are not permitted.
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
    [[nodiscard]] _Event build_event(qb::ActorId const source,
                                     _Args &&...args) const noexcept;

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
     * @brief Request discovery of other actors of specified types.
     * @tparam _Actors Variadic template pack of actor types to discover.
     * @return `true` if the discovery ping was successfully broadcasted for all types.
     * @details
     * For each type in `_Actors`, this method broadcasts a `PingEvent`.
     * Live actors of the specified types will respond with a `RequireEvent`,
     * which this actor must be registered to handle (via `registerEvent<RequireEvent>(*this)`).
     * The `on(RequireEvent&)` handler can then use `is<_ActorType>(event)` to identify responses.
     * @code
     * // bool onInit() override {
     * //   registerEvent<qb::RequireEvent>(*this);
     * //   require<ServiceA, ServiceB>(); // Discover ServiceA and ServiceB instances
     * //   return true;
     * // }
     * //
     * // void on(const qb::RequireEvent& event) {
     * //   if (is<ServiceA>(event) && event.status == qb::ActorStatus::Alive) {
     * //     // _service_a_id = event.getSource();
     * //   } else if (is<ServiceB>(event) && event.status == qb::ActorStatus::Alive) {
     * //     // _service_b_id = event.getSource();
     * //   }
     * // }
     * @endcode
     * @see qb::PingEvent, qb::RequireEvent
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
     * // auto& ev2 = comm_pipe.allocated_push<LargeEvent>(data_size, constructor_args_for_large_event);
     * @endcode
     * @see qb::Pipe
     * @see Pipe::push
     * @see Pipe::allocated_push
     */
    [[nodiscard]] Pipe getPipe(ActorId dest) const noexcept;

    /**
     * @brief Create and initialize a new referenced actor on the same VirtualCore.
     * @tparam _Actor The concrete derived actor type to create (must inherit from `qb::Actor`).
     * @tparam _Args Types of arguments to forward to the `_Actor`'s constructor.
     * @param args Arguments to forward to the constructor of `_Actor`.
     * @return A raw pointer to the newly created `_Actor` instance if successful (i.e., its `onInit()` returned `true`), 
     *         otherwise `nullptr`.
     * @details
     * Referenced actors are created on the same `VirtualCore` as the calling (parent) actor.
     * The parent receives a raw pointer to the child actor. This allows for direct method calls
     * from the parent to the child, bypassing the event queue for the child.
     * The child actor still has its own `ActorId` and can receive events normally.
     * @note The parent actor does **not** own the referenced actor. The referenced actor manages its
     *       own lifecycle and must call `kill()` to terminate. The parent must be aware that the
     *       pointer can become dangling if the child terminates independently.
     * @code
     * // // Inside ParentActor
     * // HelperActor* _helper = addRefActor<HelperActor>(initial_config_for_helper);
     * // if (_helper) {
     * //   // Helper created successfully. Can send events:
     * //   push<TaskEvent>(_helper->id(), task_data);
     * //   // Or, cautiously, call public methods (bypasses event queue):
     * //   // _helper->doSomethingSynchronously(); 
     * // } else {
     * //   // LOG_CRIT("Failed to create HelperActor for ParentActor " << id());
     * // }
     * @endcode
     * @attention Direct method calls on the referenced actor bypass its event queue and mailbox,
     *            which can break actor model guarantees if not handled with extreme care.
     *            Prefer sending events to the child's ID (`child_actor->id()`) for most interactions.
     */
    template <typename _Actor, typename... _Args>
    _Actor *addRefActor(_Args &&...args) const;

    /**
     * @brief Create a referenced actor and wrap it in a safe `RefActorHandle<T>` (finding 2.9).
     * @tparam _Actor Actor type to create.
     * @tparam _Args  Constructor argument types.
     * @param args Constructor arguments.
     * @return `RefActorHandle<_Actor>` — empty if creation failed.
     *
     * @details
     * Prefer this over the raw `addRefActor<T>()` whenever the parent actor keeps
     * the handle across event-handler boundaries: the handle performs an O(1)
     * liveness check on dereference, avoiding dangling-pointer UB if the referenced
     * actor kills itself.
     */
    template <typename _Actor, typename... _Args>
    [[nodiscard]] RefActorHandle<_Actor> addRefHandle(_Args &&...args) const {
        return RefActorHandle<_Actor>(addRefActor<_Actor>(std::forward<_Args>(args)...));
    }

    /**
     * @name Coroutine Support
     * C++23 coroutine integration for async I/O operations.
     * @{
     */

    /**
     * @brief Launch an async coroutine in isolated context
     *
     * This is the ONLY way to use coroutines within an Actor.
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
     *     spawn_async([key, sender](auto ctx) -> qb::io::async::task<void> {
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
     *     spawn_async([this](auto ctx) -> qb::io::async::task<void> {
     *         co_await sleep(100ms);  // Actor may die here
     *         
     *         // ❌ CRASH: accessing this->_member after suspension!
     *         this->_member = value;  // UNDEFINED BEHAVIOR
     *     });
     * }
     */
    template <typename Func>
    void spawn_async(Func&& func) const;

    /**
     * @brief Check if actor has active coroutines
     * @return true if any coroutines are still running
     */
    [[nodiscard]] bool has_active_coroutines() const {
        return active_coroutines_ && active_coroutines_->load(std::memory_order_relaxed) > 0;
    }

    /**
     * @brief Get number of active coroutines
     * @return Count of running coroutines
     */
    [[nodiscard]] std::size_t active_coroutine_count() const {
        return active_coroutines_ ? active_coroutines_->load(std::memory_order_relaxed) : 0;
    }

    /**
     * @brief Get the coroutine scheduler for this actor
     * @return Pointer to scheduler, or nullptr if not initialized
     * @private Internal use by VirtualCore for workflow integration
     */
    [[nodiscard]] qb::io::async::CoroutineScheduler* get_coro_scheduler() const {
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
    mutable qb::io::async::CoroutineScheduler* coro_scheduler_ = nullptr;

    /**
     * @brief Shared count of active coroutines.
     *
     * Uses `shared_ptr` so coroutine RAII guards can safely decrement even
     * after the actor is destroyed (the shared_ptr keeps the counter alive
     * until the last orphaned coroutine frame is destroyed).
     *
     * @note Eagerly allocated in the Actor constructor (finding 2.12) — this
     *       trades one heap allocation per actor construction for the removal
     *       of a `nullptr` check on every `spawn_async()` hot path. A single
     *       `make_shared` is a negligible cost compared to the rest of actor
     *       construction and guarantees branch-predicted coroutine spawning.
     */
    mutable std::shared_ptr<std::atomic<std::size_t>> active_coroutines_ =
        std::make_shared<std::atomic<std::size_t>>(0);
};

/**
 * @class CoroContext
 * @brief Safe context for coroutines spawned via spawn_async()
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
    explicit CoroContext(Actor const *actor) : actor_id_(actor->id()) {}

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
     * @brief Get the spawning actor's ID (captured at construction)
     * @return ActorId
     */
    [[nodiscard]] ActorId id() const noexcept { return actor_id_; }

    /**
     * @brief Get current time from the VirtualCore
     * @return Timestamp in nanoseconds
     */
    [[nodiscard]] uint64_t time() const noexcept;
};

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
 * @tparam Tag A unique struct Tag to identify the service type.
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
 * @class RefActorHandle
 * @ingroup Actor
 * @brief Type-safe weak reference to a referenced actor hosted on the **same** VirtualCore.
 *
 * @tparam _Actor Concrete actor type this handle resolves to.
 *
 * @details
 * `addRefActor<T>()` returns a raw `T*` whose lifetime is tied to the enclosing
 * `VirtualCore`. That pointer becomes dangling as soon as the referenced actor
 * calls `kill()` — any subsequent dereference is Undefined Behaviour (finding 2.9).
 *
 * `RefActorHandle<T>` captures both the `ActorId` **and** the cached raw pointer at
 * creation time, so callers can:
 *
 * - Cheaply send events to the referenced actor via `id()` without resolving the
 *   pointer (events are safe to dispatch to ids whose actors have died — they are
 *   simply dropped by the router).
 * - Safely dereference via `get()` / `operator->()` / `operator*()`, which re-query
 *   `VirtualCore::_handler` to confirm the actor is still registered and alive on
 *   the current core before returning the cached pointer.
 *
 * Thread-model: must only be dereferenced from the owning VirtualCore's worker
 * thread (the same thread that created the referenced actor). Cross-thread use
 * is a logic error and is asserted on in debug builds.
 */
template <typename _Actor>
class RefActorHandle {
    ActorId  _id;
    _Actor  *_cached = nullptr;

public:
    RefActorHandle() noexcept = default;

    /**
     * @brief Wrap a pointer returned by `Actor::addRefActor<_Actor>(...)`.
     * @param actor Pointer to a live actor on the current VirtualCore (may be nullptr).
     */
    explicit RefActorHandle(_Actor *actor) noexcept
        : _id(actor ? actor->id() : ActorId{})
        , _cached(actor) {}

    /** @brief The ActorId of the referenced actor (may be invalid). */
    [[nodiscard]] ActorId id() const noexcept { return _id; }

    /** @brief True iff constructed with a non-null actor. */
    [[nodiscard]] bool valid() const noexcept { return _id.is_valid(); }

    /**
     * @brief Safe pointer accessor.
     * @return Cached pointer if the actor is still alive on the current
     *         VirtualCore, otherwise `nullptr`.
     */
    [[nodiscard]] _Actor *get() const noexcept;

    /** @brief `get()` but asserted non-null in debug builds. */
    [[nodiscard]] _Actor *operator->() const noexcept {
        auto *p = get();
        assert(p && "RefActorHandle dereferenced after referenced actor was destroyed");
        return p;
    }

    /** @brief Dereference (undefined if `get() == nullptr`). */
    [[nodiscard]] _Actor &operator*() const noexcept {
        auto *p = get();
        assert(p && "RefActorHandle dereferenced after referenced actor was destroyed");
        return *p;
    }

    explicit operator bool() const noexcept { return get() != nullptr; }
};

/**
 * @interface IActorFactory
 * @brief Interface for actor factory classes.
 * @ingroup Actor
 * @details Used internally by the framework to abstract actor construction.
 */
class IActorFactory {
public:
    virtual ~IActorFactory()                     = default;
    /**
     * @brief Creates an actor instance.
     * @return Pointer to the created Actor.
     */
    virtual Actor             *create()          = 0;
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
    template <typename _Type>
    static const char *
    getName() {
#ifdef __GNUC__
        static std::unique_ptr<char, void (*)(void *)> res{
            abi::__cxa_demangle(typeid(_Type).name(), nullptr, nullptr, nullptr),
            std::free};

        return res.get();
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
concept string_literal = std::is_array_v<T> &&
                         std::is_same_v<std::remove_extent_t<T>, const char>;

/**
 * @concept reference_wrapper_type
 * @brief Concept for std::reference_wrapper types
 * @ingroup Concepts
 * @tparam T Type to check
 */
template <typename T>
concept reference_wrapper_type =
    is_specialization_of<std::reference_wrapper, std::decay_t<T>>::value;

/**
 * @struct actor_factory_param
 * @brief Utility struct for processing actor factory constructor arguments
 * @details
 * C++23: Uses concepts and explicit specializations instead of nested std::conditional_t.
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
    // C++23: Using constexpr if chain in the using declaration is not possible,
    // so we keep the std::conditional_t structure but with cleaner concept-based checks
    using type = std::conditional_t<
        is_ref_wrapper,
        no_ref, // Keep ref_wrapper untouched

        std::conditional_t<
            string_literal<T>,
            std::string,  // string literals → std::string

            std::decay_t<T> // fallback
            >
        >;
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
 * C++23: Uses string_literal concept for cleaner type checking.
 *
 * @tparam T The type of the argument to forward
 * @param val The value to forward
 * @return The transformed and properly forwarded value
 * @ingroup Actor
 */
template <typename T>
[[nodiscard]] inline auto actor_factory_forward(T&& val) {
    // C++23: Use concept for direct type checking instead of std::is_same_v
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
class TActorFactory : public IActorFactory, public ActorProxy {
    using Tuple = std::tuple<typename actor_factory_param<_Args>::type...>;

    ActorId _id;
    Tuple _parameters;

public:
    explicit TActorFactory(ActorId const id, _Args&&... args)
        : _id(id)
        , _parameters(actor_factory_forward<_Args>(std::forward<_Args>(args))...) {}

    Actor* create() final {
        return create_impl(std::index_sequence_for<_Args...>{});
    }

    [[nodiscard]] bool isService() const final {
        // C++23: Use concept instead of std::is_base_of_v
        return service_type<_Actor>;
    }

private:
    template <std::size_t... Is>
    Actor* create_impl(std::index_sequence<Is...>) {
        // Routes through the `qb::allocate_actor` customization point so users
        // can plug in pool / arena / PMR strategies without forking the framework.
        auto* actor = qb::allocate_actor<_Actor>(std::get<Is>(_parameters)...);
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
 * @tparam Tag A unique struct Tag to identify the service type
 * @ingroup Actor
 */
template <typename Tag>
using service_actor = ServiceActor<Tag>;

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
