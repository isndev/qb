@page qb_core_readme_md QB-Core Module: The Actor Engine
@brief An introduction to `qb-core`, the heart of the QB Actor Framework, enabling concurrent application logic via actors, events, and multi-core scheduling. Provides an overview and navigation for the QB-Core module documentation.

# QB-Core Module: The Actor Model Engine

Welcome to the `qb-core` module documentation. `qb-core` is the high-performance **C++23** library that brings the **Actor Model** to life within the QB Framework. Building directly upon the asynchronous foundation provided by `qb-io`, `qb-core` empowers you to design and implement complex concurrent applications by composing independent, message-driven actors.

This section provides a detailed exploration of `qb-core`'s architecture, its fundamental abstractions like actors and events, the engine that drives them, common patterns for their use, and the modern C++23 coroutine integration.

## Architecture at a Glance

```
┌───────────────────────────────────────────────────────────┐
│                       qb::Main                            │
│   (Engine orchestrator — manages VirtualCore threads)     │
└──────────┬──────────────────────────────┬─────────────────┘
           │ owns                         │ owns
    ┌──────▼──────────┐           ┌───────▼─────────┐
    │  VirtualCore 0  │           │  VirtualCore 1  │  …
    │  ┌───────────┐  │           │  ┌───────────┐  │
    │  │  Actor A  │  │           │  │  Actor C  │  │
    │  ├───────────┤  │  ◄ MPSC ► │  ├───────────┤  │
    │  │  Actor B  │  │ mailboxes │  │  Actor D  │  │
    │  └───────────┘  │           │  └───────────┘  │
    │  (event loop +  │           │  (event loop +  │
    │   io listener)  │           │   io listener)  │
    └─────────────────┘           └─────────────────┘
```

Key design points:
- Each `VirtualCore` runs an independent `qb::io::async::listener` event loop.
- Actors on the **same** core communicate via a fast local queue.
- Actors on **different** cores communicate via **lock-free MPSC mailboxes**.
- Actors may also use **C++23 coroutines** (`spawn_async`) for non-blocking async I/O flows.

## Key Topics in This Section

*   **[QB-Core: Key Features & Capabilities](./features.md)**
    *   A summary of the core features: actor lifecycle, event system, concurrency, coroutine support, and common utilities.

*   **[QB-Core: Mastering `qb::Actor`](./actor.md)**
    *   A comprehensive guide to defining, initializing, and managing actors, including the new `spawn_async` coroutine API, `RefActorHandle`, and `no_default_events`.

*   **[QB-Core: Event Messaging Between Actors](./messaging.md)**
    *   Defining custom events, all sending methods (`push`, `send`, `broadcast`, `reply`, `forward`), QoS levels, and data-carrier best practices.

*   **[QB-Core: Engine — `qb::Main` & `VirtualCore`](./engine.md)**
    *   The QB runtime engine in depth: startup, shutdown, CPU affinity, the VirtualCore loop, inter-core communication, error handling, and the stop-token cancellation mechanism.

*   **[QB-Core: Common Actor Patterns & Utilities](./patterns.md)**
    *   FSM actors, Service Actors, periodic callbacks, referenced actors with `RefActorHandle`, dependency resolution, and coroutine patterns.

## How to Use This Section

*   Begin with the **Key Features & Capabilities** for a high-level understanding.
*   Study **Mastering `qb::Actor`** and **Event Messaging** to learn the fundamentals.
*   Understand the **Engine (`qb::Main` & `VirtualCore`)** to grasp how the system operates and scales.
*   Explore **Common Actor Patterns** for practical ways to structure your application logic.

By mastering `qb-core`, you can build sophisticated, concurrent applications that are both performant and easier to reason about compared to traditional multi-threaded approaches.

**(Next:** If you haven't already, ensure you understand the [QB-IO Module Overview](../3_qb_io/README.md) as `qb-core` builds upon it. Then, proceed to [Core & IO Integration Overview](../5_core_io_integration/README.md) to see how these two modules work together in practice.)** 