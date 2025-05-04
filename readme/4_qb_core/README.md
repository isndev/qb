# QB-Core Module Overview

`qb-core` builds upon the foundation laid by `qb-io` to provide a complete, high-performance **Actor Model engine** for C++17. It manages actor lifecycles, schedules their execution across multiple cores, and facilitates communication between them using a type-safe event system.

## Purpose & Design Goals

*   **Simplify Concurrency:** Abstract low-level threading, locking, and synchronization using the Actor Model.
*   **Enable Parallelism:** Distribute actors across `VirtualCore` instances (threads) for execution on multi-core hardware.
*   **Promote Isolation:** Ensure actor state is encapsulated and accessed only via asynchronous messages.
*   **Provide Robust Communication:** Offer efficient and type-safe event passing mechanisms.
*   **Integrate with IO:** Seamlessly integrate with `qb-io` for actors that need asynchronous networking or file operations.

## Key Abstractions

*   **`qb::Actor`:** The user-defined unit of concurrency and state. ([Details](./actor.md))
*   **`qb::Event`:** The messages actors exchange. ([Details](./messaging.md))
*   **`qb::Main`:** The main engine controller, managing threads and system lifecycle. ([Details](./engine.md))
*   **`qb::VirtualCore`:** A worker thread executing actors and their event loop. ([Details](./engine.md))
*   **`qb::ActorId`:** Unique identifier used to address actors.
*   **`qb::ICallback`:** Interface for actors requiring periodic execution.
*   **`qb::ServiceActor<Tag>`:** Pattern for singleton actors per core.
*   **`qb::Pipe` / `EventBuilder`:** Tools for optimized event sending.

## Core Features

*   **Actor Lifecycle Management:** Controlled creation (`addActor`), initialization (`onInit`), termination (`kill`), and destruction.
*   **Event-Driven Execution:** Actors react to incoming events via `on(Event&)` handlers.
*   **Type-Safe Messaging:** Compile-time checks for event handling.
*   **Flexible Communication:** Ordered (`push`), unordered (`send`), broadcast, reply, forward.
*   **Multi-Core Scheduling:** Automatic distribution of actors and work across cores.
*   **Efficient Inter-Core Communication:** Transparent message routing using lock-free queues.
*   **Configurable Concurrency:** Control over core affinity and event loop latency.
*   **Common Actor Patterns:** Support for services, periodic tasks, dependency discovery.

## Relationship with `qb-io`

`qb-core` is fundamentally dependent on `qb-io`:

*   The `VirtualCore` event loop *is* the `qb::io::async::listener`.
*   Actor timers and delayed actions rely on `qb::io::async::callback`.
*   Actors performing network or file I/O use `qb-io` transports and protocols, integrated via `qb::io::use<>`.

While `qb-io` provides the *mechanisms* for async operations, `qb-core` provides the *framework* for organizing concurrent application logic using those mechanisms within the Actor Model.

**(See also:** `[Engine](./engine.md)`, `[Actor](./actor.md)`, `[Messaging](./messaging.md)`, `[Patterns](./patterns.md)`, `[Core & IO Integration](./../5_core_io_integration/)`**)** 