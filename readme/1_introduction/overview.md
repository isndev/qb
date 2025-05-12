@page intro_overview_md QB Framework: A High-Performance C++ Actor Toolkit
@brief Discover the QB Actor Framework – your C++17 toolkit for crafting powerful, concurrent, and distributed applications with ease and efficiency.

# QB Framework: A High-Performance C++ Actor Toolkit

Welcome to the QB Actor Framework! If you're looking to build sophisticated, high-performance concurrent or distributed systems in C++17, you're in the right place. QB is engineered to simplify the complexities of modern C++ development by elegantly integrating two powerful paradigms:

1.  **The Actor Model:** Structure your concurrent logic around actors – independent, isolated entities that communicate through asynchronous messages. This model naturally manages state and simplifies reasoning about concurrency.
2.  **Asynchronous I/O (`qb-io`):** Leverage a robust, non-blocking I/O engine for highly efficient network and file operations, ensuring your application remains responsive under load.

QB empowers you to focus on your application's business logic while it handles the intricate details of concurrency, parallelism, and low-level I/O.

## Who Is This For?

This documentation is tailored for **skilled C++ developers**. We assume you have a solid grasp of:

*   Modern C++ (C++17 and beyond).
*   Fundamental concurrency concepts (threads, asynchronous operations, potential pitfalls like race conditions).
*   Basic network programming (TCP/IP, UDP).
*   CMake for building projects.

While the Actor Model simplifies many concurrency challenges, a foundational understanding of these areas will help you unlock QB's full potential.

## What Can QB Do For You? Key Capabilities

QB offers a rich set of features designed for demanding applications:

*   **Core Actor Engine (`qb-core`):**
    *   **`qb::Actor`:** The cornerstone for building your message-driven components.
    *   **Managed Lifecycles:** Simplified actor creation, initialization (`onInit`), and termination (`kill`).
    *   **Type-Safe Messaging:** Asynchronous, type-safe event system using `qb::Event` for reliable communication (`push`, `send`, `broadcast`).
*   **High-Performance Async I/O (`qb-io`):**
    *   **`qb::io::async::listener`:** An efficient event loop, powered by `libev`, at the heart of each processing core.
    *   **Versatile Transports:** Non-blocking TCP, UDP, File I/O, and optional SSL/TLS.
    *   **Customizable Protocols:** An extensible framework (`qb::io::async::AProtocol`) to parse and frame your specific message formats.
    *   **Timers & Callbacks:** Schedule tasks and manage time-based events with `qb::io::async::with_timeout` and `qb::io::async::callback`.
*   **Built for Concurrency & Scale:**
    *   **Multi-Core Ready:** Effortlessly distribute actors across `qb::VirtualCore` instances using `qb::Main`.
    *   **Efficient Inter-Core Messaging:** Optimized with lock-free queues.
    *   **Fine-Grained Control:** Configure CPU core affinity and event loop latency.
*   **Comprehensive Utilities (within `qb-io`):**
    *   **`qb::io::uri`:** Robust URI parsing.
    *   **Cryptography:** Hashing, encryption, and more (requires OpenSSL).
    *   **Compression:** Gzip and Deflate (requires Zlib).
    *   **`qb::Timestamp` & `qb::Duration`:** Precise time measurement.
    *   **Optimized Containers:** Performance-focused data structures like `qb::allocator::pipe`, `qb::string`, and `qb::unordered_map`.

## Why Choose QB?

*   **Simplified Concurrency:** Write more robust concurrent code by minimizing shared state and complex locking.
*   **Peak Performance:** Engineered for speed with non-blocking I/O, efficient messaging, and minimal overhead.
*   **Scalable by Design:** Naturally leverages multi-core architectures.
*   **Modular Architecture:** Use the `qb-io` library independently if the full actor system isn't needed.
*   **Enhanced Testability:** Isolated actor components are inherently easier to unit test.

## Dive Deeper

Ready to explore further?

*   Understand the **[Core Philosophy](./philosophy.md)** that shapes the framework.
*   Grasp the **[Fundamental Core Concepts](../2_core_concepts/README.md)**.
*   Explore the capabilities of the **[QB-IO Module](../3_qb_io/README.md)** and the **[QB-Core Module](../4_qb_core/README.md)**.
*   Walk through the **[Getting Started Guide](../6_guides/getting_started.md)** to build your first QB application. 