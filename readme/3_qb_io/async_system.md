# QB-IO: Asynchronous System (`qb::io::async`)

The `qb::io::async` namespace provides the core mechanisms for non-blocking, event-driven programming in `qb-io`. It integrates with an underlying event loop (libev) to handle timers, callbacks, I/O events, and signals.

## How it Works: The Event Loop

*   **`qb::io::async::listener` (`listener.h`):** This class manages the event loop for a given thread. Each thread using async features typically has one associated `listener` (accessible via `listener::current`). When used with `qb-core`, each `VirtualCore` manages its own `listener` implicitly.
*   **Event Sources:** The listener monitors various sources:
    *   File descriptors (sockets, files) for read/write readiness (`event::io`).
    *   Timers for scheduled execution (`event::timer`).
    *   System signals (`event::signal`).
    *   File system paths for changes (`event::file`).
*   **Event Dispatching:** When an event occurs (e.g., data arrives on a socket), the OS notifies the listener. The listener identifies the corresponding handler (often an I/O object like a session or a timer wrapper) and invokes its callback method (e.g., `on(event::io&)`, `on(event::timer&)`).
*   **Running the Loop:** The loop must be actively run to process events. `qb::Main` handles this automatically for actors. For standalone use, you need to call `qb::io::async::run(flag)` in your thread(s).

## Key Components & Usage

### Scheduling Callbacks (`qb::io::async::callback`)

(`io.h`)

This is the simplest way to schedule a function (lambda, functor, function pointer) to run asynchronously after a delay.

*   **How to Use:**
    ```cpp
    #include <qb/io/async.h>
    #include <iostream>

    void myDelayedFunction() {
        std::cout << "Executed after delay!" << std::endl;
    }

    // Schedule immediately (next loop iteration)
    qb::io::async::callback([]{ std::cout << "Immediate callback." << std::endl; });

    // Schedule after 1.2 seconds
    double delay = 1.2;
    qb::io::async::callback(myDelayedFunction, delay);
    ```
*   **Lifecycle:** The `callback` function creates a temporary `Timeout` object internally, which registers itself with the listener and deletes itself after the callback executes. You don't need to manage its lifetime.
*   **Use Cases:** Delaying operations, breaking work into chunks, scheduling retries.

**(Ref:** `example1_async_io.cpp`, `test-async-io.cpp`, `test-actor-delayed-events.cpp`**)

### Handling Timeouts (`qb::io::async::with_timeout`)

(`io.h`)

A mix-in class (using CRTP) to add timeout logic to your own classes (including `qb::Actor` if needed, though `async::callback` is often simpler for actors).

*   **How to Use:**
    1.  Inherit: `class MyClass : public qb::io::async::with_timeout<MyClass> { ... };`
    2.  Constructor: Call the base constructor `with_timeout(timeout_seconds);`.
    3.  Handler: Implement `void on(qb::io::async::event::timer const &) { /* handle timeout */ }`.
    4.  Activity Reset: Call `updateTimeout()` whenever relevant activity occurs to reset the timer.
    5.  Control: Use `setTimeout(new_duration)` to change or disable (with 0.0) the timeout.

```cpp
// Example: Session with inactivity timeout
class ClientSession : public qb::io::async::with_timeout<ClientSession>, /* other bases */ {
public:
    ClientSession(/*...*/) : with_timeout(60.0) { /* 60s timeout */ }

    void processIncomingData() {
        // ... process ...
        updateTimeout(); // Reset timer on activity
    }

    void on(qb::io::async::event::timer const&) {
        std::cout << "Session timed out due to inactivity." << std::endl;
        disconnect(); // Method to close the connection
    }

    void disconnect() { setTimeout(0.0); /* ... close socket ... */ }
};
```
**(Ref:** `test-async-io.cpp::TimerHandler`, `chat_tcp/server/ChatSession.h`**)

### Handling Asynchronous Events (`qb::io::async::event::*`)

(`event/all.h`)

I/O components built using the `qb::io::async::io` or `qb::io::async::file_watcher` base classes (often via `qb::io::use<>` helpers) receive notifications through `on(EventType&)` handlers.

*   **Common Handlers:**
    *   `on(event::disconnected const&)`: Connection closed by peer or error.
    *   `on(event::eof const&)`: Input stream ended.
    *   `on(event::eos const&)`: All output flushed.
    *   `on(event::file const&)`: File/directory changed (used by watchers).
*   **Implementation:** Override the relevant `on(...)` method in your class inheriting from the async base (e.g., `qb::io::use<MyActor>::tcp::client`).

```cpp
class MyTcpClient : public qb::io::use<MyTcpClient>::tcp::client<> {
public:
    // ... protocol definition ...
    using Protocol = qb::protocol::text::command<MyTcpClient>;

    void on(Protocol::message&& msg) { /* Handle received message */ }

    // Handle disconnection
    void on(qb::io::async::event::disconnected const& event) {
        std::cerr << "Disconnected from server (reason: " << event.reason << ")" << std::endl;
        // Attempt reconnect or cleanup
    }
};
```
**(Ref:** `test-event-combined.cpp`, `test-async-io.cpp`, `chat_tcp`, `message_broker` examples**) 