# QB-Core: Engine (`qb::Main`, `VirtualCore`)

The QB engine is responsible for creating, managing, and scheduling actors across one or more threads (Virtual Cores).

## `qb::Main`: The Engine Controller

(`qb/include/qb/core/Main.h`)

This class is the entry point for managing the actor system.

### How to Initialize the Engine

1.  **Instantiate:** Create an instance of `qb::Main` in your application's main function.
    ```cpp
    #include <qb/main.h>
    int main() {
        qb::Main engine;
        // ... configure and add actors ...
    }
    ```
2.  **(Optional) Configure Cores:** Access specific `VirtualCore` configurations *before* starting.
    ```cpp
    // Get initializer for core 1
    qb::CoreInitializer& core1_config = engine.core(1);

    // Set core 1 to sleep for up to 1ms when idle
    core1_config.setLatency(1000000); // Nanoseconds

    // Pin core 1 to physical CPU 2 (example)
    qb::CoreIdSet affinity_set = {2};
    core1_config.setAffinity(affinity_set);

    // Set default latency for all other cores
    engine.setLatency(500000); // 0.5ms
    ```
3.  **Add Actors:** Add your actor instances to the desired cores.
    ```cpp
    #include "MyActor.h"
    // Add MyActor instance to core 0, passing constructor arguments
    qb::ActorId actor_id = engine.addActor<MyActor>(0, arg1, arg2);
    if (!actor_id.is_valid()) {
        // Handle error (e.g., duplicate service actor)
    }

    // Add multiple actors to core 1 using the builder
    auto actor_list = engine.core(1).builder()
        .addActor<WorkerActor>(/*args*/)
        .addActor<AnotherActor>(/*args*/)
        .idList(); // Get list of created ActorIds
    ```

### How to Run the Engine

*   **`start(bool async = true)`:** Launches the `VirtualCore` threads.
    *   `async = true` (default): Returns immediately. Your main thread continues execution separately.
    *   `async = false`: The calling thread *becomes* one of the worker threads (typically core 0 if available). This call blocks until the engine stops.
*   **`join()`:** If `start(true)` was used, call `join()` on the main thread to wait for the engine to complete its shutdown process.
*   **`stop()`:** Call `qb::Main::stop()` (static method) to signal a graceful shutdown. This can be called from any thread or a signal handler.
*   **Signal Handling:** `SIGINT` and `SIGTERM` (on non-Windows) automatically call `stop()`. Use `qb::Main::registerSignal()` / `unregisterSignal()` to customize signal handling.

```cpp
    // Start engine asynchronously
    engine.start();

    // ... main thread can do other things or wait ...

    // Wait for engine to stop (e.g., after stop() is called or error)
    engine.join();

    // Check for errors after joining
    if (engine.hasError()) {
        // Handle engine error
    }
```

**(Ref:** `test-main.cpp`, All `main()` functions in `example/`**)

## `qb::VirtualCore`: The Worker Thread & Scheduler

(`qb/include/qb/core/VirtualCore.h`)

Represents a single thread managing and executing a subset of the system's actors.

*   **Execution Model:** Each `VirtualCore` runs an **event loop** (`qb::io::async::listener`). In each iteration, it performs approximately these steps:
    1.  Processes I/O events from its `listener` (socket readiness, file events, timer expirations from `async::callback` / `with_timeout`).
    2.  Dequeues and processes events from its inter-core **mailbox** (messages sent from other cores).
    3.  Dequeues and processes events from its **local event queue** (messages sent via `push` from actors on the *same* core).
    4.  Invokes `onCallback()` for all actors on this core that have registered via `ICallback`.
    5.  Flushes outgoing event pipes to other cores.
    6.  If idle and `latency > 0`, potentially sleeps up to the configured latency.
*   **Actor Scheduling:** Actors don't have explicit time slices. An actor runs when one of its registered events arrives in the `VirtualCore`'s queue or when its `onCallback()` is due.
*   **Sequential Execution Guarantee:** The core processes **one event for one actor** to completion before moving to the next event. This ensures an actor's internal state is accessed sequentially, preventing data races *within that actor*.

## Inter-Core Communication Internals

(`qb/include/qb/core/Main.h`, `qb/include/qb/system/lockfree/mpsc.h`)

*   **`SharedCoreCommunication`:** Managed by `qb::Main`, holds the mailboxes for all cores.
*   **`Mailbox`:** A specialized `qb::lockfree::mpsc::ringbuffer`. Multiple producer cores can efficiently enqueue events into a single consumer core's mailbox without locks.
*   **`Pipe` / `VirtualPipe`:** Outgoing events added via `push` or `actor.getPipe().push()` are buffered in per-destination `VirtualPipe` objects. The `VirtualCore` flushes these pipes (sending data to the destination core's mailbox) at the end of its loop iteration.

This design minimizes cross-core contention and maximizes throughput for message passing.

**(Ref:** `test-actor-event.cpp` (Multi core, High Latency tests), `test-actor-service-event.cpp`**) 