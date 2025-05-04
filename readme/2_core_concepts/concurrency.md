# Core Concept: Concurrency and Parallelism in QB

QB manages concurrency primarily through the Actor Model, distributing actors across multiple worker threads (`VirtualCore` instances) managed by the `qb::Main` engine. This allows for parallel execution on multi-core processors.

## `qb::Main`: The Engine Orchestrator

(`qb/include/qb/core/Main.h`)

*   **Role:** Initializes, manages, and coordinates the `VirtualCore` worker threads.
*   **Core Pool:** Creates a pool of `VirtualCore` instances (threads), typically matching the number of hardware cores by default.
*   **Actor Distribution:** Assigns actors to specific `VirtualCore` instances during setup (`main.addActor<T>(core_id, ...)`).
*   **Lifecycle Control:** Starts (`start()`) and stops (`stop()`, `join()`) the entire actor system.
*   **Error Detection:** Reports (`hasError()`) if any core terminates unexpectedly.

## `qb::VirtualCore`: The Actor Execution Context

(`qb/include/qb/core/VirtualCore.h`)

*   **Role:** A single worker thread responsible for executing the actors assigned to it.
*   **Event Loop:** Each `VirtualCore` runs its own independent `qb::io::async::listener` event loop.
*   **Mailbox:** Each core has an incoming MPSC (Multiple-Producer, Single-Consumer) queue where events from *other* cores are placed.
*   **Sequential Actor Execution:** **Critically, within a single `VirtualCore`, actors execute sequentially.** The core processes one event for one actor to completion before handling the next event or running any `ICallback`. This eliminates data races *within* an actor's state.
*   **Scheduling:** The `VirtualCore` loop prioritizes processing events from its mailbox (inter-core messages) and its local event queue (same-core messages, timer/IO events), and then runs registered `ICallback`s.

## Parallelism vs. Concurrency

*   **Concurrency:** Multiple actors handle events *asynchronously*, making progress independently without blocking each other unnecessarily (especially due to I/O). This happens even on a single core.
*   **Parallelism:** Multiple actors execute *simultaneously* on different `VirtualCore` threads, leveraging multiple physical CPU cores.

## Inter-Core Communication

(`qb/include/qb/core/Main.h` - `SharedCoreCommunication`)

Communication between actors on *different* cores is handled transparently by the framework:

1.  Sender (`coreA`) `push`es or `send`s an event to a destination (`actorB` on `coreB`).
2.  `coreA` serializes the event into a shared buffer.
3.  `coreA` enqueues the event data into `coreB`'s MPSC mailbox.
4.  `coreB`'s event loop eventually dequeues the event data from its mailbox.
5.  `coreB` deserializes the event and dispatches it to `actorB`'s `on()` handler.

*   **Mechanism:** Uses high-performance, lock-free MPSC queues (`qb::lockfree::mpsc::ringbuffer`) for efficiency and low contention.
*   **Ordering:** `push` guarantees order between the *same* sender/receiver pair, even across cores. `send` provides no ordering guarantees.

**(Ref:** `test-actor-event.cpp` (Multi core), `test-actor-service-event.cpp`**)

## Configuration: Affinity and Latency

(`qb/include/qb/core/Main.h` - `CoreInitializer`, `qb/include/qb/core/CoreSet.h`)

You can influence how `VirtualCore` threads run *before* starting the engine:

*   **`main.core(id).setAffinity(CoreIdSet)`:** Pins a `VirtualCore` thread to specific physical CPU cores. This can improve performance for critical actors by minimizing thread migration and maximizing cache utilization. Requires careful planning to avoid overloading cores.
*   **`main.core(id).setLatency(ns)`:** Controls the event loop's idle behavior. `0` (default) provides the lowest latency but uses 100% CPU. A positive value allows the core to sleep briefly when idle, saving power/CPU at the cost of slightly increased latency.

**(Ref:** `test-main.cpp` (multi-core tests), `test-actor-event.cpp` (high latency test), `[Guides: Performance Tuning](./../6_guides/performance_tuning.md)`**) 