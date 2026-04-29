@page qb_io_readme_md QB-IO Module: High-Performance Asynchronous I/O & Utilities
@brief Discover `qb-io`, the powerful C++23 library underpinning QB's non-blocking I/O, also available for standalone use. Provides an overview and navigation for the QB-IO module documentation.

# QB-IO Module: High-Performance Asynchronous I/O & Utilities

Welcome to the `qb-io` module documentation. `qb-io` is the foundational **C++23** library that powers the QB Actor Framework's responsive, non-blocking input/output capabilities. More than just a component of the actor system, `qb-io` is a comprehensive, cross-platform toolkit designed for building high-performance, event-driven applications. It can be used **effectively as a standalone library** for any project requiring efficient asynchronous I/O.

`qb-io` offers two complementary async programming models:

| Model | When to use |
|---|---|
| **Event-driven callbacks** (`on()` handlers) | Server sessions, protocol parsers, low-latency paths |
| **C++23 coroutines** (`co_await`) | Sequential async flows, scatter-gather, retry logic |

Both models share the same `listener`/libev event loop and are fully interoperable.

## Key Topics in This Section:

*   **[QB-IO: Feature Showcase](./features.md)**
    *   A comprehensive rundown of all capabilities: async engine, networking, protocols, file system, coroutines, crypto, compression, and utilities.

*   **[QB-IO: The Asynchronous Engine (`qb::io::async`)](./async_system.md)**
    *   Deep dive into `qb-io`'s event loop (`listener`), timers, callbacks, and event types — the foundation shared by all async components.

*   **[QB-IO: C++23 Coroutines](./coroutines.md)**
    *   Complete guide to `task<T>`, awaiters, combinators (`when_all`, `when_any`, `race`), cancellation, channels, structured concurrency (`coroutine_scope`), generators, async streams, retry policies, and safe Actor integration patterns.

*   **[QB-IO: Understanding Transports](./transports.md)**
    *   How `qb-io`'s transport layer bridges buffered streams with TCP, UDP, SSL/TLS, and files.

*   **[QB-IO: Framing Messages with Protocols](./protocols.md)**
    *   Built-in and custom `AProtocol` implementations for defining message boundaries in byte streams.

*   **[QB-IO: Secure TCP with SSL/TLS](./ssl_transport.md)**
    *   Enabling encrypted TCP communication with OpenSSL (`QB_IO_WITH_SSL`).

*   **[QB-IO: Native QUIC](./quic_transport.md)**
    *   Optional QUIC endpoints, connection-id routing, stream events, flow-control hooks, and the QB threading model.

*   **[QB-IO: Essential Utilities & Helpers](./utilities.md)**
    *   URI parsing, cryptography, compression, high-precision time, containers, lock-free primitives, and UUID generation.

*   **[QB-IO: Async, Lifecycle & Allocation Invariants](../7_reference/io_invariants.md)**
    *   The single source of truth for thread-ownership, freelist, and CRTP-dispatch invariants. Required reading before writing a custom protocol, transport, or async component.

## Recommended Reading Order

1.  **[Feature Showcase](./features.md)** — understand what's available at a glance.
2.  **[Asynchronous Engine](./async_system.md)** — understand the event loop fundamentals.
3.  **[C++23 Coroutines](./coroutines.md)** *(new)* — if you prefer `co_await`-style code.
4.  **[Transports](./transports.md)** → **[Protocols](./protocols.md)** — for network or file I/O.
5.  **[SSL/TLS](./ssl_transport.md)** → **[Native QUIC](./quic_transport.md)** — for encrypted TCP and QUIC transports.
6.  **[Utilities](./utilities.md)** — for specialized needs.
7.  **[Invariants reference](../7_reference/io_invariants.md)** — before extending the stack.

**(Next:** [QB-Core Module Overview](../4_qb_core/README.md) | [Core & IO Integration](../5_core_io_integration/README.md))**
