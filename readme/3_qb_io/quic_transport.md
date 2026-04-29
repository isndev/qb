@page qb_io_quic_transport_md QB-IO Native QUIC
@brief Optional QUIC transport support in qb-io.

# QB-IO Native QUIC

`qb-io` exposes QUIC as an optional async I/O family. It is transport infrastructure only: request/response protocols such as HTTP/3 belong to higher modules. The QUIC layer owns UDP sockets, timers, connection-id routing, streams, stream credit, resets, stop-sending, datagrams, and typed lifecycle events.

## Build

QUIC is enabled with `QB_WITH_QUIC=ON` when SSL/TLS and `libngtcp2` are available. If dependencies are missing, QB disables QUIC cleanly and the rest of `qb-io` continues to build.

```cmake
cmake -DQB_WITH_QUIC=ON ...
```

Code that depends on QUIC can guard itself with `QB_HAS_QUIC`.

## Shape

The public async surface follows the normal QB style:

```cpp
class MyServer;

class MySession : public qb::io::use<MySession>::quic::client<MyServer> {
public:
    using Protocol = qb::protocol::text::command<MySession>;

    explicit MySession(MyServer& server)
        : client(server) {}

    void on(Protocol::message&& message);
};

class MyServer : public qb::io::use<MyServer>::quic::server<MySession> {};
```

Application callbacks remain `on(...)`. QUIC-specific internals dispatch typed events for connection open/close, stream data, stream close, datagrams, and stream-data ACKs.

## Endpoint Affinity

QUIC is not TCP with one fd per connection. A server endpoint owns one UDP fd and routes packets by connection id. Therefore the v1 threading model is endpoint affinity:

- A QUIC connection and all of its streams stay on the listener that owns the UDP socket.
- Do not extract a QUIC stream session to another listener like a TCP socket.
- With `qb-core`, scale by running endpoints on different ports/listeners, or by forwarding business messages to worker actors and posting responses back to the endpoint owner.

Future designs such as `SO_REUSEPORT`, a central CID dispatcher, or connection migration across threads can build on top of this, but the native model remains one endpoint owner per UDP socket.

## V1 Limits

The native QUIC layer is intentionally transport-only:

- no HTTP, QPACK, request, or response concepts live in `qb-io`;
- no QUIC `extractSession()` across listeners in v1;
- no cross-thread connection migration in v1;
- HTTP/3 server push and HTTP semantics belong to `qbm/http`, not here.

## Streams And Protocols

QUIC streams expose QB-style buffered stream sessions:

- received stream bytes append directly into `session.in()`;
- application writes go through `session.out()` via `publish`/`operator<<`;
- `Protocol` aliases and `switch_protocol<...>(*this)` follow the same conventions as classic `async::io`;
- stream state in the QUIC backend is metadata only.

This preserves the important invariant:

```text
durable RX payload = session.in()
durable TX payload = session.out()
backend stream state = metadata
```

For streams opened locally by the application, the endpoint/handler owns the drain because an individual QUIC stream does not have its own fd watcher:

```cpp
auto *stream = client.open_bidirectional_stream_session();
stream->publish(std::string_view{"hello\n"});
client.flush_stream_session(*stream);
client.finish_stream_session(*stream); // flushes pending output, then sends FIN
```

The same methods are available on QUIC servers with an explicit connection id:

```cpp
auto *stream = server.open_unidirectional_stream_session(connection_id);
stream->publish(payload);
server.finish_stream_session(connection_id, stream->id());
```

## Lifecycle Events

The async QUIC endpoint exposes typed events:

- `connected`
- `connection_closed`
- `stream_data`
- `stream_data_acked`
- `stream_closed`
- `datagram`

Stream close reasons distinguish normal finish, reset, stop-sending, and transport/application close paths. `reset_stream(...)` maps to abrupt stream shutdown; `stop_stream(...)` maps to the QUIC read-side stop-sending path.

## Flow Control And Backpressure

The QUIC layer separates:

- UDP packet RX/TX budgets;
- stream read and write limits;
- stream credit updates;
- connection flow control;
- congestion/loss handled by the backend;
- application pending output in QB pipes.

Protocol implementations should extend stream credit only for bytes they have consumed or handed safely to the application layer.

## Tests

The system tests cover:

- QUIC availability and clean disabled builds;
- native local client/server handshake;
- ALPN mismatch;
- multiple clients routed by connection id;
- stream data, stream ACKs, reset, stop-sending, datagrams;
- listener clear safety;
- protocol compatibility through the existing session text/json tests.

Return to [QB-IO Module README](./README.md)
