@page qb_io_async_system_md QB-IO: The Asynchronous Engine (`qb::io::async`)
@brief A deep dive into `qb-io`'s event loop, timers, callbacks, and event management for standalone asynchronous programming.

# QB-IO: The Asynchronous Engine (`qb::io::async`)

The `qb::io::async` namespace is the powerhouse behind `qb-io`'s non-blocking capabilities. It provides a complete, event-driven asynchronous programming model built around a high-performance event loop. While it seamlessly integrates with the `qb-core` actor system, `qb::io::async` is also designed to be fully usable as a standalone toolkit for any C++17 application requiring efficient asynchronous operations.

## The Event Loop: `qb::io::async::listener`

At the heart of the asynchronous system is the `qb::io::async::listener` class (defined in `qb/io/async/listener.h`). This class is your primary interface to the event loop.

*   **Underlying Engine:** `listener` wraps the `libev` C library, known for its high performance and portability across POSIX systems (epoll, kqueue) and Windows (IOCP, select).
*   **Thread-Local Instance (`listener::current`):** Crucially, `qb-io` provides a **thread-local static instance** of the `listener` named `qb::io::async::listener::current`. This means each thread that intends to perform asynchronous operations with `qb-io` has its own dedicated event loop manager. You don't need to pass listener objects around; you simply access `listener::current` from the thread where you want to register or manage events.
*   **Initialization (Standalone Usage):** If you're using `qb-io` without `qb-core`, you must initialize the listener for each participating thread by calling `qb::io::async::init();` once per thread before using other `qb::io::async` features. (When using `qb-core`, `qb::Main` handles this for its `VirtualCore` threads).

### Responsibilities of the `listener`:

1.  **Monitoring Event Sources:** The `listener` continuously monitors various sources of events:
    *   **I/O Events (`qb::io::async::event::io`):** Readiness of file descriptors (sockets, pipes, files) for reading or writing.
    *   **Timer Events (`qb::io::async::event::timer`):** Expiration of scheduled timers for delayed or periodic actions.
    *   **Signal Events (`qb::io::async::event::signal`):** Asynchronous notification of operating system signals (e.g., SIGINT, SIGTERM).
    *   **File System Stat Events (`qb::io::async::event::file`):** Notifications about changes to file attributes (used by `file_watcher` and `directory_watcher`).
2.  **Event Dispatching:** When an event source becomes active (e.g., data arrives on a socket, a timer expires), the `listener` identifies the corresponding registered handler (an object implementing `IRegisteredKernelEvent`) and invokes its `invoke()` method. This, in turn, typically calls the user-defined `on(SpecificEvent&)` method within the I/O component or handler class.

### Driving the Event Loop (Standalone Usage)

For the `listener` to process events, its loop must be actively run:

*   **`qb::io::async::run(int flag = 0)`:** This function executes the event loop for `listener::current`. The `flag` argument controls its behavior:
    *   `0` (default): Runs the loop until `qb::io::async::break_parent()` is called or no active event watchers remain. This is a blocking call.
    *   `EVRUN_ONCE`: Waits for at least one event to occur, processes the block of available events, and then returns.
    *   `EVRUN_NOWAIT`: Checks for any immediately pending events, processes them, and returns without waiting if none are pending.
*   **`qb::io::async::run_once()`:** Convenience for `run(EVRUN_ONCE)`.
*   **`qb::io::async::run_until(const bool& status_flag)`:** Runs the loop with `EVRUN_NOWAIT` repeatedly as long as `status_flag` is true.
*   **`qb::io::async::break_parent()`:** Signals `listener::current` to stop its current `run()` cycle.

```cpp
// Standalone qb-io example sketch
#include <qb/io/async.h>
#include <iostream>
#include <atomic>

std::atomic<bool> g_is_running = true;

void my_periodic_task() {
    std::cout << "Periodic task executed at: " << qb::UtcTimePoint::now().to_iso8601() << std::endl;
    if (g_is_running) {
        // Reschedule self
        qb::io::async::callback(my_periodic_task, 1.0); // Every 1 second
    }
}

int main() {
    qb::io::async::init(); // Initialize listener for the main thread

    std::cout << "Starting standalone qb-io event loop demo.\n";
    std::cout << "Press Ctrl+C to exit (if signal handling is set up, or wait for tasks).\n";

    // Schedule an initial periodic task
    qb::io::async::callback(my_periodic_task, 1.0);

    // Schedule a one-shot task to stop the loop after 5 seconds
    qb::io::async::callback([]() {
        std::cout << "5 seconds elapsed. Signaling application to stop.\n";
        g_is_running = false;
        qb::io::async::break_parent(); // Request the run loop to exit
    }, 5.0);

    // Run the event loop until break_parent() is called or g_is_running is false
    // In a real app, this might be: while(g_is_running) { qb::io::async::run(EVRUN_ONCE); /* other logic */ }
    qb::io::async::run(); // Runs until break_parent() or no more active watchers

    std::cout << "Event loop finished.\n";
    return 0;
}
```

## Registering Event Handlers

The `listener::current.registerEvent<_Event, _Actor, _Args...>(actor, args...)` method is the core mechanism for associating an event watcher (like `qb::io::async::event::io` or `timer`) with a handler object (`_Actor`) and the event loop.

*   `_Event`: The qb-io event type (e.g., `qb::io::async::event::timer`). This wraps a specific `libev` watcher.
*   `_Actor`: The class instance that will handle the event (must have an `on(_Event&)` method).
*   `_Args...`: Arguments specific to the `libev` watcher being set up (e.g., file descriptor and `EV_READ`/`EV_WRITE` flags for an `event::io` watcher, or timeout values for an `event::timer`).

Most `qb-io` components designed for asynchronous operations (e.g., those inheriting from `qb::io::async::io`, `qb::io::async::file_watcher`, or `qb::io::async::with_timeout`) handle this registration internally when they are constructed or started.

## Asynchronous Callbacks: `qb::io::async::callback`

(`qb/io/async/io.h`)

This utility function provides a straightforward way to schedule a callable (lambda, function pointer, functor) for execution by the current thread's event loop, optionally after a delay.

*   **Execution:** The callback runs on the same thread that scheduled it.
*   **Lifetime:** The internal timer object (`qb::io::async::Timeout`) created by `callback` is self-managing; it registers with the listener and deletes itself after the callback is invoked.
*   **Usage:** `qb::io::async::callback(my_function, delay_seconds);` or `qb::io::async::callback([]{ /* lambda body */ });`
*   **Use Cases:** Delaying operations, breaking work into smaller non-blocking chunks, scheduling retries, and simple periodic tasks (by re-scheduling from within the callback).

**(See practical examples in:** [Core Concepts: Asynchronous I/O Model in QB](./../2_core_concepts/async_io.md) for actor-centric usage, and `example1_async_io.cpp` for general usage.**)**

## Timeout Management: `qb::io::async::with_timeout<Derived>`

(`qb/io/async/io.h`)

This CRTP base class allows you to easily add timeout functionality to your own classes.

*   **Inheritance:** `class MyClass : public qb::io::async::with_timeout<MyClass> { ... };`
*   **Handler Method:** Implement `void on(qb::io::async::event::timer const& event)` in `MyClass` to define what happens when the timeout occurs.
*   **Control:**
    *   The constructor `with_timeout(timeout_in_seconds)` initializes and starts the timer.
    *   Call `updateTimeout()` on any activity that should reset the timeout countdown.
    *   Use `setTimeout(new_duration_seconds)` to change the timeout period or disable it (by passing `0.0`).

This mechanism is ideal for implementing inactivity timeouts in network sessions, operation timeouts, or any scenario where an action needs to be taken if something doesn't happen within a specific timeframe.

**(A detailed example is available in:** [Core Concepts: Asynchronous I/O Model in QB](./../2_core_concepts/async_io.md) and `test-async-io.cpp::TimerHandler`**)**

## Standard Asynchronous Event Types (`qb::io::async::event::*`)

(`qb/io/async/event/all.h`)

`qb-io` defines a suite of event structures used to notify components about various asynchronous occurrences. When building custom I/O components (often by inheriting from `qb::io::async::input`, `qb::io::async::output`, or `qb::io::async::io`), you will typically override `on(SpecificEvent&)` methods to handle these:

*   `disconnected`: A network connection was closed or an error occurred.
*   `eof`: End-of-file reached on an input stream; no more data to read.
*   `eos`: End-of-stream reached on an output stream; all buffered data has been sent.
*   `file`: Attributes of a monitored file or directory have changed (used by `file_watcher`).
*   `io`: Raw low-level I/O readiness on a file descriptor (e.g., socket is readable/writable).
*   `pending_read`/`pending_write`: Data remains in input/output buffers after a partial operation.
*   `signal`: An OS signal was caught.
*   `timer`/`timeout`: A previously set timer or timeout has expired.

These events form the backbone of communication between the `listener` and the I/O handling components, enabling a fully event-driven architecture.

**(Next:** [QB-IO: Transports](./transports.md) to see how these async mechanisms are applied to TCP, UDP, etc.**) 