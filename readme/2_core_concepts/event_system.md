@page core_concepts_event_system_md Core Concept: The QB Event System
@brief Dive into how actors communicate in QB using events, from definition to handling.

# Core Concept: The QB Event System

In the QB Actor Framework, **events are the exclusive means of communication between actors**. They are the lifeblood of the system, enabling actors to interact asynchronously and safely. Understanding events is crucial to designing effective actor-based applications.

## `qb::Event`: The Foundation of Actor Messages

All messages exchanged between actors must inherit publicly from the `qb::Event` base class, defined in `qb/core/Event.h`.

**Key Characteristics & Purpose:**

*   **Data Carriers:** Events are primarily designed to carry data. The receiving actor uses this data to decide what action to perform. Avoid embedding complex logic within event classes themselves.
*   **Asynchronous Nature:** Sending an event is a non-blocking operation. The sender dispatches the event and continues its work without waiting for the recipient to process it.
*   **Immutability (by convention):** While C++ doesn't enforce deep immutability easily, it's best practice to treat events as immutable once sent. If an actor receives an event and needs to pass on modified information, it should typically create and send a *new* event, unless specifically using efficient forwarding mechanisms like `reply()` or `forward()`.

**Base `qb::Event` Members (Framework-Managed):**

The `qb::Event` base class provides essential metadata used internally by the framework for routing and management:

*   `id` (`qb::EventId`): A type identifier for the event, generated using `qb::type_id<YourEventType>()`. Used by the framework to dispatch the event to the correct `on()` handler.
*   `dest` (`qb::ActorId`): The unique ID of the actor intended to receive this event.
*   `source` (`qb::ActorId`): The unique ID of the actor that sent this event.
*   `is_alive()`: An internal flag, primarily for efficient event reuse with `reply()` and `forward()`.
*   `getQOS()`: Quality of Service indicator (internal use).
*   `getSize()`: Size of the memory block allocated for this event (internal use).

### Defining Your Own Events

To create custom events, simply define a `struct` or `class` that inherits from `qb::Event` and add your data members.

```cpp
#include <qb/event.h>   // For qb::Event
#include <qb/actorid.h> // For qb::ActorId
#include <qb/string.h>  // For qb::string (efficient fixed-size string)
#include <string>       // For std::string
#include <vector>       // For std::vector
#include <memory>       // For std::shared_ptr

// 1. A simple signal event (no data)
struct StartProcessingSignal : qb::Event {};

// 2. Event with basic data members
struct UpdateValueCommand : qb::Event {
    int key;
    double value;

    // Constructor for easy initialization
    UpdateValueCommand(int k, double v) : key(k), value(v) {}
};

// 3. Event with a qb::string (efficient for small, fixed-size strings)
struct LogMessageInfo : qb::Event {
    qb::string<128> message_text; // Max 128 chars for this log message

    explicit LogMessageInfo(const char* msg) : message_text(msg) {}
};

// 4. Event with std::string (suitable for qb::Actor::push())
//    For qb::Actor::send(), the event must be trivially destructible.
struct UserCommand : qb::Event {
    std::string user_input;

    explicit UserCommand(std::string input) : user_input(std::move(input)) {}
};

// 5. Event carrying large or shared data via std::shared_ptr (recommended)
//    This avoids copying large data during message passing.
struct ProcessLargeDataset : qb::Event {
    std::shared_ptr<std::vector<uint8_t>> data_buffer;
    size_t data_size;

    ProcessLargeDataset(std::shared_ptr<std::vector<uint8_t>> buffer)
        : data_buffer(std::move(buffer)), data_size(data_buffer ? data_buffer->size() : 0) {}
};
```

**Performance Considerations for Event Data:**

*   **Small, POD-like Data:** For simple data, direct members are fine.
*   **`qb::string<N>`:** Use for strings where a known, relatively small maximum size is appropriate. This avoids heap allocations associated with `std::string`.
*   **`std::string`, `std::vector` (for `push()`):** The default `push()` mechanism correctly handles events with non-trivially destructible members like `std::string` or `std::vector`. Their destructors will be called appropriately by the framework.
*   **`std::shared_ptr` for Large Data:** When dealing with large data payloads (e.g., image buffers, large datasets), always wrap them in a `std::shared_ptr` (or `std::unique_ptr` if ownership semantics fit and you're careful with `std::move`). This ensures that only the pointer is copied during event serialization, not the entire data block, which is critical for performance, especially in inter-core communication.
*   **Trivial Destructibility for `send()`:** If you use the specialized `qb::Actor::send()` method for unordered, potentially lower-latency same-core messaging, the event type **must be trivially destructible**. This means it should generally only contain Plain Old Data (POD) types or types like `qb::string`. `std::string` or `std::vector` members are not allowed for events sent via `send()`.

## Event Delivery: Sending Events

Actors use methods inherited from `qb::Actor` to dispatch events to other actors. The primary methods include:

*   **`push<EventType>(destination_actor_id, constructor_args...)`:** This is the **recommended and most common** way to send events. It guarantees that events sent from one actor to another specific actor are processed in the order they were pushed.
*   **`send<EventType>(destination_actor_id, constructor_args...)`:** For specialized use cases requiring unordered delivery and trivially destructible events.
*   **`broadcast<EventType>(constructor_args...)`:** Sends the event to all actors on all active cores.
*   **`reply(event_reference)`:** Efficiently sends an event back to its original source.
*   **`forward(new_destination_actor_id, event_reference)`:** Efficiently redirects an event to a new destination.

Conceptual Event Flow:
```text
+-----------+       +---------------------+       +-------------------+       +-----------+
|  Sender   |------>| qb::Actor::push()   |------>| VirtualCore       |------>| Receiver  |
|  Actor    |       | (or send, reply...) |       | (Event Mailbox &  |       |  Actor    |
+-----------+       +---------------------+       |  Event Loop for   |       +-----------+
                                                  |  Receiver Actor)  |             |
                                                  +-------------------+             |
                                                        |  Dispatches Event         |
                                                        +---------------------------+
                                                          (invokes Receiver's on(Event&) handler)
```

**(For a comprehensive guide on sending events, see:** `[QB-Core: Event Messaging](../4_qb_core/messaging.md)`**)**

## Event Handling: Receiving and Processing Events

Actors process events by registering handlers for specific event types.

1.  **Register Event Handlers (`onInit()`):** Inside your actor's `onInit()` method (which is called once after construction and ID assignment), you **must** call `registerEvent<YourEventType>(*this);` for *each* distinct event type your actor needs to handle.

2.  **Implement Handler Methods:** For each registered event type, implement a corresponding public `on()` method. The signature of this method is crucial:
    *   `void on(const YourEventType& event)`: Use this if your handler only needs to read the event's data.
    *   `void on(YourEventType& event)`: Use this (a non-const reference) if your handler needs to modify the event (e.g., to fill in result fields) before using `reply(event)` or `forward(destination_id, event)`.

```cpp
class MyDataHandler : public qb::Actor {
public:
    // Constructor, etc.

    bool onInit() override {
        registerEvent<UpdateValueCommand>(*this); // From example above
        registerEvent<UserCommand>(*this);        // From example above
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    // Handler for UpdateValueCommand (read-only)
    void on(const UpdateValueCommand& event) {
        // Process event.key and event.value
        qb::io::cout() << "Received UpdateValueCommand: key=" << event.key 
                       << ", value=" << event.value << " from " << event.getSource() << ".\n";
    }

    // Handler for UserCommand (non-const, allows for reply/forward)
    void on(UserCommand& event) { // Note: non-const reference
        qb::io::cout() << "Received UserCommand: '" << event.user_input << "' from " << event.getSource() << ".\n";
        // Example: Modify and reply
        event.user_input = "Processed: " + event.user_input;
        reply(event); // Send the modified event back to its source
    }

    void on(const qb::KillEvent& /*event*/) {
        kill();
    }
};
```

*   **Dispatch Mechanism:** When an event arrives at an actor's `VirtualCore`, the framework uses the event's type ID (obtained from `event.getID()`) and its destination ID (`event.getDestination()`) to route it. An internal router (`qb::router::memh`) then invokes the specific `on()` handler method in the target actor that matches the event type.
*   **Sequential Guarantee:** Remember, an actor processes all its events sequentially. One `on()` handler will complete before the next one for that same actor begins.

**(Next:** `[Core Concepts: Asynchronous I/O Model](./async_io.md)`**)**
**(See also:** `[QB-Core: Mastering qb::Actor](../4_qb_core/actor.md)`, `[QB-Core: Event Messaging](../4_qb_core/messaging.md)`**)** 