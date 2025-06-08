@page qb_core_patterns_md QB-Core: Common Actor Patterns & Utilities
@brief Discover and implement common actor design patterns and utilities within the QB Framework for robust application structure.

# QB-Core: Common Actor Patterns & Utilities

Beyond the fundamental actor and event mechanisms, `qb-core` supports and simplifies several common design patterns that are highly useful in building robust and maintainable actor-based systems. This guide explores some key patterns and utilities available to your `qb::Actor` implementations.

## 1. Finite State Machine (FSM) with Actors

Actors are an excellent way to model entities that have distinct states and transition between them based on incoming events.

*   **Concept:** An actor's internal member variables represent its current state. Its event handlers (`on(Event&)` methods) contain the logic to process events, perform actions, and transition to new states.
*   **Implementation Strategy:**
    1.  **Define States:** Use an `enum class` for clarity: `enum class MyFsmState { INITIAL, PROCESSING, COMPLETED, ERROR };`
    2.  **Store Current State:** Add a member variable to your actor: `MyFsmState _current_state = MyFsmState::INITIAL;`
    3.  **Define Events as FSM Inputs:** Events trigger state transitions or actions within a state.
    4.  **Implement State Logic in Handlers:** Inside your `on(SpecificEvent&)` handlers, use `if` or `switch` statements based on `_current_state` to define behavior specific to that state and event.
    5.  **Transition State:** Update `_current_state` when a transition occurs.
    6.  **Actions:** Perform entry/exit actions for states, or actions during transitions, directly within the event handlers or dedicated private methods.
    7.  **Timed Transitions/Actions:** Use `qb::io::async::callback` to schedule events that trigger future state changes or actions (e.g., a timeout moving an FSM to an error state).

*   **Conceptual Example:**
    ```cpp
    #include <qb/actor.h>
    #include <qb/io.h> // For qb::io::cout
    #include <qb/io/async.h> // For qb::io::async::callback
    #include <qb/string.h> // For qb::string

    enum class OrderState { PENDING_PAYMENT, PROCESSING, SHIPPED, CANCELLED };
    struct PlaceOrderEvent : qb::Event { qb::string<128> order_details; };
    struct PaymentReceivedEvent : qb::Event { qb::string<64> payment_info; };
    struct ShipOrderEvent : qb::Event {}; // Internal event from a callback

    class OrderActor : public qb::Actor {
    private:
        OrderState _state = OrderState::PENDING_PAYMENT;
        qb::string<128> _order_data;

    public:
        bool onInit() override {
            registerEvent<PlaceOrderEvent>(*this);
            registerEvent<PaymentReceivedEvent>(*this);
            registerEvent<ShipOrderEvent>(*this);
            registerEvent<qb::KillEvent>(*this);
            return true;
        }

        void on(const PlaceOrderEvent& event) { 
            _order_data = event.order_details;
            _state = OrderState::PENDING_PAYMENT;
            qb::io::cout() << "Order [" << id() << "]: Placed. Details: " << _order_data.c_str() << ". Awaiting payment.\n";
        }

        void on(const PaymentReceivedEvent& event) {
            if (_state == OrderState::PENDING_PAYMENT) {
                qb::io::cout() << "Order [" << id() << "]: Payment received (info: " << event.payment_info.c_str() 
                               << "). Transitioning to PROCESSING.\n";
                _state = OrderState::PROCESSING;
                // Simulate processing delay then ship
                qb::io::async::callback([this](){ 
                    if (this->is_alive()) this->push<ShipOrderEvent>(this->id()); 
                }, 2.0 /* 2 seconds processing */);
            }
        }

        void on(const ShipOrderEvent& /*event*/) {
            if (_state == OrderState::PROCESSING) {
                qb::io::cout() << "Order [" << id() << "]: Processing complete. Transitioning to SHIPPED.\n";
                _state = OrderState::SHIPPED;
                // Notify customer, etc.
            }
        }
        // ... other handlers for cancellation, KillEvent etc. ...
        void on(const qb::KillEvent& ke) { kill(); }
    };
    ```
*   **(Full Example:** `example/core/example8_state_machine.cpp` provides a detailed coffee machine FSM.)**

## 2. Service Actors: Core-Local Singletons

*   **Header:** `qb/core/Actor.h`
*   **Purpose:** To implement actors that should exist as a single instance per `VirtualCore`. This is ideal for managing core-local resources, providing centralized services within a core (like logging, metrics aggregation, or specialized computation), or acting as a well-known entry point for actors on that core.
*   **Defining a Service Actor:**
    1.  **Create a Unique Tag:** Define an empty struct to serve as a unique identifier for your service type.
        ```cpp
        struct MyCoreLoggerTag {}; // Unique tag for the logger service
        ```
    2.  **Inherit from `qb::ServiceActor<Tag>`:** Your service actor class inherits from `qb::ServiceActor<YourTagType>`.
        ```cpp
        #include <qb/actor.h>
        #include <qb/event.h>
        #include <qb/io.h> // For qb::io::cout
        #include <qb/string.h>

        struct LogEvent : qb::Event {
            qb::string<128> message;
            LogEvent(const char* msg) : message(msg) {}
        };

        class CoreLoggerService : public qb::ServiceActor<MyCoreLoggerTag> {
        public:
            bool onInit() override {
                registerEvent<LogEvent>(*this);
                registerEvent<qb::KillEvent>(*this);
                qb::io::cout() << "CoreLoggerService [" << id() << "] initialized on core " << getIndex() << ".\n";
                return true;
            }

            void on(const LogEvent& event) {
                qb::io::cout() << "Core [" << getIndex() << "] Log: " << event.message.c_str() << " (from " << event.getSource() << ")\n";
            }
            void on(const qb::KillEvent& ke) { kill(); }
        };
        ```
*   **Adding to the Engine:** Add a `ServiceActor` like any other actor using `main.addActor<MyServiceActorType>(core_id, constructor_args...);`. The framework ensures that only one instance per tag can be added to each core; subsequent attempts on the same core with the same tag will fail during `addActor`.
*   **Accessing a Service Actor:**
    *   **From any actor (potentially inter-core):** Use `qb::Actor::getServiceId<ServiceTag>(target_core_id)` to get the `qb::ActorId` of the service on a specific core. Then, send events to this ID.
        ```cpp
        // Inside AnotherActor
        // qb::ActorId logger_service_id = getServiceId<MyCoreLoggerTag>(relevant_core_id);
        // if (logger_service_id.is_valid()) {
        //     push<LogEvent>(logger_service_id, "Message from AnotherActor");
        // }
        ```
    *   **From an actor on the *same* `VirtualCore`:** Use `getService<ServiceActorType>()` to get a direct raw pointer to the service actor instance. This allows for direct method calls (use with caution as it bypasses the mailbox) or for sending events via `service_ptr->id()`.
        ```cpp
        // Inside SameCoreActor, assuming CoreLoggerService is on this core
        // CoreLoggerService* logger = getService<CoreLoggerService>();
        // if (logger) {
        //     // Option 1: Send event (recommended for actor model purity)
        //     logger->push<LogEvent>(logger->id(), "Direct log from SameCoreActor");
        //     // Option 2: Direct call (use cautiously!)
        //     // logger->somePublicMethod(); 
        // }
        ```
*   **(Reference:** `test-actor-add.cpp`, `test-actor-service-event.cpp`**)

## 3. Periodic Callbacks: `qb::ICallback`

*   **Header:** `qb/core/ICallback.h`
*   **Purpose:** Enables an actor to execute a piece of code (`onCallback()`) on every iteration of its `VirtualCore`'s main processing loop. This typically happens after all queued events for that loop iteration have been processed.
*   **How to Use:**
    1.  **Multiple Inheritance:** Your actor must inherit from both `qb::Actor` and `qb::ICallback`.
        `class MyPeriodicActor : public qb::Actor, public qb::ICallback { ... };`
    2.  **Implement `onCallback()`:** Override the pure virtual method `virtual void onCallback() override;` This method will contain the logic to be executed periodically.
    3.  **Register/Unregister:**
        *   Call `registerCallback(*this);` (usually within `onInit()`) to start receiving callbacks.
        *   Call `unregisterCallback(*this);` (or just `unregisterCallback();` from within the actor) to stop receiving callbacks. This can be done from `onCallback()` itself or any event handler.
*   **Critical Note:** The `onCallback()` method **must be very fast and strictly non-blocking**. Any delay or blocking operation within `onCallback()` will directly stall the `VirtualCore`'s event loop, affecting all other actors on that core.
*   **Use Cases:** Polling external non-event-driven systems, performing periodic checks or maintenance tasks, driving simulations, implementing game loops.

```cpp
#include <qb/actor.h>
#include <qb/icallback.h>
#include <qb/io.h>
#include <qb/event.h>

struct TickEvent : qb::Event { int tick_count; };

class HeartbeatActor : public qb::Actor, public qb::ICallback {
private:
    int _heartbeat_count = 0;
    const int _max_heartbeats = 5;
public:
    bool onInit() override {
        registerEvent<qb::KillEvent>(*this);
        registerCallback(*this); // Start receiving periodic callbacks
        qb::io::cout() << "HeartbeatActor [" << id() << "] registered for callbacks.\n";
        return true;
    }

    void onCallback() override {
        _heartbeat_count++;
        qb::io::cout() << "HeartbeatActor [" << id() << "] onCallback: Tick #" << _heartbeat_count << ".\n";
        // Example: broadcast a tick event every few callbacks
        if (_heartbeat_count % 2 == 0) {
            broadcast<TickEvent>(_heartbeat_count);
        }
        if (_heartbeat_count >= _max_heartbeats) {
            qb::io::cout() << "HeartbeatActor [" << id() << "] reached max heartbeats, unregistering callback and stopping.\n";
            unregisterCallback(); // Stop receiving callbacks
            kill();               // Terminate the actor
        }
    }
    void on(const qb::KillEvent& ke) { kill(); }
};
```
*   **(Reference:** `test-actor-callback.cpp`, `example1_basic_actors.cpp` (SenderActor), `example10_distributed_computing.cpp` (TaskGeneratorActor, WorkerNodeActor for heartbeats).**)

## 4. Referenced Actors: `addRefActor`

*   **Header:** `qb/core/Actor.h`
*   **Purpose:** Allows an actor (the "parent") to create another actor (the "child") that resides **on the same `VirtualCore`** and to obtain a direct raw pointer to this child actor. This enables direct method calls from the parent to the child, bypassing the event mailbox for those specific interactions.
*   **Creation:** From within a parent actor, call `ChildActorType* child_ptr = addRefActor<ChildActorType>(constructor_args...);`
*   **Return Value:** Returns a raw pointer to the successfully created and initialized child actor. If the child's `onInit()` returns `false`, `addRefActor` returns `nullptr`.
*   **Lifecycle & Ownership:**
    *   The parent actor **does not own** the referenced child actor in terms of C++ object lifetime managed by `delete`. The child actor, like any other actor, manages its own lifecycle and must call `kill()` on itself (or be killed via an event) to terminate.
    *   The parent must be aware that the raw pointer can become dangling if the child actor terminates independently. There's no automatic notification to the parent if the child pointer becomes invalid (unless a custom protocol is implemented).
*   **Communication:**
    *   **Parent -> Child:**
        *   **Via Events (Recommended):** `push<MyEvent>(child_ptr->id(), ...);` This respects the actor model and uses the child's mailbox.
        *   **Direct Method Call (Use with EXTREME CAUTION):** `child_ptr->somePublicMethod();`. This bypasses the child's event queue and executes the method synchronously within the parent's current event processing context. This can break actor isolation guarantees and lead to complex state management issues if not handled very carefully. It should only be considered for highly performance-sensitive, simple interactions where the child is tightly controlled by the parent.
    *   **Child -> Parent:** The child needs the parent's `qb::ActorId` (e.g., passed via the child's constructor) to send events back.
*   **Use Cases:** Tightly coupled helper actors that are logically part of the parent and reside on the same core, where direct method calls offer a *significant, measured* performance benefit for very frequent, simple operations that don't naturally fit the event-passing model.

```cpp
// Forward declarations for events and child actor
struct HelperTask : qb::Event { int a, b; };
struct HelperResult : qb::Event { int result; };
class ChildHelperActor;

class ParentActor : public qb::Actor {
private:
    ChildHelperActor* _helper = nullptr;
public:
    bool onInit() override {
        _helper = addRefActor<ChildHelperActor>(id()); // Pass my ID to child
        if (!_helper) {
            qb::io::cout() << "ParentActor: Failed to create ChildHelperActor!\n";
            return false;
        }
        registerEvent<HelperResult>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }
    void doWorkViaHelper(int val_a, int val_b) {
        if (_helper && _helper->is_alive()) { // Check if helper is still valid
            push<HelperTask>(_helper->id(), val_a, val_b);
        }
    }
    void on(const HelperResult& event) {
        qb::io::cout() << "ParentActor: Received result from helper: " << event.result << ".\n";
    }
    void on(const qb::KillEvent& ke) { 
        if (_helper && _helper->is_alive()) push<qb::KillEvent>(_helper->id());
        kill(); 
    }
};

class ChildHelperActor : public qb::Actor {
private:
    qb::ActorId _parent_id;
public:
    explicit ChildHelperActor(qb::ActorId parent_id) : _parent_id(parent_id) {}
    bool onInit() override { 
        registerEvent<HelperTask>(*this); 
        registerEvent<qb::KillEvent>(*this);
        return true; 
    }
    void on(const HelperTask& event) { 
        int res = event.a + event.b; 
        push<HelperResult>(_parent_id, res); 
    }
    void on(const qb::KillEvent& ke) { kill(); }
};
```
*   **(Reference:** `test-actor-add.cpp::TestRefActor`, `test-actor-state-persistence.cpp` uses it for a mock storage actor.**)

## 5. Actor Dependency Resolution: `require<T>` & `RequireEvent`

*   **Headers:** `qb/core/Actor.h`, `qb/core/Event.h`
*   **Purpose:** Allows an actor to dynamically discover other running actors of a specific type (or types) within the system. This is particularly useful for locating `ServiceActor` instances or other key collaborators without needing their `ActorId`s at construction time.
*   **Mechanism:**
    1.  **Requester Actor:** Calls `require<TargetActorTypeOne, TargetActorTypeTwo, ...>();` (usually in its `onInit()` method). This broadcasts an internal `PingEvent` tagged with the type(s) being sought.
    2.  **Target Actors:** Live actors of the `TargetActorType` automatically respond to this specific `PingEvent` by sending a `qb::RequireEvent` back to the original requester.
    3.  **Requester Actor Handles `RequireEvent`:** The requester must register for and implement `void on(const qb::RequireEvent& event)`. Inside this handler:
        *   Use `is<TargetActorType>(event)` to check if the response pertains to the `TargetActorType` you're interested in.
        *   Check `event.status == qb::ActorStatus::Alive`.
        *   If both are true, then `event.getSource()` provides the `qb::ActorId` of a live instance of `TargetActorType`.

```cpp
// Actor needing to find a LoggerService
#include <qb/string.h> // For qb::string in LogEvent

// Assume LogEvent and CoreLoggerService are defined as in the Service Actor example earlier
// struct LogEvent : qb::Event { qb::string<128> message; ... };
// struct MyCoreLoggerTag {};
// class CoreLoggerService : public qb::ServiceActor<MyCoreLoggerTag> { ... };

struct MyMessageToLog : qb::Event { qb::string<64> content; };

class ClientWithDynamicLogger : public qb::Actor {
private:
    qb::ActorId _logger_service_id; // Will store the found logger ID
    bool _logger_found = false;

public:
    bool onInit() override {
        registerEvent<qb::RequireEvent>(*this); // Must handle RequireEvent
        registerEvent<MyMessageToLog>(*this);
        registerEvent<qb::KillEvent>(*this);

        qb::io::cout() << "Client [" << id() << "]: Sending require for CoreLoggerService.\n";
        require<CoreLoggerService>(); // Request to find CoreLoggerService instances
        return true;
    }

    void on(const qb::RequireEvent& event) {
        if (is<CoreLoggerService>(event)) { // Check if this RequireEvent is for CoreLoggerService
            if (event.status == qb::ActorStatus::Alive) {
                _logger_service_id = event.getSource();
                _logger_found = true;
                qb::io::cout() << "Client [" << id() << "]: Found CoreLoggerService at " << _logger_service_id << ".\n";
                // Now we can use it
                push<LogEvent>(_logger_service_id, "Client successfully found logger!");
                // Optional: If you only need one, you might unregister from further RequireEvents
                // unregisterEvent<qb::RequireEvent>(*this);
            } else {
                qb::io::cout() << "Client [" << id() << "]: CoreLoggerService at " << event.getSource() << " reported not alive.\n";
            }
        }
    }

    void on(const MyMessageToLog& event) {
        if (_logger_found && _logger_service_id.is_valid()) {
            push<LogEvent>(_logger_service_id, event.content.c_str());
        } else {
            qb::io::cout() << "Client [" << id() << "]: Logger not yet found. Cannot log: " << event.content.c_str() << ".\n";
        }
    }
    void on(const qb::KillEvent& ke) { kill(); }
};
```
*   **Note:** An actor might receive multiple `RequireEvent`s if multiple instances of the target type exist across different cores, or if `require` is called multiple times.
*   **(Reference:** `test-actor-dependency.cpp`**)

These patterns and utilities provide flexible and powerful ways to structure your actor-based applications in QB, promoting separation of concerns, managing dependencies, and enabling common concurrent behaviors.

**(Next:** Review [Developer Guides](./../6_guides/README.md) for more high-level application patterns and best practices.**) 