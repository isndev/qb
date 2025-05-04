# QB-Core: Actor (`qb::Actor`)

(`qb/include/qb/core/Actor.h`)

The `qb::Actor` class is the fundamental building block for concurrent applications in the QB framework. User-defined actors encapsulate state and behavior, interacting solely through asynchronous message passing.

## How to Define an Actor

1.  **Inherit:** Create a class that publicly inherits from `qb::Actor`.
2.  **State:** Define private or protected member variables to hold the actor's state.
3.  **`onInit()` (Override):** Implement the `virtual bool onInit()` method. This is called *after* the actor is constructed and assigned an `ActorId`, but *before* it processes any events. Use it for:
    *   **Event Registration:** Call `registerEvent<EventType>(*this);` for every event type the actor should handle.
    *   **Resource Acquisition:** Initialize resources (e.g., open files, establish helper connections - preferably using RAII members).
    *   **Initial Actions:** Send initial messages, start timers via `async::callback`, etc.
    *   **Return Value:** Return `true` if initialization succeeded. Returning `false` prevents the actor from starting and leads to its immediate destruction.
4.  **Event Handlers (`on(Event&)`):** Implement public `void on(const EventType& event)` or `void on(EventType& event)` methods for each registered event type. This is where the actor's main logic resides.
5.  **`on(qb::KillEvent&)`:** *Always* register and handle `qb::KillEvent`. Your handler should perform any necessary pre-shutdown cleanup and **must call the base `kill()` method** to complete termination.
6.  **Destructor (`~MyActor()`):** Implement a destructor for RAII-based resource cleanup. It's called after the actor has terminated and is being removed from the system.

```cpp
#include <qb/actor.h>
#include <qb/event.h>
#include <qb/io.h> // For qb::io::cout
#include <vector>
#include <string>
#include <memory>

// --- Events ---
struct RequestWork : qb::Event {
    int work_id;
};
struct WorkResult : qb::Event {
    int work_id;
    std::string result_data;
};
// --------------

class WorkerActor : public qb::Actor {
private:
    // Actor State
    std::string _worker_name;
    std::unique_ptr<char[]> _buffer; // Example resource
    int _processed_count = 0;

public:
    // Constructor
    explicit WorkerActor(std::string name) : _worker_name(std::move(name)) {}

    // Destructor (RAII cleanup for _buffer happens automatically)
    ~WorkerActor() override {
        qb::io::cout() << _worker_name << " [" << id() << "] destroyed. Processed: " << _processed_count << std::endl;
    }

    // Initialization
    bool onInit() override {
        qb::io::cout() << _worker_name << " [" << id() << "] onInit on Core " << getIndex() << std::endl;
        try {
            _buffer = std::make_unique<char[]>(1024); // Allocate resource
        } catch (...) {
            return false; // Signal initialization failure
        }
        // *** Register Event Handlers ***
        registerEvent<RequestWork>(*this);
        registerEvent<qb::KillEvent>(*this);
        return true;
    }

    // --- Event Handlers ---
    void on(const RequestWork& event) {
        qb::io::cout() << _worker_name << " [" << id() << "] processing work ID: " << event.work_id << std::endl;
        _processed_count++;
        std::string result = "Result for " + std::to_string(event.work_id) + " by " + _worker_name;

        // Send result back to requester
        push<WorkResult>(event.getSource(), event.work_id, result);
    }

    void on(const qb::KillEvent& event) {
        qb::io::cout() << _worker_name << " [" << id() << "] received KillEvent." << std::endl;
        // Perform specific pre-kill cleanup if needed...
        kill(); // Essential: Call base kill()
    }
    // ---------------------
};
```

## Actor State Management

*   **Isolation:** State (member variables) is protected from direct external access.
*   **Thread Safety:** Accessing member variables within `on(Event&)` or `onCallback()` is inherently safe *within that actor* because events/callbacks for a single actor are processed sequentially by its `VirtualCore`.
*   **No Blocking:** Event handlers and callbacks **must not block**. Use `qb::io::async::callback` or dedicated I/O actors for long-running or blocking operations.

## Key `qb::Actor` Methods for Usage

**(Full List:** `qb/include/qb/core/Actor.h`**)

*   **Identification:**
    *   `id()`: Get `qb::ActorId`.
    *   `getIndex()`: Get `qb::CoreId`.
    *   `getName()`: Get class name (string_view).
*   **Lifecycle & State:**
    *   `virtual bool onInit()`: Override for setup.
    *   `kill()`: Initiate termination.
    *   `is_alive()`: Check if still processing events.
*   **Event Handling:**
    *   `registerEvent<T>(*this)` / `unregisterEvent<T>(*this)`: Manage event subscriptions.
*   **Callback Handling (Requires `qb::ICallback` inheritance):**
    *   `registerCallback(*this)` / `unregisterCallback(*this)`: Manage periodic callback.
    *   `virtual void onCallback()`: Implement periodic logic.
*   **Messaging:**
    *   `push<T>(...)`, `send<T>(...)`, `broadcast<T>(...)`, `reply(Event&)`, `forward(dest, Event&)`: Send events.
    *   `to(dest)`: Get `EventBuilder`.
    *   `getPipe(dest)`: Get `qb::Pipe`.
*   **Actor Creation/Discovery:**
    *   `addRefActor<T>(...)`: Create same-core referenced actor.
    *   `getService<T>()`: Get pointer to same-core service actor.
    *   `getServiceId<Tag>(CoreId)`: Get `ActorId` of service actor.
    *   `require<T>()`: Request dependency notification.

**(Ref:** `test-actor-*.cpp` files cover most method usage patterns.**) 