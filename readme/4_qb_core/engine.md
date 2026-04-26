@page qb_core_engine_md QB-Core: Engine - `qb::Main` & `VirtualCore`
@brief Understand the QB actor system's engine, how `qb::Main` orchestrates `VirtualCore` worker threads, and how actors are scheduled.

# QB-Core: Engine — `qb::Main` & `VirtualCore`

The QB Actor Framework's engine is the runtime that brings actors to life, manages their execution, and routes events across cores. It is composed of two closely related classes: `qb::Main` (the orchestrator) and `qb::VirtualCore` (the worker).

---

## `qb::Main` — The Engine Orchestrator

(`qb/core/Main.h`)

`qb::Main` is the single entry point for the entire actor system. Create one instance, configure it, add actors, and call `start()`.

### Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│                        qb::Main                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │            SharedCoreCommunication                 │  │
│  │   (MPSC mailboxes — one per VirtualCore)           │  │
│  └──────────┬───────────────────────┬─────────────────┘  │
│             │                       │                     │
│   ┌─────────▼────────┐   ┌──────────▼───────┐            │
│   │  VirtualCore 0   │   │  VirtualCore 1   │  …         │
│   │  (std::jthread)  │   │  (std::jthread)  │            │
│   │  ┌────────────┐  │   │  ┌────────────┐  │            │
│   │  │  Actor A   │  │   │  │  Actor C   │  │            │
│   │  ├────────────┤  │   │  ├────────────┤  │            │
│   │  │  Actor B   │  │   │  │  Actor D   │  │            │
│   │  └────────────┘  │   │  └────────────┘  │            │
│   └──────────────────┘   └──────────────────┘            │
└──────────────────────────────────────────────────────────┘
```

Each `VirtualCore` runs as an independent `std::jthread`. `qb::Main` holds a `std::stop_source` and distributes a `std::stop_token` to each worker, enabling clean cancellation without OS signals.

---

### 1. Initialising the Engine

```cpp
#include <qb/main.h>
#include "MyActors.h"

int main() {
    qb::Main engine;

    // Add actors to specific cores
    auto logger_id = engine.addActor<LoggerService>(0);
    engine.addActor<WorkerA>(1, logger_id);
    engine.addActor<WorkerB>(1, logger_id);

    // Start and wait
    engine.start();   // async = true (default)
    engine.join();

    return engine.hasError() ? 1 : 0;
}
```

---

### 2. Configuring VirtualCores (`CoreInitializer`)

Access per-core configuration via `engine.core(core_id)` **before** calling `start()`.

```cpp
// Core 0: latency-sensitive, pinned to physical CPU 0
engine.core(0)
    .setLatency(0)                       // busy-spin — 100% CPU, minimum latency
    .setAffinity(qb::CoreIdSet{0});      // pin to physical core 0

// Core 1: background work, low CPU priority
engine.core(1)
    .setLatency(1'000'000)               // sleep up to 1 ms when idle
    .setAffinity(qb::CoreIdSet{1, 2});   // may run on physical core 1 or 2

// Apply a default latency to all cores not explicitly configured
engine.setLatency(500'000);  // 500 µs default
```

**Latency modes:**

| `setLatency(ns)` | Behaviour | Trade-off |
|-----------------|-----------|-----------|
| `0` | Busy-spin; polls continuously | Lowest latency, 100% CPU on that core |
| `> 0` | Sleeps up to `ns` nanoseconds when idle | Saves CPU; adds worst-case latency |

**CPU affinity** (`setAffinity`): pins the worker thread to the specified physical CPUs. Use `qb::NoAffinity` as a sentinel to explicitly request no pinning:
```cpp
engine.core(0).setAffinity(qb::CoreIdSet{qb::NoAffinity});
```

---

### 3. Adding Actors

```cpp
// Single actor — returns ActorId (invalid on failure)
qb::ActorId id = engine.addActor<MyActor>(/*core=*/0, arg1, arg2);
if (!id.is_valid()) { /* handle error */ }

// Fluent builder for multiple actors on the same core
auto ids = engine.core(1)
               .builder()
               .addActor<DataProcessor>(logger_id)
               .addActor<ReportGenerator>(logger_id)
               .idList();   // std::vector<qb::ActorId>
```

> **Note:** All actors must be added **before** `engine.start()`. Actors created at runtime must be spawned from within a running actor using `addRefActor<T>()` / `addRefHandle<T>()`.

---

### 4. Starting, Stopping, and Joining

```cpp
// Asynchronous start — returns immediately
engine.start(true);
// ...do other work or just wait...
engine.join();    // block until engine fully shuts down

// Synchronous start — calling thread becomes a VirtualCore worker
// (blocks until all cores stop)
engine.start(false);
```

**Initiating a graceful shutdown:**

```cpp
qb::Main::stop();   // callable from any thread, including signal handlers
```

`stop()` triggers the stop-token path: each worker synthesises a virtual `SIGINT` on its next iteration, sending `KillEvent` to all actors, draining queues, and exiting cleanly.

**Signal management:**

```cpp
qb::Main::registerSignal(SIGUSR1);  // route SIGUSR1 through the engine → stop()
qb::Main::unregisterSignal(SIGTERM);
qb::Main::ignoreSignal(SIGPIPE);    // common for network servers
```

By default `SIGINT` and `SIGTERM` are registered.

**Error checking:**

```cpp
engine.join();
if (engine.hasError()) {
    // One or more VirtualCores terminated with an error.
    // Likely causes: an actor's onInit() returned false (BadActorInit),
    // or an unhandled exception was thrown inside a handler (ExceptionThrown).
    return 1;
}
```

---

## `qb::VirtualCore` — The Actor Execution Environment

(`qb/core/VirtualCore.h`)

A `VirtualCore` is the per-thread engine that owns a group of actors and drives their event processing. Application code never interacts with `VirtualCore` directly — `qb::Main` manages it entirely.

### Error Codes

```cpp
enum VirtualCore::Error : uint64_t {
    BadInit         = (1u << 9u),   // VirtualCore itself failed to initialise
    NoActor         = (1u << 10u),  // Expected actor was missing
    BadActorInit    = (1u << 11u),  // Actor's onInit() returned false
    ExceptionThrown = (1u << 12u),  // Unhandled exception in an event handler
};
```

These flags are OR-combined; `engine.hasError()` returns true if any are set.

### The Event Loop (Per Iteration)

Each `VirtualCore` iteration processes work in this order:

```
1. Poll std::stop_token  ──► if stop requested: synthesise SIGINT and break
2. Process I/O events    ──► qb::io::async::listener (timers, sockets, files)
3. Drain inter-core mailbox ──► MPSC ringbuffer from other VirtualCores
4. Drain local event queue  ──► same-core actor-to-actor events
5. Execute registered callbacks ──► ICallback::onCallback() for each registered actor
6. Flush outgoing pipes  ──► transfer buffered events to destination mailboxes
7. Idle (if latency > 0) ──► condition_variable wait up to configured latency
```

Step 1 ensures signal-free shutdown works on all platforms (Windows, Linux, macOS).

### Adaptive Backoff

The `VirtualCore` maintains a `_spin_credit` counter seeded by the amount of work done in the previous iteration. As long as there is activity, the core stays in a lock-free spin. Only when the credit drains to zero does the core park on a `condition_variable` (if `latency > 0`). This prevents unnecessary parking during traffic bursts while still saving CPU during genuine idle periods.

### Sequential Execution Guarantee

A `VirtualCore` processes **one** event for **one** actor to completion before starting the next. This is the foundation of QB's thread safety model: no actor handler can be interrupted or re-entered, so actor state needs **no** internal synchronisation.

---

## Inter-Core Communication Internals

(`qb/core/Main.h` — `SharedCoreCommunication`, `qb/system/lockfree/mpsc.h`)

```
Actor A (VC0)                VC1 Mailbox (MPSC)         Actor C (VC1)
┌─────────────┐   push<E>   ┌────────────────────┐     ┌────────────┐
│  VirtPipe   │ ──────────► │ ringbuffer<Bucket>  │ ──► │  on(E&)    │
│ (per dest)  │   (batch)   │ (lock-free enqueue) │     └────────────┘
└─────────────┘             └────────────────────┘

Steps:
1. Actor A calls push<E>(actor_c_id, ...)
2. Event written to VC0's VirtualPipe for VC1 (buffered locally)
3. At end of VC0 loop iteration, VirtualPipes are flushed → event copied to VC1 mailbox
4. VC1 dequeues the event in its next loop iteration
5. VC1 dispatches to Actor C's on(E&) handler
```

**Key components:**

- **`VirtualPipe`:** Per-destination buffer inside each `VirtualCore`. Events accumulate here and are batch-written to the MPSC mailbox for efficiency.
- **`SharedCoreCommunication`:** Owns all MPSC mailboxes. One mailbox per `VirtualCore` — multiple cores can write concurrently (MP), only the owning core reads (SC).
- **`SharedCoreCommunication::Mailbox`:** A lock-free `ringbuffer<EventBucket>` with an optional `condition_variable` for the low-latency sleep path.

---

## Complete Startup / Shutdown Example

```cpp
int main() {
    qb::Main engine;

    // Configure cores
    engine.core(0).setLatency(0);           // hot path — no sleep
    engine.core(1).setLatency(500'000);     // background — 500 µs max sleep

    // Register actors
    auto svc = engine.addActor<MyService>(0);
    engine.addActor<Worker>(1, svc);
    engine.addActor<Worker>(1, svc);

    // Optional: handle extra signals
    qb::Main::ignoreSignal(SIGPIPE);

    // Run
    engine.start();  // async
    qb::io::cout() << "Engine running.\n";
    engine.join();   // wait for shutdown

    if (engine.hasError()) {
        qb::io::cout() << "Terminated with error.\n";
        return 1;
    }
    return 0;
}
```

**(Next:** Explore [QB-Core: Actor Patterns & Utilities](./patterns.md) or review [Core Concepts: Concurrency and Parallelism](./../2_core_concepts/concurrency.md) for a higher-level view.)**
**(Reference Examples:** `test-actor-event.cpp`, `test-actor-service-event.cpp`, `test-main.cpp`)**
