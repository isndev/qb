@page qb_core_messaging_md QB-Core: Event Messaging Between Actors
@brief A comprehensive guide to defining, sending, and handling events—the core communication mechanism in QB.

# QB-Core: Event Messaging Between Actors

In the QB Actor Framework, actors live in isolation and communicate *exclusively* by exchanging asynchronous messages. These messages are called **Events**. Mastering the event system is fundamental to building robust and scalable actor-based applications.

This guide covers how to define events, the various ways actors can send them, and how they are received and processed.

## Defining Events: The Structure of Actor Communication

All messages exchanged between actors must be C++ structs or classes that publicly inherit from the base `qb::Event` class (defined in `qb/core/Event.h`).

**Key Principles for Event Design:**

*   **Data Carriers:** Events are primarily containers for data. They should encapsulate the information necessary for the receiving actor to understand the request or notification and act accordingly. Avoid embedding complex processing logic within the event objects themselves.
*   **Immutability (Conventionally):** Once an event is sent, it's best to treat its payload as immutable. If an actor needs to modify data and send it onward, it should typically create a new event. The exceptions are `reply()` and `forward()`, which are designed for efficient reuse of the event object.

**The `qb::Event` Base Class Provides:**

*   `id` (`qb::EventId`): An internal type identifier for the event (e.g., `qb::type_id<MyEventType>()`). Used by the framework for dispatching to the correct handler.
*   `dest` (`qb::ActorId`): The ID of the actor intended to receive the event.
*   `source` (`qb::ActorId`): The ID of the actor that sent the event.
*   Internal flags and size information (`is_alive()`, `getQOS()`, `getSize()`).

**Creating Custom Events:**

Add member variables to your derived event struct/class to carry application-specific data.

```cpp
#include <qb/event.h>   // For qb::Event
#include <qb/actorid.h> // For qb::ActorId
#include <qb/string.h>  // For qb::string (efficient fixed-size string)
#include <string>       // For std::string
#include <vector>       // For std::vector
#include <memory>       // For std::shared_ptr

// 1. A simple signal event (no data, just the type matters)
struct SystemReadySignal : qb::Event {};

// 2. Event with basic data members and qb::string (recommended for strings)
struct UpdateConfiguration : qb::Event {
    qb::string<64> config_key;    // Max 64 chars for key
    qb::string<256> new_value;  // Max 256 chars for value
    int priority_level;

    UpdateConfiguration(const char* key, const char* val, int priority)
        : config_key(key), new_value(val), priority_level(priority) {}
};

// 3. Event carrying a large or shared payload efficiently
struct DataAnalysisTask : qb::Event {
    std::shared_ptr<std::vector<double>> data_points;
    qb::string<32> task_description;

    DataAnalysisTask(std::shared_ptr<std::vector<double>> points, const char* desc)
        : data_points(std::move(points)), task_description(desc) {}
};
```

**Performance & Data Handling in Events:**

*   **Small, POD-like Data:** For simple data, direct members are fine.
*   **`qb::string<N>` (Strongly Recommended for Direct String Members in Events):**
    *   **Purpose:** Use `qb::string<N>` when you need string data *directly as a member* of your event. `N` is the fixed maximum character capacity.
    *   **Benefits:** `qb::string<N>` stores its data directly within its own structure. This avoids heap allocations for small-to-medium strings and, critically, **circumvents potential ABI (Application Binary Interface) stability issues that can arise with `std::string` as a direct member.** The C++ standard does not guarantee ABI compatibility for `std::string` across different compilers or even different versions of the same compiler or standard library. This means that the internal layout of a `std::string` (like where it stores its pointer to heap data, its size, and capacity, especially with Small String Optimization - SSO) can vary. If QB's event system were to perform low-level copies of events containing `std::string` members, and these events cross such ABI boundaries (e.g. different shared libraries compiled differently, or even just complex internal buffering by QB), the receiving side might misinterpret the `std::string`'s internal state, leading to crashes or corrupted data. `qb::string<N>` avoids this by having a fully self-contained, predictable layout.
    *   **Transparency:** Offers implicit conversion to `std::string_view` and explicit conversion to `std::string` (e.g., `std::string(my_qb_string)`).
    *   **Example:**
        ```cpp
        struct UserLoginEvent : qb::Event {
            qb::string<64> username;
            qb::string<128> session_token;
            // ... constructor ...
        };
        ```

*   **`std::string` as a Direct Member (Use with Extreme Caution / Generally Avoid):**
    *   **With `qb::Actor::push()`:** While `push()` *can* manage the lifecycle of `std::string` direct members in events within a consistent compilation environment, **its use as a direct event member is strongly discouraged due to the ABI stability concerns mentioned above.**
    *   **Recommendation:** **For direct string members, ALWAYS prefer `qb::string<N>`.**

*   **Other STL Containers (e.g., `std::vector<T>`, `std::map<K,V>`):**
    *   **With `qb::Actor::push()`:** These containers, when direct members of an event sent via `push()`, will have their copy/move constructors and destructors correctly invoked by the framework as C++ objects. The primary concern here is **performance**: copying large vectors or maps by value in events can be very expensive.
    *   **Recommendation for Large Collections:** For any sizeable or dynamically growing collections, pass them via `std::shared_ptr` (e.g., `std::shared_ptr<std::vector<MyData>>`) to avoid deep copies during event transmission.
    *   **With `qb::Actor::send()`:** These containers are typically non-trivially destructible and thus **not permitted** as direct members of events sent via `send()`.

*   **Smart Pointers for Large, Shared, or Owned Data (Recommended for `push()`):**
    *   To pass large data payloads (binary blobs, extensive collections, complex objects) without incurring significant copying overhead, or to clearly transfer ownership, wrap the data in a smart pointer:
        *   **`std::shared_ptr<T>`:** Use when the data might be referenced by multiple actors after the event is sent, or when the lifetime is co-managed.
        *   **`std::unique_ptr<T>`:** Use when the sending actor wishes to transfer exclusive ownership of the data to the receiving actor. The data will be moved, and the sender relinquishes access. This is very efficient.
    *   This ensures only the smart pointer (which is small and typically trivially copyable/movable itself) is involved in QB's internal event transfer mechanisms, while the underlying data is managed appropriately.
    *   **Using `std::string` with Smart Pointers:** If you are passing string data via `std::shared_ptr<std::string>` or `std::unique_ptr<std::string>`, then using `std::string` (instead of `qb::string<N>`) for the heap-allocated string data is acceptable and often more convenient for dynamically sized text. The smart pointer manages the `std::string`'s lifecycle correctly, and QB interacts primarily with the smart pointer object itself.
    *   **Example with `std::unique_ptr<std::string>`:**
        ```cpp
        struct LargeTextMessageEvent : qb::Event {
            std::unique_ptr<std::string> message_content;

            explicit LargeTextMessageEvent(std::unique_ptr<std::string> content) 
                : message_content(std::move(content)) {}
        };

        // In sender actor:
        auto large_log_entry = std::make_unique<std::string>(GenerateVeryLongLog());
        push<LargeTextMessageEvent>(logger_actor_id, std::move(large_log_entry)); 
        // large_log_entry is now nullptr in sender.
        ```

*   **Trivial Destructibility (Strictly Required for `qb::Actor::send()`):**
    *   Events sent via `send()` **must** be trivially destructible. This typically means they contain only POD types or other trivially destructible types like `qb::string<N>`. Members like `std::string`, `std::vector`, or smart pointers (`std::shared_ptr`, `std::unique_ptr`) (which manage a non-trivial resource or have non-trivial destructors) are **not allowed** for events sent via `send()`.

**(See also:** [Core Concepts: QB Event System](./../2_core_concepts/event_system.md)**)**

## Sending Events: Actor Communication Methods

Actors use methods inherited from `qb::Actor` to dispatch events.

### 1. `push<EventType>(destination_actor_id, constructor_args...) const noexcept -> EventType&`

*   **Primary Choice:** This is the **default and recommended** method for sending events.
*   **Ordered Delivery:** Guarantees that events sent from a specific source actor to a specific destination actor are processed by the destination in the order they were `push`ed by the source.
*   **Asynchronous:** The call returns immediately; the event is queued and processed by the destination actor's `VirtualCore` later.
*   **Return Value:** Returns a non-const reference to the constructed event *before* it's actually sent. This allows you to modify its members if needed just after construction.
*   **Data Types:** Handles non-trivially destructible events (but see strong recommendation for `qb::string<N>` over `std::string` as direct members).
*   **Example:**
    ```cpp
    // Inside an actor
    qb::ActorId target_id = get_target_actor_id();
    auto& cmd = push<UpdateConfiguration>(target_id, "timeout_ms", "500", 1);
    // cmd.priority_level = 2; // Optionally modify before it's sent
    ```
*   **(Ref:** `test-actor-event.cpp::BasicPushActor`**)

### 2. `send<EventType>(destination_actor_id, constructor_args...) const noexcept`

*   **Specialized Use:** For scenarios where event order is not critical and the event type is simple and trivially destructible.
*   **Unordered Delivery:** Provides no guarantee about the order of arrival relative to other events (even other `send` calls from the same source to the same destination).
*   **Potentially Lower Latency (Same Core):** May offer slightly reduced latency for same-core communication, as it might bypass some queueing steps.
*   **Requirement:** `EventType` **must be trivially destructible**. (e.g. contains only POD types or `qb::string<N>`).
*   **Caution:** Use sparingly and only when the trade-offs are well understood. Incorrect use can lead to hard-to-debug issues.
*   **Example:**
    ```cpp
    // Inside an actor, for a simple, order-agnostic notification
    struct FireForgetSignal : qb::Event {}; // Must be trivially destructible
    send<FireForgetSignal>(monitor_actor_id);
    ```
*   **(Ref:** `test-actor-event.cpp::BasicSendActor`**)

### 3. Broadcasting Events

*   **`broadcast<EventType>(constructor_args...) const noexcept`**
    *   Sends the event to **all actors on all active `VirtualCore`s** within the `qb::Main` instance.
    *   Use for system-wide announcements or signals.
    *   Example: `broadcast<SystemShutdownNotice>();`
*   **`push<EventType>(qb::BroadcastId(core_id), constructor_args...)`**
    *   Sends the event (ordered relative to other pushes from the sender) to **all actors currently running on the specified `core_id`**.
    *   Useful for core-specific group notifications.
    *   Example: `push<CacheFlushCommand>(qb::BroadcastId(1), "users_cache");`
*   **(Ref:** `test-actor-broadcast.cpp`**)

### 4. `reply(Event& original_event) const noexcept`

*   **Efficient Response:** The most efficient way to send a response back to the actor that sent `original_event`.
*   **Mechanism:** Reuses the `original_event` object. It swaps the `source` and `dest` fields and resends it.
*   **Handler Requirement:** The `on(EventType& event)` handler that calls `reply(event)` **must take its event parameter by non-const reference** (`EventType& event`) to allow modification.
*   **Event Consumption:** After `reply(event)` is called, the `event` object in the handler is considered "consumed" and should not be accessed further by that handler.
*   **Example:**
    ```cpp
    // Inside ResponderActor
    struct MyRequest : qb::Event { qb::string<64> query; qb::string<128> response_data; };
    void on(MyRequest& request_event) { // Note: non-const reference
        qb::string<128> temp_response = "Processed: ";
        // temp_response.append(request_event.query.c_str()); // Assuming qb::string has append or similar
        request_event.response_data = temp_response; // Assign back
        reply(request_event); // Sends the modified MyRequest back to the original sender
    }
    ```
*   **(Ref:** `test-actor-event.cpp::TestReceiveReply`**)

### 5. `forward(ActorId new_destination, Event& original_event) const noexcept`

*   **Efficient Redirection:** An efficient way to delegate an event to another actor without creating a new event object.
*   **Mechanism:** Reuses the `original_event` object, changing its `dest` to `new_destination` but preserving the `original_event.source`.
*   **Handler Requirement:** Similar to `reply()`, the `on()` handler must take a non-const event reference (`EventType& event`).
*   **Event Consumption:** The `event` object is consumed after `forward()`.
*   **Example:**
    ```cpp
    // Inside RouterActor
    struct WorkOrder : qb::Event { /* ... */ };
    void on(WorkOrder& work_event) { // Note: non-const reference
        qb::ActorId worker_node_id = find_available_worker_for(work_event);
        forward(worker_node_id, work_event); // Delegate to a specific worker
    }
    ```
*   **(Ref:** `test-actor-event.cpp::TestReceiveReply` uses forward in one of its tests**)

### 6. Advanced Sending: `EventBuilder` and `Pipe`

For fine-tuned control or performance-critical scenarios, especially when sending multiple events or large events:

*   **`to(destination_id).push<EventType>(...)` -> `Actor::EventBuilder&`**
    *   Returns an `EventBuilder` object.
    *   Allows chaining multiple `push<EventType>()` calls to the *same destination* more fluently. This can offer a minor performance benefit by avoiding repeated lookups for the communication pipe to the destination.
    *   Example:
        ```cpp
        to(stats_service_id)
            .push<CounterIncrementEvent>("login_attempts")
            .push<TimerStartEvent>("user_session_" + user_id);
        ```
*   **`getPipe(destination_id) const noexcept -> qb::Pipe`**
    *   Provides direct access to the underlying `qb::Pipe` object, which is the communication channel to the `destination_id`.
    *   The `qb::Pipe` object itself has `push<EventType>(...)` methods.
    *   **`pipe.allocated_push<MyLargeEvent>(payload_size_hint, constructor_args...)`:** This is particularly useful for events that carry large, dynamically sized payloads (e.g., data in a `std::vector` passed via `std::shared_ptr`). By providing a `payload_size_hint` (approximate size of the event struct *plus* its dynamic payload), you can help the framework pre-allocate sufficient space in the pipe, avoiding potentially costly reallocations and memory copies during event construction and enqueuing.
    *   Example with `allocated_push`:
        ```cpp
        // Assume LargeDataEvent contains a std::shared_ptr<std::vector<char>> data;
        auto my_large_data_vec = std::make_shared<std::vector<char>>(1024 * 1024); // 1MB
        // ... fill my_large_data_vec ...
        qb::Pipe data_pipe = getPipe(data_processing_actor_id);
        size_t estimated_size = sizeof(LargeDataEvent) + my_large_data_vec->size();
        
        auto& ev = data_pipe.allocated_push<LargeDataEvent>(estimated_size, my_large_data_vec);
        // ev is a reference to the event in the pipe's buffer
        ```

**(Reference:** `test-actor-event.cpp` demonstrates various sender actor implementations using these methods.**)

## Receiving and Handling Events in Actors

Actors process events by:

1.  **Registering Event Handlers:** This is done **exclusively within the actor's `onInit()` method** by calling `registerEvent<EventType>(*this);` for each specific `EventType` the actor is designed to handle.

2.  **Implementing Handler Methods:** For each registered `EventType`, the actor must provide a corresponding `public` method with one of the following signatures:
    *   `void on(const EventType& event)`: If the handler only needs to read the event's data.
    *   `void on(EventType& event)`: If the handler intends to modify the event (e.g., add a result) and then use `reply(event)` or `forward(destination, event)`. Note that modifications are only relevant for these reuse patterns; the original event in the sender or other potential recipients (for broadcasts) is not affected.

```cpp
// In MyActor.h or .cpp
class MyActor : public qb::Actor {
public:
    bool onInit() override {
        registerEvent<UpdateConfiguration>(*this); // From example above
        registerEvent<DataAnalysisTask>(*this);  // From example above
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    // Handler for UpdateConfiguration (can be const& if not replying/forwarding)
    void on(const UpdateConfiguration& event) {
        qb::io::cout() << "MyActor [" << id() << "]: Received config update for key '"
                       << event.config_key.c_str() << "' to value '" << event.new_value.c_str() 
                       << "' with priority " << event.priority_level << ".\n";
        // Apply configuration change...
    }

    // Handler for DataAnalysisTask (non-const& if we might reply/forward)
    void on(DataAnalysisTask& event) { 
        if (event.data_points) {
            qb::io::cout() << "MyActor [" << id() << "]: Received data task '"
                           << event.task_description.c_str() << "' with " 
                           << event.data_points->size() << " data points.\n";
            // Perform analysis...
            // Example: push<AnalysisCompleteEvent>(event.getSource(), task_id, result);
        } else {
            qb::io::cout() << "MyActor [" << id() << "]: Received empty data task.\n";
        }
    }

    void on(const qb::KillEvent& /*event*/) {
        kill();
    }
};
```

*   **Dispatch Mechanism:** The `VirtualCore` responsible for an actor uses the event's type ID (`event.getID()`) and its destination actor ID (`event.getDestination()`) to route the event. An internal router (`qb::router::memh`) then invokes the specific `on()` handler in the target actor that matches the event's type.
*   **Sequential Processing Guarantee:** An actor processes events from its mailbox one at a time. One `on()` handler must complete before the next one for that same actor instance can begin.

This event-driven, message-passing architecture is central to how QB achieves concurrency and simplifies the development of complex, stateful systems.

**(Next:** [QB-Core: Engine (`qb::Main`, `VirtualCore`)](./engine.md) to understand how actors and their events are managed and scheduled.**)
**(See also:** [Core Concepts: QB Event System](./../2_core_concepts/event_system.md), [QB-Core: Mastering qb::Actor](./actor.md)**) 