# Core Concept: Asynchronous I/O Model

The foundation of QB's responsiveness and performance, especially when dealing with network or file operations, lies in its asynchronous I/O model provided by the `qb-io` library.

## Principle: Non-Blocking Operations

Instead of a thread waiting (blocking) for an I/O operation (like reading from a network socket or writing to a file) to complete, QB uses non-blocking operations.

1.  **Initiation:** An operation is initiated (e.g., request to read from a socket).
2.  **Registration:** The framework registers interest in the operation's completion with the underlying operating system's event notification system (like epoll, kqueue, or IOCP via libev).
3.  **Yielding:** The thread immediately returns control to the event loop, freeing it to process other tasks (like handling events for other actors).
4.  **Notification:** When the operation is ready (e.g., data is available to be read, the socket is ready for writing), the OS notifies the event loop.
5.  **Dispatch:** The event loop identifies which operation is ready and triggers the corresponding callback or sends an event to the component (e.g., an actor) that initiated the operation.

This prevents threads from idling while waiting for slow I/O, maximizing CPU utilization and system throughput.

## The Event Loop (`qb::io::async::listener`)

At the heart of the asynchronous system is the event loop, managed by the `qb::io::async::listener` class (`qb/include/qb/io/async/listener.h`).

*   **Basis:** It wraps the `libev` library, a high-performance event loop.
*   **Thread-Local:** Each thread managed by `qb::Main` (each `VirtualCore`) has its *own* instance of `listener`, accessible via `qb::io::async::listener::current`.
*   **Function:** It monitors various event sources:
    *   File descriptors for read/write readiness (sockets, files).
    *   Timers for delayed or periodic execution.
    *   System signals.
    *   File system changes (`directory_watcher`).
*   **Dispatching:** When an event occurs, the listener invokes the corresponding registered callback or handler.

## Driving the Event Loop

The event loop needs to be run explicitly to process events:

*   **`qb::io::async::run(flag)`:** Runs the event loop. The `flag` determines the behavior:
    *   `EVRUN_NOWAIT` (default 0 for `run()`): Checks for pending events once and returns immediately.
    *   `EVRUN_ONCE`: Waits for at least one event to occur, processes it, and returns.
*   **Integration with `qb::Main`:** The `qb::Main` engine automatically runs the event loop for each `VirtualCore` as part of its main processing cycle (`VirtualCore::__workflow__`). You generally *do not* need to call `run()` manually when using `qb-core` actors.
*   **Standalone `qb-io`:** If using `qb-io` without `qb-core`, you are responsible for driving the event loop in your application's main thread or worker threads.

**(See also:** `[QB-IO: Async System](./../3_qb_io/async_system.md)`**)**

## Asynchronous Callbacks (`qb::io::async::callback`)

A primary way to schedule work asynchronously is using `qb::io::async::callback` (`qb/include/qb/io/async/io.h`).

*   **Purpose:** Executes a provided function (lambda, function pointer, functor) after a specified delay.
*   **Mechanism:** Uses an internal `ev::timer` watcher.
*   **Usage:**
    ```cpp
    #include <qb/io/async.h>
    #include <iostream>

    // ... inside some function or actor method ...

    // Execute immediately in the next event loop iteration
    qb::io::async::callback([]() {
        std::cout << "Callback executed immediately." << std::endl;
    });

    // Execute after a 500ms delay
    double delay_seconds = 0.5;
    qb::io::async::callback([]() {
        std::cout << "Callback executed after delay." << std::endl;
    }, delay_seconds);
    ```
*   **Self-Managing:** The underlying timer object (`qb::io::async::Timeout`) manages its own lifetime and automatically cleans itself up after execution.
*   **Context:** Callbacks are executed within the event loop context of the thread where `callback()` was called.

**(Ref:** `example1_async_io.cpp`, `test-async-io.cpp`, `test-actor-delayed-events.cpp`**)**

## Timers (`qb::io::async::with_timeout`)

For components (often non-actor classes or actors needing timeout logic separate from message handling) that require recurring timer events or explicit timeout management, the `qb::io::async::with_timeout<Derived>` base class (`qb/include/qb/io/async/io.h`) provides timer functionality.

*   **Inheritance:** Classes inherit from `with_timeout<MyClass>`.
*   **Handler:** Implement `void on(qb::io::async::event::timer const&)` to handle timer expiration.
*   **Control:**
    *   Constructor takes the initial timeout duration.
    *   `setTimeout(duration)`: Changes the timeout interval.
    *   `updateTimeout()`: Resets the timer countdown (call this upon activity to prevent timeout).
    *   `getTimeout()`: Retrieves the current timeout setting.

```cpp
#include <qb/io/async.h>
#include <iostream>

class MyTimedComponent : public qb::io::async::with_timeout<MyTimedComponent> {
public:
    int tick_count = 0;

    MyTimedComponent(double interval) : with_timeout(interval) {
        std::cout << "Timer started with interval: " << interval << "s" << std::endl;
    }

    // Called when the timer expires
    void on(qb::io::async::event::timer const&) {
        tick_count++;
        std::cout << "Timer tick #" << tick_count << std::endl;

        // To make it periodic, restart the timer
        // If you don't call updateTimeout() or setTimeout(), it stops after one tick.
        updateTimeout(); // Or use setTimeout(new_interval) if needed

        // Example of stopping after 5 ticks
        if (tick_count >= 5) {
            setTimeout(0.0); // Disable the timer
            std::cout << "Timer stopped." << std::endl;
        }
    }
};

// Usage:
// MyTimedComponent timer(0.5); // Tick every 500ms
// // Need to run the event loop (e.g., qb::io::async::run())
```

## Integration with Actors

Actors can leverage the asynchronous system seamlessly:

*   Use `qb::io::async::callback` within `onInit` or event handlers to schedule future actions or break down long tasks without blocking.
*   Actors involved in network or file I/O (often using `qb::io::use<>`) inherently use the async system. Their `on(Protocol::message&)` or `on(event::disconnected&)` handlers are triggered by the event loop.

**(See also:** `[Core & IO Integration](./../5_core_io_integration/)`**)**

## I/O Events (`qb::io::async::event::*`)

Asynchronous operations often signal completion or state changes via specific event types delivered to handlers within I/O components (like those inheriting from `qb::io::use<...>::tcp::client`).

*   `disconnected`: Connection closed.
*   `eof`: No more data to read from input.
*   `eos`: All buffered output has been sent.
*   `file`: File system change detected.

**(Ref:** `[QB-IO: Async System](./../3_qb_io/async_system.md)` for a full list**)

## Integration Summary

The `qb-io` async system provides the non-blocking foundation. `qb-core` integrates this by running the `listener` event loop on each `VirtualCore`. Actors leverage this system implicitly when using integrated I/O features (`qb::io::use<>`) or explicitly via `async::callback` and `with_timeout`. 