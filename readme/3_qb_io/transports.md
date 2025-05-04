# QB-IO: Transports

Transports bridge the gap between the abstract stream interfaces (`istream`/`ostream`/`stream`) and the low-level system I/O primitives (`qb::io::socket`, `qb::io::sys::file`). They handle the specifics of reading from and writing to different types of I/O resources.

## Core Stream Abstractions

(`qb/include/qb/io/stream.h`)

Before diving into transports, recall the base stream classes:

*   **`qb::io::istream<IO_>`:** Manages buffered input via `_in_buffer` (`qb::allocator::pipe`). Provides `read()` to fill the buffer from the underlying `_IO_` transport and `flush(size)` to consume data from the buffer.
*   **`qb::io::ostream<IO_>`:** Manages buffered output via `_out_buffer`. Provides `publish(data, size)` to add data to the buffer and `write()` to send buffer contents to the underlying `_IO_` transport.
*   **`qb::io::stream<IO_>`:** Combines `istream` and `ostream` for bidirectional I/O, using a single underlying `_IO_` object (typically referred to as `_in` within the class).

## Available Transports

### 1. TCP Transport (`qb::io::transport::tcp`)

*   **Header:** `qb/include/qb/io/transport/tcp.h`
*   **Underlying IO:** `qb::io::tcp::socket`
*   **Base Class:** `qb::io::stream<qb::io::tcp::socket>`
*   **Purpose:** Standard, reliable stream-based communication over TCP/IP (v4/v6) and Unix Domain Sockets.
*   **How to Use:** This is the foundation for asynchronous TCP clients and server sessions. You typically don't use `transport::tcp` directly but rather inherit from `qb::io::use<...>::tcp::client` or let `qb::io::use<...>::tcp::server` manage sessions based on it.
    *   Its `read()` calls `tcp::socket::read()`.
    *   Its `write()` calls `tcp::socket::write()`.

**(Ref:** `test-io.cpp` (TCP tests), `example3_tcp_networking.cpp`, `chat_tcp` example**)

### 2. UDP Transport (`qb::io::transport::udp`)

*   **Header:** `qb/include/qb/io/transport/udp.h`
*   **Underlying IO:** `qb::io::udp::socket`
*   **Base Class:** `qb::io::stream<qb::io::udp::socket>`
*   **Purpose:** Connectionless, datagram-based communication over UDP/IP (v4/v6) and Unix Domain Sockets.
*   **Key Differences & Usage:**
    *   **Endpoint Handling:** UDP requires managing source/destination endpoints for each datagram.
        *   `getSource() const -> const udp::identity&`: Returns the endpoint (`qb::io::endpoint`) of the sender of the *last received* datagram.
        *   `setDestination(const udp::identity& to)`: Sets the default destination endpoint for subsequent writes using the `out()` buffer proxy.
        *   `publish_to(const udp::identity& to, const char* data, size_t size)`: Enqueues data specifically targeted at the given endpoint `to`.
    *   **Datagram Operations:**
        *   `read()`: Reads a *single* datagram into the input buffer and sets the source (`_remote_source`) for potential replies.
        *   `write()`: Sends the *next complete message* from the output buffer to its associated destination endpoint.
    *   **Output Buffering:** Manages messages in the output buffer using an internal `pushed_message` struct to track destination and size for each queued datagram.
    *   **How to Use:** Typically used as a base for `qb::io::use<...>::udp::client` or `::server`.

**(Ref:** `test-io.cpp` (UDP tests), `example4_udp_networking.cpp`**)

### 3. File Transport (`qb::io::transport::file`)

*   **Header:** `qb/include/qb/io/transport/file.h`
*   **Underlying IO:** `qb::io::sys::file`
*   **Base Class:** `qb::io::stream<qb::io::sys::file>`
*   **Purpose:** Stream-based access to local files.
*   **How to Use:** Can be used directly for buffered file access or as a base for file processing components (e.g., in conjunction with `async::file_watcher`).
    *   Its `read()` calls `sys::file::read()`.
    *   Its `write()` (as implemented in `file`) is a no-op; writing is typically done via `publish()` followed by application-level logic to flush the buffer or implicitly via the stream destructor.

**(Ref:** `test-file-operations.cpp`, `test-stream-operations.cpp`, `file_monitor` example**)

### 4. Secure TCP (SSL/TLS) Transport (`qb::io::transport::stcp`)

*   **Header:** `qb/include/qb/io/transport/stcp.h`
*   **Underlying IO:** `qb::io::tcp::ssl::socket`
*   **Base Class:** `qb::io::stream<qb::io::tcp::ssl::socket>`
*   **Requires:** `QB_IO_WITH_SSL=ON`, OpenSSL library.
*   **Purpose:** Provides encrypted stream-based communication over TCP using SSL/TLS.
*   **How to Use:** Used similarly to `transport::tcp`, serving as the foundation for `qb::io::use<...>::tcp::ssl::client` and server sessions.
*   **Key Differences & Usage:**
    *   Underlying `ssl::socket` handles the SSL handshake and encryption/decryption.
    *   The `read()` method in `transport::stcp` is overridden to specifically handle potential buffering within the OpenSSL `SSL` object. After reading from the socket, it calls `SSL_pending()` and performs an additional read if necessary to ensure all decrypted data is retrieved from OpenSSL's internal buffers.

**(Ref:** `test-async-io.cpp` (SSL test), `test-session-text.cpp` (Secure test), `[SSL Transport Details](./ssl_transport.md)`**) 