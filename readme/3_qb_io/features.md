# QB-IO Features

`qb-io` provides a comprehensive set of features for building asynchronous I/O applications:

## Core Asynchronous System (`qb::io::async`)

*   **Event Loop (`listener`):** High-performance event loop (based on libev).
*   **Callbacks (`callback`):** Schedule asynchronous function execution with delays.
*   **Timers (`with_timeout`, `event::timer`):** Integrated timeout management and periodic timers.
*   **Event Handling (`event::*`):** Infrastructure for handling I/O readiness, disconnections, signals, file changes, stream status (EOF/EOS), and pending data.
*   **Signal Handling (`event::signal`):** Asynchronous handling of system signals.
*   **File Watching (`file_watcher`, `directory_watcher`):** Asynchronous file system monitoring.

## Networking

*   **Socket API (`qb::io::socket`):** Cross-platform (Winsock/POSIX) socket wrapper.
*   **TCP (`qb::io::tcp`, `qb::io::transport::tcp`):**
    *   Client (`tcp::socket`) and Server (`tcp::listener`) capabilities.
    *   Asynchronous connect (`async::tcp::connect`) and accept (`async::tcp::acceptor`).
    *   Stream transport (`transport::tcp`).
*   **UDP (`qb::io::udp`, `qb::io::transport::udp`):**
    *   Datagram socket (`udp::socket`).
    *   Endpoint identity management (`transport::udp::identity`).
    *   Multicast support.
    *   Stream transport (`transport::udp`).
*   **SSL/TLS (Optional: `QB_IO_WITH_SSL`):
    *   Secure sockets (`tcp::ssl::socket`) and listeners (`tcp::ssl::listener`).
    *   SSL context management (`ssl::create_*_context`).
    *   Secure stream transport (`transport::stcp`).
*   **Addressing:**
    *   Endpoint representation (`qb::io::endpoint`) for IPv4, IPv6, Unix sockets.
    *   URI parsing (`qb::io::uri`) compliant with RFC 3986.
    *   Hostname resolution (`socket::resolve*`).

## Protocol Handling

*   **Framework (`qb::io::async::AProtocol`):** Base for defining custom message framing.
*   **Built-in Protocols:**
    *   Delimiter-based (`protocol::base::byte_terminated`, `bytes_terminated`, `text::string`, `text::command`).
    *   Zero-copy views (`text::string_view`, `text::command_view`).
    *   Size-prefixed (`protocol::base::size_as_header`, `text::binary8/16/32`).
    *   JSON (`protocol::json`, `protocol::json_packed`) using nlohmann::json.

## File System

*   **File Operations (`qb::io::sys::file`):** Cross-platform file descriptor operations.
*   **Efficient Transfers (`qb::io::sys::file_to_pipe`, `pipe_to_file`):** Stream between files and memory pipes.
*   **Async Watching (`qb::io::async::file_watcher`, `directory_watcher`):** Monitor file system events.

## Utilities

*   **Time (`qb::Timestamp`, `qb::Duration`):** High-precision time points and durations.
*   **Cryptography (Optional: `QB_IO_WITH_SSL`):
    *   Hashing (MD5, SHA*, HMAC).
    *   Key Derivation (PBKDF2, HKDF, Argon2).
    *   Encoding (Base64, Base64URL, Hex).
    *   Symmetric Encryption (AES-CBC, AES-GCM, ChaCha20-Poly1305).
    *   Asymmetric Cryptography (RSA, ECDSA, EdDSA, X25519, ECDH).
    *   Hybrid Encryption (ECIES, Envelope).
    *   Secure Tokens, Password Hashing, JWT.
*   **Compression (Optional: `QB_IO_WITH_ZLIB`):
    *   Gzip, Deflate algorithms.
    *   Streaming and direct compression/decompression functions.
*   **System Info:** CPU (`qb::CPU`), Endianness (`qb::endian`).
*   **Containers & Allocators:**
    *   `qb::allocator::pipe`: Efficient dynamic buffer.
    *   `qb::string`: Fixed-size string.
    *   `qb::unordered_map/set`: High-performance hash tables.
    *   `qb::icase_unordered_map`: Case-insensitive map.
    *   `qb::lockfree::*`: SPSC/MPSC queues, Spinlock.
*   **UUID (`qb::uuid`):** RFC 4122 UUID generation. 