# QB Framework Philosophy

The design and implementation of the QB Actor Framework are guided by several core principles aimed at enabling the development of efficient, scalable, and maintainable concurrent C++ applications.

## 1. The Actor Model

At its heart, QB embraces the Actor Model. This model simplifies concurrency by treating independent units of computation, called **actors**, as the fundamental building blocks.

*   **Concurrency via Actors:** Instead of managing threads and locks directly, concurrency arises from having many actors running potentially in parallel (on different cores) and interacting.
*   **State Encapsulation:** An actor's internal data (its state) is strictly private. No other actor can directly access or modify it. This is the key to avoiding data races and the need for mutexes to protect actor state.
*   **Asynchronous Message Passing:** Actors communicate *only* by sending immutable messages (called **events**, derived from `qb::Event`) to each other's unique `qb::ActorId`. Sending a message is non-blocking; the sender doesn't wait for the message to be processed or for a reply.
*   **Mailboxes & Sequential Processing:** Each actor has an implicit mailbox that queues incoming events. The actor processes these events one by one, in the order they were effectively received by its managing `VirtualCore`. This ensures that an actor's state is modified sequentially and predictably, preventing internal race conditions.

This approach leads to systems that are easier to reason about, more resilient to failures (errors are often contained within an actor), and inherently scalable.

**(See:** `[Core Concepts: Actor Model](./../2_core_concepts/actor_model.md)`, `[QB-Core: Actor](./../4_qb_core/actor.md)`, `[QB-Core: Messaging](./../4_qb_core/messaging.md)`**)**

## 2. Asynchronous I/O (`qb-io`)

Traditional blocking I/O (where a thread waits for an operation like reading a file or network socket to complete) is detrimental to high-concurrency systems. QB relies on its `qb-io` module for a fully asynchronous I/O model:

*   **Non-Blocking Operations:** Network and file operations are initiated without waiting for completion. The calling thread (typically a `VirtualCore`) immediately returns to process other work.
*   **Event Loop Integration:** `qb-io` uses an efficient event loop (`qb::io::async::listener`, based on `libev`) running on each `VirtualCore`. This loop monitors I/O resources (sockets, file descriptors) for readiness (e.g., data available to read, socket ready for writing, connection accepted).
*   **Callback/Event-Driven Notifications:** When an I/O operation is ready or completes, the event loop triggers a notification. This notification is delivered either as a direct callback (`qb::io::async::callback`) or, more commonly within the actor model, as a specific event (`qb::io::async::event::*`) handled by the relevant I/O component or actor (e.g., `on(event::disconnected&)`, `on(Protocol::message&)`).

This ensures that `VirtualCore` threads spend their time executing actor logic or processing events, maximizing throughput and responsiveness, instead of being idle waiting for I/O.

**(See:** `[Core Concepts: Asynchronous I/O](./../2_core_concepts/async_io.md)`, `[QB-IO: Async System](./../3_qb_io/async_system.md)`**)**

## 3. Performance and Efficiency

Performance is a key design goal:

*   **Multi-Core Scalability:** Actors are distributed across `VirtualCore` threads, enabling true parallelism. Core affinity can be controlled for optimal cache usage (`qb::Main`, `qb::CoreSet`).
*   **Efficient Messaging:** Inter-core communication utilizes high-performance, low-contention lock-free MPSC queues (`qb::lockfree::mpsc::ringbuffer`). Message serialization aims to minimize copying, especially with patterns like `reply`, `forward`, and passing large data via `shared_ptr`.
*   **Low-Overhead Abstractions:** C++ features like templates and CRTP (e.g., `qb::io::use<>`) are used to provide high-level abstractions with minimal runtime cost.
*   **Optimized Memory:** Efficient buffer reuse (`qb::allocator::pipe`) is employed for I/O and event data.

**(See:** `[Guides: Performance Tuning](./../6_guides/performance_tuning.md)`**)**

## 4. Modularity and Extensibility

*   **Layered Design:** `qb-io` provides the core async foundation and can be used independently. `qb-core` builds the actor layer on top.
*   **Extensible Protocols:** The `qb::io::async::AProtocol` interface allows defining custom network or data protocols.
*   **Clear Actor Interface:** `qb::Actor` provides virtual methods (`onInit`) and event handling (`on(Event&)`, `registerEvent`) as primary extension points.

## 5. Developer Productivity

While performance is key, QB also aims to simplify complex concurrent development:

*   **Abstraction:** Hides low-level details of threading, synchronization, and event loop management.
*   **Type Safety:** Utilizes C++ types and templates for compile-time checking of event types and handlers.
*   **Reduced Boilerplate:** Provides helper templates (`qb::io::use<>`) and base classes to quickly build common actor types (network clients/servers). 