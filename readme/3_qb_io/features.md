@page qb_io_features_md QB-IO: Feature Showcase
@brief A comprehensive rundown of the capabilities offered by the `qb-io` library for high-performance asynchronous C++ development.

# QB-IO: Feature Showcase

The `qb-io` library is a versatile and powerful C++17 toolkit designed for building applications that demand high-performance asynchronous I/O and robust system utilities. Here's a look at its key feature areas:

## 1. Core Asynchronous System (`qb::io::async`)

*   **Event Loop (`listener`):** At its core, `qb-io` provides a high-performance event loop (leveraging `libev`) for managing all asynchronous operations on a per-thread basis.
*   **Delayed Callbacks (`callback`):** Easily schedule functions or lambdas for asynchronous execution after a specified delay, or for the next event loop iteration.
*   **Integrated Timers (`with_timeout`, `event::timer`):** Build classes with inherent timeout logic or create periodic timers for recurring tasks.
*   **Comprehensive Event Handling (`event::*`):** A rich set of predefined event types for:
    *   I/O readiness (socket readable/writable).
    *   Connection lifecycle (disconnections).
    *   Stream status (end-of-file, end-of-stream).
    *   Pending data notifications.
    *   File system changes.
*   **Asynchronous Signal Handling (`event::signal`):** Process system signals (e.g., SIGINT, SIGTERM) gracefully within the event loop.
*   **File System Monitoring (`file_watcher`, `directory_watcher`):** Asynchronously watch files and directories for changes (modifications, creation, deletion).

## 2. Networking Capabilities

*   **Unified Socket API (`qb::io::socket`):** A cross-platform (POSIX & Winsock) abstraction for raw socket operations.
*   **TCP Communication (`qb::io::tcp`, `qb::io::transport::tcp`):
    *   Robust client (`tcp::socket`) and server (`tcp::listener`) implementations.
    *   Streamlined asynchronous connect (`async::tcp::connect`) and accept (`async::tcp::acceptor`) utilities.
    *   Buffered stream transport (`transport::tcp`) for easy data handling.
*   **UDP Communication (`qb::io::udp`, `qb::io::transport::udp`):
    *   Datagram sockets (`udp::socket`) for connectionless messaging.
    *   Endpoint identity management (`transport::udp::identity`) for tracking peers.
    *   Support for multicast group membership.
    *   Buffered stream-like transport (`transport::udp`) for datagrams.
*   **SSL/TLS Security (Optional: `QB_IO_WITH_SSL`):
    *   Secure socket variants: `tcp::ssl::socket` and `tcp::ssl::listener`.
    *   Simplified SSL context creation (`ssl::create_client_context`, `ssl::create_server_context`).
    *   Secure stream transport (`transport::stcp`) for encrypted TCP.
*   **Addressing & Resolution:**
    *   `qb::io::endpoint`: Versatile representation for IPv4, IPv6, and Unix domain socket addresses.
    *   `qb::io::uri`: RFC 3986 compliant URI parsing and manipulation.
    *   `socket::resolve()` family: Asynchronous and synchronous hostname resolution.

## 3. Protocol Framework & Built-in Parsers

*   **Extensible Protocol Interface (`qb::io::async::AProtocol`):** A clear C++ interface (using CRTP) for defining custom message framing and parsing logic.
*   **Ready-to-Use Protocols:**
    *   **Delimiter-Based:** `text::string` (null-terminated), `text::command` (newline-terminated), and their `string_view` counterparts for zero-copy reads (`text::string_view`, `text::command_view`). Also, generic `base::byte_terminated` and `base::bytes_terminated`.
    *   **Size-Prefixed:** `text::binary8`, `text::binary16`, `text::binary32` for messages preceded by a 1, 2, or 4-byte length header (handles network byte order).
    *   **JSON Support:** `protocol::json` (for null-terminated JSON strings) and `protocol::json_packed` (for null-terminated MessagePack-encoded JSON), both integrating with `nlohmann::json`.

## 4. File System Operations

*   **Direct File Access (`qb::io::sys::file`):** Cross-platform, descriptor-based synchronous file I/O.
*   **Efficient Bulk Transfers (`qb::io::sys::file_to_pipe`, `pipe_to_file`):** Streamline reading entire files into memory pipes or writing pipe contents to files.
*   **Asynchronous Monitoring:** (See Core Asynchronous System: `file_watcher`, `directory_watcher`).

## 5. Essential Utilities

*   **High-Precision Time (`qb::Timestamp`, `qb::Duration`):** Nanosecond-accurate time points and durations for measurements and scheduling.
*   **Cryptography (Optional: `QB_IO_WITH_SSL`):
    *   **Hashing:** MD5, SHA-1, SHA-2 (SHA-256, SHA-384, SHA-512), HMAC variants.
    *   **Key Derivation:** PBKDF2, HKDF, Argon2 for secure key generation from passwords or other entropy sources.
    *   **Encoding:** Base64, Base64URL, Hexadecimal.
    *   **Symmetric Encryption:** AES (CBC, GCM modes), ChaCha20-Poly1305.
    *   **Asymmetric Cryptography:** RSA, ECDSA (P-256, P-384, P-521), EdDSA (Ed25519), X25519 key exchange, and ECIES (Elliptic Curve Integrated Encryption Scheme).
    *   **JSON Web Tokens (`qb::jwt`):** Creation, signing, and verification of JWTs.
    *   **Secure Utilities:** Secure password hashing, random data generation, and token management.
*   **Compression (Optional: `QB_IO_WITH_ZLIB`):
    *   Support for Gzip and Deflate compression algorithms.
    *   Functions for both direct in-memory compression/decompression and stream-based processing.
*   **System Information:**
    *   CPU details (`qb::CPU`): Architecture, core counts, clock speed.
    *   Endianness detection (`qb::endian`): Runtime and compile-time checks, byte-swapping utilities.
*   **High-Performance Containers & Allocators:**
    *   `qb::allocator::pipe<T>`: An efficient, dynamically resizable buffer optimized for I/O operations.
    *   `qb::string<N>`: A fixed-capacity string optimized for small string operations, avoiding heap allocations.
    *   `qb::unordered_map`, `qb::unordered_set`: High-performance hash table implementations (using `ska::flat_hash_map/set` in release builds).
    *   `qb::icase_unordered_map`: Case-insensitive string key map.
*   **Lock-Free Primitives (`qb::lockfree`):**
    *   `SpinLock`: Low-overhead spinlock for short critical sections.
    *   SPSC/MPSC Queues: Single-Producer/Single-Consumer and Multiple-Producer/Single-Consumer lock-free ring buffers, primarily used internally by `qb-core` for inter-thread communication.
*   **UUID Generation (`qb::uuid`):** Create RFC 4122 compliant Universally Unique Identifiers.

This rich feature set makes `qb-io` a solid foundation for building a wide range of demanding C++ applications.

**(Next:** `[QB-IO: Async System (`qb::io::async`)](./async_system.md)` or explore other specific feature pages.**) 