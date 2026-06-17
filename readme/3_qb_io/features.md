# qb-io feature catalog

> **Audience:** Evaluator · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

An index of every `qb-io` capability — async engine, coroutines, transports, protocols, TLS, QUIC, crypto, compression, and utilities — with one-line summaries that link to the detail page that owns each topic.

**Prerequisites:** none — **See also:** [qb-io overview](./README.md), [the asynchronous engine](./async_system.md), [io invariants reference](../7_reference/io_invariants.md)

`qb-io` is the C++20 asynchronous runtime with optional C++23 support under the qb actor framework. It is also usable standalone: the event loop, transports, protocols, and utilities have no dependency on `qb-core`. This page is a map. Each entry names the type or namespace, states what it does in one line, and links to the page that documents it in full. Where a capability is optional, the entry names the compile-time feature macro that gates it and the CMake option that defines that macro.

## Optional feature gates

Three capability groups are compiled conditionally. The CMake option is what you set; the preprocessor macro is what guards the C++ code. When the dependency is absent, the option is forced off and the corresponding API is not compiled.

| Capability | CMake option | Compiled-in macro | Backing library |
|---|---|---|---|
| SSL/TLS and crypto | `QB_WITH_SSL` (default `ON`) | `QB_HAS_SSL` | OpenSSL |
| Compression | `QB_WITH_COMPRESSION` (default `ON`) | `QB_HAS_COMPRESSION` | zlib |
| QUIC / HTTP/3 transport | `QB_WITH_QUIC` (default `AUTO`) | `QB_HAS_QUIC` | ngtcp2 (requires SSL) |

<!-- src: source/io/CMakeLists.txt (QB_HAS_* definitions); README.md (QB_WITH_* options) -->

See [Building from source](../7_reference/building.md) for the full option list and the auto-disable rules.

## 1. Asynchronous engine (`qb::io::async`)

The event loop and its callback-driven building blocks. One `listener` runs per thread; all timers, socket readiness, signals, and filesystem watchers are driven by it.

- **Event loop (`qb::io::async::listener`)** — the per-thread libev-backed reactor that owns every async watcher. Driven with `qb::io::async::run`, `run_once`, or `run_until`.
- **Delayed callbacks (`qb::io::async::callback`, `scoped_callback`)** — schedule a callable to run after a `qb::duration` delay. `callback` is fire-and-forget; `scoped_callback` returns an RAII handle that cancels the timer on destruction.
- **Timeout mixin (`qb::io::async::with_timeout<Derived>`)** — a CRTP base that gives a class an inherent, resettable timeout (`setTimeout`, `updateTimeout`, `getTimeout`).
- **Event types (`qb::io::async::event::*`)** — typed payloads delivered to `on()` handlers: I/O readiness (`io`), timers (`timer`), connection lifecycle (`disconnected`, carrying an `int reason` code, an `error_code`, and a `message`; the documented codes are enumerated by `disconnect_reason`), stream status (`eos`, `input_drained`), pending-data notifications (`pending_read`, `pending_write`), filesystem changes (`file`), and TLS handshake completion (`handshake`).
- **Signal handling (`qb::io::async::event::signal`)** — process OS signals (for example `SIGINT`, `SIGTERM`) inside the loop instead of in an interrupt context.
- **Filesystem watchers (`qb::io::async::file_watcher`, `directory_watcher`)** — watch a file or directory and receive `event::file` notifications on change, creation, or deletion.

→ [The asynchronous engine](./async_system.md)

## 2. C++20 coroutines (`qb::io::async` coroutine layer)

A coroutine runtime layered on the same `listener`. Callbacks and coroutines share one single-threaded execution model and interoperate freely; only one coroutine runs at a time per thread, and another can run only at a `co_await` suspension point.

- **Return types (`task<T>`, `shared_task<T>`)** — `task<T>` is a lazy, move-only coroutine result; `shared_task<T>` is copyable and lets several coroutines `co_await` one computation without recomputation.
- **Awaiters** — `sleep(qb::duration)`, `wait_readable` / `wait_writable`, and `async_awaiter<T>` to bridge any callback API into `co_await`. `co_await qb::io::async::tcp::connect(...)` awaits a TCP connection.
- **Combinators** — `when_all` (scatter-gather), `when_any` (first to finish), `race`, and `coro_with_timeout` (deadline wrapper).
- **Cancellation** — `cancellation_token`, `cancellable_sleep`, `with_deadline`, `check_cancelled`, `make_cancellable`. Tokens are single-threaded.
- **Synchronization primitives** — `semaphore`, `async_mutex`, `async_rw_lock`, `barrier`, `async_event`, `async_latch`. These need no OS locks because the single-thread model already serializes execution.
- **Channels (`channel<T>`)** — in-thread communication with `send` / `recv`, timed variants (`send_for` / `recv_for`), `select` across channels, and `make_pipeline` / `transform` / `filter` / `collect` helpers.
- **Structured concurrency (`coroutine_scope`)** — own a set of child coroutines with `join_all` / `join_any`, cancellation propagation, and `parallel_map` / `repeat_while`.
- **Generators** — `generator<T>` (synchronous, range-for compatible) and `async_generator<T>` (`co_yield` plus `co_await`), with consumers `ag_for_each`, `ag_collect`, `ag_map`, `ag_filter`, `ag_reduce`.
- **Async streams (`async_stream<T>`)** — a lazy functional pipeline with `map` / `filter` / `take` / `skip`, terminal consumers, `merge_streams`, `zip`, and `interval`.
- **Retry (`with_retry`, `with_retry_until`, `make_retryable`)** — retry an operation under a `retry_policy` with a configurable `backoff_strategy`.

→ [C++20 coroutines](./coroutines.md)

## 3. Networking

Cross-platform sockets, the buffered transports built on them, and addressing.

- **Socket API (`qb::io::socket`)** — a cross-platform (POSIX and Winsock) abstraction over raw socket descriptors.
- **TCP (`qb::io::tcp::socket`, `qb::io::tcp::listener`)** — connection-oriented client and server primitives. Asynchronous connect via `qb::io::async::tcp::connect` and accept via `qb::io::async::tcp::acceptor`; buffered framing via `qb::io::transport::tcp`.
- **UDP (`qb::io::udp::socket`)** — connectionless datagram sockets with multicast group membership and per-peer endpoint tracking (`qb::io::transport::udp::identity`); buffered datagram I/O via `qb::io::transport::udp`.
- **Addressing** — `qb::io::endpoint` represents IPv4, IPv6, and Unix-domain addresses; `qb::io::uri` parses and manipulates RFC 3986 URIs; the `qb::io::socket::resolve` family performs synchronous hostname resolution over `getaddrinfo` (`resolve`, `resolve_v4`, `resolve_v6`).

→ [Transports](./transports.md)

## 4. SSL/TLS (optional, `QB_HAS_SSL`)

Encrypted TCP layered on OpenSSL.

- **Secure sockets (`qb::io::tcp::ssl::socket`, `qb::io::tcp::ssl::listener`)** — TLS-wrapped client and server sockets that manage the handshake transparently.
- **Context helpers (`qb::io::ssl::create_client_context`, `create_server_context`)** — build configured `SSL_CTX` objects without hand-rolling OpenSSL setup.
- **Secure transport (`qb::io::transport::stcp`)** — buffered stream transport over `ssl::socket`, used by the secure `use<...>::tcp::ssl` clients and sessions.

By default an auto-created client context loads the system trust store, enables peer verification, and checks the server certificate and hostname unless that policy is explicitly relaxed before connecting.

→ [Secure TCP with SSL/TLS](./ssl_transport.md)

## 5. QUIC / HTTP/3 (optional, `QB_HAS_QUIC`)

A reactor-driven QUIC transport over ngtcp2 and OpenSSL. Requires SSL to be enabled.

- **Endpoint (`qb::io::async::quic::endpoint`)** — owns the UDP socket and backend, drives the handshake, and dispatches connection and stream events. `listen` for servers, `connect` for clients.
- **Streams and sessions** — logical per-stream buffered sessions (`stream_session`, `client`), bidirectional and unidirectional stream creation, flow-control hooks (`extend_stream_credit`), and reset/stop controls.
- **Events (`qb::io::async::quic::event::*`)** — `connected`, `connection_closed`, `stream_started`, `stream_data`, `stream_data_acked`, `stream_closed`, `datagram`.
- **Settings (`qb::io::quic::settings`)** — `handshake_timeout` (default 10 s) and `idle_timeout` (default 30 s) as `qb::duration`, plus address-validation Retry via `enable_stateless_retry` (default on). The only offered ALPN is `h3` (HTTP/3).

→ [Native QUIC](./quic_transport.md)

## 6. Protocols (`qb::protocol`, `qb::io::async::AProtocol`)

Message framing over byte streams. A protocol cuts raw transport bytes into discrete messages; implement `qb::io::async::AProtocol<IO>` (CRTP) for a custom one, or reuse the built-ins.

- **Delimiter-based (`qb::protocol::text`)** — `string` (NUL-terminated) and `command` (newline-terminated), with zero-copy `string_view` and `command_view` variants; generic `qb::protocol::base::byte_terminated` and `bytes_terminated`.
- **Size-prefixed (`qb::protocol::text`)** — `binary8`, `binary16`, `binary32` for messages preceded by a 1-, 2-, or 4-byte length header.
- **JSON (`qb::protocol::json`, `json_packed`)** — NUL-terminated JSON and MessagePack-packed JSON over `nlohmann::json`, with a nesting-depth limit that rejects pathologically nested input.
- **Framework protocols (`qb::io::protocol::accept`, `handshake`)** — internal protocols that hand an accepted socket to its I/O component and drive a TLS handshake to completion.

→ [Framing messages with protocols](./protocols.md)

## 7. Filesystem

- **Direct file access (`qb::io::sys::file`)** — cross-platform, descriptor-based synchronous file I/O.
- **Bulk transfers (`qb::io::sys::file_to_pipe`, `pipe_to_file`)** — read a whole file into an in-memory pipe or write a pipe's contents to a file.
- **Buffered file transport (`qb::io::transport::file`)** — stream-style buffered reads and writes over `sys::file`.
- **Asynchronous monitoring** — `file_watcher` and `directory_watcher` (see [the asynchronous engine](./async_system.md)).

→ [Transports](./transports.md)

## 8. Utilities

Cross-cutting helpers usable from any qb-io or qb-core code.

- **Time vocabulary (`qb::duration`, `qb::mono_time`, `qb::wall_time`)** — the canonical `std::chrono` types: `duration` is `std::chrono::nanoseconds`, `mono_time` is a `steady_clock` time point, `wall_time` is a `system_clock` time point. Defined in `qb/system/timestamp.h`.
- **Cryptography (`qb::crypto`, optional `QB_HAS_SSL`)** — hashing (MD5, SHA-1, SHA-256, SHA-512, HMAC), key derivation (PBKDF2, HKDF, Argon2), encoding (Base64, Base64URL, hex), symmetric encryption (AES-CBC, AES-GCM, ChaCha20-Poly1305), asymmetric primitives (RSA, ECDSA over P-256/P-384/P-521, Ed25519, X25519 exchange, ECIES), and secure random/token helpers.
- **JSON Web Tokens (`qb::jwt`, optional `QB_HAS_SSL`)** — create, sign, decode, and verify JWTs.
- **Compression (`qb::compression`, optional `QB_HAS_COMPRESSION`)** — gzip and deflate, in-memory (`compress` / `uncompress`, `qb::gzip`, `qb::deflate`) and streaming (`compress_provider` / `decompress_provider`); `uncompress` enforces a caller-supplied output budget to reject decompression bombs.
- **System information** — CPU details (`qb::CPU`) and endianness checks plus byte-swap helpers (`qb::endian`, in `qb/system/endian.h`).
- **Containers and allocators** — `qb::allocator::pipe<T>` (resizable I/O buffer), `qb::string<N>` (fixed-capacity, heap-free small string), `qb::unordered_map` / `qb::unordered_set` (ska flat-hash maps in release builds), and `qb::icase_unordered_map` (case-insensitive string keys).
- **Lock-free primitives (`qb::lockfree`)** — `SpinLock` plus SPSC and MPSC ring-buffer queues, used internally by `qb-core` for inter-core message passing.
- **UUID (`qb::uuid`)** — RFC 4122 identifiers (an alias for `uuids::uuid` from stduuid).

→ [Essential utilities](./utilities.md)

## See also

- [qb-io overview and reading order](./README.md)
- [The asynchronous engine](./async_system.md) — the event loop every entry on this page sits on
- [C++20 coroutines](./coroutines.md)
- [io invariants reference](../7_reference/io_invariants.md) — required reading before writing a custom protocol, transport, or async component
- [Building from source](../7_reference/building.md) — the optional-feature gates and CMake options in full
