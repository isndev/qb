@page qb_core_engine_md QB-Core: Engine - `qb::Main` & `VirtualCore`
@brief Understand the QB actor system's engine, how `qb::Main` orchestrates `VirtualCore` worker threads, and how actors are scheduled.

# QB-Core: Engine - `qb::Main` & `VirtualCore`

The QB Actor Framework's engine is the runtime environment that brings your actors to life, manages their execution, and facilitates their communication. It's primarily composed of the `qb::Main` class, which orchestrates multiple `qb::VirtualCore` instances (worker threads).

## `qb::Main`: The Conductor of Your Actor Symphony

(`qb/core/Main.h`)

Think of `qb::Main` as the central controller for your entire actor system. It's responsible for setting up the environment, launching worker threads, and managing the overall lifecycle.

Basic Engine Architecture:
```text
+------------------------------------+
|             qb::Main               |
|  (Engine Orchestrator)             |
+-----------------|------------------+
                  | Manages
      +-----------+-----------+
      |                       |
+-----v-----------+     +-----v-----------+
|  VirtualCore 0  |     |  VirtualCore 1  |  ... (More VirtualCores)
|  (Worker Thread)|     |  (Worker Thread)|
| +-------------+ |     | +-------------+ |
| |   Actor A   | |     | |   Actor C   | |
| +-------------+ |     | +-------------+ |
| +-------------+ |     | +-------------+ |
| |   Actor B   | |     | |   Actor D   | |
| +-------------+ |     | +-------------+ |
| (Event Loop &   |     | (Event Loop &   |
|  Mailbox)       |     |  Mailbox)       |
+-----------------+     +-----------------+
```

### Initializing and Configuring the Engine

1.  **Instantiate `qb::Main`:** Your application typically starts by creating an instance of `qb::Main`.
    ```cpp
    #include <qb/main.h> // For qb::Main, qb::CoreInitializer, qb::CoreIdSet
    #include "MyActor.h" // Your actor definitions

    int main() {
        qb::Main engine;
        // ... configuration and actor addition follow ...
    }
    ```

2.  **(Optional) Configure `VirtualCore` Behavior (Before Starting):**
    `qb::Main` allows you to fine-tune the behavior of each `VirtualCore` *before* the engine starts. You access a core's configuration through `engine.core(core_id)`, which returns a `qb::CoreInitializer&`.
    *   **Set Event Loop Latency:** `core_initializer.setLatency(nanoseconds)`:
        *   `0` (default): Lowest latency; the `VirtualCore` spins actively, consuming 100% CPU. Ideal for highly responsive tasks.
        *   `>0`: The `VirtualCore` may sleep for up to this duration if idle, reducing CPU usage at the cost of slightly increased event processing latency.
    *   **Set CPU Affinity:** `core_initializer.setAffinity(qb::CoreIdSet{cpu_id1, cpu_id2, ...})`:
        *   Pins the `VirtualCore` thread to specific physical CPU cores. This can improve performance by enhancing cache locality and reducing thread migration. Requires careful planning.
    ```cpp
    // Example: Configure core 1 for lower CPU usage, core 2 for high responsiveness
    qb::CoreIdSet core2_affinity = {2}; // Assuming physical core 2
    engine.core(1).setLatency(1000000); // 1ms latency for core 1
    engine.core(2).setAffinity(core2_affinity).setLatency(0); // Core 2 on physical CPU 2, no latency

    // Set a default latency for any other cores that might be implicitly created or used
    engine.setLatency(500000); // Default 0.5ms for other cores
    ```

3.  **Add Actors to Cores:** Assign your actor instances to specific `VirtualCore`s.
    *   `engine.addActor<MyActorType>(core_id, constructor_arg1, ...)`: Adds a single actor to the specified `core_id`.
    *   `engine.core(core_id).builder().addActor<ActorA>(...).addActor<ActorB>(...).idList()`: A fluent interface for adding multiple actors to the same core and retrieving their `ActorId`s.
    ```cpp
    // Add a LoggerService actor to core 0
    qb::ActorId logger_id = engine.addActor<LoggerService>(0, "system_log.txt");
    if (!logger_id.is_valid()) { /* Handle error, e.g., duplicate service */ }

    // Add multiple worker actors to core 1
    auto worker_ids = engine.core(1).builder()
                          .addActor<DataProcessor>(logger_id)
                          .addActor<ReportGenerator>(logger_id)
                          .idList(); // Returns std::vector<qb::ActorId>
    ```

### Running and Stopping the Engine

*   **`engine.start(bool async = true)`:** This crucial method launches the `VirtualCore` threads and starts their event loops.
    *   `async = true` (default): `start()` returns immediately. Your `main()` thread (or the calling thread) continues execution independently. You'll typically call `engine.join()` later to wait for the engine to shut down.
    *   `async = false`: The calling thread **becomes** one of the `VirtualCore` worker threads (usually the one with the lowest available `core_id` if not explicitly configured, or the last one in a single-threaded setup for `start(false)`). This call **blocks** until the entire engine is stopped.
*   **`engine.join()`:** If you started the engine with `async = true`, call `join()` on the `engine` object in your main thread. This will block until all `VirtualCore` threads have completed their shutdown and terminated.
*   **`qb::Main::stop()` (Static Method):** This is the recommended way to initiate a graceful shutdown of the entire actor system. It can be called from any thread, including OS signal handlers. It typically works by sending `qb::KillEvent` or a similar signal to all actors/cores.
*   **Signal Handling:** By default, `qb::Main` registers handlers for `SIGINT` and `SIGTERM` (on POSIX-like systems) that will call `qb::Main::stop()`. You can customize this using `qb::Main::registerSignal(int signum)` and `qb::Main::unregisterSignal(int signum)`.
*   **Error Checking (`engine.hasError()`):** After `engine.join()` returns, call `engine.hasError()` to check if any `VirtualCore` terminated prematurely due to an unhandled exception or other critical error.

```cpp
// Typical asynchronous startup and shutdown
int main() {
    qb::Main engine;
    // ... configure engine and add actors ...

    engine.start(); // Start asynchronously

    qb::io::cout() << "QB Engine running in background. Main thread can do other work or wait.\n";
    // Example: Let the engine run for a while, then stop it
    // In a real server, this might be an indefinite wait or controlled by other logic
    std::this_thread::sleep_for(std::chrono::seconds(10));
    qb::io::cout() << "Requesting engine stop...\n";
    qb::Main::stop(); // Signal all cores/actors to stop

    engine.join(); // Wait for graceful shutdown

    if (engine.hasError()) {
        qb::io::cout() << "Engine stopped with an error!\n";
        return 1;
    }
    qb::io::cout() << "Engine stopped successfully.\n";
    return 0;
}
```

**(Reference:** `test-main.cpp` for various startup/shutdown scenarios. All examples in `example/` showcase `qb::Main` usage.**)

## `qb::VirtualCore`: The Actor's Execution Environment

(`qb/core/VirtualCore.h`)

A `qb::VirtualCore` represents a single, independent worker thread that is responsible for executing the actors assigned to it. You don't typically interact with `VirtualCore` objects directly; `qb::Main` manages them.

*   **Execution Model & Event Loop:** At its heart, each `VirtualCore` runs an event loop powered by `qb::io::async::listener` (from the `qb-io` library). This loop is the engine that drives all activity for the actors on that core.
*   **Mailbox & Event Queues:** To handle messages:
    *   **Inter-Core Mailbox:** Each `VirtualCore` has an incoming MPSC (Multiple-Producer, Single-Consumer) queue. When an actor on *another* core sends an event to an actor on *this* core, the event data is placed into this mailbox.
    *   **Local Event Queue:** Events sent between actors residing on the *same* `VirtualCore` (e.g., via `actor.push()`) are typically handled through a more direct local queueing mechanism within that core.
*   **Sequential Actor Processing (The Key Guarantee):** A `VirtualCore` processes **one event for one actor to completion** before moving to the next event or any other task (like callbacks) for any actor on that same core. This sequential execution of an actor's event handlers is what provides inherent thread safety for an actor's internal state, eliminating the need for manual locking *within* an actor for its own data.
*   **Scheduling Cycle (Conceptual):** In each iteration of its main processing loop, a `VirtualCore` generally performs these steps:
    1.  **Process I/O Events:** Polls its `qb::io::async::listener` for any pending I/O events (socket readiness, file changes, timer expirations from `qb::io::async::callback` or `qb::io::async::with_timeout`) and dispatches them.
    2.  **Process Inter-Core Mailbox:** Dequeues and processes events that have arrived from other `VirtualCore`s.
    3.  **Process Local Event Queue:** Dequeues and processes events sent between actors on this same core.
    4.  **Execute Callbacks:** Invokes the `onCallback()` method for all actors on this core that have registered via `qb::ICallback`.
    5.  **Flush Outgoing Pipes:** Sends any buffered outgoing events to their destination `VirtualCore` mailboxes.
    6.  **Idle Behavior:** If configured with a `latency > 0` and there's no immediate work, the `VirtualCore` may briefly sleep to conserve CPU resources.

## Inter-Core Communication Internals: A Glimpse

(`qb/core/Main.h` - `SharedCoreCommunication`, `qb/system/lockfree/mpsc.h`)

While largely transparent to the application developer, understanding the basics of inter-core messaging can be insightful:

*   **`SharedCoreCommunication`:** An internal component managed by `qb::Main`. It owns and provides access to the MPSC mailboxes for all `VirtualCore`s.
*   **Mailboxes (MPSC Queues):** Each `VirtualCore` has one such incoming mailbox. Events destined for actors on this core from *other* cores are enqueued here. The use of lock-free MPSC queues (`qb::lockfree::mpsc::ringbuffer`) is critical for minimizing contention and maximizing throughput when multiple cores are sending events to a single target core.
*   **`VirtualPipe`:** When an actor calls `push()` or `getPipe().push()`, events are initially buffered in per-destination `VirtualPipe` objects within the sending `VirtualCore`. The `VirtualCore` then flushes these pipes at an appropriate point in its loop, transferring the event data to the destination core's MPSC mailbox.

Inter-Core Event Flow (Actor A on VC0 sends to Actor C on VC1):
```text
+-----------------+       +-----------------+       +----------------------+
| VirtualCore 0   |       | Shared MPSC     |       | VirtualCore 1        |
|  +-----------+  |       | Mailbox for VC1 |       |      +-----------+   |
|  | Actor A   |-->Event->| (from VC0)      |--->Event---->| Actor C   |   |
|  | (Sender)  |  |       +-----------------+       |      | (Receiver)|   |
|  +-----------+  |                                 |      +-----------+   |
+-----------------+                                 +----------------------+

1. Actor A (on VC0) calls push<Event>(actor_c_id, ...)
2. VC0 places event data into VC1's MPSC Mailbox.
3. VC1, during its event loop, dequeues event from its Mailbox.
4. VC1 dispatches event to Actor C's on(Event&) handler.
```

This architecture is designed to ensure efficient, low-contention message passing, forming the backbone of QB's scalable actor communication.

**(Next:** Explore `[QB-Core: Actor Patterns & Utilities](./patterns.md)` or review `[Core Concepts: Concurrency and Parallelism in QB](./../2_core_concepts/concurrency.md)` for a higher-level view.**)
**(Reference Examples:** `test-actor-event.cpp` (Multi core, High Latency tests), `test-actor-service-event.cpp` for inter-core messaging demonstrations.**) 