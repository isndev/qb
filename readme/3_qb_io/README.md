# QB-IO Module Overview

`qb-io` is the **foundational asynchronous Input/Output library** for the QB Actor Framework. Designed for C++17, it provides a robust, cross-platform, and efficient layer for handling network and file system operations without blocking threads. It can also be used as a **standalone library** independent of the `qb-core` actor engine.

## Purpose & Design Goals

*   **Asynchronous Operations:** Eliminate blocking I/O calls to maximize thread utilization and application responsiveness.
*   **High Performance:** Leverage efficient event notification systems (like epoll, kqueue via libev) and minimize data copying.
*   **Cross-Platform:** Provide a consistent API across Linux, Windows, and macOS.
*   **Modularity:** Offer distinct components for different I/O types (TCP, UDP, File, SSL) and utilities.
*   **Extensibility:** Allow developers to implement custom application-level protocols.

## Key Abstractions

*   **Async System (`qb::io::async`):** Manages the event loop (`listener`), event types (`event::*`), timers (`with_timeout`), and callbacks (`callback`). This is the core engine for asynchronous behavior.
*   **Stream (`qb::io::stream`, `istream`, `ostream`):** Base templates for buffered I/O, providing `in()` and `out()` buffers (`qb::allocator::pipe`) and common operations (`read`, `write`, `flush`, `close`).
*   **Transport (`qb::io::transport::*`):** Concrete implementations built on top of `stream` templates and system wrappers, handling the specifics of TCP, UDP, File, or SSL I/O.
*   **Protocol (`qb::io::async::AProtocol`):** Interface for defining how byte streams are framed and parsed into meaningful application messages.
*   **System Wrappers (`qb::io::socket`, `qb::io::sys::file`):** Low-level, cross-platform wrappers for native sockets and file descriptors.
*   **URI & Endpoint (`qb::io::uri`, `qb::io::endpoint`):** Utilities for parsing URIs and representing network addresses.

## Relationship with `qb-core`

`qb-core` relies entirely on `qb-io` for its asynchronous foundation:

*   Each `qb::VirtualCore` runs a `qb::io::async::listener`.
*   Actors needing network or file capabilities inherit from `qb::io::use<>` templates, which integrate `qb-io` components.
*   Timers and delayed actions within actors use `qb::io::async::callback`.

## Standalone Usage

To use `qb-io` without `qb-core`:

1.  Include `<qb/io.h>` and specific headers as needed (e.g., `<qb/io/async.h>`, `<qb/io/tcp/socket.h>`).
2.  Initialize the async system on your thread(s): `qb::io::async::init();`
3.  Create and configure transport objects (e.g., `qb::io::tcp::socket`, `qb::io::tcp::listener`).
4.  If needed, implement or use existing protocols (`AProtocol`).
5.  Integrate components with the event loop (often done via base classes like `qb::io::async::io` or `qb::io::async::with_timeout`).
6.  **Drive the event loop:** Explicitly call `qb::io::async::run()` in your application's main loop or worker threads.

**(See also:** `[Async System](./async_system.md)`, `[Transports](./transports.md)`, `[Protocols](./protocols.md)`, `[Utilities](./utilities.md)`**)** 