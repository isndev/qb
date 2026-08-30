# Building network actors

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 3.1.0 (C++20 default, C++23 supported)

Turn an actor into a non-blocking TCP, UDP, or SSL/TLS endpoint by inheriting from a `qb::io::use<Self>` mixin, so network I/O runs on the actor's own `VirtualCore` event loop and arrives as ordinary `on(...)` handler calls.

**Prerequisites:** [The actor model](../2_core_concepts/actor_model.md), [qb-io transports](../3_qb_io/transports.md), [qb-io protocols](../3_qb_io/protocols.md), [async operations in actors](./async_in_actors.md) — **See also:** [SSL/TLS transport](../3_qb_io/ssl_transport.md), [chat_tcp walkthrough](./examples/chat_tcp_analysis.md), [message_broker walkthrough](./examples/message_broker_analysis.md)

## Summary

`qb-io` integrates asynchronous network I/O directly into actors through the `qb::io::use<Derived>` CRTP helper (declared in `qb/io/async.h`). When an actor inherits from one of its nested aliases — for example `qb::io::use<MyClient>::tcp::client<>` — it gains the transport, the input/output buffers, the protocol framer, and the event-loop registration needed to act as a specific kind of endpoint. The actor's reads and writes become part of the `listener` loop already running on its `VirtualCore`, so they never block the thread, and parsed messages and connection events are delivered as `on(...)` calls in the same single-threaded context as the actor's other events.

Because each `VirtualCore` owns its own `listener::current` loop, all I/O objects created through `use<>` are thread-affine: they must not be shared across cores. This is the source of qb-io's thread safety — isolation, not locks — and it shapes the architectural patterns below.

## Concepts

### The `qb::io::use<>` aliases

`qb::io::use<Derived>` exposes nested type aliases that attach async behavior to your class. The networking-relevant ones, verified against `qb/io/async.h`:

| Alias | Role |
| --- | --- |
| `use<D>::tcp::client<Server = void>` | Asynchronous TCP client / server-side session endpoint. |
| `use<D>::tcp::server<Session>` | Combined acceptor plus session pool for `Session`. |
| `use<D>::tcp::acceptor` | Listen-and-accept only; hands accepted sockets to `on(accepted_socket_type&&)`. |
| `use<D>::tcp::io_handler<Session>` | Session pool with no listener of its own. |
| `use<D>::tcp::ssl::client<Server = void>` / `::ssl::server<Session>` / `::ssl::acceptor` | SSL/TLS variants (compiled only when `QB_HAS_SSL` is defined). |
| `use<D>::udp::client` / `use<D>::udp::server` | Datagram endpoints (see [UDP endpoints](#udp-endpoints)). |
| `use<D>::timeout` | Inactivity-timeout mixin (`qb::io::async::with_timeout<D>`). |

The SSL aliases exist only under `#ifdef QB_HAS_SSL`. UDP servers are currently datagram-oriented: there is no per-peer session demultiplexing, so all datagrams funnel through the same `Derived` instance.

### What a networked actor gains

Inheriting from a `use<>` networking alias adds:

1. **`transport()`** — the underlying transport object (`qb::io::tcp::socket`, `qb::io::tcp::listener`, `qb::io::udp::socket`, or their SSL counterparts). This is where you call `listen()`, hold a connected socket, and query the peer endpoint.
2. **`in()` and `out()`** — `qb::allocator::pipe<char>` input and output buffers for the byte stream.
3. **A protocol framer** — driven by a nested `using Protocol = ...;` alias on your class. The framer cuts the inbound byte stream into messages and calls your `on(Protocol::message&&)`. Pure acceptors do not need a `Protocol`.
4. **Event-loop registration** — `start()` wires the transport's file descriptor into `listener::current` for read/write readiness.
5. **`on(...)` handlers** you implement to react to parsed messages, `qb::io::async::event::disconnected`, `qb::io::async::event::timer`, and (for acceptors) `on(accepted_socket_type&&)`.

### Sending and receiving

Once a connection is live and a `Protocol` is active:

- **Receive:** the framer calls `void on(Protocol::message&& msg)` (or `on(const Protocol::message&)`) for each complete message.
- **Send:** stream a value into the endpoint with `*this << value;`. `operator<<` forwards to `publish()` (see `qb/io/async/io.h`), which appends to `out()` and arms the write watcher. A custom protocol provides serialization by specializing `qb::allocator::pipe<char>::put<YourMessage>`, which lets you write `*this << my_message;` directly.

### Roles in a server

A TCP server is built from two roles that can live in one actor or be split across several:

- **Acceptor** — owns the listening socket, accepts new connections, and produces accepted sockets.
- **Session manager** — an `io_handler<Session>` that owns a pool of `Session` objects, one per connected client, and routes their I/O.

The `Session` class itself is a `use<Session>::tcp::client<Manager>`: from qb-io's perspective it is a client endpoint, but it is owned and driven by its managing actor rather than connecting outward.

## TCP client actor

The following client actor connects to a server, frames messages with a custom protocol, and reconnects on connection loss. It is derived from `examples/05-services/01-tcp-chat/client/ClientActor.{h,cpp}`, re-grounded against the current `connect` and timeout APIs. The reconnect delay below matches the shipped example: both of its retry paths now go through one `scheduleReconnect()` that waits inside the actor's cancellation scope and wakes the actor with a `ReconnectTickEvent`. They used to arm `qb::io::async::callback([this]{ … }, RECONNECT_DELAY)` at two sites with no liveness guard at all — that is the shape to move away from, for the reasons in [Capture safety](./async_in_actors.md#capture-safety-the-actor-may-be-gone). <!-- src: examples/05-services/01-tcp-chat/client/ClientActor.cpp:151-156 -->

```cpp
// src: examples/05-services/01-tcp-chat/client/ClientActor.h (adapted)
#include <qb/actor.h>
#include <qb/io/async.h>      // qb::io::use<>, qb::io::async::tcp::connect
#include <qb/io/uri.h>
#include <qb/io.h>            // qb::io::cout
#include <chrono>
#include "Protocol.h"          // chat::ChatProtocol<IO_>, chat::Message

using namespace std::chrono_literals;   // 5s literal; qb re-exports these via qb::time_literals

// Self-addressed: "the backoff has elapsed, try connecting again".
struct Reconnect : qb::Event {};

class ClientActor : public qb::Actor,
                    public qb::io::use<ClientActor>::tcp::client<> {
public:
    // The framer reads this alias; the base constructor activates it automatically.
    using Protocol = chat::ChatProtocol<ClientActor>;

    explicit ClientActor(qb::io::uri server_uri)
        : _server_uri(std::move(server_uri)) {}

    qb::io::async::task<bool> onInit() override {
        registerEvent<Reconnect>(*this);
        connect();
        co_return true;
    }

    // Parsed message from the server.
    void on(const chat::Message &msg) {
        qb::io::cout() << "server: " << msg.payload << "\n";
    }

    // Connection loss: reset state and schedule a delayed retry.
    void on(qb::io::async::event::disconnected const &) {
        _connected = false;
        if (_should_reconnect)
            arm_reconnect();
    }

    // The retry lands here — an ordinary handler, on an actor that is live by construction.
    void on(Reconnect const &) { connect(); }

private:
    // The delay belongs to this actor: kill() cancels the pending sleep, and the
    // coroutine captures no `this` — `ctx` carries the ActorId by value.
    void arm_reconnect() {
        const qb::duration delay = RECONNECT_DELAY;
        spawn([delay](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            co_await ctx.sleep(delay);
            ctx.template push<Reconnect>();
        });
    }

    void connect() {
        // connect() takes a qb::duration; a chrono literal satisfies it.
        qb::io::async::tcp::connect<qb::io::tcp::socket>(
            _server_uri,
            [this](qb::io::tcp::socket socket) {
                if (socket.is_open())
                    onConnected(std::move(socket));
                else if (_should_reconnect)
                    arm_reconnect();
            },
            CONNECT_TIMEOUT);
    }

    void onConnected(qb::io::tcp::socket &&socket) {
        _connected = true;
        this->transport() = std::move(socket);          // adopt the live socket
        this->template switch_protocol<Protocol>(*this); // activate the framer
        this->start();                                   // join the event loop
        *this << chat::Message{chat::MessageType::AUTH_REQUEST, "alice"};
    }

    const qb::io::uri _server_uri;
    bool _connected{false};
    bool _should_reconnect{true};

    static constexpr qb::duration CONNECT_TIMEOUT  = 5s;
    static constexpr qb::duration RECONNECT_DELAY  = 5s;
};
```

Points worth noting:

- `qb::io::async::tcp::connect<Socket>(uri, callback, timeout, verify_peer)` performs the non-blocking connect; `timeout` is a `qb::duration` and defaults to `qb::duration::zero()` (no deadline). The callback receives a `Socket` whose `is_open()` is `false` on failure or timeout. For SSL clients, instantiate `connect<qb::io::tcp::ssl::socket>`; `verify_peer` (default `true`) controls certificate-chain and hostname verification.
- After adopting the socket, call `switch_protocol<Protocol>(*this)` and then `start()`. `start()` registers the descriptor with the event loop; until it runs, no reads or writes are dispatched.
- Reconnection is scheduled with `spawn(...)` + `co_await ctx.sleep(delay)`, not with `qb::io::async::callback`. Both arm a real timer, but only the coroutine's is bound to the actor: `kill()` cancels it, whereas a `callback` timer is owned by the event loop and fires after the actor is gone. See [async operations in actors](./async_in_actors.md#capture-safety-the-actor-may-be-gone) for why the `is_alive()` guard people reach for cannot fix that.

> **Time API.** `connect`, `callback` and `ctx.sleep` all take a `std::chrono` duration (`qb::duration` is `std::chrono::nanoseconds`). A raw `double` does not convert to a chrono duration and will not compile — write `5s`, `250ms`, or an explicit `qb::duration` rather than a bare number.

## TCP server: combined acceptor and session pool

For a single-actor server, inherit from `use<Self>::tcp::server<Session>`. This base combines the acceptor and the `io_handler<Session>` pool: it accepts connections, constructs a `Session` per client, and (when present) calls your `on(Session&)` hook after registration.

```cpp
// src: qb/src/qb/io/async/tcp/server.h (base contract; pattern adapted)
#include <qb/actor.h>
#include <qb/io/async.h>
#include <qb/io/uri.h>
#include "MyClientSession.h"   // a use<MyClientSession>::tcp::client<MyServer>

class MyServer : public qb::Actor,
                 public qb::io::use<MyServer>::tcp::server<MyClientSession> {
public:
    explicit MyServer(qb::io::uri listen_at) : _listen_at(std::move(listen_at)) {}

    qb::io::async::task<bool> onInit() override {
        // transport().listen(uri) returns 0 on success (non-zero is an error).
        if (this->transport().listen(_listen_at) != 0) {
            qb::io::cerr() << "listen failed on " << _listen_at.source() << "\n";
            co_return false;
        }
        this->start();   // begin accepting connections
        qb::io::cout() << "listening on " << _listen_at.source() << "\n";
        co_return true;
    }

    // Optional: called by the base right after a session is registered and started.
    void on(MyClientSession &session) {
        qb::io::cout() << "client " << session.id() << " connected\n";
        session << "welcome\n";
    }

    void on(const qb::KillEvent &) {
        for (auto &[id, session] : this->sessions())
            if (session) session->disconnect();
        this->sessions().clear();
        this->kill();
    }
};
```

The session pool is reachable through `sessions()` (a `qb::unordered_map<qb::uuid, std::shared_ptr<Session>>`), `session(uuid)`, and `session_count()`, all declared in `qb/io/async/io_handler.h`. To cap concurrency, call `set_max_sessions(n)`; once the cap is reached, `registerSession()` closes the incoming socket and returns `nullptr` rather than allocating.

> **Two `listen` methods.** The raw transport's `transport().listen(uri)` returns `int` (`0` = success) and does **not** start the accept watcher, so you call `start()` yourself. The acceptor base also exposes a separate `[[nodiscard]] bool listen(uri, cert_file, key_file, alpn)` convenience that returns `true` on success and auto-starts; for SSL acceptors it also installs the server certificate. Pick one; do not mix them.

## TCP server: separate acceptor and session managers

To spread session handling across cores, split the two roles. An `AcceptActor` owns the listener and forwards each accepted socket, as a `qb::Event`, to one of several `ServerActor` session managers — which can run on different `VirtualCore`s. This is the architecture used by both `examples/05-services/01-tcp-chat` and `examples/05-services/02-pubsub-broker`.

```text
            External clients
                  |  connect
                  v
        +---------------------+
        | AcceptActor (VC0)   |   use<AcceptActor>::tcp::acceptor
        |  on(accepted&&)     |
        +----------+----------+
                   | push<NewSessionEvent>{socket}   (round-robin)
       +-----------+-----------+
       v                       v
+----------------+     +----------------+
| ServerActor    | ... | ServerActor    |   use<ServerActor>::tcp::io_handler<Session>
| (VC1)          |     | (VC2)          |
|  registerSession()   |  registerSession()
+-------+--------+     +-------+--------+
        | owns                 | owns
        v                      v
   Session (one per client)  ...           use<Session>::tcp::client<ServerActor>
```

### Acceptor actor

The acceptor listens and distributes. Verified against `examples/05-services/01-tcp-chat/server/AcceptActor.{h,cpp}`:

```cpp
// src: examples/05-services/01-tcp-chat/server/AcceptActor.h
class AcceptActor : public qb::Actor,
                    public qb::io::use<AcceptActor>::tcp::acceptor {
public:
    AcceptActor(qb::io::uri listen_at, qb::ActorIdList pool);

    qb::io::async::task<bool> onInit() override;
    void on(accepted_socket_type &&new_io);                // a new TCP connection
    void on(qb::io::async::event::disconnected const &);   // listener failure

private:
    const qb::io::uri      _listen_at;
    const qb::ActorIdList  _server_pool;
    std::size_t            _session_counter{0};
};
```

```cpp
// src: examples/05-services/01-tcp-chat/server/AcceptActor.cpp
qb::io::async::task<bool> AcceptActor::onInit() {
    if (_server_pool.empty()) {
        qb::io::cerr() << "empty server pool\n";
        co_return false;
    }
    if (this->transport().listen(_listen_at)) {  // non-zero == failure
        qb::io::cerr() << "cannot listen on " << _listen_at.source() << "\n";
        co_return false;
    }
    qb::io::cout() << "AcceptActor listening on " << _listen_at.source() << "\n";
    this->start();
    co_return true;
}

void AcceptActor::on(accepted_socket_type &&new_io) {
    // Round-robin the new socket to a session-managing ServerActor.
    auto server_id = _server_pool[_session_counter++ % _server_pool.size()];
    auto &evt = push<NewSessionEvent>(server_id);
    evt.socket = std::move(new_io);   // transfer socket ownership into the event
}

void AcceptActor::on(qb::io::async::event::disconnected const &) {
    // The listening socket itself failed — shut the system down.
    broadcast<qb::KillEvent>();
}
```

`accepted_socket_type` is `qb::io::tcp::socket` for a plain acceptor and `qb::io::tcp::ssl::socket` for an SSL acceptor (it is `_Prot::socket_type`). The accepted socket is moved into a `qb::Event` field — `NewSessionEvent { qb::io::tcp::socket socket; }` — to carry it, and ownership transfers because sockets are move-only.

> **Cross-core socket transfer is safe; sharing is not.** Moving a connected socket into an event and pushing it to another core hands the descriptor to that core's loop. After the move, the acceptor no longer touches it. Never keep a copy or call into a transport from a core other than the one whose `listener` owns it.

### Session-managing actor

A session manager inherits `use<Self>::tcp::io_handler<Session>` — a pool with no listener. It receives the accepted socket and calls `registerSession`, which constructs the `Session`, adopts the socket, and starts it. Verified against `examples/05-services/01-tcp-chat/server/ServerActor.{h,cpp}`:

```cpp
// src: examples/05-services/01-tcp-chat/server/ServerActor.h
class ServerActor : public qb::Actor,
                    public qb::io::use<ServerActor>::tcp::io_handler<ChatSession> {
public:
    explicit ServerActor(qb::ActorId chatroom_id);
    qb::io::async::task<bool> onInit() override;
    void on(NewSessionEvent &evt);
    void on(SendMessageEvent &evt);
    // ... delegated handlers called by ChatSession ...
};
```

```cpp
// src: examples/05-services/01-tcp-chat/server/ServerActor.cpp (re-grounded return type)
qb::io::async::task<bool> ServerActor::onInit() {
    registerEvent<NewSessionEvent>(*this);
    registerEvent<SendMessageEvent>(*this);
    co_return true;
}

void ServerActor::on(NewSessionEvent &evt) {
    // registerSession returns _Session* (nullptr if the session cap is hit).
    if (auto *session = this->registerSession(std::move(evt.socket)))
        qb::io::cout() << "registered session " << session->id() << "\n";
}

void ServerActor::on(SendMessageEvent &evt) {
    auto it = this->sessions().find(evt.session_id);
    if (it != this->sessions().end())
        *it->second << *evt.message;  // the event holds a shared_ptr — never a by-value
}                                     // std::string, which memcpy relocation cannot move
```

`registerSession(transport_io_type&&, args...)` returns a `Session*` (not a reference); it is `nullptr` when the session limit is reached, so null-check it. Extra arguments are forwarded to the `Session` constructor after the managing actor reference. To reclaim a live descriptor — for example, to hand a connection to another handler during a protocol upgrade — use `extractSession(uuid)`, which removes the session and returns `{transport_io_type, bool}`.

> **Override `disconnected` carefully.** `io_handler::disconnected(uuid)` erases the session from the pool, releasing the last `shared_ptr`. If you override it to add cleanup logic, you **must** forward to the base (`io_handler<Self, Session>::disconnected(id);`) or the session is never removed and its `shared_ptr` leaks.

### Session class

The `Session` handles one client. It is a server-side `client<Manager>`: a `use<Session>::tcp::client<ServerActor>` endpoint owned by its manager. It may also mix in `use<Session>::timeout` for inactivity handling. Verified against `examples/05-services/01-tcp-chat/server/ChatSession.{h,cpp}`:

```cpp
// src: examples/05-services/01-tcp-chat/server/ChatSession.h
class ServerActor;   // forward declaration — the managing actor

class ChatSession : public qb::io::use<ChatSession>::tcp::client<ServerActor>,
                    public qb::io::use<ChatSession>::timeout {
public:
    using Protocol = chat::ChatProtocol<ChatSession>;

    explicit ChatSession(ServerActor &server);
    ~ChatSession();

    void on(const chat::Message &msg);                     // parsed message
    void on(qb::io::async::event::disconnected const &);   // client dropped
    void on(qb::io::async::event::timer const &);          // inactivity timeout
};
```

```cpp
// src: examples/05-services/01-tcp-chat/server/ChatSession.cpp
ChatSession::ChatSession(ServerActor &server)
    : client(server) {
    this->template switch_protocol<Protocol>(*this);
    this->setTimeout(std::chrono::seconds(120));   // setTimeout takes a qb::duration
}

void ChatSession::on(const chat::Message &msg) {
    // Delegate application logic to the managing actor via server().
    this->server().handleChat(this->id(), msg.payload);
    this->updateTimeout();   // reset the inactivity timer on activity
}

void ChatSession::on(qb::io::async::event::disconnected const &) {
    this->server().handleDisconnect(this->id());
}

void ChatSession::on(qb::io::async::event::timer const &) {
    this->disconnect();      // idle too long — drop the client
}
```

Inside a session, `server()` returns the managing actor (`ServerActor&`) and `id()` returns the session's `qb::uuid const&`. The inactivity timer comes from the `use<>::timeout` mixin (`qb::io::async::with_timeout`): `setTimeout(qb::duration)` arms or re-arms the timer, `updateTimeout()` takes no argument and resets the countdown to "now" on activity, and `getTimeout()` returns the configured `qb::duration` (zero when disabled). A session delegates application decisions back to its manager rather than reaching across the actor boundary itself.

## SSL/TLS endpoints

The SSL aliases mirror the plain ones and are available when the build defines `QB_HAS_SSL`:

- Client: `qb::io::use<Self>::tcp::ssl::client<>`, connected with `qb::io::async::tcp::connect<qb::io::tcp::ssl::socket>(...)`. `verify_peer` defaults to `true`.
- Acceptor / server: `qb::io::use<Self>::tcp::ssl::acceptor` and `::ssl::server<Session>`. The acceptor's `bool listen(uri, cert_file, key_file, alpn)` overload installs the server certificate; for SSL acceptors it builds the context from `cert_file` and `key_file` and fails (returns `false`) if the certificate cannot be loaded.
- `accepted_socket_type` for an SSL acceptor is `qb::io::tcp::ssl::socket`.

See [SSL/TLS transport](../3_qb_io/ssl_transport.md) for context creation, certificate handling, and the handshake model.

## UDP endpoints

`qb::io::use<Self>::udp::server` and `::udp::client` provide datagram endpoints. Unlike TCP, the UDP server is datagram-oriented with no built-in per-peer session pool: every datagram is delivered to the same `Derived` instance. If you need per-peer state, key your own map on `qb::io::transport::udp::identity` inside your handler. UDP is all-or-nothing per datagram — a datagram larger than `qb::io::udp::socket::MaxDatagramSize` (65507) is rejected with `EMSGSIZE`, never truncated. See [transports](../3_qb_io/transports.md) for the UDP transport details.

## Pitfalls

- **Sharing I/O across cores.** Every `use<>` object is bound to the `listener` of the core that created it. Move sockets between cores via events; never share a transport, session, or buffer across threads.
- **Forgetting `start()`.** Adopting a socket or calling `listen()` on the raw transport is not enough — `start()` registers the descriptor with the event loop. Without it, no reads, writes, or accepts fire. The acceptor's `bool listen(...)` convenience auto-starts; the raw `transport().listen()` does not.
- **Passing a `double` where a duration is expected.** `connect`, `callback`, and `setTimeout` take `std::chrono`/`qb::duration`. A bare `5.0` will not compile; write `5s` or `250ms`.
- **Binding `registerSession` to a reference.** It returns `Session*` (nullable at the session cap). Use `auto *session = registerSession(...)` and null-check; an `auto&` binding does not compile.
- **Overriding `io_handler::disconnected` without forwarding.** Omitting the base call leaks the session's `shared_ptr` because it is never erased from the pool. Always call the base `disconnected(id)`.
- **Ignoring `on(qb::io::async::event::disconnected const&)`.** Clients and sessions should handle it to reset state and (for clients) schedule reconnection. Acceptors should treat listener disconnection as fatal, typically `broadcast<qb::KillEvent>()`.
- **A reconnect timer that captures `this` and re-enters a dead actor.** `qb::io::async::callback` keeps no claim on the actor: a 5-second retry armed from `on(disconnected&)` fires whenever the loop says so, and by then the actor is typically destroyed — `VirtualCore` reaps in the same or the next loop turn, seconds before the timer. An `is_alive()` guard inside the closure does **not** rescue this: `is_alive()` reads an actor member, so on a destroyed actor evaluating the guard is itself the use-after-free. Use a timer whose lifetime is the actor's — `spawn(...)` + `co_await ctx.sleep(delay)` + a self-addressed event, as in the client above, or a `scoped_callback` handle held as a member so the actor's own destructor cancels the watcher. See [Capture safety](./async_in_actors.md#capture-safety-the-actor-may-be-gone) and [Error handling](../6_guides/error_handling.md#fire-and-forget-callbacks-outlive-their-captures). <!-- src: qb/src/qb/core/Actor.cpp:205-208, qb/src/qb/io/async/io.h:312-318,343 -->
- **Reconnecting from inside the handler that is destroying the connection.** If the retry path frees and recreates the connection object the handler is running on, do not run it inline — `qb::io::async::callback(fn)` with no delay does exactly that. Use `qb::io::async::defer(fn)`, which runs at the tail of the loop turn, after the handler unwinds. <!-- src: qb/src/qb/io/async/listener.h:1032 -->
- **Leaking on shutdown.** In a `qb::KillEvent` handler, disconnect or clear sessions and close listeners before `kill()`. RAII closes descriptors, but an orderly drain avoids resetting live clients abruptly.

## See also

- [Async operations in actors](./async_in_actors.md) — `qb::io::async::callback`, `with_timeout`, and deferred work inside actors.
- [qb-io transports](../3_qb_io/transports.md) — TCP, UDP, and Unix-socket transport details.
- [qb-io protocols](../3_qb_io/protocols.md) — writing an `AProtocol` and serializing messages.
- [SSL/TLS transport](../3_qb_io/ssl_transport.md) — secure context setup and handshakes.
- [chat_tcp walkthrough](./examples/chat_tcp_analysis.md) and [message_broker walkthrough](./examples/message_broker_analysis.md) — the full multi-actor server architecture in context.
