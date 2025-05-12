@page core_concepts_readme QB Framework: Core Concepts
@brief Understand the fundamental building blocks of the QB Actor Framework, including actors, events, asynchronous I/O, and concurrency management.

# QB Framework: Core Concepts

This section delves into the foundational concepts that underpin the QB Actor Framework. A solid grasp of these principles is essential for effectively designing and building applications with QB. Each page explores a critical aspect of the framework's architecture and programming model.

## Chapters in this Section:

*   **[The Actor Model in QB](./actor_model.md)**
    *   Learn how QB implements the Actor Model, with a focus on `qb::Actor` as the primary unit of concurrency and state, and `qb::ActorId` for unique identification.

*   **[The QB Event System](./event_system.md)**
    *   Discover how actors communicate exclusively through asynchronous messages (events derived from `qb::Event`), how to define events, and the basics of event handling.

*   **[Asynchronous I/O Model in QB](./async_io.md)**
    *   Explore QB's non-blocking, event-driven I/O model, powered by `qb-io`, including the role of the event loop (`qb::io::async::listener`) and asynchronous callbacks.

*   **[Concurrency & Parallelism in QB](./concurrency.md)**
    *   Understand how QB manages concurrent actor execution and achieves true parallelism on multi-core systems through `qb::Main` and `qb::VirtualCore`s.

## Next Steps

Once you are comfortable with these core concepts, you may want to explore:

*   The **[QB-IO Module Documentation](../3_qb_io/README.md)** for a detailed look at the asynchronous I/O library.
*   The **[QB-Core Module Documentation](../4_qb_core/README.md)** for specifics on the actor engine.
*   The **[Getting Started Guide](../6_guides/getting_started.md)** if you haven't already, to see these concepts in a practical example. 