# qb-io: asynchronous I/O runtime

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

`qb-io` is the C++23 asynchronous I/O runtime under the qb actor framework: a single-threaded, libev-backed event loop with non-blocking TCP, UDP, SSL/TLS, and QUIC transports, a pluggable protocol layer, native C++20/23 coroutines, and a set of standalone utilities.

**Prerequisites:** none — **See also:** [qb-core module overview](../4_qb_core/README.md), [core and I/O integration](../5_core_io_integration/README.md), [async, lifecycle, and allocation invariants](../7_reference/io_invariants.md)

## Summary

`qb-io` powers the framework's non-blocking input and output, but it does not depend on `qb-core`. The event loop, transports, protocols, coroutine scheduler, and utilities have no reference to actors, so the library can be used on its own in any event-driven C++23 project. When it does run under the actor engine, each `VirtualCore` worker thread drives its own `qb::io::async::listener` event loop, and all of the components in this section run on that loop.

`qb-io` offers two complementary asynchronous programming models that share the same loop and are interoperable:

| Model | When to use |
|---|---|
| Event-driven callbacks (`on()` handlers) | Server sessions, protocol parsers, low-latency read/write paths |
| C++23 coroutines (`co_await`) | Sequential async flows, scatter-gather, retry and timeout logic |

Both run on the same `listener`/libev loop, so a coroutine can `co_await` socket readiness while callback-driven sessions on the same thread continue to make progress.

## Pages in this section

| Page | What it covers |
|---|---|
| [qb-io feature catalog](./features.md) | A map of every `qb-io` capability — async engine, coroutines, transports, protocols, TLS, QUIC, crypto, compression, and utilities — with one-line summaries linking to the page that owns each topic. |
| [The async runtime: event loop, timers, and callbacks](./async_system.md) | The libev-backed event loop (`listener`), `init`/`run`/`run_once`, timers and `with_timeout`, scheduled `callback`, file-system watching, and the async event types shared by every component. |
| [C++23 coroutines](./coroutines.md) | The native `task<T>` coroutine layer: awaiters, combinators (`when_all`, `when_any`, `race`), channels, structured-concurrency scopes, generators, async streams, retry policies, cancellation, and safe actor integration. |
| [TCP and UDP transports and sockets](./transports.md) | How the buffered `istream`/`ostream`/`stream` abstractions bind to concrete `tcp::socket`, `udp::socket`, listeners, and files to form read/write/buffer units for the async layer and protocols. |
| [Framing messages with protocols](./protocols.md) | Built-in and custom `AProtocol` implementations that turn a continuous byte stream into discrete messages — byte/sequence-terminated and size-header framing, text, JSON/MessagePack, and the accept/handshake protocols. |
| [Secure TCP with SSL/TLS](./ssl_transport.md) | OpenSSL-backed SSL/TLS over the TCP stack (`QB_IO_WITH_SSL`): secure-by-default client verification, a context-owning listener, and a stream transport that drains OpenSSL's internal buffers. |
| [Native QUIC and HTTP/3 transport](./quic_transport.md) | The optional QUIC family built on libngtcp2 (`QB_WITH_QUIC`): a reactor-driven endpoint over one UDP socket, connection-id routing, typed stream and datagram events, flow-control hooks, and the threading model. |
| [qb-io utilities](./utilities.md) | The standalone helpers — canonical time vocabulary, cryptography and JWT, compression, URI parsing, fixed-capacity strings and flat hash maps, UUIDs, JSON, and endian helpers — usable without the actor runtime. |
| [Async, lifecycle, and allocation invariants](../7_reference/io_invariants.md) | The single source of truth for thread-ownership, freelist, and CRTP-dispatch invariants. Required reading before writing a custom protocol, transport, or async component. |

## Suggested reading order

1. [qb-io feature catalog](./features.md) — see what is available at a glance and where each topic is documented.
2. [The async runtime](./async_system.md) — understand the event loop fundamentals that everything else builds on.
3. [C++23 coroutines](./coroutines.md) — read this if you prefer `co_await`-style sequential async code.
4. [TCP and UDP transports](./transports.md), then [Framing messages with protocols](./protocols.md) — for network or file I/O.
5. [Secure TCP with SSL/TLS](./ssl_transport.md), then [Native QUIC and HTTP/3 transport](./quic_transport.md) — for encrypted TCP and QUIC.
6. [qb-io utilities](./utilities.md) — for time, crypto, compression, URI, container, and UUID needs.
7. [Async, lifecycle, and allocation invariants](../7_reference/io_invariants.md) — before extending the stack with a custom component.

## See also

- [qb-core module overview](../4_qb_core/README.md) — the actor engine that runs each `listener` loop on a `VirtualCore` worker thread.
- [Core and I/O integration](../5_core_io_integration/README.md) — how actors compose `qb-io` sessions, acceptors, and clients.
