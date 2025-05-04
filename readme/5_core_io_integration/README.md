# Core & IO Integration Overview

While `qb-io` can function independently, its primary role within the QB framework is to serve as the asynchronous foundation for `qb-core`. Actors seamlessly integrate with `qb-io` components to handle networking, file operations, and timers without blocking their execution threads.

## The Synergy: How They Work Together

*   **Shared Event Loop:** The key integration point is the event loop (`qb::io::async::listener`). Each `qb::VirtualCore` (the execution context for actors) runs its own `listener`. This *single loop* per core manages both:
    *   **Actor Events:** Messages (`qb::Event`) passed between actors.
    *   **I/O Events:** Notifications from the OS about socket readiness, file changes, timer expirations (`qb::io::async::event::*`).
*   **Non-Blocking Actors:** When an actor performs an I/O operation using `qb-io` (e.g., reads from a socket using `qb::io::use<...>::tcp::client`), the operation is initiated non-blockingly. The actor (and its `VirtualCore`) doesn't wait. The `listener` later notifies the actor when the operation completes or data is ready, typically by invoking an `on(...)` handler (like `on(Protocol::message&)` or `on(event::disconnected&)`).
*   **Unified Scheduling:** Timers scheduled via `qb::io::async::callback` or `with_timeout` are handled by the same `listener` that dispatches actor events, ensuring integrated scheduling of both actor logic and timed operations.

## Key Integration Points & Usage Patterns

1.  **Networked Actors (`qb::io::use<>`):**
    *   Actors become network endpoints (clients/servers) by inheriting from `qb::io::use<MyActor>::tcp::*`, `::udp::*`, or `::tcp::ssl::*` templates.
    *   These base classes provide the necessary `transport()`, `in()`, `out()` methods and integrate with the `listener` for async operations.
    *   Network events (like received messages or disconnections) are dispatched to the actor's `on(...)` handlers.
    *   **See:** `[Network Actors](./network_actors.md)`
    *   **Examples:** `chat_tcp`, `message_broker`.

2.  **Asynchronous Operations within Actors:**
    *   Actors can directly use `qb::io::async::callback` to schedule future work or break down long tasks without blocking.
    *   Actors can use `qb::io::async::with_timeout` for internal timeout logic, although `async::callback` is often simpler for actor-specific timeouts.
    *   Blocking file I/O can be wrapped in `async::callback`, but dedicated file processing actors are often a cleaner pattern for complex scenarios.
    *   **See:** `[Async Operations in Actors](./async_in_actors.md)`
    *   **Examples:** `file_monitor`, `file_processor`, `example3_lifecycle.cpp`, `example8_state_machine.cpp`.

3.  **Example Analysis:**
    *   Detailed breakdowns of how the `core_io` examples leverage this integration are provided.
    *   **See:** `[Example Analysis](./examples/)`

Understanding this integration is key to building efficient and responsive applications with QB, where actors can handle both computation and I/O concurrently without performance degradation due to blocking. 