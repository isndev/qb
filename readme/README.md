@page readme_main QB Actor Framework README  
@brief Main README for the QB Actor Framework, providing an overview and navigation.

# QB Actor Framework

**High-Performance C++17 Actor Framework for Concurrent & Distributed Systems**

## Overview

The QB Actor Framework is a modern C++17 library designed for building high-performance, scalable, and robust concurrent and distributed applications. It leverages the **Actor Model** paradigm and an efficient **Asynchronous I/O** foundation (`qb-io`) to simplify the development of complex systems.

QB provides developers with tools to manage concurrency through isolated actors and asynchronous message passing, while handling low-level I/O and system details efficiently.

**Target Audience:** Experienced C++ developers familiar with concurrency concepts, asynchronous programming, and system design.

## Core Philosophy

QB is built upon principles designed for performance and maintainability in concurrent environments:

1.  **Actor Model:** State encapsulation and asynchronous message passing as the primary means of interaction, reducing complexity associated with shared state and locking. ([Read More](./1_introduction/philosophy.md#1-the-actor-model))
2.  **Asynchronous I/O:** Non-blocking, event-driven I/O for high throughput and responsiveness, managed by the `qb-io` layer. ([Read More](./1_introduction/philosophy.md#2-asynchronous-io-qb-io))
3.  **Performance:** Emphasis on multi-core scalability, efficient messaging (including lock-free mechanisms), and low-overhead abstractions. ([Read More](./1_introduction/philosophy.md#3-performance-and-efficiency))

## High-Level Architecture

The framework consists of two main libraries:

*   **`qb-io`:** The foundational asynchronous I/O and utilities library. It can be used standalone.
*   **`qb-core`:** The actor engine, built upon `qb-io`, providing the actor implementation, scheduling, and messaging.

```
+---------------------+      +----------------------+
|    Your Application |      |   Framework Examples |
+----------^----------+      +----------^-----------+
           |                         |
           |  Uses                   |  Uses
           v                         v
+-----------------------------------------------------+
|                     qb-core                         |
|    (qb::Actor, qb::Main, qb::VirtualCore, qb::Event)|
+--------------------------^--------------------------+
                           |
                           | Depends on / Integrates with
                           v
+-----------------------------------------------------+
|                      qb-io                          |
| (async::io, transports, protocols, crypto, utils)   |
+-----------------------------------------------------+
                           |
                           | Uses / Abstracts
                           v
+-----------------------------------------------------+
|          System (libev, Sockets, Files, OS)         |
+-----------------------------------------------------+
```

## Documentation Navigation

This documentation provides a comprehensive guide to the QB framework:

1.  **[Introduction](./1_introduction/)**: Detailed overview and core design philosophy.
2.  **[Core Concepts](./2_core_concepts/)**: Fundamental ideas behind the framework (Actors, Events, Async IO, Concurrency).
3.  **[QB-IO Module](./3_qb_io/)**: Deep dive into the asynchronous I/O library.
4.  **[QB-Core Module](./4_qb_core/)**: Detailed exploration of the actor engine.
5.  **[Core & IO Integration](./5_core_io_integration/)**: How actors utilize `qb-io` features, with example analysis.
6.  **[Guides](./6_guides/)**: Practical tutorials and pattern implementations.
7.  **[Reference](./7_reference/)**: Build system, testing procedures, and API summaries.

**New Users:** Start with the **[Getting Started Guide](./6_guides/getting_started.md)**.

## License

QB Actor Framework is licensed under the Apache License, Version 2.0. 