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
#include <map>
#include <tuple>
#include <utility>
#include <vector>
// include from qb
#include <qb/system/container/unordered_map.h>
#include <qb/utility/nocopy.h>
#include <qb/utility/type_traits.h>
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

/**
 * @class Actor
 * @brief Base class for all actors in the qb framework
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
 *
 * @ingroup Actor
 */
class Actor : nocopy {
    friend class VirtualCore;
    friend class ActorProxy;
    friend class Service;

    const char   *name = "unnamed";
    ActorId       _id;
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
     * @brief Virtual destructor
     *
     * Ensures proper cleanup of derived actor classes.
     */
    virtual ~Actor() noexcept = default;

    /**
     * @brief Initialization callback
     *
     * This method is called when the actor is added to the system.
     * Override this method to perform initialization tasks such as
     * registering event handlers and setting up actor state.
     *
     * Example:
     * @code
     * bool onInit() override {
     *   // Register events
     *   registerEvent<CustomEvent>(*this);
     *   registerEvent<qb::KillEvent>(*this);
     *
     *   // Initialize resources or state
     *   myResource = allocateResource();
     *   if (!myResource)
     *     return false;  // Initialization failed
     *
     *   return true;     // Initialization successful
     * }
     * @endcode
     *
     * @return true if initialization was successful
     * @return false if initialization failed, which prevents the actor from
     *         being added to the engine
     */
    virtual bool
    onInit() {
        return true;
    };

public:
    /**
     * @brief Terminate this actor
     *
     * Marks the actor for removal from the system. After calling this method,
     * the actor will no longer receive events and will be cleaned up by the
     * framework.
     *
     * This method is typically called from event handlers when the actor
     * needs to terminate itself, or it can be triggered by sending a KillEvent
     * to the actor.
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
     * @brief Handler for KillEvent
     *
     * Default handler for the KillEvent which terminates the actor.
     * Derived classes can override this handler to perform cleanup
     * before termination, but should typically call kill() at the end
     * of their implementation.
     *
     * Example of overriding:
     * @code
     * void on(qb::KillEvent const &event) override {
     *   // Perform cleanup
     *   closeConnections();
     *   releaseResources();
     *
     *   // Finally, kill the actor
     *   kill();
     * }
     * @endcode
     *
     * @param event The received kill event
     */
    void on(KillEvent const &event) noexcept;

    /**
     * @brief Handler for SignalEvent
     *
     * Default handler for system signals that terminates the actor on signals
     * like SIGINT. Derived classes can override this handler to perform custom
     * signal handling.
     *
     * Example of overriding:
     * @code
     * void on(qb::SignalEvent const &event) override {
     *   if (event.signum == SIGINT) {
     *     // Handle interrupt signal
     *     LOG_INFO("Received interrupt signal, performing graceful shutdown");
     *     kill();
     *   } else if (event.signum == SIGUSR1) {
     *     // Handle custom signals
     *     LOG_INFO("Received SIGUSR1, reloading configuration");
     *     reloadConfig();
     *   }
     * }
     * @endcode
     *
     * @param event The received signal event
     */
    void on(SignalEvent const &event) noexcept;

    /**
     * @brief Handler for UnregisterCallbackEvent
     *
     * This handler unregisters a previously registered callback.
     * It should not be overridden by derived classes.
     *
     * @param event The received unregister callback event
     */
    void on(UnregisterCallbackEvent const &event) noexcept;

    /**
     * @brief Handler for PingEvent
     *
     * Responds to ping requests, used for actor alive checks and diagnostics.
     *
     * @param event The received ping event
     */
    void on(PingEvent const &event) noexcept;

    /**
     * @}
     */

public:
    /**
     * @class EventBuilder
     * @brief Helper class for building and sending events to actors
     *
     * This class simplifies the process of sending multiple events to a target
     * actor. It provides a fluent interface for chaining event sends, ensuring
     * that events are delivered in the order they are pushed.
     *
     * @ingroup Event
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
        template <typename _Event, typename... _Args>
        EventBuilder &push(_Args &&...args) noexcept;
    };

    /**
     * @name Public Accessors
     * @{
     */

    /**
     * Get ActorId
     * @return ActorId of this actor
     */
    [[nodiscard]] ActorId
    id() const noexcept {
        return _id;
    }

    /**
     * Get core index
     * @return core index where this actor is running
     */
    [[nodiscard]] CoreId getIndex() const noexcept;

    /**
     * Get derived class name
     * @return name as string_view of this actor's class
     */
    [[nodiscard]] std::string_view getName() const noexcept;

    /**
     * @brief Get the set of cores that this actor can communicate with
     * @return Set of core IDs this actor can communicate with
     */
    [[nodiscard]] const CoreIdSet &getCoreSet() const noexcept;

    /**
     * @brief Get current time
     * @return nano timestamp since epoch
     * @details
     * @note
     * This value is optimized and updated each VirtualCore loop.
     * @code
     * // ...
     * auto t1 = time();
     * // ... some heavy calculation
     * assert(t1 == time()); // true - will not assert
     * @endcode
     * To get precise time use NanoTimestamp.
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
     * @brief Check if Actor is alive
     * @return true if Actor is alive else false
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
     * @brief Register a looped callback
     * @param actor reference of DerivedActor
     * @details
     * The registered callback will be called each VirtualCore loop.\n
     * _Actor must inherit and implement ICallback interface.\n
     * example:
     * @code
     * class MyActor
     * : public qb::Actor
     * , public qb::ICallback
     * {
     *   virtual bool onInit() {
     *     registerCallback(*this);
     *   }
     * // ...
     *   virtual void onCallback() final {
     *     // do something
     *   }
     * }
     * // ...
     * }
     * @endcode
     */
    template <typename _Actor>
    void registerCallback(_Actor &actor) const noexcept;

    /**
     * @brief Unregister actor callback
     * @param actor reference of DerivedActor
     * @details
     * example:
     * @code
     * unregisterCallback(*this);
     * @endcode
     */
    template <typename _Actor>
    void unregisterCallback(_Actor &actor) const noexcept;

    /**
     * @private
     */
    void unregisterCallback() const noexcept;

    /**
     * @brief Actor will listen on new _Event
     * @tparam _Event DerivedEvent type
     * @param actor reference of DerivedActor
     * @details
     * example:
     * @code
     * virtual bool onInit() {
     *   registerEvent<MyEvent>(*this);
     *   // ...
     * }
     * @endcode
     * @note
     * _Actor must define the callback event function.
     * @code
     * void on(MyEvent &event) {
     *   // do something
     * }
     * @endcode
     */
    template <typename _Event, typename _Actor>
    void registerEvent(_Actor &actor) const noexcept;

    /**
     * @brief Actor will stop listening _Event
     * @tparam _Event DerivedEvent type
     * @param actor reference of DerivedActor
     * @details
     * example:
     * @code
     * unregisterEvent<MyEvent>(*this);
     * @endcode
     */
    template <typename _Event, typename _Actor>
    void unregisterEvent(_Actor &actor) const noexcept;

    /**
     * @private
     * @tparam _Event
     */
    template <typename _Event>
    void unregisterEvent() const noexcept;

    /**
     * @brief Get EventBuilder for ActorId destination
     * @param dest ActorId destination
     * @return EventBuilder for chaining event pushes
     * @details
     * A way to push chained events to destination Actor.\n
     * example:
     * @code
     * to(destId)
     * .push<MyEvent1>()
     * .push<MyEvent2>(param1, param2)
     * // ...
     * ;
     * @endcode
     * @attention
     * @code
     * auto builder1 = main.to(sameid);
     * auto builder2 = main.to(sameid);
     * // builder1 == builder2 -> true
     * @endcode
     */
    [[nodiscard]] EventBuilder to(ActorId dest) const noexcept;

    /**
     * @brief Send a new ordered event
     * @tparam _Event DerivedEvent type
     * @param dest destination ActorId
     * @param args arguments to forward to the constructor of the _Event
     * @return a reference to the constructed _Event to send
     * @details
     * All events pushed to same actors id in context will be received ordered by push
     * order.\n example:
     * @code
     * // ...
     * auto &e = push<MyEvent>(id_1); // (1) first push
     * e.some_data = 1337; // set my event data without using constructor
     * push<MyEvent>(id_2, param2); // (2) id_2 != id_1 /!\ possible to be received
     * before (1) push<MyEvent>(id_1, param3); // (3) Guaranteed to be received after (1)
     * // ...
     * @endcode
     * @note
     * Pushed events are sent at the end of the core loop.
     * @attention
     * /!\ We recommend to non advanced users to use only this function to send events.
     */
    template <typename _Event, typename... _Args>
    _Event &push(ActorId const &dest, _Args &&...args) const noexcept;

    /**
     * @brief Send a new unordered event
     * @tparam _Event DerivedEvent type
     * @param dest destination ActorId
     * @param args arguments to forward to the constructor of the _Event
     * @details
     * All events sent using send function are not guaranteed to be received in order.\n
     * example:
     * @code
     * // ...
     * send<MyEvent>(id_1, param1); // (1)
     * send<MyEvent>(id_1, param2); // (2)
     * send<MyEvent>(id_1, param3); // (3)
     * // Actor with id_1 will receive events in random order
     * // ...
     * @endcode
     * @note
     * send may be faster than push in some cases.
     * @attention
     * /!\ We recommend to non advanced users to not use this function to send events.
     */
    template <typename _Event, typename... _Args>
    void send(ActorId const &dest, _Args &&...args) const noexcept;

    /**
     * @brief build local event
     * @tparam _Event Event type
     * @param source ActorId source
     * @param args arguments to forward to the constructor of the _Event
     * @return a local constructed _Event
     * @details
     * Local events are used to make a direct call of on callbacks.\n example:
     * @code
     * // ...
     * auto e = local<MyEvent>(id()); // (1) build local event
     * e.some_data = 1337; // (2) set my event data without using constructor
     * on(e); // (3) call directly
     * // ...
     * @endcode
     * @note
     * event dest_id will be set to id of actor builder
     * @attention
     * /!\ We recommend to non advanced users to use only this function to send events.
     */
    template <typename _Event, typename... _Args>
    [[nodiscard]] _Event build_event(qb::ActorId const source,
                                     _Args &&...args) const noexcept;

    template <typename _Type>
    [[nodiscard]] inline bool
    is(uint32_t const id) const noexcept {
        return id == type_id<_Type>();
    }

    template <typename _Type>
    [[nodiscard]] inline bool
    is(RequireEvent const &event) const noexcept {
        return event.type == type_id<_Type>();
    }

    template <typename... _Actors>
    bool require() const noexcept;

    template <typename _Event, typename... _Args>
    void broadcast(_Args &&...args) const noexcept;

    /**
     * @brief Reply an event
     * @param event any received event
     * @details
     * example:
     * @code
     * // ...
     * void on(MyEvent &event) {
     *   // do something...
     *   reply(event);
     * }
     * @endcode
     * @note
     * Replying an event is faster than pushing a new one.
     */
    void reply(Event &event) const noexcept;

    /**
     * @brief Forward an event
     * @param dest destination ActorId
     * @param event any received event
     * @details
     * example:
     * @code
     * // ...
     * void on(MyEvent &event) {
     *   // do something...
     *   forward(id_1, event);
     * }
     * @endcode
     * @note
     * Forwarding an event is faster than pushing a new one.
     */
    void forward(ActorId dest, Event &event) const noexcept;

    // OpenApi : used for module
    void send(Event const &event) const noexcept;
    void push(Event const &event) const noexcept;
    bool try_send(Event const &event) const noexcept;

    /**
     * @brief Get access to unidirectional out events pipe
     * @param dest destination ActorId
     * @return destination Pipe
     * @details
     * If you want to send several events to same Actor or push dynamic sized events,\n
     * Actor API allows to retrieve a Pipe to a desired Actor.\n
     * more details on Pipe section
     */
    [[nodiscard]] Pipe getPipe(ActorId dest) const noexcept;

    /**
     * @brief Create new referenced _Actor
     * @tparam _Actor DerivedActor type
     * @param args arguments to forward to the constructor of the _Actor
     * @return _Actor * on success or nullptr on failure
     * @details
     * create and initialize new _Actor on same VirtualCore as the callee Actor.\n
     * example:
     * @code
     * auto actor = addRefActor<MyActor>(param1, param2);
     * if (actor) {
     *   // actor was created and pushed to the engine
     * }
     * @endcode
     * @note
     * Referenced actors can be used as a normal C++ class,
     * the parent Actor can make direct calls to created actors "on" functions.\n
     * This is very useful to limit the number of events managed by the engine
     * then improve global performance.
     */
    template <typename _Actor, typename... _Args>
    _Actor *addRefActor(_Args &&...args) const;

    /**
     * @}
     */
};

/**
 * @class Service
 * @brief internal
 */
class Service : public Actor {
public:
    explicit Service(ServiceId sid);
};

/**
 * @class ServiceActor actor.h qb/actor.h
 * @ingroup Core
 * @brief SingletonActor base class
 * @tparam Tag is a uniq struct Tag
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

class IActorFactory {
public:
    virtual ~IActorFactory()                     = default;
    virtual Actor             *create()          = 0;
    [[nodiscard]] virtual bool isService() const = 0;
};

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
};

// ======= Utility: detect std::reference_wrapper<T> =========

template <template <typename...> class Template, typename T>
struct is_specialization_of : std::false_type {};

template <template <typename...> class Template, typename... Args>
struct is_specialization_of<Template, Template<Args...>> : std::true_type {};

// ======= Meta: type stored in tuple per argument ===========

template <typename T>
struct actor_factory_param {
    using no_ref = std::remove_reference_t<T>;

    static constexpr bool is_ref_wrapper =
        is_specialization_of<std::reference_wrapper, std::decay_t<no_ref>>::value;

    using type = std::conditional_t<
        is_ref_wrapper,
        no_ref, // Keep ref_wrapper untouched

        std::conditional_t<
            std::is_array_v<T> && std::is_same_v<std::remove_extent_t<T>, const char>,
            std::string,  // string literals → std::string

            std::decay_t<T> // fallback
            >
        >;
};

// ======= Argument transformer: cast string literals, keep ref_wrapper =========

template <typename T>
inline auto actor_factory_forward(T&& val) {
    using Target = typename actor_factory_param<T>::type;

    if constexpr (std::is_same_v<Target, std::string>) {
        return std::string(std::forward<T>(val)); // copy literal
    } else {
        return std::forward<T>(val); // forward all others
    }
}

// ======= Factory class ===========

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
        return std::is_base_of_v<Service, _Actor>;
    }

private:
    template <std::size_t... Is>
    Actor* create_impl(std::index_sequence<Is...>) {
        auto actor = new _Actor(std::get<Is>(_parameters)...);
        ActorProxy::setType<_Actor>(*actor);
        ActorProxy::setName<_Actor>(*actor);
        return actor;
    }
};

using actor = Actor;
template <typename Tag>
using service_actor = ServiceActor<Tag>;
#ifdef QB_LOGGER
qb::io::log::stream &operator<<(qb::io::log::stream &os, qb::Actor const &actor);
#endif
std::ostream &operator<<(std::ostream &os, qb::Actor const &actor);

} // namespace qb

#endif // QB_ACTOR_H
