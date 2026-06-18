# TCP chat: an annotated walkthrough

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (C++20 default, C++23 supported)

A line-by-line reading of the `chat_tcp` example: how a multi-actor TCP server accepts connections, frames a custom binary protocol, manages per-client sessions, and centralizes chat state, paired with a reconnecting client.

**Prerequisites:** [Network-enabled actors](../network_actors.md), [Async I/O inside actors](../async_in_actors.md) — **See also:** [Message broker analysis](./message_broker_analysis.md), [Custom protocols](../../3_qb_io/protocols.md)

## Summary

The example lives at `examples/core_io/chat_tcp/` and builds two executables, a server and a client, that share one protocol definition. It is a working reference for five concerns that recur in every networked qb application:

- accepting TCP connections and handing them off across cores (`qb::io::use<T>::tcp::acceptor`);
- managing a pool of per-client sessions inside one actor (`qb::io::use<T>::tcp::io_handler<Session>`);
- framing a custom binary message format (`qb::io::async::AProtocol<IO_>` plus a `qb::allocator::pipe<char>::put<T>` specialization);
- centralizing shared application state in a single-owner actor (`ChatRoomActor`);
- a client that connects asynchronously and reconnects on failure (`qb::io::async::tcp::connect`, `qb::io::async::callback`).

The directory layout:

```text
examples/core_io/chat_tcp/
├── shared/        Protocol.h, Protocol.cpp, Events.h  (linked into both binaries)
├── server/        AcceptActor, ServerActor, ChatSession, ChatRoomActor, main.cpp
└── client/        ClientActor, InputActor, main.cpp
```

<!-- src: examples/core_io/chat_tcp/CMakeLists.txt -->

> **A note on the source.** The example predates the DoS-hardening change that made `io_handler::registerSession()` return a nullable pointer rather than a reference (qb-io commit *protocol-switch UAF guard, DoS and fail-closed hardening*). This page documents the current framework contract and flags where the checked-in example uses the older signature.

## Architecture

The server splits four responsibilities across cores. Connection acceptance, session I/O, and chat logic each run on their own `VirtualCore`, so a burst of accepts never stalls message broadcasting and vice versa.

```text
        TCP clients
             │  connect to :3001 / :3002
             ▼
   ┌──────────────────────┐   core 0
   │  AcceptActor ×2       │   listen + round-robin
   └──────────┬───────────┘
              │  NewSessionEvent (carries the accepted socket)
              ▼
   ┌──────────────────────┐   core 1
   │  ServerActor ×2       │   owns a map of ChatSession objects
   │   └─ ChatSession ×N   │   protocol parse + per-client timeout
   └──────────┬───────────┘
              │  AuthEvent / ChatEvent / DisconnectEvent
              ▼
   ┌──────────────────────┐   core 3
   │  ChatRoomActor        │   usernames, presence, broadcast
   └──────────┬───────────┘
              │  SendMessageEvent (routed back to the owning ServerActor)
              ▼
        back to ChatSession ──► client socket
```

The actual core placement is set in `server/main.cpp`: `ChatRoomActor` on core 3, two `ServerActor`s on core 1, and two `AcceptActor`s on core 0 listening on ports 3001 and 3002. There is no actor on core 2; the numbering leaves headroom.

<!-- src: examples/core_io/chat_tcp/server/main.cpp -->

```cpp
// src: examples/core_io/chat_tcp/server/main.cpp
qb::Main engine;

auto chatroom_id = engine.addActor<ChatRoomActor>(3);

auto server_ids = engine.core(1).builder()
                      .addActor<ServerActor>(chatroom_id)
                      .addActor<ServerActor>(chatroom_id)
                      .idList();

engine.core(0).builder()
    .addActor<AcceptActor>(qb::io::uri{"tcp://0.0.0.0:3001"}, server_ids)
    .addActor<AcceptActor>(qb::io::uri{"tcp://0.0.0.0:3002"}, server_ids);

engine.start(true);   // asynchronous start; the calling thread continues
std::cin.get();        // block on stdin until the operator presses Enter
engine.stop();
engine.join();
```

`engine.core(1).builder()` is the fluent multi-actor form of `addActor`; `idList()` returns a `qb::ActorIdList` (an alias for `std::vector<qb::ActorId>`) holding the IDs of the two `ServerActor`s, which is then passed to each `AcceptActor` for round-robin dispatch.

## The shared protocol

Both binaries link `shared/Protocol.{h,cpp}`. The wire format is a fixed 8-byte header followed by a UTF-8 payload.

```cpp
// src: examples/core_io/chat_tcp/shared/Protocol.h
struct MessageHeader {
    uint16_t magic;    // 'QC' == 0x5143
    uint8_t  version;  // 0x01
    uint8_t  type;     // chat::MessageType
    uint32_t length;   // payload byte count
};

constexpr uint16_t PROTOCOL_MAGIC   = 0x5143;
constexpr uint8_t  PROTOCOL_VERSION = 0x01;

enum class MessageType : uint8_t {
    AUTH_REQUEST = 1,  // client -> server: join with a username
    AUTH_RESPONSE,     // server -> client: authentication result
    CHAT_MESSAGE,      // bidirectional: a chat line
    USER_LIST,         // server -> client: active users (declared, unused here)
    ERROR              // server -> client: error notification
};

struct Message {
    MessageType type;
    std::string payload;
};
```

### Parsing: `ChatProtocol<IO_>`

`ChatProtocol` derives from `qb::io::async::AProtocol<IO_>` and implements the three pure-virtual members the framework calls (`getMessageSize`, `onMessage`, `reset`), plus the `using message = Message;` alias the I/O layer expects.

The framework drives parsing in two phases. After each read it calls `getMessageSize()`; the protocol inspects the input buffer (`this->_io.in()`) and returns either `0` ("need more bytes") or the total size of one complete message. When a non-zero size is returned and that many bytes are buffered, the framework calls `onMessage(size)`, which reconstructs the `Message` and dispatches it to the owning I/O object via `this->_io.on(msg)`.

```cpp
// src: examples/core_io/chat_tcp/shared/Protocol.h
std::size_t getMessageSize() noexcept override {
    auto& buffer = this->_io.in();
    if (buffer.empty()) return 0;

    if (_reading_header) {
        if (buffer.size() < HEADER_SIZE) return 0;           // header not complete

        std::memcpy(&_header, buffer.cbegin(), HEADER_SIZE);
        if (_header.magic != PROTOCOL_MAGIC ||
            _header.version != PROTOCOL_VERSION) {
            reset();                                          // bad frame: resynchronize
            return 0;
        }

        _reading_header = false;
        _payload.resize(_header.length);
        return HEADER_SIZE + _header.length;
    }
    return HEADER_SIZE + _header.length;
}
```

Returning `0` is the framework's sentinel for "incomplete"; it leaves the buffered bytes untouched and retries on the next read. `onMessage()` copies the payload out, builds a `Message`, hands it to `this->_io.on(msg)`, and sets `_reading_header = true` for the next frame.

> **Resynchronization caveat.** On a bad magic or version, `reset()` clears the parse state but does not discard bytes from the input buffer. Because `getMessageSize()` then returns `0`, the same malformed bytes are re-examined on the next read and the connection cannot recover. This is acceptable for an example over a trusted protocol; a production parser should consume or close the stream on a framing error. See [Custom protocols](../../3_qb_io/protocols.md) for the framing contract.

### Serialization: a `pipe<char>::put<chat::Message>` specialization

Sending is symmetric. `shared/Protocol.cpp` specializes `qb::allocator::pipe<char>::put` for `chat::Message`, so any I/O object can write a message with `*this << msg;`. The `operator<<` on a qb-io object forwards to `publish()`, which appends to the output pipe; the specialization decides the bytes.

```cpp
// src: examples/core_io/chat_tcp/shared/Protocol.cpp
namespace qb::allocator {
template<>
pipe<char>& pipe<char>::put<chat::Message>(const chat::Message& msg) {
    chat::MessageHeader header{
        chat::PROTOCOL_MAGIC,
        chat::PROTOCOL_VERSION,
        static_cast<uint8_t>(msg.type),
        static_cast<uint32_t>(msg.payload.size())
    };
    this->put(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!msg.payload.empty())
        this->put(msg.payload.data(), msg.payload.size());
    return *this;
}
} // namespace qb::allocator
```

`ChatProtocol` defines no `end` sentinel: the header's length field frames the message, so the framework writes exactly the bytes `put` appended. Code in this example sends with a plain `*this << msg;` — there is no `<< Protocol::end`.

### Events

`shared/Events.h` declares the typed `qb::Event` structs that flow between actors. The notable ones:

- `NewSessionEvent` carries a `qb::io::tcp::socket` by value, transferring ownership of the accepted connection from `AcceptActor` to a `ServerActor`.
- `AuthEvent` / `ChatEvent` carry a `qb::uuid session_id` plus a `qb::string<32>` username or `qb::string<256>` message — fixed-capacity inline strings that avoid a heap allocation per event.
- `SendMessageEvent` carries a `qb::uuid session_id` and a `chat::Message message` to deliver to one client.
- `DisconnectEvent` carries the `qb::uuid` of the closed session.
- `ChatInputEvent` (client side) carries one line of console input as a `qb::string<256>`.

## Server walkthrough

### `AcceptActor` — listen and dispatch

`AcceptActor` mixes `qb::Actor` with `qb::io::use<AcceptActor>::tcp::acceptor`. The acceptor base owns a `qb::io::tcp::listener` (reached through `transport()`) and raises `on(accepted_socket_type&&)` for each completed connection. `accepted_socket_type` resolves to `qb::io::tcp::socket`.

```cpp
// src: examples/core_io/chat_tcp/server/AcceptActor.cpp
bool AcceptActor::onInit() {
    if (_server_pool.empty()) {
        qb::io::cerr() << "Cannot init AcceptActor with empty server pool" << std::endl;
        return false;
    }
    if (transport().listen(_listen_at)) {            // listener::listen returns int; non-zero == failure
        qb::io::cerr() << "Cannot listen on " << _listen_at.source() << std::endl;
        return false;
    }
    qb::io::cout() << "AcceptActor listening on " << _listen_at.source() << std::endl;
    start();                                         // arm the accept watcher on this core's event loop
    return true;
}
```

Two details correct common misreadings:

- The actor listens on **one** URI, stored as `const qb::io::uri _listen_at`. Two listening ports come from constructing two separate `AcceptActor`s in `main.cpp`, not from one actor binding a list.
- `transport().listen()` is the low-level `qb::io::tcp::listener::listen(uri)`, which returns `int` (`0` on success). The example calls it directly and then `start()` separately. The acceptor mixin also offers a higher-level `bool listen(...)` that auto-arms the watcher; this example does not use it.

Dispatch is round-robin over the server pool, advancing a counter per accept:

```cpp
// src: examples/core_io/chat_tcp/server/AcceptActor.cpp
void AcceptActor::on(accepted_socket_type&& new_io) {
    auto server_id = _server_pool[_session_counter++ % _server_pool.size()];
    auto& evt = push<NewSessionEvent>(server_id);
    evt.socket = std::move(new_io);                  // move the socket into the cross-core event
}

void AcceptActor::on(qb::io::async::event::disconnected const&) {
    qb::io::cout() << "AcceptActor disconnected" << std::endl;
    broadcast<qb::KillEvent>();                       // listener died: bring the system down
}
```

Moving the socket into `NewSessionEvent` is what lets the connection travel from core 0 to core 1 safely: the event owns the file descriptor until the `ServerActor` takes it.

### `ServerActor` — a pool of sessions

`ServerActor` mixes `qb::Actor` with `qb::io::use<ServerActor>::tcp::io_handler<ChatSession>`. The `io_handler` base owns a `qb::unordered_map<qb::uuid, std::shared_ptr<ChatSession>>` reached through `sessions()`, and provides `registerSession()` / `unregisterSession()` / `extractSession()`.

> `io_handler` is the session-pool half of the server stack. `qb::io::use<T>::tcp::server<Session>` bundles an acceptor *and* an `io_handler` in one actor; this example keeps them separate (`AcceptActor` + `ServerActor`) so acceptance and session I/O run on different cores.

On a `NewSessionEvent`, the actor adopts the socket into a fresh session. The current framework contract is shown below; the checked-in example uses the older reference-returning form (see the API-drift note that follows):

```cpp
// current-API pattern; cf. examples/core_io/chat_tcp/server/ServerActor.cpp (older signature)
void ServerActor::on(NewSessionEvent& evt) {
    auto* session = registerSession(std::move(evt.socket));
    if (session)                                      // nullptr when the session cap is reached
        qb::io::cout() << "New session registered: " << session->id() << std::endl;
}
```

`registerSession()` checks the session cap first, then constructs the `ChatSession`, inserts it into the map keyed by the session's `qb::uuid`, moves the socket into it, calls the session's `start()`, and returns a `ChatSession*`. It returns `nullptr` when a configured session cap is hit (default: unlimited — `QB_DEFAULT_MAX_SESSIONS` is `0`) or on an ID collision, closing the incoming socket in either case; the caller must null-check. <!-- src: qb/include/qb/io/async/io_handler.h:209 -->

> **API drift.** The checked-in example binds `auto& session = registerSession(...)`, matching an earlier signature that returned a reference. Against current qb-io the return type is `ChatSession*`, so the null-checked pointer form above is the correct pattern to copy. See `qb/include/qb/io/async/io_handler.h`.

`ServerActor` is the bridge between sessions and the room. Sessions call back into it (`server().handleAuth(...)`, `handleChat(...)`, `handleDisconnect(...)`), and it translates those into events for `ChatRoomActor`:

```cpp
// src: examples/core_io/chat_tcp/server/ServerActor.cpp
void ServerActor::handleAuth(qb::uuid session_id, const std::string& username) {
    auto& evt = push<AuthEvent>(_chatroom_id);
    evt.session_id = session_id;
    evt.username   = username;
}
```

In the other direction, `ChatRoomActor` sends `SendMessageEvent`s back. `ServerActor` looks up the target session in its own map and writes the message through it:

```cpp
// src: examples/core_io/chat_tcp/server/ServerActor.cpp
void ServerActor::on(SendMessageEvent& evt) {
    auto it = sessions().find(evt.session_id);
    if (it != sessions().end()) {
        *it->second << evt.message;     // serialize via the pipe<char>::put<chat::Message> specialization
        it->second->updateTimeout();    // outbound traffic also counts as activity
    }
}
```

`onInit()` registers only the two events this actor receives — `NewSessionEvent` and `SendMessageEvent`. Events it merely *sends* need no registration.

### `ChatSession` — one client, protocol plus timeout

`ChatSession` composes two qb-io mixins:

```cpp
// src: examples/core_io/chat_tcp/server/ChatSession.h
class ChatSession
    : public qb::io::use<ChatSession>::tcp::client<ServerActor>,
      public qb::io::use<ChatSession>::timeout {
public:
    using Protocol = chat::ChatProtocol<ChatSession>;
    // ...
};
```

- `tcp::client<ServerActor>` makes the session the server-side endpoint of a client connection. The `ServerActor` template parameter is the owner type: `server()` returns the managing `ServerActor&`, which is how the session delegates application logic.
- `timeout` is `qb::io::async::with_timeout<ChatSession>`. It schedules a one-shot timer; if `updateTimeout()` is not called before it fires, the framework invokes `on(qb::io::async::event::timer const&)`.

The constructor wires the protocol and arms the idle timer:

```cpp
// src: examples/core_io/chat_tcp/server/ChatSession.cpp
ChatSession::ChatSession(ServerActor& server)
    : client(server) {
    this->template switch_protocol<Protocol>(*this);
    this->setTimeout(std::chrono::seconds(120));   // converts to qb::duration; idle clients drop after 120 s
    qb::io::cout() << "New chat client connected" << std::endl;
}
```

`setTimeout` takes a `qb::duration` (a `std::chrono::nanoseconds` span); `std::chrono::seconds(120)` converts implicitly. Parsed messages route by type, and every received frame resets the idle timer:

```cpp
// src: examples/core_io/chat_tcp/server/ChatSession.cpp
void ChatSession::on(const chat::Message& msg) {
    switch (msg.type) {
        case chat::MessageType::AUTH_REQUEST:
            this->server().handleAuth(this->id(), msg.payload);
            break;
        case chat::MessageType::CHAT_MESSAGE:
            this->server().handleChat(this->id(), msg.payload);
            break;
        default:
            qb::io::cerr() << "Unknown message type: "
                           << static_cast<int>(msg.type) << std::endl;
            break;
    }
    this->updateTimeout();
}

void ChatSession::on(qb::io::async::event::disconnected const&) {
    this->server().handleDisconnect(this->id());     // notify the room via the owning ServerActor
}

void ChatSession::on(qb::io::async::event::timer const&) {
    this->disconnect();                              // idle too long: close; disconnected fires next
}
```

`this->id()` is the session's `qb::uuid` — the same key the `io_handler` map uses and the identifier carried in every event.

### `ChatRoomActor` — single-owner application state

`ChatRoomActor` is a plain `qb::Actor` with no I/O mixins. It owns the only authoritative copy of chat state:

```cpp
// src: examples/core_io/chat_tcp/server/ChatRoomActor.h
std::map<qb::uuid, SessionInfo>  _sessions;   // session_id -> { owning ServerActor id, username }
std::map<std::string, qb::uuid>  _usernames;  // username   -> session_id (duplicate check)
```

Because one actor processes its events sequentially, no locks guard these maps: that is the actor model's guarantee in practice. Authentication validates username uniqueness, registers the user, replies to the joining client, and announces the arrival to everyone:

```cpp
// src: examples/core_io/chat_tcp/server/ChatRoomActor.cpp
void ChatRoomActor::on(AuthEvent& evt) {
    auto session_id = evt.session_id;
    auto username   = evt.username;
    auto server_id  = evt.getSource();               // the ServerActor that forwarded this event

    if (_usernames.find(username) != _usernames.end()) {
        sendError(session_id, server_id, "Username already taken");
        return;
    }

    _sessions[session_id] = SessionInfo{server_id, username};
    _usernames[username]  = session_id;

    chat::Message response;
    response.type    = chat::MessageType::AUTH_RESPONSE;
    response.payload = "Welcome " + std::string(username);
    sendToSession(session_id, server_id, response);

    broadcastMessage(std::string(username) + " has joined the chat");
}
```

`evt.getSource()` is how the room learns which `ServerActor` owns the session, so replies route back to the right core. `broadcastMessage` builds one `CHAT_MESSAGE` and pushes a `SendMessageEvent` per recipient, each addressed to that recipient's owning `ServerActor`:

```cpp
// src: examples/core_io/chat_tcp/server/ChatRoomActor.cpp
void ChatRoomActor::broadcastMessage(const std::string& content) {
    chat::Message msg;
    msg.type    = chat::MessageType::CHAT_MESSAGE;
    msg.payload = content;
    for (const auto& [session_id, info] : _sessions)
        sendToSession(session_id, info.server_id, msg);
}
```

`on(ChatEvent&)` formats `"username: text"` and broadcasts it; `on(DisconnectEvent&)` erases the user from both maps and announces the departure. Chat broadcasts reuse `CHAT_MESSAGE`; there is no separate broadcast message type.

## Client walkthrough

### `InputActor` — console input off the I/O path

`InputActor` mixes `qb::Actor` with `qb::ICallback`. `onInit()` calls `registerCallback(*this)`; thereafter the engine invokes `onCallback()` once per loop iteration on the actor's core.

```cpp
// src: examples/core_io/chat_tcp/client/InputActor.cpp
void InputActor::onCallback() {
    std::string line;
    std::getline(std::cin, line);                    // see the blocking note below

    if (line == "quit") {
        push<qb::KillEvent>(_client_id);             // shut down the ClientActor
        push<qb::KillEvent>(id());                   // and self
        return;
    }
    if (!line.empty()) {
        auto& evt = push<ChatInputEvent>(_client_id);
        evt.message = std::move(line);
    }
}
```

> **Blocking-I/O caveat.** `std::getline(std::cin, ...)` blocks the calling `VirtualCore` until a line arrives, which stalls that core's event loop. Placing `InputActor` on its own core (core 0) keeps the blocking out of the network path, but it is not truly non-blocking. A production client would use platform non-blocking console reads or a dedicated reader thread that messages the actor. The example keeps it simple deliberately.

### `ClientActor` — connect, authenticate, reconnect

`ClientActor` mixes `qb::Actor` with `qb::io::use<ClientActor>::tcp::client<>`. The empty `<>` (owner type `void`) is the standalone client form: there is no owning `io_handler`, so `server()` is not available — unlike `ChatSession`, which names `ServerActor` as its owner.

`onInit()` registers the input event and kicks off the first connect. Connection is asynchronous via `qb::io::async::tcp::connect`. The timeout argument is a `qb::duration` (a `std::chrono::nanoseconds` span), so pass a chrono value, not a bare `double`:

```cpp
// current-API pattern; cf. examples/core_io/chat_tcp/client/ClientActor.cpp (passes a double)
void ClientActor::connect() {
    qb::io::async::tcp::connect<qb::io::tcp::socket>(
        _server_uri,
        [this](qb::io::tcp::socket socket) {
            if (socket.is_open())
                onConnected(std::move(socket));
            else
                onConnectionFailed();
        },
        std::chrono::seconds(5));                     // connect deadline
}
```

The callback receives the connected socket by value; an open socket means success.

> **Framework contract vs. the example.** `qb::io::async::tcp::connect()` takes its timeout as a `qb::duration` (`std::chrono::nanoseconds`, per [the canonical time model](../../7_reference/glossary.md)), and `qb::io::async::callback()` takes any `std::chrono::duration`. The checked-in example stores both deadlines as `static constexpr double CONNECT_TIMEOUT = 5.0;` / `RECONNECT_DELAY = 5.0;` and passes them directly to `connect()` and `callback()` — a form that predates the time migration and no longer compiles, because a bare `double` does not convert to `qb::duration`. New code should pass a chrono literal such as `std::chrono::seconds(5)`, as shown above.

`onConnected` adopts the socket into the client transport, switches on the protocol, and starts the I/O before authenticating:

```cpp
// src: examples/core_io/chat_tcp/client/ClientActor.cpp
void ClientActor::onConnected(qb::io::tcp::socket&& socket) {
    _connected = true;

    this->transport().close();                       // reset any prior state
    this->in().reset();
    this->out().reset();
    this->transport() = std::move(socket);           // adopt the freshly connected socket
    this->template switch_protocol<Protocol>(*this);
    this->start();                                   // arm read/write on this core's loop

    authenticate();
}

void ClientActor::authenticate() {
    chat::Message auth;
    auth.type    = chat::MessageType::AUTH_REQUEST;
    auth.payload = _username;
    *this << auth;                                   // serialized by the pipe specialization
}
```

The transport is assigned directly (`this->transport() = std::move(socket)`), not nested through a second `transport()` call. Inbound server messages are handled by type — `AUTH_RESPONSE` flips the `_authenticated` flag, `CHAT_MESSAGE` prints, `ERROR` reports:

```cpp
// src: examples/core_io/chat_tcp/client/ClientActor.cpp
void ClientActor::on(const chat::Message& msg) {
    switch (msg.type) {
        case chat::MessageType::AUTH_RESPONSE:
            qb::io::cout() << "Server: " << msg.payload << std::endl;
            _authenticated = true;
            break;
        case chat::MessageType::CHAT_MESSAGE:
            qb::io::cout() << msg.payload << std::endl;
            break;
        case chat::MessageType::ERROR:
            qb::io::cerr() << "Error: " << msg.payload << std::endl;
            break;
        default:
            break;
    }
}
```

Reconnection uses `qb::io::async::callback` to retry after a fixed delay rather than spin:

```cpp
// current-API pattern; cf. examples/core_io/chat_tcp/client/ClientActor.cpp (passes a double)
void ClientActor::on(qb::io::async::event::disconnected const&) {
    _connected     = false;
    _authenticated = false;
    if (_should_reconnect)
        qb::io::async::callback([this]() { connect(); }, std::chrono::seconds(5));
}
```

The connect deadline and the reconnect delay are both fixed at five seconds; the delay is constant, not exponential backoff. The `_should_reconnect` flag is cleared only by the actor's own `disconnect()` method, which is not invoked on shutdown in this example — when `InputActor` sends `qb::KillEvent`, the framework's default kill handling tears the actor down.

## Lifecycle, end to end

1. The server starts: `ChatRoomActor`, two `ServerActor`s, and two `AcceptActor`s come up; each `AcceptActor` binds its port and arms its accept watcher.
2. A client starts: `ClientActor::connect()` runs; on success it adopts the socket, switches on `ChatProtocol`, and sends `AUTH_REQUEST` with its username.
3. The accepting `AcceptActor` raises `on(accepted_socket_type&&)`, picks the next `ServerActor` round-robin, and forwards the socket in a `NewSessionEvent`.
4. That `ServerActor` calls `registerSession()`, creating a `ChatSession` that owns the socket, runs `ChatProtocol`, and holds a 120-second idle timer.
5. `ChatProtocol` parses `AUTH_REQUEST`; `ChatSession::on(Message)` calls `server().handleAuth(...)`, which pushes an `AuthEvent` to `ChatRoomActor`.
6. `ChatRoomActor` registers the user, replies via `SendMessageEvent` → the owning `ServerActor` → the client's `ChatSession`, and broadcasts a join line to every session.
7. Chat lines follow the same path: `CHAT_MESSAGE` → `ChatEvent` → `ChatRoomActor` formats and broadcasts → `SendMessageEvent`s fan back out.
8. On disconnect or idle timeout, the `ChatSession` notifies its `ServerActor`, which pushes a `DisconnectEvent`; `ChatRoomActor` cleans up its maps and announces the departure. The client, if still running, schedules a reconnect.
9. The operator presses Enter at the server console; `engine.stop()` then `engine.join()` drains and joins all cores.

## Pitfalls

- **`registerSession()` returns a nullable pointer.** Treat it as `ChatSession*` and null-check it; the cap-exceeded and collision paths return `nullptr` after closing the incoming socket. The checked-in example's reference binding reflects the older API.
- **One `AcceptActor` binds one URI.** Multiple ports mean multiple `AcceptActor`s. Do not expect a single actor to accept a list of addresses.
- **`transport().listen()` is the low-level call.** It returns `int` (`0` == success) and does not arm the watcher; call `start()` afterward. The mixin's higher-level `bool listen(...)` auto-arms — choose one, not both.
- **No `Protocol::end` here.** Length-prefixed framing means `*this << msg;` sends a complete message. A trailing `<< Protocol::end` belongs to delimiter-based protocols, not this one.
- **`setTimeout` is on the timeout mixin, not the client.** `updateTimeout()` must be called on every meaningful activity (inbound *and* outbound), or the session drops mid-conversation. The example resets it in both `ChatSession::on(Message)` and `ServerActor::on(SendMessageEvent&)`.
- **Console input blocks its core.** Keep `std::getline`-style readers off any core that carries network actors. The example isolates `InputActor` on core 0 for exactly this reason.
- **Bad-frame handling is minimal.** `ChatProtocol::reset()` does not drain the buffer, so a malformed header wedges the parser. Harden framing before reuse.
- **Timeouts are `qb::duration` now, not `double`.** The checked-in client passes `static constexpr double` constants (`CONNECT_TIMEOUT`, `RECONNECT_DELAY`, both `5.0`) to `qb::io::async::tcp::connect()` and `qb::io::async::callback()`. That predates the canonical time model and no longer compiles — a bare `double` does not convert to `qb::duration`. Pass a chrono literal such as `std::chrono::seconds(5)`. See [Async I/O inside actors](../async_in_actors.md). <!-- src: qb/include/qb/system/timestamp.h:82 -->

## See also

- [Network-enabled actors](../network_actors.md) — the `qb::io::use<>` mixins (`acceptor`, `io_handler`, `server`, `client`) in depth.
- [Async I/O inside actors](../async_in_actors.md) — `qb::io::async::callback`, timers, and the event loop.
- [Custom protocols](../../3_qb_io/protocols.md) — the `AProtocol` framing contract and `pipe<char>::put<T>` serialization.
- [Message broker analysis](./message_broker_analysis.md) — a related multi-actor networking example.
