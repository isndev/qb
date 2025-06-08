@page qb_io_transports_md QB-IO: Understanding Transports
@brief Explore `qb-io`'s transport layer, which bridges abstract streams with concrete I/O mechanisms like TCP, UDP, SSL, and files.

# QB-IO: Understanding Transports

In the `qb-io` library, **transports** are the crucial components that connect the high-level, buffered stream abstractions (`qb::io::istream`, `ostream`, `stream`) to the low-level, platform-specific I/O primitives (`qb::io::socket`, `qb::io::sys::file`). They handle the actual reading from and writing to different types of I/O resources, making the stream interface work seamlessly across various communication methods.

## The Role of Stream Abstractions

Before diving into specific transports, let's recap the core stream classes they build upon (defined in `qb/io/stream.h`):

*   **`qb::io::istream<IO_Type>`:** Manages an input buffer (`qb::allocator::pipe`) and provides a `read()` method to populate this buffer from an underlying `IO_Type` object. Also offers `flush(size)` to consume data from the buffer.
*   **`qb::io::ostream<IO_Type>`:** Manages an output buffer (`qb::allocator::pipe`). It provides `publish(data, size)` (or `operator<<`) to add data to the buffer and a `write()` method to send the buffer's contents using the underlying `IO_Type`.
*   **`qb::io::stream<IO_Type>`:** Combines `istream` and `ostream` capabilities for bidirectional communication, using a single `IO_Type` object for both input and output.

Each transport specializes one of these stream templates with a concrete `IO_Type`.

## Available `qb-io` Transports

### 1. TCP Transport: `qb::io::transport::tcp`

*   **Header:** `qb/io/transport/tcp.h`
*   **Underlying I/O Primitive:** `qb::io::tcp::socket` (which itself wraps a system socket descriptor for TCP/IP v4/v6 or Unix Domain Sockets).
*   **Base Class:** `qb::io::stream<qb::io::tcp::socket>`
*   **Purpose:** Provides reliable, ordered, stream-based communication. This is the workhorse for most client-server network applications.
*   **Key Operations:** Its `read()` method calls `qb::io::tcp::socket::read()`, and its `write()` method calls `qb::io::tcp::socket::write()`.
*   **Typical Usage:** Forms the foundation for asynchronous TCP clients and server-side client session handlers, often used via `qb::io::use<...>::tcp::client` or managed by `qb::io::use<...>::tcp::server`.

**(Reference:** See `test-io.cpp` for TCP socket tests, `example3_tcp_networking.cpp`, and the `chat_tcp` example for practical application.**)

### 2. UDP Transport: `qb::io::transport::udp`

*   **Header:** `qb/io/transport/udp.h`
*   **Underlying I/O Primitive:** `qb::io::udp::socket` (wrapping a system socket descriptor for UDP/IP v4/v6 or datagram-style Unix Domain Sockets).
*   **Base Class:** `qb::io::stream<qb::io::udp::socket>`
*   **Purpose:** Enables connectionless, datagram-based (message-oriented) communication.
*   **Key Features & Usage:**
    *   **Endpoint Management:** Essential for UDP, as each datagram can have a different source or destination.
        *   `getSource() const -> const udp::identity&`: Returns the `qb::io::endpoint` of the sender of the *last successfully received* datagram.
        *   `setDestination(const udp::identity& to)`: Sets the default remote endpoint for data sent via the `out()` proxy stream.
        *   `publish_to(const udp::identity& to, const char* data, size_t size)`: Enqueues data specifically targeted at the given endpoint `to` as a distinct datagram.
    *   **Datagram-Oriented I/O:**
        *   `read()`: Attempts to read a *single, complete* datagram into the input buffer and updates `getSource()`.
        *   `write()`: Attempts to send the *next complete datagram* from the output buffer to its designated recipient.
    *   **Output Buffering:** Manages an output buffer where each datagram is queued with its destination `udp::identity`.
*   **Typical Usage:** Used as the base for `qb::io::use<...>::udp::client` and `qb::io::use<...>::udp::server` components.

**(Reference:** Consult `test-io.cpp` for UDP socket tests and `example4_udp_networking.cpp`.**)

### 3. File Transport: `qb::io::transport::file`

*   **Header:** `qb/io/transport/file.h`
*   **Underlying I/O Primitive:** `qb::io::sys::file` (a cross-platform wrapper for native file descriptors/handles).
*   **Base Class:** `qb::io::stream<qb::io::sys::file>`
*   **Purpose:** Provides stream-based access to local files for buffered reading and writing.
*   **Key Operations:** Its `read()` method calls `qb::io::sys::file::read()`. The `write()` method inherited from `stream` (which calls `_in.write()`) is typically used to write data buffered via `publish()` or `operator<<` to the file.
*   **Typical Usage:** Can be used directly for synchronous buffered file I/O. In asynchronous contexts, it might be paired with `qb::io::async::file_watcher` within a file processing actor.

**(Reference:** See `test-file-operations.cpp`, `test-stream-operations.cpp`, and the `file_monitor` example.**)

### 4. Secure TCP (SSL/TLS) Transport: `qb::io::transport::stcp`

*   **Header:** `qb/io/transport/stcp.h`
*   **Underlying I/O Primitive:** `qb::io::tcp::ssl::socket` (which layers SSL/TLS encryption via OpenSSL on top of a `qb::io::tcp::socket`).
*   **Base Class:** `qb::io::stream<qb::io::tcp::ssl::socket>`
*   **Prerequisites:** Requires `QB_IO_WITH_SSL=ON` during CMake configuration and the OpenSSL library linked to your application.
*   **Purpose:** Facilitates reliable, ordered, stream-based communication over TCP/IP, with the added security of SSL/TLS encryption.
*   **Key Operations & Behavior:**
    *   The underlying `ssl::socket` manages the SSL handshake process and transparently encrypts outgoing data and decrypts incoming data.
    *   The `read()` method in `transport::stcp` is specifically overridden. After reading from the underlying socket, it calls `SSL_pending()` to check if OpenSSL has buffered any additional decrypted bytes. If so, it performs further reads to retrieve this pending data, ensuring all application-level data is promptly available.
*   **Typical Usage:** This is the transport used by `qb::io::use<...>::tcp::ssl::client` and server-side SSL session handlers.

**(Reference:** Explore `test-async-io.cpp` (SSL test), `test-session-text.cpp` (Secure test), and the detailed [QB-IO: Secure TCP (SSL/TLS) Transport](./ssl_transport.md) page.**)

By providing these specialized transports, `qb-io` offers a flexible and consistent way to handle diverse I/O requirements while abstracting away many platform-specific and protocol-specific details.

**(Next:** [QB-IO: Protocols](./protocols.md) to learn how data streams from these transports are interpreted as messages.**) 