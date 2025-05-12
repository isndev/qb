@page qb_core_readme_md QB-Core Module: The Actor Engine
@brief An introduction to `qb-core`, the heart of the QB Actor Framework, enabling concurrent application logic via actors, events, and multi-core scheduling. Provides an overview and navigation for the QB-Core module documentation.

# QB-Core Module: The Actor Model Engine

Welcome to the `qb-core` module documentation. `qb-core` is the high-performance C++17 library that brings the **Actor Model** to life within the QB Framework. Building directly upon the asynchronous foundation provided by `qb-io`, `qb-core` empowers you to design and implement complex concurrent applications by composing independent, message-driven actors.

This section provides a detailed exploration of `qb-core`'s architecture, its fundamental abstractions like actors and events, the engine that drives them, and common patterns for their use.

## Key Topics in This Section:

*   **[QB-Core: Key Features & Capabilities](./features.md)**
    *   A summary of the core features provided by the QB Actor Engine, including actor lifecycle management, the event system, concurrency and scheduling capabilities, and common actor utilities.

*   **[QB-Core: Mastering `qb::Actor`](./actor.md)**
    *   A comprehensive guide to defining, initializing, and managing the lifecycle of actors using the `qb::Actor` base class, including state management and event handler implementation.

*   **[QB-Core: Event Messaging Between Actors](./messaging.md)**
    *   Detailed information on defining custom events derived from `qb::Event`, the various methods actors use to send events (`push`, `send`, `broadcast`, `reply`, `forward`), and how events are received and processed.

*   **[QB-Core: Engine - `qb::Main` & `VirtualCore`](./engine.md)**
    *   An explanation of the QB actor system's engine, detailing how `qb::Main` orchestrates `VirtualCore` worker threads and how actors are scheduled and executed within this environment.

*   **[QB-Core: Common Actor Patterns & Utilities](./patterns.md)**
    *   Discover and learn how to implement common and effective actor-based design patterns with QB, such as Finite State Machines, Service Actors, Periodic Callbacks, Referenced Actors, and Dependency Resolution.

## How to Use This Section

*   Begin with the **Key Features & Capabilities** for a high-level understanding.
*   Study **Mastering `qb::Actor`** and **Event Messaging** to learn the fundamentals of creating and communicating with actors.
*   Understand the **Engine (`qb::Main` & `VirtualCore`)** to grasp how the system operates and scales.
*   Explore **Common Actor Patterns** for practical ways to structure your application logic.

By mastering `qb-core`, you can build sophisticated, concurrent applications that are both performant and easier to reason about compared to traditional multi-threaded approaches.

**(Next:** If you haven't already, ensure you understand the `[QB-IO Module Overview](../3_qb_io/README.md)` as `qb-core` builds upon it. Then, proceed to `[Core & IO Integration Overview](../5_core_io_integration/README.md)` to see how these two modules work together in practice.**) 