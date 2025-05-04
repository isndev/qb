# Core Concept: The Actor Model in QB

The QB framework implements the [Actor Model](https://en.wikipedia.org/wiki/Actor_model) as its core concurrency paradigm. Actors are the fundamental primitives for computation and state management.

## `qb::Actor` - The Building Block

All actors in the system derive publicly from the `qb::Actor` base class.

**Key Characteristics:**

*   **Autonomous Unit:** Each actor is an independent unit with its own state and behavior.
*   **State Encapsulation:** An actor's internal state (member variables) is private and logically protected. Direct access from outside the actor is impossible. All interactions that might modify state occur via message passing.
*   **Message-Driven Behavior:** An actor's logic is defined by how it reacts to incoming messages (events). This logic resides in `on(Event&)` handler methods.
*   **Sequential Processing:** Each actor processes messages from its implicit mailbox one at a time, in the order they are received by its `VirtualCore`. This guarantees that an actor's internal state is never subject to data races from concurrent event handling *within that actor*.
*   **Asynchronous Communication:** Actors send messages (`qb::Event`) to other actors via their `qb::ActorId` without waiting for processing or reply.

```cpp
// Simplified Example (See example/core/simple_actor.cpp for a runnable version)
#include <qb/actor.h>
#include <qb/event.h>
#include <qb/io.h> // For qb::io::cout
#include <iostream>

struct CountEvent : qb::Event {
    int increment;
};

class CounterActor : public qb::Actor {
private:
    int _count = 0;

public:
    // Initialization - Called once after construction and ID assignment
    bool onInit() override {
        qb::io::cout() << "CounterActor [" << id() << "] Initialized." << std::endl;
        // *** Crucial: Register event handlers here ***
        registerEvent<CountEvent>(*this);
        registerEvent<qb::KillEvent>(*this); // Best practice: handle shutdown
        return true; // Return false to abort actor creation
    }

    // Event Handler for CountEvent
    void on(const CountEvent& event) {
        _count += event.increment;
        qb::io::cout() << "CounterActor [" << id() << "] received CountEvent. Count is now: " << _count << std::endl;

        // Example termination condition
        if (_count >= 5) {
            qb::io::cout() << "CounterActor [" << id() << "] reached target count, terminating." << std::endl;
            kill();
        }
    }

    // Event Handler for KillEvent
    void on(const qb::KillEvent& event) {
        qb::io::cout() << "CounterActor [" << id() << "] received KillEvent. Shutting down." << std::endl;
        // Perform any pre-shutdown cleanup here if necessary
        kill(); // MUST call base kill() to actually terminate
    }

    // Destructor - Called after the actor is terminated and removed
    ~CounterActor() override {
        qb::io::cout() << "CounterActor [" << id() << "] destroyed. Final count: " << _count << std::endl;
    }
};
```

## Actor Identification: `qb::ActorId`

Every actor running in the system has a unique `qb::ActorId` (`qb/include/qb/core/ActorId.h`). This ID is crucial for addressing messages.

*   **Structure:** A 32-bit identifier combining:
    *   `CoreId` (typically uint16_t): The index of the `VirtualCore` thread hosting the actor.
    *   `ServiceId` (typically uint16_t): A unique ID *within* that core.
*   **Obtaining:** `id()` method within an actor; returned by `main.addActor()`, `core.addActor()`, `actor.addRefActor()->id()`.
*   **Methods:**
    *   `sid()`: Get the Service ID part.
    *   `index()`: Get the Core ID part.
    *   `is_valid()`: Check if the ID is not the default/invalid ID (`ActorId()`).
    *   `is_broadcast()`: Check if it's a broadcast ID.
*   **Special IDs:**
    *   `ActorId()`: Default-constructed, invalid ID.
    *   `qb::BroadcastId(core_id)`: An `ActorId` specifically constructed to target all actors on a given core (used with `push`/`send`). `ActorId::BroadcastSid` is the underlying `ServiceId` constant.

```cpp
qb::ActorId self = id();
qb::ActorId target = ...; // Get target ID
qb::ActorId invalid_id;

if (target.is_valid()) {
    push<MyEvent>(target);
}

// Broadcast to all actors on core 1
push<SystemUpdate>(qb::BroadcastId(1));
```

## Actor Lifecycle

**(Detailed lifecycle steps:** `[QB-Core: Actor Lifecycle](./lifecycle.md)`**)**

The lifecycle involves construction, initialization (`onInit`), event processing, termination (`kill`), and destruction (`~Actor`). `onInit` is critical for registration. RAII should be used for resource management tied to the destructor.

**(Ref:** `test-actor-lifecycle-hooks.cpp`**)

## State Management Principles

*   **Encapsulation:** Keep actor state in private/protected member variables.
*   **Sequential Access:** The framework guarantees that only one event handler (or callback) for a specific actor instance runs at any given time on its `VirtualCore`. This eliminates the need for internal locks to protect the actor's own state against concurrent access *by its own methods*.
*   **No Blocking:** Handlers (`on(Event&)`) and callbacks (`onCallback()`) **must not block**. Blocking operations (long computations, synchronous I/O, waiting on locks) will stall the entire `VirtualCore`, preventing other actors on that core from executing.
*   **External State:** If an actor needs to interact with shared state *outside* the actor model (e.g., a global resource, a non-thread-safe library), use appropriate C++ synchronization mechanisms (mutexes, atomics) or, preferably, encapsulate that shared resource within another dedicated actor (See [Shared Resource Manager Pattern](./../6_guides/patterns_cookbook.md#pattern-shared-resource-manager)).

**(Ref:** `example6_shared_queue.cpp` uses an external thread-safe queue, but access logic could be encapsulated in an actor.**) 