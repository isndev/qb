# Core Concept: QB Event System

Events are the **sole mechanism** for communication between actors in the QB framework. They are plain C++ structs or classes inheriting publicly from `qb::Event`.

## `qb::Event` Base Class

(`qb/include/qb/core/Event.h`)

All custom events must inherit from `qb::Event`. The base class provides essential metadata used by the framework:

*   **`id` (`qb::EventId`):** A compile-time or runtime (debug vs release) identifier for the *type* of the event. Generated internally using `qb::type_id<T>()`.
*   **`dest` (`qb::ActorId`):** The intended recipient actor's ID.
*   **`source` (`qb::ActorId`):** The ID of the actor that sent the event.
*   **`is_alive()`:** A flag used internally (primarily for `reply`/`forward`) to check if the event object is still valid.
*   **`getQOS()`:** Internal Quality of Service indicator (not typically used directly by application code).
*   **`getSize()`:** The size of the memory block allocated for this event in the framework's internal buffer (in multiples of `QB_LOCKFREE_EVENT_BUCKET_BYTES`).

**Design Principles:**

*   **Data Carriers:** Events should primarily hold the data needed for the receiving actor to perform an action. Avoid complex logic within event classes.
*   **Immutability (Logical):** Once sent, consider an event immutable. If a receiver needs to modify the data and send it on, it should typically create a *new* event, unless using `reply` or `forward` efficiently.

```cpp
#include <qb/event.h>
#include <qb/actorid.h>
#include <string>
#include <vector>
#include <memory>

// Simple event
struct UpdateCounter : qb::Event {
    int delta;
    UpdateCounter(int d) : delta(d) {}
};

// Event with string data (non-trivial)
struct LogMessage : qb::Event {
    std::string message;
    explicit LogMessage(std::string msg) : message(std::move(msg)) {}
};

// Event carrying large data via shared_ptr (efficient)
struct ProcessData : qb::Event {
    std::shared_ptr<std::vector<double>> data_buffer;
    explicit ProcessData(std::shared_ptr<std::vector<double>> buf) : data_buffer(std::move(buf)) {}
};
```

**Performance Notes:**
*   Events are allocated from efficient internal pools, often aligned to cache lines (`QB_LOCKFREE_EVENT_BUCKET_BYTES`).
*   Large events might span multiple buckets.
*   **Trivial Destructibility:** Events sent via `send()` (unordered) *must* be trivially destructible (contain only POD types or types like `qb::string`). Events sent via `push()` (ordered, default) *can* contain types like `std::string`, `std::vector`, or `std::shared_ptr` as their destructors will be properly called by the framework.

## Event Delivery

Actors use methods inherited from `qb::Actor` to send events. See `[QB-Core: Messaging](./../4_qb_core/messaging.md)` for detailed explanations of:

*   **`push<T>(dest, ...)`:** Ordered, default.
*   **`send<T>(dest, ...)`:** Unordered, requires trivial destructor.
*   **`broadcast<T>(...)` / `push<T>(BroadcastId(core), ...)`:** System-wide or core-wide delivery.
*   **`reply(Event&)`:** Efficient reply to source.
*   **`forward(dest, Event&)`:** Efficient redirection.
*   **`to(dest).push<T>(...)`:** Builder for chained pushes.
*   **`getPipe(dest).push<T>(...)` / `allocated_push<T>(...)`:** Pipe access for performance tuning.

## Event Handling

Actors process events they subscribe to:

1.  **Registration (`registerEvent<T>(*this)`):** Crucially called within the actor's `onInit()` method for each event type it needs to handle.
2.  **Handler Method (`on(const T&)` or `on(T&)`):** Implement a public method with the exact signature matching the event type. The non-const version `on(T& event)` allows modifying the event object, which is necessary if you intend to use `reply(event)` or `forward(..., event)`.

```cpp
class MyHandler : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<UpdateCounter>(*this);
        registerEvent<LogMessage>(*this);
        registerEvent<ProcessData>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    // Handles UpdateCounter
    void on(const UpdateCounter& event) {
        // Process event.delta
    }

    // Handles LogMessage (const ref is fine if not replying/forwarding)
    void on(const LogMessage& event) {
        // Log event.message
    }

    // Handles ProcessData
    void on(const ProcessData& event) {
        if (event.data_buffer) {
            // Process *event.data_buffer
        }
    }

    void on(const qb::KillEvent& event) {
        kill();
    }
};
```

*   **Dispatch:** The `VirtualCore` uses the event's type ID (`event.getID()`) and destination (`event.getDestination()`) to route the event to the correct actor and invoke the matching `on()` handler via an internal router (`qb::router::memh`).
*   **Serialization:** Event handling within an actor is strictly sequential.

**(See also:** `[QB-Core: Actor](./../4_qb_core/actor.md)`, `[QB-Core: Messaging](./../4_qb_core/messaging.md)`**)** 