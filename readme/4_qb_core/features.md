@page qb_core_features_md QB-Core: Key Features & Capabilities
@brief A summary of the core features provided by the QB Actor Engine for concurrent C++ development.

# QB-Core: Key Features & Capabilities

`qb-core` equips developers with a powerful set of features to build robust and scalable actor-based applications on top of the `qb-io` asynchronous foundation. Here's a rundown of its main capabilities:

## I. Actor Lifecycle & Management

*   **Core Actor Abstraction (`qb::Actor`):** The fundamental base class for all user-defined actors, encapsulating state and behavior.
*   **Flexible Actor Creation:**
    *   System-Wide: `qb::Main::addActor<MyActor>(core_id, ...)` to assign actors to specific `VirtualCore` threads during setup.
    *   Per-Core Builder: `qb::Main::core(core_id).builder().addActor<MyActor>(...)` for fluent addition of multiple actors to a core.
    *   Referenced (Child) Actors: `qb::Actor::addRefActor<MyChildActor>(...)` to create and directly reference child actors residing on the *same core* as the parent.
*   **Unique Actor Identification (`qb::ActorId`):** Each actor receives a system-unique ID (composed of `CoreId` and `ServiceId`) for addressing messages.
*   **Controlled Initialization (`virtual bool onInit()`):** A dedicated virtual method called after an actor is constructed and its ID is assigned. This is the designated place for:
    *   Registering event handlers (`registerEvent<MyEvent>(*this)`).
    *   Acquiring resources.
    *   Returning `false` from `onInit()` aborts the actor's startup and leads to its destruction.
*   **Graceful Termination (`kill()`):** A method for actors to request their own shutdown or for other actors/systems to request an actor's termination.
*   **Guaranteed Destruction (`virtual ~Actor()`):** The actor's destructor is called only after it has fully terminated and been removed from its `VirtualCore`, ensuring proper RAII-based resource cleanup.
*   **Liveness Check (`is_alive()`):** Allows querying if an actor is still active and processing events.

## II. Event System & Asynchronous Messaging

*   **Event Definition (`qb::Event`):** The base class for all inter-actor messages. Events are primarily data carriers.
*   **Type-Safe Dispatch:** The framework uses internal type identifiers (`qb::type_id`) derived from event class types to ensure messages are routed to correctly typed `on(EventType&)` handlers.
*   **Event Subscription:**
    *   `registerEvent<MyEvent>(*this)`: Subscribes an actor to handle a specific event type.
    *   `unregisterEvent<MyEvent>(*this)`: Dynamically unsubscribes from an event type.
*   **Event Handling Methods (`on(const EventType&)` or `on(EventType&)`):** User-defined public methods that implement the logic for processing specific incoming events.
*   **Versatile Message Sending Patterns:**
    *   **`push<MyEvent>(dest, ...)`:** The **default and recommended** method for sending events. Guarantees ordered delivery (relative to other `push` calls from the same source to the same destination) and handles non-trivially destructible event types.
    *   **`send<MyEvent>(dest, ...)`:** An **unordered**, potentially lower-latency option for same-core communication. **Requires `MyEvent` to be trivially destructible.** Use with caution.
    *   **`broadcast<MyEvent>(...)`:** Sends an event to all actors on all active `VirtualCore`s.
    *   **`push<MyEvent>(qb::BroadcastId(core_id), ...)`:** Sends an event to all actors on a *specific* `VirtualCore`.
    *   **`reply(Event& original_event)`:** Efficiently sends the `original_event` (potentially modified) back to its source. Requires the `on()` handler to take a non-const event reference.
    *   **`forward(ActorId new_dest, Event& original_event)`:** Efficiently redirects the `original_event` to a `new_dest`, preserving the original source. Requires a non-const event reference.
*   **Optimized Sending Utilities:**
    *   `to(destination_id).push<MyEvent>(...)`: An `EventBuilder` for fluently chaining multiple `push` calls to the same destination, avoiding repeated pipe lookups.
    *   `getPipe(destination_id)`: Provides direct access to the underlying `qb::Pipe` communication channel.
    *   `pipe.allocated_push<MyLargeEvent>(payload_size_hint, ...)`: Pre-allocates buffer space in the pipe for large events, preventing reallocations.

## III. Concurrency, Parallelism & Scheduling

*   **Engine Controller (`qb::Main`):** Manages the overall actor system, including `VirtualCore` threads, startup, and shutdown procedures.
*   **Worker Threads (`qb::VirtualCore`):** Each `VirtualCore` is an independent thread that executes a group of actors and runs its own `qb-io` event loop (`qb::io::async::listener`).
*   **Multi-Core Execution:** Actors are distributed across `VirtualCore`s, enabling true parallel processing on multi-core CPUs.
*   **Transparent Inter-Core Communication:** Messages between actors on different cores are routed efficiently and transparently using lock-free MPSC (Multiple-Producer, Single-Consumer) queues.
*   **Configuration & Tuning (`CoreInitializer` via `qb::Main::core(id)`):
    *   `setAffinity(CoreIdSet)`: Allows pinning `VirtualCore` threads to specific physical CPU cores for performance optimization (e.g., improving cache locality).
    *   `setLatency(nanoseconds)`: Controls the idle behavior of a `VirtualCore`'s event loop, balancing responsiveness against CPU usage.

## IV. Actor Patterns & Utilities

*   **Periodic Tasks (`qb::ICallback`):** An interface enabling actors to implement an `onCallback()` method that gets executed on each iteration of their `VirtualCore`'s loop. Managed via `actor.registerCallback(*this)` and `actor.unregisterCallback()`.
*   **Service Actors (`qb::ServiceActor<Tag>`):** A base class for creating singleton actors per `VirtualCore`. These are identified by a unique `Tag` struct and can be discovered via `qb::Actor::getService<MyService>()` (for same-core access) or `qb::Actor::getServiceId<MyServiceTag>(core_id)`.
*   **Dependency Discovery (`qb::Actor::require<TargetActor>()`):** Allows an actor to request notifications (via `qb::RequireEvent`) when actors of `TargetActor` type become available or change status. Useful for discovering services or collaborators dynamically.

These features provide a comprehensive toolkit for building complex, concurrent, and high-performance applications using the Actor Model in C++.

**(Next:** Explore [QB-Core: Mastering qb::Actor](./actor.md) for a deep dive into defining actors.**) 