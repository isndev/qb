@page intro_philosophy_md QB Framework Philosophy: Building Modern C++ Systems
@brief Delve into the foundational design principles of the QB Actor Framework, crafted for efficient, scalable, and maintainable concurrent C++ applications.

# QB Framework Philosophy: Building Modern C++ Systems

The QB Actor Framework isn't just a collection of tools; it's built on a set of core philosophies designed to tackle the inherent complexities of concurrent C++ development. Understanding these principles will help you leverage QB to its fullest potential.

## 1. Actors: Your Concurrency Primitives

At the very heart of QB lies the **Actor Model**. This isn't just a feature; it's the primary way QB approaches concurrency. Instead of juggling raw threads, mutexes, and condition variables, you build your system from **actors** – self-contained, independent units of computation.

*   **Simplified Concurrency:** Actors interact by exchanging asynchronous messages. This eliminates direct memory sharing between concurrent tasks, drastically reducing the risk of data races and deadlocks that plague traditional threaded programming.
*   **Stateful, Isolated Units:** Each actor manages its own internal state, which is *never* directly accessed from the outside. This encapsulation is key to building robust and predictable components.
*   **Sequential Processing per Actor:** An actor processes messages from its dedicated mailbox one at a time. This means you can write an actor's internal logic as if it were single-threaded, without worrying about internal race conditions on its own state.

**Why Actors?** They lead to systems that are easier to reason about, more resilient (errors are often contained within an actor), and inherently scalable by distributing actors across available cores.

**(Explore Further:** `[Core Concepts: The Actor Model in QB](./../2_core_concepts/actor_model.md)`, `[QB-Core: Actor (`qb::Actor`)](./../4_qb_core/actor.md)`, `[QB-Core: Event Messaging](./../4_qb_core/messaging.md)`**)**

## 2. Asynchronous I/O: The Key to Responsiveness (`qb-io`)

High-performance systems cannot afford to wait. Blocking I/O operations (where a thread halts, waiting for a network packet or disk read) are a major bottleneck. QB's `qb-io` module is built from the ground up for **asynchronous, non-blocking I/O**.

*   **Non-Blocking Everywhere:** Whether it's TCP, UDP, or file operations, QB initiates them without stalling the calling thread. The thread is immediately free to process other work, typically other actor messages.
*   **Event-Driven Notifications:** `qb-io` integrates with the operating system's most efficient event notification mechanisms (like epoll on Linux or kqueue on macOS, via `libev`). When an I/O operation is ready (e.g., data arrives, a socket can be written to), the event loop is notified.
*   **Seamless Integration:** This event loop (`qb::io::async::listener`) is run by each `VirtualCore`. I/O readiness events are seamlessly translated into messages for your actors or trigger asynchronous callbacks, ensuring your application remains highly responsive.

**Why Async I/O?** It ensures your CPU cores are always busy doing useful work—processing actor logic or handling events—rather than idling, leading to superior throughput and lower latency under load.

**(Explore Further:** `[Core Concepts: Asynchronous I/O Model](./../2_core_concepts/async_io.md)`, `[QB-IO: Async System (`qb::io::async`)](./../3_qb_io/async_system.md)`**)**

## 3. Performance & Efficiency: Built for Speed

QB is engineered for applications where performance is paramount.

*   **Multi-Core Scalability:** Actors are naturally distributed across `VirtualCore` threads, enabling true parallel processing. Fine-grained control over core affinity (`qb::Main`, `qb::CoreSet`) allows for optimizing cache utilization.
*   **Optimized Messaging:** Inter-actor communication, especially across cores, uses high-performance, low-contention lock-free MPSC (Multiple-Producer, Single-Consumer) queues. Message serialization is designed to minimize copying (e.g., via `reply`, `forward`, and `std::shared_ptr` for large data).
*   **Low-Overhead Abstractions:** Modern C++ techniques like templates and CRTP (Curiously Recurring Template Pattern, seen in `qb::io::use<>`) are employed to offer high-level, developer-friendly abstractions with minimal runtime cost.
*   **Efficient Memory Management:** Utilities like `qb::allocator::pipe` provide efficient, resizable buffers crucial for I/O and event data, reducing memory fragmentation and allocation overhead.

**Why This Focus?** For many applications, especially in areas like finance, gaming, or real-time data processing, minimizing latency and maximizing throughput are critical requirements. QB provides the tools to achieve this.

**(Explore Further:** `[Guides: Performance Tuning Guide](./../6_guides/performance_tuning.md)`**)**

## 4. Modularity & Extensibility: Flexible by Design

QB is not a monolithic black box. It's designed to be adaptable.

*   **Layered Architecture:** The `qb-io` library provides the foundational asynchronous I/O and utilities. It can be used entirely independently of the `qb-core` actor system if your needs are purely I/O-focused.
*   **Customizable Protocols:** The `qb::io::async::AProtocol` interface allows you to define precisely how your application's data is framed and parsed over network connections.
*   **Clear Actor Interface:** `qb::Actor` itself provides clear extension points through virtual methods like `onInit()` and the event handling mechanism (`on(EventType&)`, `registerEvent<EventType>()`).

**Why Modularity?** It allows QB to be integrated into diverse projects and enables developers to replace or extend parts of the framework to suit specific needs.

## 5. Developer Productivity: Simplifying Complexity

While prioritizing performance and control, QB also aims to make the developer's life easier when tackling complex concurrent systems.

*   **High-Level Abstractions:** The actor model itself abstracts away the raw complexities of threads, locks, and manual synchronization.
*   **Type Safety:** C++'s strong type system is leveraged, especially in event handling, to catch errors at compile time.
*   **Reduced Boilerplate:** Utility templates like `qb::io::use<>` help quickly set up common actor types (e.g., network clients, servers) with appropriate I/O capabilities, reducing repetitive code.

**Why Productivity?** Building correct and maintainable concurrent systems is challenging. QB provides a structured approach that reduces common pitfalls and allows developers to concentrate more on application logic and less on low-level plumbing. 