@page core_concepts_async_io_md Core Concept: Asynchronous I/O Model in QB
@brief Uncover QB's non-blocking, event-driven I/O model powered by `qb-io` and its integration with actors.

# Core Concept: Asynchronous I/O Model in QB

The QB framework's ability to handle numerous concurrent operations efficiently, especially network and file interactions, stems from its powerful **asynchronous I/O model**. This model, provided by the `qb-io` library, ensures that your application remains responsive and scalable.

## The Non-Blocking Principle: Never Wait for I/O

Traditional I/O operations are often *blocking*: when your code tries to read from a network socket or a file, it pauses (blocks) until the data is available or the operation completes. In a high-concurrency environment, this leads to idle threads and wasted CPU cycles.

QB champions a **non-blocking** approach:

1.  **Initiate Operation:** Your code requests an I/O operation (e.g., start reading from a socket).
2.  **Register Interest:** The QB framework, through `qb-io`, tells the operating system (via efficient mechanisms like epoll, kqueue, or IOCP, abstracted by the internal `libev` library) that it's interested in knowing when this operation is ready (e.g., when data arrives).
3.  **Return Immediately:** The calling thread (typically a `VirtualCore` executing an actor) **does not wait**. It immediately returns to its event loop, free to process other events for other actors or perform other tasks.
4.  **OS Notification:** When the I/O operation can proceed without blocking (e.g., data is available for reading, or a socket is ready for writing), the operating system notifies QB's event loop.
5.  **Dispatch to Handler:** The event loop identifies which operation is ready and dispatches this readiness notification. In the context of actors using `qb-io` features, this usually means invoking an appropriate `on()` handler (e.g., `on(MyProtocol::Message&)` when data forms a complete message, or `on(qb::io::async::event::disconnected&)` if a connection drops).

This non-blocking philosophy is key to maximizing CPU utilization and enabling QB applications to handle thousands of concurrent connections or operations with high throughput.

## The Event Loop: `qb::io::async::listener`

At the core of QB's asynchronous I/O is the **event loop**, managed by the `qb::io::async::listener` class (defined in `qb/io/async/listener.h`).

*   **Engine:** It wraps the `libev` library, a mature and high-performance C event loop.
*   **Thread-Local Instance:** Each `VirtualCore` (worker thread managed by `qb::Main`) runs its own dedicated `listener` instance. This instance is accessible within that thread via `qb::io::async::listener::current`.
*   **Monitoring Event Sources:** The `listener` diligently monitors various event sources:
    *   **File Descriptors:** For read/write readiness on sockets, files (leading to `qb::io::async::event::io`).
    *   **Timers:** For scheduled, delayed, or periodic execution of code (leading to `qb::io::async::event::timer`).
    *   **System Signals:** For handling OS signals like SIGINT, SIGTERM asynchronously (`qb::io::async::event::signal`).
    *   **File System Changes:** For watching directories or files for modifications (`qb::io::async::event::file`).
*   **Dispatching:** When an event source becomes active, the `listener` determines the registered handler (often an I/O component within an actor or a dedicated callback mechanism) and invokes its corresponding method (e.g., `on(qb::io::async::event::timer&)`).

**(See also:** [QB-IO: Asynchronous System (`qb::io::async`)](./../3_qb_io/async_system.md)**)**

## Driving the Event Loop: Who Runs `run()`?

The event loop must be actively "run" to process pending events.

*   **Actors & `qb::Main`:** If you're using `qb-core` and actors, you generally **do not need to call `qb::io::async::run()` manually**. The `qb::Main` engine ensures that each `VirtualCore` continuously runs its associated `listener` event loop as part of its main actor-processing cycle (`VirtualCore::__workflow__`).
*   **Standalone `qb-io`:** If you are using the `qb-io` library without the `qb-core` actor system, your application is responsible for driving the event loop in its main thread or any worker threads that use `qb-io`'s asynchronous features. This is done by calling `qb::io::async::run(flag)`, where `flag` can be `EVRUN_NOWAIT` (check once and return) or `EVRUN_ONCE` (wait for and process one block of events), or `0` (default, runs until `break_loop()` is called or no active watchers remain).

## Scheduling Asynchronous Callbacks: `qb::io::async::callback`

One of the most direct ways for an actor (or any code running on a `VirtualCore` thread) to perform actions asynchronously or after a delay is using `qb::io::async::callback` (from `qb/io/async/io.h`).

*   **Purpose:** Executes a provided callable (lambda, function pointer, functor) after a specified delay within the event loop of the *calling thread*.
*   **Usage Example (within an Actor):**
    ```cpp
    #include <qb/actor.h>     // For qb::Actor, qb::ActorId
    #include <qb/io/async.h>  // For qb::io::async::callback
    #include <qb/io.h>        // For qb::io::cout
    #include <chrono>         // For std::chrono::seconds

    struct MyDelayedTaskEvent : qb::Event {};

    class MyActor : public qb::Actor {
    public:
        bool onInit() override {
            registerEvent<MyDelayedTaskEvent>(*this);

            qb::io::cout() << "Actor [" << id() << "]: Scheduling immediate task.\n";
            // Schedule a lambda to execute in the next event loop iteration on this core
            qb::io::async::callback([self_id = id()]() {
                // This lambda runs on the same VirtualCore as MyActor
                // Best practice: send an event back to the actor to handle the logic
                if (VirtualCore::_handler) { // Ensure VirtualCore handler is accessible
                     VirtualCore::_handler->push<MyDelayedTaskEvent>(self_id, self_id);
                }
            });

            qb::io::cout() << "Actor [" << id() << "]: Scheduling task in 2.5 seconds.\n";
            double delay_seconds = 2.5;
            qb::io::async::callback([self_id = id()]() {
                qb::io::cout() << "Actor [" << self_id << "]: Delayed task (2.5s) executing!\n";
                 VirtualCore::_handler->push<MyDelayedTaskEvent>(self_id, self_id);
            }, delay_seconds);

            return true;
        }

        void on(const MyDelayedTaskEvent& /*event*/) {
            if (!is_alive()) return; // Good practice if callback might outlive actor
            qb::io::cout() << "Actor [" << id() << "]: Processed a MyDelayedTaskEvent.\n";
        }
        // ... other handlers, including qb::KillEvent ...
    };
    ```
*   **Self-Managing Lifetime:** The underlying mechanism for `qb::io::async::callback` (an internal `qb::io::async::Timeout` object) manages its own lifetime, registering with the listener and automatically cleaning up after the callback executes.
*   **Actor Safety:** When capturing `this` or `id()` in a callback lambda, always check `is_alive()` at the beginning of the lambda if there's a chance the actor might be terminated before the callback runs.
*   **Common Use Cases for Actors:**
    *   Implementing timeouts for operations.
    *   Scheduling retries for failed operations (e.g., network connection attempts).
    *   Breaking down long-running, non-blocking tasks into smaller chunks to yield control back to the event loop periodically.
    *   Simulating processing delays in examples or tests.

**(Reference:** `example1_async_io.cpp`, `test-actor-delayed-events.cpp`, `file_processor/file_worker.h` for wrapping blocking calls**)**

## Managing Timeouts with `qb::io::async::with_timeout`

For classes (including actors, though `async::callback` is often more idiomatic for actor-specific timeouts) that need more explicit timeout management or recurring timer events, the `qb::io::async::with_timeout<Derived>` CRTP base class (`qb/io/async/io.h`) is available.

*   **Inheritance:** Your class inherits from `with_timeout<MyClass>`.
*   **Handler:** Implement `void on(qb::io::async::event::timer const&)` to be called when the timer expires.
*   **Control:**
    *   The constructor `with_timeout(timeout_seconds)` sets and starts the initial timer.
    *   `updateTimeout()`: Call this upon relevant activity to reset the timeout countdown.
    *   `setTimeout(new_duration_seconds)`: Changes the timeout interval. Use `0.0` to disable.

```cpp
// Example: An actor session that times out due to inactivity
class InactiveSession : public qb::Actor, 
                        public qb::io::async::with_timeout<InactiveSession> {
public:
    InactiveSession() : with_timeout(60.0) { /* 60-second inactivity timeout */ }

    bool onInit() override {
        registerEvent<ClientActivityEvent>(*this);
        registerEvent<qb::KillEvent>(*this);
        updateTimeout(); // Start the initial countdown
        return true;
    }

    void on(const ClientActivityEvent& /*event*/) {
        qb::io::cout() << "Session [" << id() << "]: Activity detected, resetting timeout.\n";
        updateTimeout(); // Reset the timer on client activity
    }

    // Called by with_timeout if updateTimeout() isn't called within 60s
    void on(qb::io::async::event::timer const& /*event*/) {
        qb::io::cout() << "Session [" << id() << "]: Inactivity timeout! Terminating.\n";
        kill();
    }

    void on(const qb::KillEvent& /*event*/) { setTimeout(0.0); kill(); }
};
```
**(Reference:** `test-async-io.cpp::TimerHandler`, `chat_tcp/server/ChatSession.h` uses this for client inactivity.)**

## QB-IO Asynchronous Event Types (`qb::io::async::event::*`)

When actors use `qb-io` components for networking or file watching (often via `qb::io::use<>` helper templates), they receive notifications about I/O status changes through specific event types derived from `qb::io::async::event::base`.

Common events handled by I/O-enabled actors include:

*   `qb::io::async::event::disconnected`: Signals that a network connection has been closed or lost.
*   `qb::io::async::event::eof` (End-Of-File): Indicates no more data is available for reading from an input stream.
*   `qb::io::async::event::eos` (End-Of-Stream): Signals that all buffered output data has been successfully written to the transport.
*   `qb::io::async::event::file`: Used by `file_watcher` or `directory_watcher` to notify about changes to a monitored file or directory.

These events are delivered to the actor's corresponding `on(EventType&)` handlers, allowing the actor to react to I/O state changes asynchronously.

**(Reference:** The `qb/io/async/event/all.h` header includes all standard I/O event types. See specific examples like `chat_tcp` for their usage.**)

By leveraging this asynchronous I/O model, QB actors can efficiently manage numerous concurrent operations, making the framework well-suited for building high-performance, responsive applications.

**(Next:** [Core Concepts: Concurrency and Parallelism in QB](./concurrency.md)**)
**(See also:** [Core & IO Integration Overview](./../5_core_io_integration/README.md)**)** 