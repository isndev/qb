# QB-Core: Event Messaging

Actors in QB communicate exclusively by sending asynchronous messages called Events. This chapter details how to define, send, and handle these events.

## Defining Events (`qb::Event`)

*   **Header:** `qb/include/qb/core/Event.h`
*   **Inheritance:** All custom events *must* publicly inherit from `qb::Event`.
*   **Purpose:** Events act as data carriers. They should contain the information the receiving actor needs to perform an action. Avoid putting complex logic within the event itself.
*   **Base Members:** The base `qb::Event` provides:
    *   `id`: Event type identifier (used internally).
    *   `dest`: Destination `qb::ActorId`.
    *   `source`: Sender `qb::ActorId`.
    *   `is_alive()`, `getQOS()`, `getSize()`: Internal state/info.
*   **Custom Data:** Add member variables to your derived event struct/class to carry application-specific data.
*   **Trivial Destructibility:** Events sent via `send()` **must** be trivially destructible (contain only POD or types like `qb::string`). Events sent via `push()` (default) **can** contain non-trivially destructible types like `std::string`, `std::vector`, `std::shared_ptr`.
*   **Large Data:** Pass large data payloads using `std::shared_ptr` within the event to avoid expensive copying during serialization/deserialization, especially for inter-core communication.

```cpp
#include <qb/event.h>
#include <qb/string.h>
#include <vector>
#include <string>
#include <memory>

// Simple signal
struct StartProcessing : qb::Event {};

// Event with simple data
struct UpdateValue : qb::Event {
    int key;
    double value;
    UpdateValue(int k, double v) : key(k), value(v) {}
};

// Event with non-trivial data (for push)
struct LogEntry : qb::Event {
    std::string message; // std::string is okay for push()
    explicit LogEntry(std::string msg) : message(std::move(msg)) {}
};

// Event with large data (use shared_ptr)
struct LargeDataset : qb::Event {
    std::shared_ptr<std::vector<uint8_t>> data;
    explicit LargeDataset(std::shared_ptr<std::vector<uint8_t>> d) : data(std::move(d)) {}
};
```

**(See also:** `[Core Concepts: Event System](./../2_core_concepts/event_system.md)`**)

## Sending Events (from within an Actor)

Actors use methods inherited from `qb::Actor`.

### `push<EventType>(dest, args...)`

*   **How:** `auto& ev = push<UpdateValue>(target_id, key, value);`
*   **Delivery:** **Ordered** (relative to other pushes from *this* actor to the *same* `dest`), asynchronous.
*   **Mechanism:** Queued locally, sent at end of core loop.
*   **Return:** `EventType&` - Reference to the event *before* it's sent, allowing modification.
*   **Types:** Handles non-trivially destructible events.
*   **Use Case:** **Recommended default** for most communication where order is important or events contain complex types.
*   **(Ref:** `test-actor-event.cpp::BasicPushActor`**)

### `send<EventType>(dest, args...)`

*   **How:** `send<StartProcessing>(target_id);`
*   **Delivery:** **Unordered**, asynchronous.
*   **Mechanism:** Attempts more direct delivery (especially same-core), potentially lower latency but no order guarantees.
*   **Types:** Requires `EventType` to be **trivially destructible**.
*   **Use Case:** Rare. Only when order doesn't matter, same-core latency is critical, *and* the event is trivial.
*   **(Ref:** `test-actor-event.cpp::BasicSendActor`**)

### `broadcast<EventType>(args...)`

*   **How:** `broadcast<SystemAlert>("High Temperature!");`
*   **Delivery:** Sends to *all* actors on *all* cores managed by the `qb::Main` instance.
*   **Use Case:** System-wide notifications.
*   **(Ref:** `test-actor-broadcast.cpp`**)

### `push<EventType>(BroadcastId(core_id), args...)`

*   **How:** `push<ConfigUpdate>(qb::BroadcastId(1), new_config_data);`
*   **Delivery:** Sends (ordered relative to other pushes from sender) to *all* actors on the *specified* `core_id`.
*   **Use Case:** Core-specific notifications.
*   **(Ref:** `test-actor-broadcast.cpp`**)

### `reply(Event& original_event)`

*   **How:** `void on(MyRequest& req) { req.result = compute(); reply(req); }`
*   **Requires:** The `on()` handler must take a non-const `Event&`.
*   **Mechanism:** Reuses the received event object, sending it back to `original_event.getSource()`. Most efficient way to reply.
*   **Consumption:** The `original_event` object becomes invalid after `reply()`.
*   **(Ref:** `test-actor-event.cpp::TestReceiveReply`**)

### `forward(ActorId new_dest, Event& original_event)`

*   **How:** `void on(WorkItem& item) { forward(worker_id, item); }`
*   **Requires:** The `on()` handler must take a non-const `Event&`.
*   **Mechanism:** Reuses the received event object, sending it to `new_dest` but keeping the original `source`.
*   **Consumption:** The `original_event` object becomes invalid after `forward()`.
*   **(Ref:** `test-actor-event.cpp::TestReceiveReply`**)

### Advanced: `to(dest)` and `getPipe(dest)`

*   **`to(dest).push<T>(...)`:** Returns an `EventBuilder` for chaining multiple `push` calls to the same `dest`. Minor optimization.
    ```cpp
    to(stats_actor_id)
        .push<StatIncrement>(counter_a)
        .push<StatIncrement>(counter_b);
    ```
*   **`getPipe(dest)`:** Returns a `qb::Pipe` object.
    *   `pipe.push<T>(...)`: Similar to actor's `push`.
    *   **`pipe.allocated_push<T>(payload_size, args...)`:** Use this for events containing large variable data (e.g., passed via `shared_ptr`). Pre-allocating `sizeof(T) + payload_size` helps avoid buffer reallocations in the pipe.
    ```cpp
    qb::Pipe data_pipe = getPipe(data_handler_id);
    auto large_buffer = std::make_shared<std::vector<uint8_t>>(10 * 1024 * 1024);
    // ... fill buffer ...
    // Pre-allocate space for event struct + approx buffer size
    auto& ev = data_pipe.allocated_push<LargeDataset>(large_buffer->size(), large_buffer);
    ```
*   **(Ref:** `test-actor-event.cpp` (various sender actors)**)

## Receiving and Handling Events

1.  **Register Handler:** In `onInit()`, call `registerEvent<EventType>(*this)` for each event the actor should process.
2.  **Implement Handler:** Define `void on(const EventType& event)` or `void on(EventType& event)`.
    *   Use `const EventType&` if you only need to read the event data.
    *   Use `EventType&` (non-const) if you intend to use `reply(event)` or `forward(..., event)`. Modify the event *before* calling reply/forward.

```cpp
class DataProcessor : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<ProcessData>(*this);
        registerEvent<UpdateConfig>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    // Read-only handling
    void on(const ProcessData& event) {
        if (event.data_buffer) {
            // Process data without modifying event
            auto result = process(*event.data_buffer);
            push<ProcessingResult>(event.getSource(), result);
        }
    }

    // Handling allowing reply/forward
    void on(UpdateConfig& event) { // Non-const
        if (applyConfig(event.config_data)) {
            event.status = "OK";
        } else {
            event.status = "Failed";
        }
        reply(event); // Send modified event back
    }

    void on(const qb::KillEvent& event) { kill(); }

private:
    Data process(const std::vector<uint8_t>& data) { /* ... */ return {}; }
    bool applyConfig(const ConfigData& data) { /* ... */ return true; }
};
```

**(See also:** `[QB-Core: Actor](./actor.md)`, `[Core Concepts: Event System](./../2_core_concepts/event_system.md)`**) 