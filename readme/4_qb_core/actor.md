@page qb_core_actor_md QB-Core: Mastering `qb::Actor`
@brief A comprehensive guide to defining, initializing, and managing the lifecycle of actors using `qb::Actor`.

# QB-Core: Mastering `qb::Actor`

(`qb/core/Actor.h`)

The `qb::Actor` class is the cornerstone of applications built with the QB Actor Framework. It's the base class from which all your custom actors will inherit, providing the structure for encapsulated state, message-driven behavior, and controlled lifecycle management. This guide delves into the practical aspects of working with `qb::Actor`.

## Defining Your Actor

Creating a custom actor involves several key steps:

1.  **Public Inheritance:** Your class must publicly inherit from `qb::Actor`.
    ```cpp
    class MyWorker : public qb::Actor {
        // ... actor implementation ...
    };
    ```

2.  **State Encapsulation:** Define member variables (`private` or `protected`) to hold the actor's internal state. This state is accessible only by the actor itself, ensuring data integrity in a concurrent environment.
    ```cpp
    class MyStatefulActor : public qb::Actor {
    private:
        int _counter = 0;
        std::string _status_message;
        std::vector<double> _data_points;
        // Potentially RAII wrappers for external resources
        // std::unique_ptr<DatabaseConnection> _db_conn;
    public:
        // ...
    };
    ```

3.  **Constructor (Optional but Common):** You can define constructors to initialize your actor's state, often taking parameters passed during actor creation (e.g., via `main.addActor<MyActor>(core_id, arg1, arg2)`).
    ```cpp
    class ConfigurableActor : public qb::Actor {
    private:
        const std::string _config_path;
        int _initial_value;
    public:
        ConfigurableActor(std::string config_path, int initial_val)
            : _config_path(std::move(config_path)), _initial_value(initial_val) {}
        // ...
    };
    ```

4.  **`onInit()` - Essential Initialization & Event Registration (Override):**
    This virtual method is **critical**. It's called by the framework *after* your actor is constructed and *after* its unique `qb::ActorId` has been assigned, but *before* it begins processing any events.

    *   **Purpose:** Perform essential setup that might depend on the actor having an ID or needing to interact with the framework (like registering for events).
    *   **Event Registration:** **You must call `registerEvent<EventType>(*this);` within `onInit()` for every type of event your actor intends to handle.** Failure to do so means the corresponding `on(const EventType&)` handlers will never be invoked.
    *   **Resource Acquisition:** Initialize resources, load configurations, establish connections to helper services (e.g., get `ActorId` of a `ServiceActor`).
    *   **Return Value:** `onInit()` must return `bool`. 
        *   `true`: Initialization was successful; the actor will proceed to its active state.
        *   `false`: Initialization failed; the actor will **not** be started, and its destructor will be called shortly after. This is a way to gracefully abort an actor's launch if preconditions aren't met.

    ```cpp
    // Inside ConfigurableActor from above
    bool onInit() override {
        qb::io::cout() << "Actor [" << id() << "] onInit on Core " << getIndex() << ".\n";
        
        // Example: Load configuration from _config_path (pseudo-code)
        // if (!loadConfiguration(_config_path)) {
        //     qb::io::cout() << "Actor [" << id() << "] failed to load config: " << _config_path << ".\n";
        //     return false; // Signal initialization failure
        // }
        _status_message = "Initialized with value: " + std::to_string(_initial_value);

        // *** Register Event Handlers ***
        registerEvent<ProcessDataEvent>(*this);
        registerEvent<UpdateRequestEvent>(*this);
        registerEvent<qb::KillEvent>(*this); // Always handle KillEvent

        qb::io::cout() << "Actor [" << id() << "] initialized successfully.\n";
        return true;
    }
    ```

5.  **Event Handlers - Defining Behavior (`on(const EventType&)` or `on(EventType&)`):**
    For each event type registered in `onInit()`, you must implement a corresponding public `on()` method. This is where your actor's primary logic resides.

    *   **Signature:** `void on(const YourEventType& event)` or `void on(YourEventType& event)`.
    *   **`const&` vs. `&`:**
        *   Use `const YourEventType& event` if your handler only needs to read the event's data.
        *   Use `YourEventType& event` (non-const reference) if you intend to modify the event (e.g., to fill in result fields) **before using `reply(event)` or `forward(destination, event)`**. Modifying an event that isn't being replied or forwarded has no effect on other potential recipients if it were a broadcast, for example.

    ```cpp
    // Inside an actor
    void on(const ProcessDataEvent& event) {
        // Process event.payload, but cannot modify event itself
        // Example: _internal_state += event.value_to_add;
    }

    void on(UpdateRequestEvent& event) { // Non-const for reply
        // Process event.query
        event.response_data = "Processed: " + event.query;
        reply(event); // Send the modified event back to its source
    }
    ```

6.  **`on(const qb::KillEvent&)` - Graceful Shutdown (Override):**
    It is crucial to register for and handle `qb::KillEvent`.
    *   Perform any necessary cleanup or final actions specific to *your actor* before it fully terminates.
    *   **You MUST call the base `kill()` method** at the end of your handler to signal the framework to complete the termination process.

    ```cpp
    // Inside an actor
    void on(const qb::KillEvent& /*event*/) { // event parameter often unused
        qb::io::cout() << "Actor [" << id() << "] received KillEvent. Performing cleanup...\n";
        // Example: notify other actors, flush pending data, etc.
        // if (_manager_id.is_valid()) {
        //     push<WorkerStoppingEvent>(_manager_id, id());
        // }
        kill(); // Essential: signals framework to proceed with termination
    }
    ```

7.  **Destructor (`virtual ~MyActor()` - Override, Optional but Good Practice):**
    The destructor is called *after* the actor has been fully terminated (i.e., after `kill()` has completed its work and the actor is removed from the `VirtualCore`'s management).
    *   **RAII Cleanup:** This is the primary place for RAII-managed resources (like `std::unique_ptr` members, `std::fstream`, etc.) to be automatically cleaned up.
    *   Avoid complex logic or sending messages from the destructor, as the actor is no longer active in the system.

    ```cpp
    // Inside ConfigurableActor
    ~ConfigurableActor() override {
        qb::io::cout() << "Actor [" << id() << "] named '" << _config_path 
                       << "' destroyed. Final status: " << _status_message << ".\n";
        // _db_conn (if it was a unique_ptr) would be automatically released here.
    }
    ```

## Actor State Management: The Core Principles

*   **Isolation & Encapsulation:** An actor's member variables are its private world. No other actor or external code should directly access or modify them. All interactions that affect state should occur via received events.
*   **Sequential Processing, Inherent Thread Safety (for self-state):** The QB framework guarantees that for any single actor instance, its `on(Event&)` handlers and `onCallback()` (if using `qb::ICallback`) are executed sequentially by its assigned `VirtualCore`. This means you do not need to use mutexes or other synchronization primitives to protect the actor's *own* member variables from race conditions *caused by its own methods*.
*   **Non-Blocking Operations:** Event handlers and callbacks **must not block**. Avoid long-running computations, synchronous I/O calls (like direct file reads/writes that might block), or waiting indefinitely on external locks. Such blocking behavior will stall the entire `VirtualCore`, preventing other actors on that core from making progress. Use `qb::io::async::callback` to offload work or design your interactions to be fully asynchronous (e.g., using I/O actors).

## Key `qb::Actor` Methods for Everyday Use

Beyond lifecycle and event handling, `qb::Actor` provides several utility methods:

*   **Identification:**
    *   `id() const noexcept -> qb::ActorId`: Returns the actor's unique ID.
    *   `getIndex() const noexcept -> qb::CoreId`: Returns the ID of the `VirtualCore` this actor is running on.
    *   `getName() const noexcept -> std::string_view`: Returns the demangled class name of the actor.
*   **Lifecycle & Status:**
    *   `kill() const noexcept`: Initiates the actor's termination sequence.
    *   `is_alive() const noexcept -> bool`: Checks if the actor is still active and processing events (i.e., `kill()` has not yet fully taken effect).
*   **Event Handling Registration (typically in `onInit()`):**
    *   `registerEvent<EventType>(*this) const noexcept`: Subscribes the actor to handle `EventType`.
    *   `unregisterEvent<EventType>(*this) const noexcept`: Unsubscribes from `EventType`.
*   **Periodic Callbacks (requires inheriting `qb::ICallback` additionally):**
    *   `registerCallback(DerivedActor& actor) const noexcept`: Registers `actor.onCallback()` to be called by the `VirtualCore` loop.
    *   `unregisterCallback(DerivedActor& actor) const noexcept`: Stops periodic calls.
    *   `unregisterCallback() const noexcept`: Unregisters self from callbacks.
    *   `virtual void onCallback() = 0;` (to be implemented by derived class).
*   **Sending Messages (Events):**
    *   `push<Event>(dest, args...) const noexcept -> Event&`: Ordered, default send.
    *   `send<Event>(dest, args...) const noexcept`: Unordered, requires trivially destructible `Event`.
    *   `broadcast<Event>(args...) const noexcept`: Send to all actors on all cores.
    *   `reply(Event& event) const noexcept`: Efficiently send `event` back to its source.
    *   `forward(ActorId dest, Event& event) const noexcept`: Efficiently redirect `event` to `dest`.
    *   `to(ActorId dest) const noexcept -> EventBuilder`: Get a builder for chained `push` calls.
    *   `getPipe(ActorId dest) const noexcept -> qb::Pipe`: Get a direct communication pipe for optimized sending (e.g., `allocated_push`).
*   **Actor Creation & Discovery:**
    *   `addRefActor<ChildActorType>(args...) const -> ChildActorType*`: Creates a child actor on the *same core*. Parent gets a raw pointer but doesn't own the child.
    *   `getService<ServiceActorType>() const noexcept -> ServiceActorType*`: Gets a raw pointer to a `ServiceActor` instance *on the same core*. Returns `nullptr` if not found.
    *   `static getServiceId<ServiceTag>(CoreId core_idx) noexcept -> qb::ActorId`: Gets the `ActorId` of a `ServiceActor` (identified by `ServiceTag`) on a potentially different `core_idx`.
    *   `require<ActorType...>() const noexcept`: Broadcasts a request to discover live instances of the specified `ActorType`(s). Responses arrive as `qb::RequireEvent`.
*   **Framework Interaction:**
    *   `time() const noexcept -> uint64_t`: Get the current cached time (nanoseconds since epoch) from the `VirtualCore`.
    *   `getCoreSet() const noexcept -> const qb::CoreIdSet&`: Get the set of `CoreId`s this actor's `VirtualCore` can communicate with.

By mastering these aspects of `qb::Actor`, you can effectively build modular, concurrent, and robust components for your applications.

**(Next:** [QB-Core: Event Messaging](./messaging.md) to delve deeper into how actors communicate.**)
**(See also:** [Core Concepts: The Actor Model in QB](./../2_core_concepts/actor_model.md), [QB-Core: Actor Lifecycle (TBD)](), [QB-Core: Actor Patterns & Utilities](./patterns.md)**) 