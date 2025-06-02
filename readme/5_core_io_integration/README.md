@page core_io_integration_readme_md QB Framework: Integrating Core Actors with Asynchronous I/O
@brief Understand how `qb-core` actors seamlessly leverage `qb-io` for non-blocking network operations, timers, and file handling.

# QB Framework: Integrating Core Actors with Asynchronous I/O

The QB Actor Framework achieves its power and efficiency through the tight and seamless integration of its two primary components: the `qb-core` actor engine and the `qb-io` asynchronous I/O library. While `qb-io` can function as a standalone library, its design is pivotal to how actors in `qb-core` interact with the outside world (networks, file systems) and manage time-based events without blocking.

This section explores how these two modules work in concert, enabling you to build truly concurrent and responsive applications.

## Core Principles of Integration

*   **Shared Event Loop:** The key integration point is the event loop (`qb::io::async::listener`). Each `qb::VirtualCore` (the execution context for actors) runs its own `listener`. This *single loop* per core manages both actor events and I/O events.
*   **Non-Blocking Actors:** When an actor performs an I/O operation using `qb-io` (e.g., network reads/writes), the operation is initiated non-blockingly. The `listener` notifies the actor when the operation completes or data is ready, typically by invoking an `on(...)` handler.
*   **Unified Scheduling:** Timers and delayed tasks scheduled via `qb::io::async::callback` or `qb::io::async::with_timeout` are handled by the same `listener` that dispatches actor events.

## Key Topics in This Section:

*   **[Asynchronous Operations within Actors](./async_in_actors.md)**
    *   Learn how actors can use `qb::io::async::callback` for timers and deferred tasks, and `qb::io::async::with_timeout` for managing inactivity or operational timeouts. Also covers strategies for handling blocking file I/O from actors asynchronously.

*   **[Building Network-Enabled Actors](./network_actors.md)**
    *   Discover how to make your actors network clients or servers using the `qb::io::use<>` helper template, integrating TCP, UDP, and SSL/TLS capabilities directly into actor logic.

*   **[Case Studies: Example Analyses](./examples/README.md)**
    *   Explore detailed walkthroughs of complex examples (`chat_tcp`, `distributed_computing`, `file_monitor`, `file_processor`, `message_broker`) to see how `qb-core` and `qb-io` are used together to build substantial applications.

Understanding this integration is key to unlocking the full potential of the QB Actor Framework, enabling you to build applications that are not only concurrently sound but also exceptionally performant and responsive to external events and I/O demands.

**(Next:** Dive into [Integrating Core & IO: Async Operations in Actors](./async_in_actors.md) or [Integrating Core & IO: Network Actors](./network_actors.md) for more specific details.**) 