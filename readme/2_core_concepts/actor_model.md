@page core_concepts_actor_model_md Core Concept: The Actor Model in QB
@brief Understand how QB implements the Actor Model for robust, concurrent programming, focusing on `qb::Actor` and `qb::ActorId`.

# Core Concept: The Actor Model in QB

The QB framework is built upon the [Actor Model](https://en.wikipedia.org/wiki/Actor_model), a powerful paradigm for designing concurrent and distributed systems. Instead of directly managing threads and locks, you work with **actors**: independent, isolated units of computation and state.

## `qb::Actor`: The Heart of Your Concurrent Logic

Every actor in your system will inherit from the `qb::Actor` base class (`qb/core/Actor.h`). Think of an actor as a specialized object with these key characteristics:

*   **Autonomous and Isolated:** Each actor is a self-contained entity. Its internal data (member variables, or "state") is strictly private and protected from direct access by any other actor. This is fundamental to preventing data races and simplifying concurrent state management.
*   **Message-Driven:** Actors interact *exclusively* by sending asynchronous messages (called **events**) to one another. An actor's behavior is defined by how it reacts to the events it receives.
*   **Sequential Processing:** Each actor has an implicit mailbox where incoming events are queued. The actor processes these events one at a time, in the order they are effectively received by its managing `VirtualCore`. This sequential processing of its own events guarantees that an actor's internal state is modified safely, without internal race conditions.

A simplified view of actor interaction:
```text
+-----------+      +-----------------+      +-----------+
|  Actor A  |----->|   Event Message |----->|  Actor B  |
| (Sender)  |      | (e.g., MyEvent) |      | (Receiver)|
+-----------+      +-----------------+      +-----------+
     |                                           ^
     | 1. push<MyEvent>(actor_b_id, ...)         | 3. Processes MyEvent
     +-------------------------------------------+
                                                 | 2. Event arrives in
                                                 |    Actor B's mailbox
```

### Defining a Simple Actor

Let's look at a basic example. (A runnable version can be found in `example/core/example1_simple_actor.cpp`.)

```cpp
// Include necessary QB headers
#include <qb/actor.h> // For qb::Actor, qb::ActorId
#include <qb/event.h>   // For qb::Event
#include <qb/io.h>      // For qb::io::cout (thread-safe console output)
#include <iostream>     // For std::endl

// --- 1. Define Your Event(s) ---
// Events are simple structs or classes inheriting from qb::Event
struct CountEvent : qb::Event {
    int increment_by;
    // Constructor to easily create the event with data
    explicit CountEvent(int amount) : increment_by(amount) {}
};

// --- 2. Define Your Actor ---
class CounterActor : public qb::Actor {
private:
    // --- Actor's Private State ---
    int _current_count = 0;

public:
    // --- Constructor (optional) ---
    CounterActor() = default;

    // --- Initialization (Essential) ---
    // Called once after construction and after the actor is assigned its unique ID.
    bool onInit() override {
        qb::io::cout() << "CounterActor [" << id() << "] initialized on core " << getIndex() << ".\n";
        
        // *** CRUCIAL: Register event handlers here! ***
        registerEvent<CountEvent>(*this);       // This actor will now handle CountEvent
        registerEvent<qb::KillEvent>(*this);  // Best practice: always handle KillEvent for graceful shutdown
        
        return true; // Return false to prevent the actor from starting
    }

    // --- Event Handlers ---
    // Implement public methods to handle registered events.
    // The method signature must match the event type.
    void on(const CountEvent& event) {
        _current_count += event.increment_by;
        qb::io::cout() << "CounterActor [" << id() << "] received CountEvent. Count is now: " << _current_count << ".\n";

        // Example: Actor decides to terminate itself based on its state
        if (_current_count >= 10) {
            qb::io::cout() << "CounterActor [" << id() << "] reached target. Terminating.\n";
            kill(); // Request self-termination
        }
    }

    void on(const qb::KillEvent& /*event*/) { // Parameter often unused for KillEvent
        qb::io::cout() << "CounterActor [" << id() << "] received KillEvent. Shutting down.\n";
        // Perform any pre-shutdown cleanup specific to this actor if necessary
        kill(); // IMPORTANT: Must call base kill() to complete termination
    }

    // --- Destructor (Optional but good for RAII) ---
    // Called after the actor has fully terminated and is being removed from the system.
    ~CounterActor() override {
        qb::io::cout() << "CounterActor [" << id() << "] destroyed. Final count: " << _current_count << ".\n";
    }
};
```

Key takeaways from the example:
*   **`onInit()` is Vital:** This is where you *must* call `registerEvent<YourEventType>(*this)` for every type of event your actor intends to process.
*   **Event Handlers:** Public methods named `on` with a parameter of the event type (e.g., `void on(const CountEvent& event)`).
*   **Self-Termination:** An actor can decide to shut itself down by calling `kill()`.
*   **RAII:** Use member variables with destructors (like `std::unique_ptr`, `std::fstream`, custom classes) for resource management. They will be cleaned up when the actor is destroyed.

## Actor Identity: `qb::ActorId`

Every active actor in the QB system possesses a unique `qb::ActorId` (`qb/core/ActorId.h`). This ID is the "address" you use to send events to a specific actor.

*   **Composition:** It's a 32-bit identifier combining:
    *   `CoreId` (uint16_t): The index of the `VirtualCore` (worker thread) hosting the actor.
    *   `ServiceId` (uint16_t): An ID unique *within that specific core*.
*   **How to Get It:**
    *   Inside an actor: Call the `id()` method.
    *   When creating an actor: Returned by `main.addActor<MyActor>(core_id, ...)`, `core.addActor<MyActor>(...)`, or `parent_actor.addRefActor<ChildActor>()->id()`.
*   **Key `qb::ActorId` Methods:**
    *   `sid()`: Returns the `ServiceId` part.
    *   `index()`: Returns the `CoreId` part.
    *   `is_valid()`: Checks if the ID is a valid, assigned ID (not the default-constructed, invalid `ActorId()`).
    *   `is_broadcast()`: Checks if it's a special broadcast ID.
*   **Special `ActorId` Values:**
    *   `qb::ActorId()`: A default-constructed, invalid ID. Useful for uninitialized ID members.
    *   `qb::BroadcastId(core_id)`: A special ID used with `push<Event>(BroadcastId(core_id), ...)` to send an event to *all* actors running on the specified `core_id`. The underlying `ServiceId` for this is `ActorId::BroadcastSid`.

```cpp
// Inside an actor method:
qb::ActorId self_id = id();
qb::ActorId some_target_actor_id = getTarget(); // Assume getTarget() returns a valid ActorId

if (some_target_actor_id.is_valid()) {
    push<MyEvent>(some_target_actor_id, /* event data */);
}

// To broadcast an event to all actors on core 1:
qb::ActorId core1_broadcast_target = qb::BroadcastId(1);
push<SystemUpdateEvent>(core1_broadcast_target, /* update data */);
```

## Actor Lifecycle: A Brief Overview

The journey of an actor involves several stages, from creation to destruction. The `onInit()` and `~Actor()` methods are critical hooks into this lifecycle.

**(For a detailed step-by-step breakdown, see:** [QB-Core: Actor Lifecycle](./lifecycle.md)**)**

## Managing Actor State

*   **Encapsulation is Key:** Always keep an actor's state (its member variables) `private` or `protected`.
*   **Sequential Access = Thread Safety:** Because a `VirtualCore` processes events for an actor one by one, you don't need mutexes or other locks to protect the actor's *own* member variables from race conditions caused by *its own event handlers*.
*   **Avoid Blocking:** Event handlers (`on(Event&)` methods) and `ICallback::onCallback()` implementations **must never block**. Long computations, synchronous I/O (like reading a file directly in a handler), or waiting on external locks will freeze the entire `VirtualCore`, preventing it from processing events for *any* actor assigned to it. Offload such tasks using `qb::io::async::callback` or by delegating to other specialized actors.
*   **Interacting with External Shared State:** If an actor *must* interact with resources shared outside the actor model (e.g., a global static variable, a non-thread-safe third-party library), you are responsible for ensuring thread-safe access to that external resource. Consider encapsulating such resources within a dedicated "manager" actor to serialize access.

By adhering to these principles, you can build complex, concurrent applications where state management is significantly simplified and common concurrency bugs are inherently avoided.

**(Next:** [Core Concepts: QB Event System](./event_system.md)**)** 