# Native QUIC and HTTP/3 transport

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (C++20 default, C++23 supported)

`qb-io` exposes QUIC as an optional asynchronous I/O family built on libngtcp2 and OpenSSL: a reactor-driven endpoint owns one UDP socket, drives the ngtcp2 backend, routes packets by connection id, and dispatches typed lifecycle events for connections, streams, and datagrams.

**Prerequisites:** [qb-io module overview](./README.md), [Transports](./transports.md), [SSL/TLS transport](./ssl_transport.md) — **See also:** [Protocols](./protocols.md), [Async system](./async_system.md)

## Summary

QUIC in `qb-io` is transport infrastructure only. The layer owns UDP sockets, timers, connection-id routing, streams, stream credit, resets, stop-sending, datagrams, and typed lifecycle events. Request/response semantics such as HTTP/3 framing and QPACK belong to higher modules such as `qbm/http`, not to `qb-io`. The default Application-Layer Protocol Negotiation (ALPN) identifier is `h3`, so an endpoint is wired for HTTP/3 out of the box, but the transport itself carries opaque stream bytes.

The feature is gated at build time. When libngtcp2 and SSL are present the QUIC types compile and `qb::io::quic::available()` returns `true`; otherwise the rest of `qb-io` builds unchanged and `available()` returns `false`. Application code can branch on `available()` at runtime or on the `QB_HAS_QUIC` macro at compile time.

## Build

QUIC is controlled by the tri-state CMake cache variable `QB_WITH_QUIC`, which defaults to `AUTO`.

| Value           | Behavior                                                                       |
| --------------- | ------------------------------------------------------------------------------ |
| `AUTO` (default)| Enable QUIC if and only if libngtcp2 is found; stay quiet when it is absent.    |
| `ON`            | Require libngtcp2; warn and disable QUIC if it (or SSL) is missing.            |
| `OFF`           | Disable QUIC outright.                                                          |

<!-- src: qb/cmake/qbConfig.cmake:103-106 -->
<!-- src: qb/cmake/qbDependencies.cmake:198-236 -->

```sh
cmake -DQB_WITH_QUIC=ON ...
```

QUIC requires SSL. If `QB_HAS_SSL` is false, QUIC is disabled regardless of `QB_WITH_QUIC`; under `AUTO` the disable is silent, under `ON` it warns. libngtcp2 is resolved through `find_package` only and is never fetched; the custom `FindNgtcp2.cmake` module creates the imported targets `Ngtcp2::ngtcp2` and `Ngtcp2::crypto_ossl`. A successful detection defines the `QB_HAS_QUIC` compile macro that guards all QUIC code. The native backend currently targets the ngtcp2 OpenSSL helper APIs; the GnuTLS helper package is a different backend, not a link-compatible substitute.

```cpp
// src: qb/include/qb/io/quic/types.h:109-125
#include <qb/io/quic.h>

if (!qb::io::quic::available()) {
    // qb::io::quic::unavailable_reason() returns a human-readable message
    // when QUIC was not compiled in.
    return;
}
```

> **Note for CI.** A build configured with `QB_WITH_QUIC=AUTO` on a runner without libngtcp2 silently disables QUIC. If you require the QUIC path to be exercised, install libngtcp2 and configure with `QB_WITH_QUIC=ON` so a missing dependency fails loudly.

## Concepts

### Endpoint, backend, and stream session

Three types carry the model.

The **endpoint** (`qb::io::async::quic::endpoint`) is the reactor object. It owns the UDP socket, holds a `std::unique_ptr<qb::io::quic::backend>`, bridges the libev I/O and timer watchers, arms the handshake and idle timeouts, flushes outbound packets, and dispatches backend events to virtual hooks. It is the single polymorphic class of this slice: it has a virtual destructor and virtual `dispatch(...)` overloads. The endpoint is **non-copyable and non-movable** — all four special member operations are deleted — so it must be held in place and cannot be relocated after construction.

The **backend** (`qb::io::quic::backend`) is the abstract engine contract: `configure`, `start_server`, `start_client`, `on_udp_datagram`, `on_timeout`, `next_timeout`, `wants_write`, `drain_packets`, `drain_events`, the stream and datagram mutators, and `current_stats`. The shipped implementation drives libngtcp2 plus OpenSSL and is obtained through `qb::io::quic::make_native_backend()`. Custom backends are possible by implementing the contract and passing the instance to the endpoint constructor or `set_backend(...)`.

<!-- src: qb/include/qb/io/quic/backend.h:61-105 -->

A **stream session** (`qb::io::async::quic::client` / `detail::session_base`) is a logical, per-stream buffered session with its own protocol and in/out pipes. Unlike a TCP session, it is not a movable kernel socket. A TCP session owns one file descriptor and can be extracted and moved to another `io_handler`; a QUIC connection owns one UDP descriptor plus shared congestion, ACK/loss recovery, TLS, timer, and connection-id state for every stream it carries. Moving a single stream to another thread would split that shared state, so streams stay on the endpoint owner.

### Endpoint affinity

QUIC is not TCP with one descriptor per connection. A server endpoint owns one UDP descriptor and routes inbound datagrams to per-connection child backends by connection id. The threading model follows from that:

- A QUIC connection and all of its streams stay on the listener that owns the UDP socket.
- A QUIC stream is not extracted to another listener the way a TCP session is.
- Under `qb-core`, scale by running endpoints on separate ports or listeners, or by forwarding business work to worker actors through `qb` events that carry the connection and stream ids, then posting responses back to the actor that owns the endpoint.

The internal `connection_id` is a monotonically assigned `std::uint64_t`, not the on-wire connection id; the endpoint and `io_handler` key streams by the `{connection_id, stream_id}` pair.

### The `use<>::quic` aliases

The declarative `qb::io::use<Derived>` helper exposes a `quic` group whose nested aliases attach QUIC behavior to a user class.

| Alias                          | Resolves to                                              |
| ------------------------------ | ------------------------------------------------------- |
| `use<D>::quic::session`        | `async::quic::client<D>` (stream session, no server)    |
| `use<D>::quic::stream_session` | `async::quic::client<D>` (same)                         |
| `use<D>::quic::client<Server>` | `async::quic::client<D, Server>`                        |
| `use<D>::quic::server<S>`      | `async::quic::server<D, S>`                             |
| `use<D>::quic::connector<S>`   | `async::quic::connector<D, S>` (`S` defaults to `void`) |
| `use<D>::quic::io_handler<S>`  | `async::quic::io_handler<D, S>`                         |
| `use<D>::quic::endpoint`       | `async::quic::endpoint`                                 |
| `use<D>::quic::stream`         | `async::quic::stream`                                   |

<!-- src: qb/include/qb/io/async.h:147-164 -->

A server derives from `server<Derived, StreamSession>`; a client that manages stream sessions derives from `connector<Derived, StreamSession>`; a client that handles events directly with no per-stream sessions derives from `connector<Derived>` (the `StreamSession=void` specialization). Application message callbacks remain ordinary `on(...)` methods. The QUIC facades dispatch the typed events below to matching `on(event::...)` overloads when the derived class declares them.

## Settings and timeouts

`qb::io::quic::settings` carries the connection-level configuration. The timeout fields are `qb::duration` (a `std::chrono::nanoseconds` span); the windows, limits, and batch sizes are `std::uint64_t`. The verified defaults are:

| Field                       | Type            | Default              |
| --------------------------- | --------------- | -------------------- |
| `handshake_timeout`         | `qb::duration`  | `seconds(10)`        |
| `idle_timeout`              | `qb::duration`  | `seconds(30)`        |
| `stream_recv_window`        | `std::uint64_t` | `1 MiB`              |
| `connection_recv_window`    | `std::uint64_t` | `16 MiB`             |
| `max_stream_data_bidi_local`| `std::uint64_t` | `1 MiB`              |
| `max_stream_data_bidi_remote`| `std::uint64_t`| `1 MiB`              |
| `max_stream_data_uni`       | `std::uint64_t` | `1 MiB`              |
| `max_streams_bidi`          | `std::uint64_t` | `100`                |
| `max_streams_uni`           | `std::uint64_t` | `100`                |
| `max_datagram_frame_size`   | `std::uint64_t` | `0` (datagrams off)  |
| `max_connections`           | `std::uint64_t` | `4096`               |
| `max_pending_stream_bytes`  | `std::uint64_t` | `16 MiB`             |
| `max_pending_stream_frames` | `std::uint64_t` | `4096`               |
| `max_pending_datagram_bytes`| `std::uint64_t` | `4 MiB`              |
| `max_pending_datagram_frames`| `std::uint64_t`| `1024`               |
| `udp_rx_batch_size`         | `std::uint64_t` | `256`                |
| `udp_tx_batch_size`         | `std::uint64_t` | `256`                |
| `enable_stateless_retry`    | `bool`          | `true`               |
| `enable_datagrams`          | `bool`          | `false`              |
| `enable_keylog`             | `bool`          | `false`              |

<!-- src: qb/include/qb/io/quic/types.h:51-72 -->

`handshake_timeout` bounds how long the QUIC/TLS handshake may take before failing; it maps to `ngtcp2_settings.handshake_timeout`. `idle_timeout` is the inactivity ceiling after which the connection closes with `disconnect_reason::idle_timeout`; it maps to the QUIC `max_idle_timeout` transport parameter. Both are converted to milliseconds and multiplied by `NGTCP2_MILLISECONDS` when the native settings are built, so any sub-millisecond precision is truncated.

<!-- src: qb/source/io/src/quic.cpp:780-799 -->

Not every field is wired in the shipped backend. The verified enforcement points are:

- The transport-parameter fields (`max_stream_data_*`, `connection_recv_window` → `initial_max_data`, `max_streams_bidi`, `max_streams_uni`, `max_datagram_frame_size`) are written into the ngtcp2 transport parameters when the connection starts. <!-- src: qb/source/io/src/quic.cpp:787-805 -->
- `max_pending_stream_bytes` / `max_pending_stream_frames` and `max_pending_datagram_bytes` / `max_pending_datagram_frames` are enforced inside the native backend; overrunning a pending queue closes the connection with `disconnect_reason::buffer_overflow`. <!-- src: qb/source/io/src/quic.cpp:400-412 -->
- `udp_rx_batch_size` and `udp_tx_batch_size` are enforced by the endpoint's UDP read and write loops, not the backend; a value of `0` means an unbounded batch. <!-- src: qb/include/qb/io/async/quic/endpoint.h:151,509 -->

The following fields are carried in the struct but have **no observed effect** in the shipped native backend, so do not rely on them: `stream_recv_window` is not read anywhere; `enable_stateless_retry` is stored and forwarded to `configure(...)` but never acted on, and `disconnect_reason::stateless_retry_failed` is never emitted; `enable_keylog` has no TLS keylog wiring. Treat all three as reserved until the backend implements them. <!-- TODO(verify): stream_recv_window / enable_stateless_retry / enable_keylog unreferenced in qb/source/io/src/quic.cpp as of this revision -->

Apply settings through the constructor or `set_settings(...)` before `listen` / `connect`:

```cpp
qb::io::quic::settings cfg;
cfg.handshake_timeout = std::chrono::seconds(5);
cfg.idle_timeout      = std::chrono::seconds(20);
cfg.max_connections   = 1024;
endpoint.set_settings(cfg);
```

### Connection limit

`settings.max_connections` caps concurrent server connections; `0` means unlimited. When a server is at the cap, an over-limit new-connection datagram is **silently dropped**, not closed. Dropping rejects only that one new connection, keeps the listener up, and is repeatable; existing connections (matched by their destination connection id) are unaffected and a client may retry after a timeout. The drop is deliberate: queuing a close event on the parent listener would carry the listener's sentinel connection id `0`, which the endpoint interprets as "the listener itself closed", and that close is sticky — a single over-limit datagram would otherwise permanently shut the whole server down, turning a per-client limit into a full-server denial of service.

<!-- src: qb/source/io/src/quic.cpp:648-697 -->

### TLS and hostname verification

`qb::io::quic::tls_config` carries `certificate_file`, `private_key_file`, `server_name`, and `verify_peer` (default `true`).

<!-- src: qb/include/qb/io/quic/types.h:74-79 -->

A server requires `certificate_file` and `private_key_file`. For a client, `verify_peer` validates the certificate chain, but chain validation alone accepts any CA-trusted certificate for any host. The backend binds OpenSSL hostname verification (`SSL_set1_host`) only when `verify_peer` is true **and** `server_name` is set. The string-overload of `endpoint::connect` populates `tls.server_name` from the URI host automatically; if you build the `tls_config` yourself, set `server_name` explicitly so client connections are protected against an on-path attacker.

<!-- src: qb/source/io/src/quic.cpp:756-768 -->

## Examples

### Server

```cpp
// src: qb/source/io/tests/system/test-quic.cpp:219-251 (shape)
#include <qb/io/async.h>

// The per-stream session: a buffered session with its own protocol pipe.
class StreamSession : public qb::io::use<StreamSession>::quic::session {
public:
    using Base = qb::io::use<StreamSession>::quic::session;
    using Base::Base;

    // Optional: declare a protocol with `using Protocol = ...;` and override
    // on(Protocol::message&&). Inbound stream bytes land in in(); writes go
    // through out() via publish()/operator<<.
};

class Server : public qb::io::use<Server>::quic::server<StreamSession> {
public:
    void on(qb::io::async::quic::event::connected const&) {}
    void on(qb::io::async::quic::event::stream_data const& ev) {
        // ev.payload is a borrowed std::string_view valid only for this call.
    }
    void on(qb::io::async::quic::event::connection_closed const&) {}
};

void run_server(Server& server) {
    server.listen(qb::io::uri{"quic://0.0.0.0:4433"},
                  "server.crt", "server.key");      // default ALPN: {"h3"}
}
```

`listen(bind_uri, cert_file, key_file, alpn_protocols = {"h3"})` binds the UDP socket, configures the backend, starts the server role, registers the I/O and timer watchers, and drains the first batch of packets. It returns `false` if the bind fails.

<!-- src: qb/include/qb/io/async/quic/endpoint.h:302-324 -->

### Client

```cpp
// src: qb/source/io/tests/system/test-quic.cpp:253-257 (shape)
#include <qb/io/async.h>

class Client : public qb::io::use<Client>::quic::connector<StreamSession> {
public:
    using connector::connector;

    void on(qb::io::async::quic::event::connected const&) {}
    void on(qb::io::async::quic::event::connection_closed const&) {}
};

void run_client(Client& client) {
    // The string overload sets tls.server_name from the URI host for you.
    client.connect(qb::io::uri{"quic://example.org:4433"});  // default ALPN: {"h3"}

    auto* stream = client.open_bidirectional_stream_session();
    *stream << std::string_view{"hello\n"};
    client.finish_stream_session(*stream);  // flush, then send FIN
}
```

`connect(remote_uri, alpn_protocols = {"h3"})` and its `tls_config` overload init and bind a local UDP socket, configure the backend, start the client role, and move the endpoint to the `connecting` state. ALPN entries must be 1 to 255 bytes long and at least one is required; otherwise the wire-ALPN builder throws `std::invalid_argument`.

<!-- src: qb/include/qb/io/async/quic/endpoint.h:326-362 -->
<!-- src: qb/source/io/src/quic.cpp:34-44 -->

### Streams and protocols

QUIC streams reuse the standard buffered-session conventions:

- received stream bytes append directly into `session.in()`;
- application writes go through `session.out()` via `publish` / `operator<<`;
- a `using Protocol = ...;` alias and `switch_protocol<...>(*this)` work exactly as in classic `async::io`;
- backend stream state is metadata only.

This keeps the invariant that the durable receive payload lives in `session.in()`, the durable transmit payload lives in `session.out()`, and the backend stream state is metadata.

<!-- src: qb/include/qb/io/async/quic/stream.h:139-194 -->

For streams opened locally by the application, the endpoint or handler owns the drain because an individual QUIC stream has no fd watcher of its own:

```cpp
auto* stream = client.open_bidirectional_stream_session();
stream->publish(std::string_view{"hello\n"});
client.flush_stream_session(*stream);   // write pending output
client.finish_stream_session(*stream);  // flush, then send FIN
```

The same methods exist on servers with an explicit connection id:

```cpp
auto* stream = server.open_unidirectional_stream_session(connection_id);
*stream << std::string_view{"event: ready\n"};
server.finish_stream_session(connection_id, stream->id());
```

<!-- src: qb/include/qb/io/async/quic/server.h:38-93 -->

## Lifecycle events

The endpoint dispatches seven typed events to derived `on(event::...)` overloads. All live in `qb::io::async::quic::event`.

| Event               | Key fields                                                        |
| ------------------- | ---------------------------------------------------------------- |
| `connected`         | `connection_id`, `negotiated_alpn`                               |
| `connection_closed` | `connection_id`, `reason`, `error_code`, `reason_phrase`         |
| `stream_started`    | `connection_id`, `id`, `direction`, `origin`                     |
| `stream_data`       | `connection_id`, `id`, `payload`, `fin`                          |
| `stream_data_acked` | `connection_id`, `id`, `bytes`                                   |
| `stream_closed`     | `connection_id`, `id`, `reason`, `error_code`                    |
| `datagram`          | `connection_id`, `payload`                                       |

<!-- src: qb/include/qb/io/async/quic/events.h:16-58 -->

`reason` on `connection_closed` is a `qb::io::quic::disconnect_reason` (`idle_timeout`, `handshake_failed`, `stateless_retry_failed`, `transport_error`, `application_close`, and others). `reason` on `stream_closed` is a `qb::io::quic::stream_close_reason` that distinguishes normal finish from `reset`, `stop_sending`, `flow_control_error`, and `connection_closed`. `reset_stream(...)` maps to an abrupt stream shutdown; `stop_stream(...)` maps to the QUIC read-side stop-sending path.

<!-- src: qb/include/qb/io/quic/types.h:18-39 -->

Server-side and client-side close semantics differ. A `connection_closed` carrying connection id `0` (or any client) sets the endpoint's `_open` flag to false, which means the whole endpoint is down. For a server child connection it only downgrades the endpoint state to `listening` or `connected` and keeps the listener open.

<!-- src: qb/include/qb/io/async/quic/endpoint.h:198-215 -->

> **Borrowed payloads.** The `payload` on `event::stream_data` and `event::datagram` is a `std::string_view` into the backend's drain buffer. It is valid only for the duration of the dispatch call. Copy the bytes if you need to retain them.

<!-- src: qb/include/qb/io/async/quic/endpoint.h:223-256 -->

## Flow control and datagrams

The QUIC layer separates UDP packet RX/TX budgets, per-stream read and write limits, stream credit updates, connection flow control, congestion and loss handled by the backend, and application pending output in `qb` pipes. A protocol should extend stream credit only for bytes it has consumed or handed safely to the application.

Inbound credit must be re-flushed. `extend_stream_credit(...)` and every backend mutator (send, reset, stop, datagram) call the internal drain so the generated `MAX_STREAM_DATA` / `MAX_DATA` frames are actually written; skipping that flush would queue the credit grant but never send it, stalling the receive path.

<!-- src: qb/include/qb/io/async/quic/endpoint.h:417-428 -->

QUIC DATAGRAMs are off by default: `settings.enable_datagrams` is `false` and `max_datagram_frame_size` is `0`. Calling `send_datagram(...)` while datagrams are disabled does not silently no-op — the native backend queues a `connection_closed` event with `disconnect_reason::protocol_error` and the reason phrase `QUIC DATAGRAM is not enabled`. Enable datagrams (`settings.enable_datagrams = true` plus a non-zero `max_datagram_frame_size`) before sending. A payload that exceeds `max_datagram_frame_size`, or that overflows the `max_pending_datagram_bytes` / `max_pending_datagram_frames` queue, closes the connection with `disconnect_reason::buffer_overflow` instead. An empty payload, or a send issued while the connection is closing, is dropped without error.

<!-- src: qb/include/qb/io/quic/types.h:61-71 -->
<!-- src: qb/source/io/src/quic.cpp:482-524 -->

A stream-session buffer overflow is fatal to the stream. Appending past `max_read_buffer_size` or publishing past `max_write_buffer_size` both disconnect the session with `buffer_overflow`; the handler turns the resulting feed failure into a `reset_stream` with error code `1`.

<!-- src: qb/include/qb/io/async/quic/stream.h:139-169 -->
<!-- src: qb/include/qb/io/async/quic/server.h:114-122 -->

## Pitfalls

- **The endpoint cannot be moved.** All copy and move operations are deleted. Construct it in place and hold it by pointer or reference; never store it in a container that relocates its elements.
  <!-- src: qb/include/qb/io/async/quic/endpoint.h:271-274 -->
- **`listen` / `connect` and the stream mutators throw when QUIC is absent.** `ensure_backend()` throws `std::runtime_error` if `qb::io::quic::available()` is false or no backend could be created, and `make_native_backend()` throws when `QB_HAS_QUIC` is undefined. Guard with `available()` or `QB_HAS_QUIC` before calling them.
  <!-- src: qb/include/qb/io/async/quic/endpoint.h:78-86 -->
  <!-- src: qb/source/io/src/quic.cpp:1335-1341 -->
- **Borrowed event payloads do not outlive the dispatch.** Copy `event::stream_data.payload` / `event::datagram.payload` before returning from the handler.
- **Set `tls.server_name` for client connections.** Without it, the chain is validated but the hostname is not, leaving the connection open to an on-path certificate substitution. The string overload of `connect` sets it for you from the URI host.
- **Do not move a stream across threads.** Keep the `io_handler` on the core that owns the endpoint and delegate work through `qb` events that carry the connection and stream ids.
  <!-- src: qb/include/qb/io/async/quic/io_handler.h:38-50 -->
- **HTTP/3 semantics are not here.** Server push, request/response framing, and QPACK belong to `qbm/http`. The `qb-io` layer carries opaque stream bytes under the `h3` ALPN.

## V1 limits

The native QUIC layer is intentionally transport-only:

- no HTTP, QPACK, request, or response concepts live in `qb-io`;
- no QUIC stream extraction across listeners;
- no cross-thread connection migration;
- HTTP/3 server push and HTTP semantics belong to `qbm/http`.

`SO_REUSEPORT`, a central connection-id dispatcher, and connection migration across threads can be layered on top later, but the model remains one endpoint owner per UDP socket.

## See also

- [qb-io module overview](./README.md)
- [Transports](./transports.md) — TCP, UDP, and the transport abstraction QUIC sits beside
- [SSL/TLS transport](./ssl_transport.md) — the OpenSSL dependency QUIC shares
- [Protocols](./protocols.md) — declaring a `Protocol` for stream sessions
- [Async system](./async_system.md) — the listener, watchers, and event loop the endpoint binds to
