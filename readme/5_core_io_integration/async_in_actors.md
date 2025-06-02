@page core_io_async_in_actors_md Integrating Core & IO: Asynchronous Operations within Actors
@brief Guide to using `qb-io` asynchronous features like callbacks and timers directly within QB actors for non-blocking behavior.

# Integrating Core & IO: Asynchronous Operations within Actors

Actors in the QB Framework can directly and seamlessly leverage the asynchronous capabilities of `qb-io`. This allows actors to perform time-based operations, schedule deferred tasks, or manage I/O-bound activities without blocking their `VirtualCore` and impeding the progress of other actors on the same core.

## Using `qb::io::async::callback` for Delayed or Deferred Actions

(`qb/io/async/io.h`)

This is the **primary and most flexible way** for an actor to schedule a piece of code (typically a lambda) to be executed later by its own `VirtualCore`'s event loop.

*   **How to Use:** Call `qb::io::async::callback(your_lambda, delay_in_seconds);` from within an actor's `onInit()`, an `on(Event&)` handler, or even another `onCallback()` (if the actor inherits `qb::ICallback`).
    *   A `delay_in_seconds` of `0.0` (or omitting it) schedules the lambda for the next available iteration of the event loop.
*   **Execution Context:** The provided lambda will execute on the **same `VirtualCore`** (and thus the same thread) as the actor that scheduled it. This means the lambda can safely interact with the actor's state *if and only if* the actor is still alive and proper checks are made.
*   **Capturing Actor Context (`this` or `id()`):**
    *   **Crucial Safety Check:** When a lambda captures `this` (or `auto self_id = id();`), it is **essential** to check `this->is_alive()` (or `if (auto actor_ptr = VirtualCore::_handler->getActor(self_id)) { ... }` if only ID was captured and direct actor pointer needed, though this is less common for self-interaction) at the very beginning of the lambda's body. This is because the actor might be terminated and destroyed before the delayed callback gets a chance to execute.
    *   **Sending Events Back to Self:** Instead of complex direct state manipulation from the lambda, a robust pattern is for the lambda to `push` a new, specific event back to the scheduling actor itself. The actor then handles this event in a dedicated `on()` handler.

*   **Common Use Cases for Actors:**
    *   **Implementing Timeouts:** For an operation initiated by the actor, schedule a callback. If the operation doesn't complete (e.g., by an incoming response event clearing a pending flag) before the callback runs, the callback can trigger timeout logic.
    *   **Periodic Actions:** A callback can reschedule itself to create periodic behavior (an alternative to using `qb::ICallback`).
    *   **Breaking Down Complex Non-Blocking Tasks:** If an actor needs to perform a series of non-blocking steps, it can execute one step and then schedule the next step via `async::callback` with a zero delay, effectively yielding control back to the event loop between steps.
    *   **Retry Mechanisms:** After a failed attempt (e.g., network connection), schedule a retry attempt using a delayed callback.
    *   **Simulating Work/Delays:** Useful in examples or tests to introduce artificial processing times without actual blocking.

```cpp
// Inside MyProcessingActor class
#include <qb/actor.h>
#include <qb/io/async.h> // For qb::io::async::callback
#include <qb/io.h>       // For qb::io::cout

// Event to signal task completion or timeout
struct ProcessTaskCompleteEvent : qb::Event {
    int task_id;
    bool timed_out;
    explicit ProcessTaskCompleteEvent(int id, bool timeout_status) 
        : task_id(id), timed_out(timeout_status) {}
};

class MyProcessingActor : public qb::Actor {
private:
    std::map<int, bool> _pending_tasks; // task_id -> is_pending
    int _next_task_id = 1;

public:
    bool onInit() override {
        registerEvent<ProcessTaskCompleteEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        // Example: Start a task immediately
        startLongOperation(5.0); // Operation that we expect to complete or timeout in 5s
        return true;
    }

    void startLongOperation(double timeout_seconds) {
        int current_task_id = _next_task_id++;
        _pending_tasks[current_task_id] = true;
        qb::io::cout() << "Actor [" << id() << "]: Starting operation for task " << current_task_id 
                       << ", timeout in " << timeout_seconds << "s.\n";

        // Simulate starting an external non-blocking operation here...

        // Schedule a timeout check using async::callback
        // Capture 'this' to access actor members and methods
        qb::io::async::callback([this, current_task_id]() {
            if (!this->is_alive()) { // Crucial check!
                // qb::io::cout() << "Actor for task " << current_task_id << " no longer alive for timeout check.\n";
                return;
            }
            // Check if the task is still marked as pending
            if (_pending_tasks.count(current_task_id) && _pending_tasks[current_task_id]) {
                // Task timed out, send a completion event indicating timeout
                qb::io::cout() << "Actor [" << id() << "]: Task " << current_task_id << " timed out!\n";
                this->push<ProcessTaskCompleteEvent>(this->id(), current_task_id, true);
                _pending_tasks.erase(current_task_id); // Clean up
            }
            // If not pending, it means a response was received before timeout
        }, timeout_seconds);
    }

    // Imagine this event is received from an external system or another actor
    // when the long operation actually finishes (before timeout).
    void onOperationActuallyCompleted(int task_id_completed) { // Not a qb::Event, but a conceptual method
        if (_pending_tasks.count(task_id_completed) && _pending_tasks[task_id_completed]) {
            qb::io::cout() << "Actor [" << id() << "]: Task " << task_id_completed << " completed successfully (before timeout).\n";
            this->push<ProcessTaskCompleteEvent>(this->id(), task_id_completed, false);
            _pending_tasks.erase(task_id_completed);
        }
    }

    void on(const ProcessTaskCompleteEvent& event) {
        qb::io::cout() << "Actor [" << id() << "]: Handling ProcessTaskCompleteEvent for task " << event.task_id
                       << (event.timed_out ? " (TIMED OUT)" : " (SUCCESS)") << ".\n";
        // Further processing based on completion or timeout...
    }

    void on(const qb::KillEvent& /*event*/) { kill(); }
};
```
**(Reference:** `test-actor-delayed-events.cpp`, `example10_distributed_computing.cpp` (extensive use for simulation and scheduling), `file_processor/file_worker.h` (wrapping blocking calls).**)

## Using `qb::io::async::with_timeout` for Actor Inactivity

While `async::callback` is versatile, if an actor needs a simple inactivity timeout (i.e., it should terminate or take action if no relevant messages arrive for a period), inheriting from `qb::io::async::with_timeout<DerivedActor>` can be an option.

*   **How it Works:**
    1.  **Inheritance:** `class MyActor : public qb::Actor, public qb::io::async::with_timeout<MyActor> { ... };`
    2.  **Constructor Initialization:** Call the base `with_timeout(timeout_duration_seconds);` in your actor's constructor.
    3.  **Implement Timeout Handler:** `void on(qb::io::async::event::timer const& event)` will be called when the timer expires.
    4.  **Reset on Activity:** In your relevant `on(ApplicationEvent&)` handlers (those that signify activity), call `this->updateTimeout();` to reset the inactivity countdown.
*   **Use Case:** Terminating idle sessions, cleaning up actors that haven't received heartbeats or work for a while.

```cpp
#include <qb/actor.h>
#include <qb/io/async.h> // For qb::io::async::with_timeout and event::timer
#include <qb/io.h>

struct ClientActivityPing : qb::Event {};

class SessionActorWithInactivity : public qb::Actor, 
                                   public qb::io::async::with_timeout<SessionActorWithInactivity> {
public:
    SessionActorWithInactivity() : with_timeout(30.0) { // 30-second inactivity timeout
        qb::io::cout() << "SessionActor [" << id() << "]: Created with 30s inactivity timeout.\n";
    }

    bool onInit() override {
        registerEvent<ClientActivityPing>(*this);
        registerEvent<qb::KillEvent>(*this);
        updateTimeout(); // Start the initial countdown
        return true;
    }

    void on(const ClientActivityPing& /*event*/) {
        qb::io::cout() << "SessionActor [" << id() << "]: Ping received, activity confirmed. Resetting timeout.\n";
        updateTimeout(); // Reset inactivity timer upon receiving a ping
    }

    // This handler is called by the with_timeout base when the timer expires
    void on(qb::io::async::event::timer const& /*event*/) {
        qb::io::cout() << "SessionActor [" << id() << "]: Inactivity timeout reached! Terminating session.\n";
        kill(); // Terminate the actor due to inactivity
    }

    void on(const qb::KillEvent& /*event*/) {
        setTimeout(0.0); // Disable timer explicitly during shutdown
        kill();
    }
    
    ~SessionActorWithInactivity() override {
        qb::io::cout() << "SessionActor [" << id() << "] destroyed.\n";
    }
};
```
**(Reference:** `chat_tcp/server/ChatSession.h` uses this pattern, though it's a non-actor class in that example, the principle is the same. `test-async-io.cpp::TimerHandler` also demonstrates `with_timeout`.)**

## Asynchronous File Operations within Actors

Directly performing blocking file I/O (like `std::fstream::read` or `qb::io::sys::file::write`) within an actor's event handler or `onCallback` is **highly discouraged** as it will block the `VirtualCore`.

To handle file operations asynchronously from an actor:

1.  **Wrap Blocking Calls in `qb::io::async::callback` (for simpler cases):**
    *   The actor receives a request to read/write a file.
    *   It captures necessary parameters (file path, data, original requester ID) into a lambda.
    *   It schedules this lambda using `qb::io::async::callback`.
    *   The lambda executes on the actor's `VirtualCore` but in a way that doesn't block the primary event processing flow for *other* events while the blocking call itself is in progress (though the callback itself will block its turn in the event loop queue).
    *   After the blocking I/O in the lambda completes, the lambda should `push` a result event (containing data or error status) back to the original requester or to the scheduling actor itself.
    *   **Caution:** While this makes the actor *appear* non-blocking to other actors, the `VirtualCore` processing this specific callback *will* be blocked during the actual file I/O. This is suitable for infrequent or less performance-critical file operations.

2.  **Dedicated File Worker Actors (Recommended for complex or frequent I/O):**
    *   Create specialized worker actors whose sole purpose is to perform file I/O.
    *   The main actor delegates file operation requests (as events) to these worker actors.
    *   Worker actors can use the `async::callback` pattern internally to perform the blocking I/O.
    *   This isolates blocking operations to specific actors/cores, potentially configured with higher latency if they are expected to block often.
    *   **(Reference:** The `example/core_io/file_processor/` provides a good example of this pattern.)**

3.  **Asynchronous File Watching (`qb::io::async::file_watcher` or `directory_watcher`):**
    *   For scenarios where an actor needs to react to changes in the file system (file creation, modification, deletion) without polling.
    *   The actor (or a helper class it owns) can inherit from `qb::io::async::file_watcher<MyActor>` or `directory_watcher<MyActor>`.
    *   It then implements `void on(qb::io::async::event::file const& event)` to handle notifications.
    *   The watcher is started with `this->start(path_to_watch, interval);`.
    *   **(Reference:** The `example/core_io/file_monitor/` demonstrates this pattern with a `DirectoryWatcher` actor.)**

By using these asynchronous patterns, actors can effectively manage time-based logic and interact with potentially blocking resources like the file system without compromising the overall responsiveness and concurrency of the QB application.

**(Next:** [Integrating Core & IO: Network Actors](./network_actors.md) to see how actors handle network I/O.**) 