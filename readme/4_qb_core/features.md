# QB-Core Features

`qb-core` provides the engine and features necessary to implement the Actor Model on top of `qb-io`.

## Actor Management & Lifecycle

*   **Actor Abstraction (`qb::Actor`):** Base class defining the actor concept.
*   **Actor Creation:**
    *   `qb::Main::addActor<T>(core_id, ...)`: Add actor to specific core.
    *   `qb::Main::core(core_id).builder()`: Chain multiple actor additions.
    *   `qb::Actor::addRefActor<T>(...)`: Create same-core referenced (child) actors.
*   **Identification (`qb::ActorId`):** Unique `CoreId` + `ServiceId` combination.
*   **Initialization (`onInit()`):** Virtual method called after construction; must register events here. Return `false` to abort.
*   **Termination (`kill()`):** Method to signal actor shutdown.
*   **Destruction (`~Actor()`):** Called after termination for RAII cleanup.
*   **Liveness Check (`is_alive()`):** Check if an actor is still processing events.

## Event System & Messaging

*   **Event Definition (`qb::Event`):** Base for all messages; carries `source`, `dest`, type `id`.
*   **Type Safety:** Uses internal type IDs (`qb::type_id`) for handler dispatch.
*   **Event Subscription (`registerEvent<T>(*)`, `unregisterEvent<T>(*)`):** Manage which event types an actor handles.
*   **Event Handling (`on(Event&)`):** User-implemented methods to process specific event types.
*   **Message Sending:**
    *   `push<T>(dest, ...)`: **Ordered**, asynchronous, default.
    *   `send<T>(dest, ...)`: **Unordered**, asynchronous, requires trivial destructor.
    *   `broadcast<T>(...)`: Send to all actors on all cores.
    *   `push<T>(BroadcastId(core), ...)`: Send to all actors on a specific core.
    *   `reply(Event&)`: Send event back to source (efficient reuse).
    *   `forward(dest, Event&)`: Redirect event to new destination (efficient reuse).
*   **Optimized Sending:**
    *   `to(dest).push<T>(...)`: `EventBuilder` for chained pushes.
    *   `getPipe(dest)`: Access `qb::Pipe` communication channel.
    *   `pipe.allocated_push<T>(size, ...)`: Pre-allocate buffer for large events.

## Concurrency & Scheduling

*   **Engine (`qb::Main`):** Manages system startup, shutdown, and `VirtualCore` threads.
*   **Worker Threads (`qb::VirtualCore`):** Executes actors assigned to it, runs the `qb-io` event loop.
*   **Multi-Core Execution:** Distributes actors across cores.
*   **Inter-Core Communication:** Transparent messaging via lock-free MPSC queues.
*   **Configuration (`CoreInitializer`):
    *   `setAffinity(CoreIdSet)`: Control CPU core pinning.
    *   `setLatency(ns)`: Tune event loop responsiveness vs. CPU usage.

## Actor Patterns & Utilities

*   **Periodic Tasks (`qb::ICallback`):** Interface for `onCallback()` execution in each core loop (`registerCallback`).
*   **Service Actors (`qb::ServiceActor<Tag>`):** Singleton-per-core pattern (`getService<T>`, `getServiceId<Tag>`).
*   **Dependency Discovery (`require<T>`, `RequireEvent`):** Find other running actors by type. 