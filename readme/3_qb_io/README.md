@page qb_io_readme_md QB-IO Module: High-Performance Asynchronous I/O & Utilities
@brief Discover `qb-io`, the powerful C++17 library underpinning QB's non-blocking I/O, also available for standalone use. Provides an overview and navigation for the QB-IO module documentation.

# QB-IO Module: High-Performance Asynchronous I/O & Utilities

Welcome to the `qb-io` module documentation. `qb-io` is the foundational C++17 library that powers the QB Actor Framework's responsive, non-blocking input/output capabilities. More than just a component of the actor system, `qb-io` is a comprehensive, cross-platform toolkit designed for building high-performance, event-driven applications. It can be used **effectively as a standalone library** for any project requiring efficient asynchronous I/O.

This section provides a detailed exploration of `qb-io`'s features, architecture, and usage.

## Key Topics in This Section:

*   **[QB-IO: Feature Showcase](./features.md)**
    *   A comprehensive rundown of the capabilities offered by the `qb-io` library, from its core asynchronous engine to networking, protocol handling, file system operations, and essential utilities like crypto and compression.

*   **[QB-IO: The Asynchronous Engine (`qb::io::async`)](./async_system.md)**
    *   A deep dive into `qb-io`'s event loop (`listener`), timers, asynchronous callbacks, and event management framework, crucial for standalone asynchronous programming and for understanding the base of `qb-core`.

*   **[QB-IO: Understanding Transports](./transports.md)**
    *   Explore how `qb-io`'s transport layer bridges abstract streams with concrete I/O mechanisms like TCP, UDP, SSL/TLS, and local files.

*   **[QB-IO: Framing Messages with Protocols](./protocols.md)**
    *   Learn how `qb-io` uses protocols (both built-in and custom via `AProtocol`) to define message boundaries and parse data from byte streams for various transports.

*   **[QB-IO: Secure TCP with SSL/TLS](./ssl_transport.md)**
    *   Detailed information on enabling and using SSL/TLS for secure, encrypted TCP communication within `qb-io`, leveraging OpenSSL.

*   **[QB-IO: Essential Utilities & Helpers](./utilities.md)**
    *   Discover the rich set of utility classes and functions in `qb-io` for common system programming tasks, including URI parsing, cryptography, compression, high-precision time, optimized containers, and system information.

## How to Use This Section

*   Start with the **Feature Showcase** for a broad overview.
*   Dive into **The Asynchronous Engine** to understand the fundamentals of `qb-io`'s event-driven model.
*   Explore **Transports** and **Protocols** to learn how to handle specific types of I/O and data framing.
*   Refer to **Secure TCP** and **Utilities** for specialized needs.

By understanding these components, you can leverage `qb-io` to build highly concurrent and performant C++ applications, either as part of the full QB Actor Framework or as a powerful standalone I/O solution.

**(Next:** After exploring `qb-io`, you might want to see how it integrates with the actor model in the `[QB-Core Module Overview](../4_qb_core/README.md)` or review the `[Core & IO Integration Overview](../5_core_io_integration/README.md)`.)** 