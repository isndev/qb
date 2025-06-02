@page core_concepts_concurrency_md Core Concept: Concurrency & Parallelism in QB
@brief Explore how QB orchestrates concurrent actor execution and achieves true parallelism using VirtualCores.

# Core Concept: Concurrency and Parallelism in QB

The QB Actor Framework is designed to make concurrent programming more manageable and to effectively utilize modern multi-core processors for parallel execution. It achieves this through a clear separation of concerns managed by the `qb::Main` engine and its `VirtualCore` worker threads.

## Concurrency vs. Parallelism in QB

It's important to distinguish these two terms in the context of QB:

*   **Concurrency:** This refers to the ability of the system to handle multiple tasks or actors *seemingly* at the same time. Even on a single CPU core, actors can make progress independently, especially when some are waiting for I/O (handled asynchronously by `qb-io`) while others are processing events. QB provides concurrency by allowing many actors to exist and react to messages without blocking each other unnecessarily.
*   **Parallelism:** This refers to the ability of the system to execute multiple tasks or actors *literally* at the same time, by running them on different physical CPU cores. QB achieves parallelism by distributing actors across multiple `VirtualCore` threads, each of which can be mapped to a physical core.

## `qb::Main`: The Engine Orchestrator

(`qb/core/Main.h`)

The `qb::Main` class is the central controller of your actor system. Its primary responsibilities include:

*   **VirtualCore Management:** It creates and manages a pool of `VirtualCore` instances. By default, it often attempts to match the number of available hardware cores, but this is configurable.
*   **Actor Distribution:** When you add actors to the system using `main.addActor<MyActor>(core_id, constructor_args...)`, you specify which `VirtualCore` (by its `core_id`) will be responsible for that actor.
*   **System Lifecycle:** `qb::Main` controls the startup (`start()`) and shutdown (`stop()`, `join()`) of the entire actor system, including all `VirtualCore` threads.
*   **Error Reporting:** It can detect if a `VirtualCore` terminates unexpectedly (e.g., due to an unhandled exception in an actor) via `main.hasError()`.

## `qb::VirtualCore`: The Actor's Execution Realm

(`qb/core/VirtualCore.h`)

A `VirtualCore` is essentially a dedicated worker thread that hosts and executes a subset of the system's actors.

*   **Event Loop Hub:** Each `VirtualCore` runs its own independent `qb::io::async::listener` event loop. This loop is the engine that drives all activity on that core, processing I/O events, timer events, and actor messages.
*   **Actor Mailboxes & Event Queues:**
    *   **Inter-Core Mailbox:** Each `VirtualCore` has an incoming MPSC (Multiple-Producer, Single-Consumer) queue. When an actor on *another* core sends a message to an actor on *this* core, the message (or its data) is placed into this mailbox.
    *   **Local Event Queue:** Events sent between actors residing on the *same* `VirtualCore` (e.g., via `push()`) are typically handled through a more direct local queueing mechanism within that core.
*   **Sequential Actor Execution (Crucial Guarantee):** Within a single `VirtualCore`, actors execute their event handlers (`on(Event&)` methods) and callbacks (`onCallback()`) **one at a time, to completion**. The `VirtualCore` processes one event for one actor fully before picking up the next event for any actor on that core. This fundamental guarantee eliminates data races on an actor's internal state *from its own event handlers*, simplifying state management significantly.
*   **Scheduling Logic (Simplified):** In each iteration of its main loop, a `VirtualCore` typically:
    1.  Checks for and processes I/O events from its `listener` (socket readiness, file system changes, timer expirations from `qb::io::async::callback` or `qb::io::async::with_timeout`).
    2.  Dequeues and processes events from its inter-core mailbox.
    3.  Dequeues and processes events from its local event queue.
    4.  Invokes `onCallback()` for all actors on this core that have registered via `qb::ICallback`.
    5.  Flushes any outgoing event pipes to other cores.
    6.  If configured with a non-zero latency and currently idle, it may briefly sleep.

## Inter-Core Communication: Efficient & Transparent

(`qb/core/Main.h` - `SharedCoreCommunication`, `qb/system/lockfree/mpsc.h`)

When an actor on `CoreA` sends an event to an actor on `CoreB`, QB handles the communication efficiently and (mostly) transparently:

1.  **Serialization:** The sending `VirtualCore` (CoreA) prepares the event data for transfer. For complex events, this might involve serialization; for simple events or those using `std::shared_ptr` for payloads, it primarily involves packaging pointers or small data.
2.  **Enqueue to Mailbox:** CoreA places the event (or a reference/pointer to its data) into the dedicated MPSC mailbox of the destination `VirtualCore` (CoreB).
3.  **Notification (Potentially):** CoreA might notify CoreB that new data is available in its mailbox, especially if CoreB was sleeping due_to_idle_latency.
4.  **Dequeue & Dispatch:** CoreB, during its event loop cycle, dequeues the event data from its mailbox.
5.  **Delivery:** CoreB reconstructs the event if necessary and dispatches it to the target actor's appropriate `on()` handler.

*   **Underlying Mechanism:** This inter-core communication relies on high-performance, lock-free MPSC queues (`qb::lockfree::mpsc::ringbuffer`). This design minimizes contention between producing cores and ensures efficient delivery to the single consuming core.
*   **Ordering Guarantees:**
    *   `push<Event>(dest, ...)`: Events sent from a specific actor A to a specific actor B will be processed by B in the order A pushed them, *even if A and B are on different cores*.
    *   `send<Event>(dest, ...)`: Provides no ordering guarantees, even for same-core communication.

**(Reference Example:** `test-actor-event.cpp` (Multi core tests), `test-actor-service-event.cpp` illustrate inter-core messaging.**)

## Configuring Parallelism: Affinity & Latency

While QB handles much of the complexity, you can fine-tune how `VirtualCore` threads behave *before* starting the engine (`main.start()`):

*   **CPU Affinity (`main.core(core_id).setAffinity(CoreIdSet)`):** Pin a `VirtualCore` thread to one or more specific physical CPU cores. This can be beneficial for cache performance and reducing thread migration overhead for critical actors. It requires careful consideration to avoid overloading physical cores.
*   **Event Loop Latency (`main.core(core_id).setLatency(nanoseconds)`):** Control how long a `VirtualCore` might sleep if it finds no immediate work. 
    *   `0` (default): Lowest latency, but the core spins at 100% CPU even if idle.
    *   `>0`: Allows the core to sleep, reducing CPU usage at the cost of potentially introducing a slight delay in picking up new events. A small value (e.g., a few hundred microseconds to a millisecond) often provides a good balance.

Thoughtful configuration of actor placement, core affinity, and latency allows you to tailor QB's parallelism to your application's specific workload and hardware.

**(Reference Guide:** [Guides: Performance Tuning Guide](./../6_guides/performance_tuning.md)**)
**(Next:** [QB-IO Module Overview](./../3_qb_io/README.md) or [QB-Core Module Overview](./../4_qb_core/README.md)**)** 