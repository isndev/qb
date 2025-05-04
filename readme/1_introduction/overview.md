# QB Framework Overview

## Introduction

The QB Actor Framework is a C++17 library designed for building **high-performance, concurrent, and distributed systems**. It integrates two core ideas:

1.  **The Actor Model:** For structuring concurrent logic using isolated entities (actors) that communicate via asynchronous messages.
2.  **Asynchronous I/O (`qb-io`):** A robust, non-blocking I/O layer for efficient network and file operations.

By combining these, QB aims to simplify the creation of complex, scalable C++ applications, handling low-level concurrency and I/O details internally.

## Target Audience

This documentation assumes you are an **experienced C++ developer** with a good understanding of:

*   Modern C++ features (C++17 or later).
*   Concurrency principles (threading, race conditions, deadlocks, asynchronous patterns).
*   Network programming fundamentals (TCP/IP, UDP, sockets).
*   Build systems, specifically CMake.

The Actor Model itself simplifies certain concurrency aspects, but leveraging the framework effectively benefits from understanding the underlying system concepts.

## Key Features

*   **Actor Model Engine (`qb-core`):**
    *   `qb::Actor` base class for stateful, message-driven components.
    *   Managed actor lifecycle (`onInit`, `kill`, destructors).
    *   Type-safe, asynchronous event system (`qb::Event`, `push`, `send`, `broadcast`).
*   **Asynchronous I/O Engine (`qb-io`):**
    *   Efficient event loop based on `libev` (`qb::io::async::listener`).
    *   Non-blocking TCP, UDP, File I/O, and optional SSL/TLS transports.
    *   Extensible protocol framework for message framing/parsing (`qb::io::async::AProtocol`).
    *   Asynchronous timers and callbacks (`qb::io::async::with_timeout`, `qb::io::async::callback`).
*   **Concurrency & Scalability:**
    *   Multi-core execution via `qb::Main` and `qb::VirtualCore`.
    *   Efficient inter-core communication using lock-free queues.
    *   Configurable core affinity and event loop latency.
*   **Utilities (`qb-io`):**
    *   URI parsing (`qb::io::uri`).
    *   Cryptography (Hashing, Encryption - requires OpenSSL).
    *   Compression (Gzip, Deflate - requires Zlib).
    *   High-precision time (`qb::Timestamp`, `qb::Duration`).
    *   High-performance containers (`qb::allocator::pipe`, `qb::string`, `qb::unordered_map`).

## Benefits

*   **Simplified Concurrency:** Reduces bugs related to shared state and locking.
*   **High Performance:** Built for speed using non-blocking I/O and efficient messaging.
*   **Scalability:** Leverages multiple CPU cores effectively.
*   **Modularity:** `qb-io` can be used independently of `qb-core`.
*   **Testability:** Isolated actors are often easier to unit test.

## Next Steps

*   Understand the **[Core Philosophy](./philosophy.md)** guiding the framework's design.
*   Explore the **[Core Concepts](../2_core_concepts/)**.
*   Dive into the **[QB-IO Module](../3_qb_io/)** or **[QB-Core Module](../4_qb_core/)** documentation.
*   Follow the **[Getting Started Guide](../6_guides/getting_started.md)**. 