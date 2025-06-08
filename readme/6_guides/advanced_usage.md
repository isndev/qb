@page guides_advanced_usage_md QB Framework: Advanced Techniques & System Design
@brief Explore sophisticated usage patterns, advanced system design considerations, and performance optimization strategies for complex QB applications.

# QB Framework: Advanced Techniques & System Design

Once you're comfortable with the core concepts and basic patterns of the QB Actor Framework, you can begin to explore more advanced techniques to build truly sophisticated, scalable, and resilient systems. This guide delves into such practices, often drawing inspiration from the more complex examples provided with the framework.

## 1. Complex Actor Hierarchies & Supervision

While QB doesn't enforce a built-in supervisor hierarchy like some other actor systems, you can implement robust supervision trees using standard actor patterns.

*   **Parent-Child Relationships (Logical):**
    *   An actor (supervisor) can create other actors (workers/children) and store their `ActorId`s.
    *   The supervisor is responsible for the *logical* lifecycle of its children: sending them initial configuration, work, and potentially `qb::KillEvent`s when the supervisor itself is shutting down.
*   **Health Monitoring & Restart Strategies:**
    *   **Heartbeats/Pings:** Supervisors can periodically send `PingEvent`s to children. Children reply with `PongEvent`. If a pong is missed (detected via a timeout scheduled with `qb::io::async::callback` in the supervisor), the child might be considered unresponsive.
    *   **Explicit Failure Reporting:** Children can `push` a specific `ChildFailedEvent(reason)` to their supervisor upon encountering an unrecoverable internal error before terminating themselves.
    *   **Restart Logic:** Upon detecting a child failure, the supervisor can:
        *   **Restart:** Create a new instance of the failed child actor (`addActor` or `addRefActor`). The new actor might need to recover state (e.g., from a persistent store or by requesting it from peers).
        *   **Delegate:** Reassign the failed child's pending tasks to other available workers.
        *   **Escalate:** If the failure is critical or repeated, the supervisor might notify its own supervisor or a system-level manager actor.
*   **Fault Isolation:** A key benefit is that the failure of a child actor (even an unhandled exception causing its `VirtualCore` to terminate, if not designed carefully) can be contained. The supervisor, ideally on a different `VirtualCore`, can detect this and react without being directly affected by the crash itself (though it needs to handle the loss of the child).

**(Reference:** The `example/core/example10_distributed_computing.cpp` showcases a `SystemMonitorActor` that launches and (conceptually) oversees other top-level actors. The `TaskSchedulerActor` in the same example implicitly supervises workers by tracking their heartbeats and status.)**

## 2. Advanced Message Design & Serialization

For highly performance-sensitive applications or when integrating with external systems, consider these advanced messaging aspects:

*   **Custom Serialization for Large/Complex Events:**
    *   While `std::shared_ptr` is excellent for passing large data within events without copying the payload, if you need to send events over a network to non-QB systems or require a very compact representation, you might implement custom serialization/deserialization directly within your event types or specialized `Protocol` classes.
    *   This could involve using libraries like Protocol Buffers, FlatBuffers, Cap'n Proto, or a custom binary format. Your `Protocol::onMessage` would deserialize, and your sending logic would serialize.
*   **Zero-Copy Techniques (Conceptual within QB):**
    *   **`qb::string_view` Protocols:** Using protocols like `text::string_view` or `text::command_view` allows your actor to receive a view of the data directly from the `qb-io` input buffer without copying the payload into a `std::string` or `qb::string` within the `Protocol::message` struct. This is effective if the message is processed immediately and the buffer isn't modified or invalidated before the view is used.
    *   **`qb::allocator::pipe` and `allocated_push`:** When sending large, self-constructed messages, using `actor.getPipe(...).allocated_push<MyEvent>(total_size_hint, ...)` can help pre-allocate a sufficiently large contiguous block in the communication pipe. If `MyEvent` is designed to then directly use parts of this pre-allocated block for its payload (e.g., by constructing an internal `qb::string_view` that points into this tail data), you can minimize intermediate buffer copies.
*   **Message Batching:** For very high-throughput scenarios where individual event overhead is a concern, actors can implement a batching pattern: accumulate multiple small logical messages into a single larger `BatchEvent` before sending. The recipient then iterates through the items in the batch.

## 3. Dynamic Load Balancing & Work Distribution

Beyond simple round-robin, more sophisticated load balancing can be implemented:

*   **Metrics-Based Dispatch:** A dispatcher/scheduler actor can collect metrics from worker actors (e.g., current queue depth, CPU utilization, processing time per task via `WorkerStatusMessage`). It then uses these metrics to route new tasks to the least loaded or most performant available worker.
    *   Workers would periodically `push` `WorkerStatusUpdate` events to the scheduler.
*   **Work-Stealing (Conceptual):** Idle worker actors could proactively request work from a central queue or even from other busy worker actors (requires careful protocol design to avoid contention).
*   **Adaptive Concurrency:** A manager actor could monitor system load and dynamically create or terminate worker actors to match demand (though actor creation/destruction has overhead, so this is for longer-term load changes).

**(Reference:** `example/core/example10_distributed_computing.cpp`'s `TaskSchedulerActor` implements a basic form of load awareness by checking worker availability and (conceptually) utilization before assigning tasks.)**

## 4. Advanced State Management & Persistence

For actors requiring durable state or complex state recovery:

*   **Event Sourcing:** Instead of just storing the current state, an actor persists the sequence of events that led to its current state. To recover, it replays these events. This can be complex but offers powerful auditing and debugging capabilities.
    *   The actor would, upon processing a state-changing event, also send this event (or a derivative) to a dedicated `PersistenceActor`.
*   **Snapshotting:** Periodically, or after a certain number_of_events, an actor can serialize its current full state (a snapshot) and send it to a `PersistenceActor`. Recovery involves loading the latest snapshot and then replaying only the events that occurred after that snapshot.
*   **Dedicated Persistence Actor(s):** Abstract database or file system interactions into one or more `PersistenceActor`s. Other actors send `SaveStateEvent` or `LoadStateRequest` events. This centralizes and serializes access to the storage medium.

**(Reference:** `qb/source/core/tests/system/test-actor-state-persistence.cpp` demonstrates a basic in-memory persistence and recovery simulation.)**

## 5. Interfacing with Blocking/External Systems Safely

When actors must interact with legacy blocking libraries or external systems that don't offer asynchronous APIs:

*   **Dedicated Worker Actor Pool:** Create a pool of worker actors (potentially on `VirtualCore`s configured with higher latency to reduce CPU spin when blocked) whose sole job is to handle these blocking calls.
    *   Your main application actors `push` a `BlockingRequestEvent` to a dispatcher.
    *   The dispatcher forwards the request to an available worker from the pool.
    *   The worker actor performs the blocking call (its `VirtualCore` will block for that actor, but other cores/actors remain responsive).
    *   Upon completion, the worker `push`es a `BlockingResponseEvent` back to the original requester.
*   **`qb::io::async::callback` (for short, infrequent calls):** As detailed in [Integrating Core & IO: Asynchronous Operations within Actors](./../5_core_io_integration/async_in_actors.md), you can wrap a blocking call in a lambda scheduled by `async::callback`. While the callback itself will block its turn on the `VirtualCore`, it prevents the main event handler from blocking.

**(Reference:** `example/core_io/file_processor/` uses `async::callback` within its `FileWorker` actors to handle blocking file I/O.)**

## 6. Custom Actor Schedulers & `VirtualCore` Customization (Conceptual/Advanced)

While `qb-core` provides a robust general-purpose scheduler within each `VirtualCore`, extremely advanced scenarios *could* (with framework modification or careful hooking, if possible) involve:

*   **Priority Queues for Events:** Modifying `VirtualCore` to use priority queues for actor mailboxes if certain actors or event types have stringent real-time requirements over others on the same core.
*   **Custom `VirtualCore` Logic:** For deeply embedded systems or specialized hardware, one might envision a scenario requiring modifications to the `VirtualCore`'s main loop itself. This is far beyond typical usage and would involve in-depth understanding of QB's internals.

These advanced patterns and techniques require a solid understanding of both the Actor Model and QB's specific implementation. Always start with simpler patterns and only introduce more complexity when performance measurements or system requirements clearly justify it. The provided examples are excellent resources for seeing many of these concepts in action.

**(Next:** Consider exploring [Guides: Performance Tuning Guide](./performance_tuning.md) or [Guides: Error Handling & Resilience Guide](./error_handling.md) for more specialized advice.**) 