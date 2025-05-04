# QB Framework: Advanced Usage & Patterns

This guide explores more advanced techniques and common patterns for building applications with the QB framework, leveraging insights from the included examples and tests.

## Implementing State Machines

Actors are naturally suited for implementing Finite State Machines (FSMs). The actor's internal state represents the FSM state, and incoming events trigger state transitions and actions.

*   **State Representation:** Use an `enum class` or similar type as a member variable to hold the current state.
*   **Transitions:** Event handlers (`on(Event&)` methods) implement the transition logic. Based on the current state and the received event, the handler performs actions, updates the state variable, and potentially sends new events.
*   **Actions:** Actions associated with states or transitions (entry/exit actions, actions during transitions) are performed within the event handlers.

```cpp
// Conceptual Example (See example/core/example8_state_machine.cpp for full implementation)
#include <qb/actor.h>

enum class MachineState { IDLE, PROCESSING, ERROR };
struct StartProcessing : qb::Event { /* ... */ };
struct DataPacket : qb::Event { /* ... */ };
struct ProcessingError : qb::Event { /* ... */ };
struct Reset : qb::Event { /* ... */ };

class FsmActor : public qb::Actor {
private:
    MachineState _current_state = MachineState::IDLE;

public:
    bool onInit() override {
        registerEvent<StartProcessing>(*this);
        registerEvent<DataPacket>(*this);
        registerEvent<ProcessingError>(*this);
        registerEvent<Reset>(*this);
        return true;
    }

    void on(const StartProcessing& event) {
        if (_current_state == MachineState::IDLE) {
            std::cout << "Transitioning to PROCESSING" << std::endl;
            _current_state = MachineState::PROCESSING;
            // Start some async work...
        }
    }

    void on(const DataPacket& event) {
        if (_current_state == MachineState::PROCESSING) {
            // Process data...
        } else {
            // Ignore or error
        }
    }

    void on(const ProcessingError& event) {
        if (_current_state == MachineState::PROCESSING) {
            std::cout << "Transitioning to ERROR" << std::endl;
            _current_state = MachineState::ERROR;
        }
    }

    void on(const Reset& event) {
        std::cout << "Transitioning to IDLE" << std::endl;
        _current_state = MachineState::IDLE;
        // Reset internal state...
    }
};
```
**(Ref:** `example/core/example8_state_machine.cpp`**)

## Publish/Subscribe Pattern

A central "Broker" or "Topic Manager" actor can manage subscriptions and distribute messages.

*   **Broker Actor:**
    *   Maintains a map of topics (e.g., `std::string`) to sets of subscriber `ActorId`s.
    *   Handles `SubscribeEvent`: Adds the sender's `ActorId` to the topic's subscriber set.
    *   Handles `UnsubscribeEvent`: Removes the sender from the set.
    *   Handles `PublishEvent`: Iterates through all subscribers for the given topic and `push`es the message content (often wrapped in a new `MessageDataEvent`) to each subscriber.
*   **Publisher Actors:** Send `PublishEvent` messages to the Broker.
*   **Subscriber Actors:** Send `SubscribeEvent`/`UnsubscribeEvent` to the Broker and handle the `MessageDataEvent` pushed by the Broker.

**(Ref:** `example/core/example7_pub_sub.cpp`, `example/core_io/message_broker/`**)

## Managing Shared Resources

Avoid sharing resources directly between actors. Instead, encapsulate the resource within a dedicated actor.

*   **Resource Actor:** An actor that owns and manages exclusive access to a resource (e.g., a database connection pool, a hardware device, a shared data structure like a queue in `example6_shared_queue.cpp`).
*   **Interaction:** Other actors interact with the resource *only* by sending request events to the Resource Actor.
*   **Replies:** The Resource Actor performs the operation and sends response events back to the requester.
*   **Concurrency:** The Resource Actor processes requests sequentially, ensuring safe access to the resource.

```cpp
// Simplified concept
struct DbQuery : qb::Event { /* ... */ };
struct DbResult : qb::Event { /* ... */ };

class DatabaseActor : public qb::Actor {
private:
    DatabaseConnection _connection; // The managed resource
public:
    bool onInit() override { registerEvent<DbQuery>(*this); /* ... connect ... */ return true; }

    void on(const DbQuery& event) {
        // Execute query using _connection
        ResultData result = _connection.execute(event.sql);
        // Send result back
        reply(DbResult(result)); // Or push<DbResult>(event.getSource(), result);
    }
};

class ClientActor : public qb::Actor {
private:
    qb::ActorId _db_actor_id;
public:
    // ... onInit to get _db_actor_id ...
    void queryDatabase() {
        push<DbQuery>(_db_actor_id, "SELECT ...");
    }
    void on(const DbResult& event) {
        // Process result
    }
};
```
**(Ref:** `example6_shared_queue.cpp` (uses a thread-safe queue shared via `shared_ptr`, but the principle of a central manager actor applies)**)

## Request/Reply Pattern

While actors are asynchronous, request/reply interactions are common.

*   **Mechanism:**
    1.  Actor A sends a `RequestEvent` (containing a unique request ID) to Actor B.
    2.  Actor A might store the request ID and its own state related to the request (e.g., in a map).
    3.  Actor B processes the request.
    4.  Actor B sends a `ResponseEvent` (containing the same request ID and the result) back to Actor A (using `event.getSource()` from the request or `reply()`).
    5.  Actor A receives the `ResponseEvent`, uses the request ID to look up the original context, and processes the result.
*   **Timeouts:** Actor A might use `qb::io::async::callback` to schedule a timeout check for the request ID. If the response doesn't arrive in time, it can handle the timeout (e.g., retry, report error).

**(Ref:** Many examples implicitly use this, e.g., `example8_state_machine.cpp::StatusRequestEvent/StatusResponseEvent`, `example/core_io/file_processor/`**)

## Error Handling and Supervision

QB Core itself doesn't impose a specific supervision strategy, but the Actor Model enables various approaches:

*   **Let It Crash:** Often the simplest. If an actor encounters a critical error (e.g., unhandled exception, logic error), let its `VirtualCore` catch it (which sets `main.hasError()`) or let the actor `kill()` itself. The rest of the system continues running. This relies on other parts of the system being resilient to the failure of one actor.
*   **Explicit Error Events:** Actors can catch exceptions or detect errors and send specific `ErrorEvent` messages to designated error handlers or supervisors.
*   **Supervisor Actors:** A parent/supervisor actor can monitor its children (e.g., using `require<T>` or by tracking child IDs). If a child fails (detected perhaps by a lack of response or an explicit error event), the supervisor can decide whether to restart it, delegate its work, or escalate the failure.
*   **Timeouts:** Use timeouts (via `with_timeout` or `async::callback`) to detect unresponsive actors.

**(Ref:** `test-actor-error-handling.cpp` explores basic error simulation, `test-main.cpp` checks `hasError()`**)

## Performance Tuning

*   **Core Affinity (`setAffinity`):** Pin critical actors or groups of communicating actors to specific CPU cores to improve cache locality and reduce context switching.
*   **Core Latency (`setLatency`):** Use `0` for cores handling latency-sensitive tasks (e.g., network polling, real-time processing). Use small positive values for cores with less critical timing requirements to save CPU cycles.
*   **`push` vs. `send`:** Prefer `push` for ordered delivery and non-trivial events. Consider `send` *only* for same-core, unordered, trivially destructible events where minimal latency is paramount.
*   **Pipes & Builders (`getPipe`, `to`):** Use `pipe.allocated_push` for potentially large events to avoid reallocations. Use `to(...).push(...).push(...)` if sending many events sequentially to the same destination for minor efficiency gains.
*   **Referenced Actors (`addRefActor`):** Can reduce event passing overhead for tightly coupled actors *on the same core*, but introduces direct coupling and potential state consistency risks if not used carefully.
*   **Event Design:** Pass large data via `std::shared_ptr` within events rather than copying large objects.

**(Ref:** `[QB-Core: Engine](./../4_qb_core/engine.md)`, `[QB-Core: Messaging](./../4_qb_core/messaging.md)`, `[Guides: Performance Tuning](./performance_tuning.md)`**)

## Distributed Computation Patterns

QB actors can form the basis of distributed task processing systems.

*   **Manager/Worker:** A central manager actor distributes tasks to multiple worker actors (potentially across cores).
*   **Load Balancing:** The manager can track worker availability/load (e.g., via heartbeats or status events) to distribute tasks intelligently.
*   **Result Aggregation:** A dedicated collector actor can receive results from workers.
*   **Monitoring:** A monitor actor can oversee the system, track statistics, and manage startup/shutdown.

**(Ref:** `example/core/example10_distributed_computing.cpp` provides a sophisticated example of this.**) 