# Publish/subscribe message broker: an annotated walkthrough

> **Audience:** Adopter · **Status:** stable · **Verified-against:** qb 2.0.0 (c++23)

A reading of the `message_broker` example: a topic-based pub/sub broker that fans one published message out to many subscribers by sharing a single reference-counted payload across cores, framed over a custom binary protocol.

**Prerequisites:** [TCP chat analysis](./chat_tcp_analysis.md), [Network-enabled actors](../network_actors.md), [Async I/O inside actors](../async_in_actors.md) — **See also:** [Custom protocols](../../3_qb_io/protocols.md), [The event system](../../2_core_concepts/event_system.md)

## Summary

The example lives at `examples/core_io/message_broker/` and builds two executables, `broker_server` and `broker_client`, that share one protocol and one set of event types. It extends the [`chat_tcp`](./chat_tcp_analysis.md) topology with a topic registry and a broadcast path, so read that walkthrough first; this page focuses on what is new.

Three concerns distinguish the broker from a flat chat server:

- a topic registry that maps each topic to its subscriber set, owned by a single actor (`TopicManagerActor`);
- a fan-out that builds the broadcast payload once and shares it across every delivery event, rather than copying it per subscriber (`broker::MessageContainer`, backed by `std::shared_ptr`);
- `std::string_view`s that reference topic and content inside a live, owned payload, so parsing never copies the bytes it slices.

The directory layout:

```text
examples/core_io/message_broker/
├── shared/        Protocol.h, Protocol.cpp, Events.h  (linked into both binaries)
├── server/        AcceptActor, ServerActor, BrokerSession, TopicManagerActor, main.cpp
└── client/        ClientActor, InputActor, main.cpp
```

<!-- src: examples/core_io/message_broker/CMakeLists.txt -->

> **A note on the source.** The example predates the DoS-hardening change that made `io_handler::registerSession()` return a nullable pointer rather than a reference (see [Network-enabled actors](../network_actors.md)). The checked-in `ServerActor::on(NewSessionEvent&)` binds the result to `auto& session`, which matches the older signature. This page documents the current framework contract and notes where the example diverges.

## Architecture

The server splits four responsibilities across three cores. Connection acceptance, session I/O, and topic logic each run on a dedicated `VirtualCore`, so an accept burst never stalls a broadcast and a broadcast never stalls accepts. The placement below is from `server/main.cpp`.

```text
        TCP clients
             │  connect to :12345
             ▼
   ┌──────────────────────┐   core 0
   │     AcceptActor      │   tcp::acceptor
   └──────────┬───────────┘
              │  NewSessionEvent (round-robin)
              ▼
   ┌──────────────────────┐   core 1
   │  ServerActor × 2     │   tcp::io_handler<BrokerSession>
   │   owns BrokerSession │
   └───┬──────────────▲───┘
       │ Sub/Unsub/   │ SendMessageEvent
       │ Publish/     │  (shared payload)
       │ Disconnect   │
       ▼              │
   ┌──────────────────┴───┐   core 2
   │  TopicManagerActor   │   topic registry + fan-out
   └──────────────────────┘
```

<!-- src: examples/core_io/message_broker/server/main.cpp -->

Every component except `BrokerSession` is a `qb::Actor`. `BrokerSession` is a plain object owned by a `ServerActor` and run on that actor's `VirtualCore`; it never crosses a core boundary. Cross-actor traffic is asynchronous events; the only data that travels with a published message is a copyable handle to a shared payload, not the payload itself.

## The shared protocol

Both binaries link `shared/Protocol.{h,cpp}` and `shared/Events.h`. The wire format is an 8-byte fixed header followed by a UTF-8 string payload.

```cpp
// src: examples/core_io/message_broker/shared/Protocol.h
namespace broker {

struct MessageHeader {
    uint16_t magic;    // 'QM'  (0x514D)
    uint8_t  version;  // 0x01
    uint8_t  type;     // broker::MessageType
    uint32_t length;   // payload length in bytes
};

constexpr uint16_t PROTOCOL_MAGIC   = 0x514D;
constexpr uint8_t  PROTOCOL_VERSION = 0x01;

enum class MessageType : uint8_t {
    SUBSCRIBE = 1,  // client -> server
    UNSUBSCRIBE,    // client -> server
    PUBLISH,        // client -> server
    MESSAGE,        // server -> client: a published message
    RESPONSE,       // server -> client: command acknowledgement
    ERROR           // server -> client: error notification
};

struct Message {
    MessageType type;
    std::string payload;

    Message() = default;
    Message(MessageType t, std::string p) : type(t), payload(std::move(p)) {}
};

} // namespace broker
```

`MessageType` is `uint8_t`-backed. The header places `#undef ERROR` as the first line inside the enum body because some platform headers define `ERROR` as a macro; the example removes that definition so the `ERROR` enumerator compiles.

### Framing

`broker::BrokerProtocol<IO_>` derives from `qb::io::async::AProtocol<IO_>` and implements the framing contract. `getMessageSize()` is called by the framework to learn whether the input buffer holds a complete message; `onMessage(size)` is called once it does. (For the full contract see [Custom protocols](../../3_qb_io/protocols.md).)

```cpp
// src: examples/core_io/message_broker/shared/Protocol.h
template <typename IO_>
class BrokerProtocol : public qb::io::async::AProtocol<IO_> {
    static constexpr size_t HEADER_SIZE = sizeof(MessageHeader);
    bool            _reading_header = true;
    MessageHeader   _header{};
    std::vector<char> _payload;

public:
    using message = Message;  // required by the framework

    explicit BrokerProtocol(IO_ &io) noexcept
        : qb::io::async::AProtocol<IO_>(io) {}

    std::size_t getMessageSize() noexcept override {
        auto &buffer = this->_io.in();
        if (buffer.empty()) return 0;

        if (_reading_header) {
            if (buffer.size() < HEADER_SIZE) return 0;           // wait for the header
            std::memcpy(&_header, buffer.cbegin(), HEADER_SIZE);

            if (_header.magic != PROTOCOL_MAGIC ||
                _header.version != PROTOCOL_VERSION) {
                reset();                                          // resynchronize on bad header
                return 0;
            }
            _reading_header = false;
            _payload.resize(_header.length);
            return HEADER_SIZE + _header.length;
        }
        return HEADER_SIZE + _header.length;
    }

    void onMessage(std::size_t /*size*/) noexcept override {
        auto &buffer = this->_io.in();
        Message msg;
        msg.type = static_cast<MessageType>(_header.type);
        if (_header.length > 0)
            msg.payload.assign(buffer.cbegin() + HEADER_SIZE, _header.length);

        this->_io.on(std::move(msg));   // dispatch to the owning I/O component
        _reading_header = true;         // ready for the next frame
    }

    void reset() noexcept override {
        _reading_header = true;
        _payload.clear();
    }
};
```

`onMessage()` constructs a `broker::Message` and hands it to the owning component with `this->_io.on(std::move(msg))`. The `IO_` type — `BrokerSession` on the server, `ClientActor` on the client — supplies a matching `on(broker::Message)` overload.

> **Pitfall — `getMessageSize()` must be a pure query.** The base-protocol contract expects `getMessageSize()` to return the next complete message size (or `0`) without side effects, because the framework may call it more than once before a message completes. `BrokerProtocol` satisfies this: it only mutates `_reading_header`/`_payload` once the full header is present, and returns the cached total on subsequent calls. Performing I/O or consuming the buffer here would corrupt framing.

### Serialization

Outbound serialization is a `qb::allocator::pipe<char>::put<T>` specialization. Writing `*this << message` from any I/O component routes through it.

```cpp
// src: examples/core_io/message_broker/shared/Protocol.cpp
template <>
pipe<char> &pipe<char>::put<broker::Message>(const broker::Message &msg) {
    broker::MessageHeader header{
        broker::PROTOCOL_MAGIC,
        broker::PROTOCOL_VERSION,
        static_cast<uint8_t>(msg.type),
        static_cast<uint32_t>(msg.payload.size())
    };
    this->put(reinterpret_cast<const char *>(&header), sizeof(header));
    if (!msg.payload.empty())
        this->put(msg.payload.data(), msg.payload.size());
    return *this;
}
```

The header is written from a stack value; the payload is appended only when non-empty.

## Shared payloads: `broker::MessageContainer`

The fan-out optimization lives in `shared/Events.h`. `broker::MessageContainer` wraps a `std::shared_ptr<broker::Message>` and exposes its accessors through atomic load/store so the handle can be copied safely across cores.

```cpp
// src: examples/core_io/message_broker/shared/Events.h
class MessageContainer {
    std::shared_ptr<broker::Message> _message;

public:
    MessageContainer() = default;

    MessageContainer(const MessageContainer &other)
        : _message(std::atomic_load(&other._message)) {}

    explicit MessageContainer(broker::Message &&msg)
        : _message(std::make_shared<broker::Message>(std::move(msg))) {}

    MessageContainer(broker::MessageType type, std::string payload)
        : _message(std::make_shared<broker::Message>(type, std::move(payload))) {}

    std::string_view payload() const {
        auto msg = std::atomic_load(&_message);
        return msg ? std::string_view(msg->payload) : std::string_view{};
    }

    const broker::Message &message() const {
        static const broker::Message empty_msg{};
        auto msg = std::atomic_load(&_message);
        return msg ? *msg : empty_msg;
    }
    // type(), valid(), operator bool elided
};
```

Copying a `MessageContainer` shares the underlying `broker::Message`; it does not duplicate the payload string. Because the payload lives behind a stable `shared_ptr`, a `std::string_view` taken from `payload()` stays valid as long as any copy of the container is alive. The events below exploit exactly that.

> **A note on atomics.** The container uses the `std::atomic_load`/`std::atomic_store` free-function overloads for `std::shared_ptr`. These are deprecated in C++20 in favor of `std::atomic<std::shared_ptr<T>>`, but remain valid; the example compiles against the c++23 toolchain. Inside one `TopicManagerActor` the broadcast container is never mutated after construction, so the synchronization that matters is the reference-count traffic when copies are made on, and destroyed on, different cores — which `shared_ptr` handles regardless.

## Event types

`shared/Events.h` defines the typed events that carry work between actors. The three request events each own a `MessageContainer` and expose `string_view`s into its payload.

| Event | From → to | Payload of note |
|---|---|---|
| `NewSessionEvent` | AcceptActor → ServerActor | `qb::io::tcp::socket socket` (moved) |
| `SubscribeEvent` | ServerActor → TopicManagerActor | `session_id`, `message_data`, `topic` view |
| `UnsubscribeEvent` | ServerActor → TopicManagerActor | `session_id`, `message_data`, `topic` view |
| `PublishEvent` | ServerActor → TopicManagerActor | `session_id`, `message_data`, `topic` + `content` views |
| `SendMessageEvent` | TopicManagerActor → ServerActor | `session_id`, `message_data` (shared) |
| `DisconnectEvent` | ServerActor → TopicManagerActor | `session_id` |
| `BrokerInputEvent` | InputActor → ClientActor | `qb::string<1024> command` |

`SubscribeEvent` shows the ownership pattern: it takes a `broker::Message&&`, moves it into the container, then takes the topic view from the container's now-stable payload.

```cpp
// src: examples/core_io/message_broker/shared/Events.h
struct SubscribeEvent : public qb::Event {
    qb::uuid                 session_id;
    broker::MessageContainer message_data;
    std::string_view         topic;

    SubscribeEvent(qb::uuid id, broker::Message &&msg)
        : session_id(id)
        , message_data(std::move(msg))      // payload now owned and stable
        , topic(message_data.payload()) {}  // view into the owned payload

    SubscribeEvent() = default;
};
```

The member initialization order matters: `message_data` is constructed before `topic`, so the view points into a payload that already exists. `qb::uuid` is the session identifier type (`include/qb/uuid.h`); it is the same value a `BrokerSession` returns from `id()`.

## Server walkthrough

### AcceptActor — accept and distribute

`AcceptActor` inherits `qb::Actor` and `qb::io::use<AcceptActor>::tcp::acceptor`. `onInit()` listens on the configured URI and calls `start()`; each accepted socket arrives in `on(accepted_socket_type&&)` and is round-robined to a `ServerActor`.

```cpp
// src: examples/core_io/message_broker/server/AcceptActor.cpp
bool AcceptActor::onInit() {
    if (_server_pool.empty()) {
        qb::io::cerr() << "Cannot init AcceptActor with empty server pool" << std::endl;
        return false;
    }
    if (transport().listen(_listen_at)) {        // non-zero return == failure
        qb::io::cerr() << "Cannot listen on " << _listen_at.source() << std::endl;
        return false;
    }
    start();
    return true;
}

void AcceptActor::on(accepted_socket_type &&new_io) {
    auto server_id = _server_pool[_session_counter++ % _server_pool.size()];
    auto &evt  = push<NewSessionEvent>(server_id);   // event lives on the target core
    evt.socket = std::move(new_io);                  // hand the socket over
}

void AcceptActor::on(qb::io::async::event::disconnected const &) {
    broadcast<qb::KillEvent>();   // listener died: shut the whole system down
}
```

`push<NewSessionEvent>(server_id)` allocates the event on the recipient's core and returns a reference for in-place field assignment. Moving the socket into `evt.socket` transfers the live file descriptor; nothing is duplicated.

### ServerActor — own sessions, bridge to the topic manager

`ServerActor` inherits `qb::Actor` and `qb::io::use<ServerActor>::tcp::io_handler<BrokerSession>`. The `io_handler` mixin owns a `uuid → shared_ptr<BrokerSession>` registry, reachable through `sessions()`.

```cpp
// src: examples/core_io/message_broker/server/ServerActor.cpp
bool ServerActor::onInit() {
    registerEvent<NewSessionEvent>(*this);
    registerEvent<SendMessageEvent>(*this);
    return true;
}

void ServerActor::on(NewSessionEvent &evt) {
    auto &session = registerSession(std::move(evt.socket));   // see note below
    qb::io::cout() << "New broker session registered: " << session.id() << std::endl;
}
```

> **Framework contract vs. the example.** In current qb, `registerSession()` returns `BrokerSession *` and yields `nullptr` when the session cap (`set_max_sessions()`, default unlimited) is reached, closing the incoming socket instead of allocating. The checked-in example binds `auto& session` and dereferences it unconditionally, which matches the older reference-returning signature. New code should write:
>
> ```cpp
> if (auto *session = registerSession(std::move(evt.socket)))
>     qb::io::cout() << "New broker session: " << session->id() << std::endl;
> // else: session limit reached, socket already closed
> ```

The `handle*` methods are called by a `BrokerSession` (same core, plain method calls) and forward typed events to the `TopicManagerActor`. `handlePublish` is the one that preserves zero-copy ownership end to end:

```cpp
// src: examples/core_io/message_broker/server/ServerActor.cpp
void ServerActor::handleSubscribe(qb::uuid session_id, broker::Message &&msg) {
    push<SubscribeEvent>(_topic_manager_id, session_id, std::move(msg));
}

void ServerActor::handlePublish(qb::uuid session_id, broker::MessageContainer &&container,
                                std::string_view topic, std::string_view content) {
    push<PublishEvent>(_topic_manager_id, session_id,
                       std::move(container), topic, content);
}
```

Replies arrive as `SendMessageEvent`, which the actor delivers to the named session by writing the contained message to its socket:

```cpp
// src: examples/core_io/message_broker/server/ServerActor.cpp
void ServerActor::on(SendMessageEvent &evt) {
    auto it = sessions().find(evt.session_id);
    if (it != sessions().end()) {
        *it->second << evt.message();   // serialize via pipe::put<broker::Message>
        it->second->updateTimeout();    // count delivery as activity
    }
}
```

`evt.message()` returns the `const broker::Message&` held by the event's container; `operator<<` routes through the `pipe::put` specialization shown earlier. The session lookup tolerates a missing entry: a client that disconnected between fan-out and delivery is silently skipped.

### BrokerSession — parse and delegate

`BrokerSession` inherits `qb::io::use<BrokerSession>::tcp::client<ServerActor>` (a server-side client endpoint that keeps a reference to its owner) and `qb::io::use<BrokerSession>::timeout`. The constructor installs the protocol and arms an inactivity timeout.

```cpp
// src: examples/core_io/message_broker/server/BrokerSession.cpp
BrokerSession::BrokerSession(ServerActor &server)
    : client(server) {
    this->template switch_protocol<Protocol>(*this);
    this->setTimeout(std::chrono::seconds(600));   // 600 s inactivity window
}
```

`setTimeout()` takes a `qb::duration` (a `std::chrono::nanoseconds` span); `std::chrono::seconds(600)` converts implicitly. The inline comment in the source that reads "120 second timeout" is stale — the armed value is 600 seconds. `updateTimeout()` resets the window and is called on every inbound message and every delivery.

The message handler is the zero-copy hinge. For `PUBLISH`, the payload is `"<topic> <content>"`; the session moves the whole message into a `MessageContainer`, then slices topic and content as views into that owned buffer before handing all three to `ServerActor`:

```cpp
// src: examples/core_io/message_broker/server/BrokerSession.cpp
void BrokerSession::on(broker::Message msg) {     // by value: free to move out of it
    switch (msg.type) {
    case broker::MessageType::SUBSCRIBE:
        this->server().handleSubscribe(this->id(), std::move(msg));
        break;
    case broker::MessageType::UNSUBSCRIBE:
        this->server().handleUnsubscribe(this->id(), std::move(msg));
        break;
    case broker::MessageType::PUBLISH: {
        std::string_view payload_view = msg.payload;
        size_t space_pos = payload_view.find(' ');
        if (space_pos != std::string_view::npos) {
            broker::MessageContainer container(std::move(msg));      // own the bytes
            std::string_view p     = container.payload();            // view the owned bytes
            std::string_view topic = p.substr(0, space_pos);
            std::string_view body  = p.substr(space_pos + 1);
            this->server().handlePublish(this->id(), std::move(container), topic, body);
        } else {
            broker::Message error_msg{broker::MessageType::ERROR,
                                      "Invalid publish format. Use: PUB <topic> <message>"};
            *this << error_msg;
        }
        break;
    }
    default:
        qb::io::cerr() << "Unknown message type: " << int(msg.type) << std::endl;
        break;
    }
    this->updateTimeout();
}
```

Taking `msg` **by value** is deliberate: it lets the handler move the message out (into the container, or into a forwarded event) without a copy. The two views are derived only after `container` owns the buffer, so they never dangle. Disconnection and timeout each notify the owner:

```cpp
// src: examples/core_io/message_broker/server/BrokerSession.cpp
void BrokerSession::on(qb::io::async::event::disconnected const &) {
    this->server().handleDisconnect(this->id());
}

void BrokerSession::on(qb::io::async::event::timer const &) {
    this->disconnect();   // idle too long: close, which raises disconnected
}
```

### TopicManagerActor — the registry and the fan-out

`TopicManagerActor` is a plain `qb::Actor`. It holds three maps and never touches a socket:

```cpp
// src: examples/core_io/message_broker/server/TopicManagerActor.h
std::map<qb::uuid, SessionInfo>                  _sessions;        // session -> owning ServerActor
std::map<std::string, std::set<qb::uuid>>        _subscriptions;   // topic   -> subscribers
std::map<qb::uuid, std::set<std::string>>        _session_topics;  // session -> its topics
```

`SessionInfo` records only the `qb::ActorId` of the `ServerActor` that owns a session, which is how a reply is routed back to the right core. The owner is learned from the event source — `evt.getSource()` returns the `ServerActor`'s id — and recorded on first subscribe:

```cpp
// src: examples/core_io/message_broker/server/TopicManagerActor.cpp
void TopicManagerActor::on(SubscribeEvent &evt) {
    auto session_id = evt.session_id;
    auto server_id  = evt.getSource();
    std::string topic(evt.topic);              // copy only at the storage boundary

    if (_sessions.find(session_id) == _sessions.end())
        _sessions[session_id] = SessionInfo{server_id};

    _subscriptions[topic].insert(session_id);
    _session_topics[session_id].insert(topic);
    sendResponse(session_id, server_id, "Subscribed to topic: " + topic);
}
```

The topic view is converted to `std::string` exactly once, at the point it becomes a map key — the only unavoidable copy on the subscribe path. The publish handler is where the fan-out pays off:

```cpp
// src: examples/core_io/message_broker/server/TopicManagerActor.cpp
void TopicManagerActor::on(PublishEvent &evt) {
    std::string topic(evt.topic);

    auto topic_it = _subscriptions.find(topic);
    if (topic_it == _subscriptions.end() || topic_it->second.empty()) {
        sendResponse(evt.session_id, evt.getSource(),
                     "Message published to topic with no subscribers: " + topic);
        return;
    }

    std::string formatted = topic + ": " + std::string(evt.content);

    // One container for the whole broadcast. Each SendMessageEvent copies the
    // handle, not the payload.
    broker::MessageContainer shared_message(broker::MessageType::MESSAGE, formatted);

    for (const auto &subscriber_id : topic_it->second) {
        auto sub_it = _sessions.find(subscriber_id);
        if (sub_it != _sessions.end())
            sendToSession(subscriber_id, sub_it->second.server_id, shared_message);
    }
}
```

`sendToSession(..., const broker::MessageContainer&)` pushes a `SendMessageEvent` whose copy constructor shares `shared_message`'s `shared_ptr`. For N subscribers the formatted payload is allocated once; the N events carry N handles to it. The payload is freed when the last `SendMessageEvent` — wherever it lands — is destroyed.

> **What "zero-copy fan-out" does and does not mean here.** The published bytes are formatted into one `std::string` and stored once for the whole broadcast; the per-subscriber events copy only the `shared_ptr` handle. The final socket write in `ServerActor::on(SendMessageEvent&)` still serializes that payload per client through `pipe::put` — there is no way around writing the bytes to each kernel socket. The optimization removes the intermediate per-subscriber heap allocations and string copies, not the terminal socket writes.

Disconnect cleanup walks the session's topic set and removes it from each subscriber set, erasing topics that become empty:

```cpp
// src: examples/core_io/message_broker/server/TopicManagerActor.cpp
void TopicManagerActor::on(DisconnectEvent &evt) {
    auto session_it = _sessions.find(evt.session_id);
    if (session_it == _sessions.end()) return;

    if (auto topics_it = _session_topics.find(evt.session_id);
        topics_it != _session_topics.end()) {
        for (const auto &topic : topics_it->second) {
            auto &subscribers = _subscriptions[topic];
            subscribers.erase(evt.session_id);
            if (subscribers.empty())
                _subscriptions.erase(topic);
        }
        _session_topics.erase(topics_it);
    }
    _sessions.erase(session_it);
}
```

Keeping `_session_topics` makes cleanup O(topics-of-this-session) rather than a scan of every topic — the reason the reverse index exists.

### Server wiring

`server/main.cpp` places the actors and starts the engine asynchronously so the main thread can wait for an Enter key to trigger a graceful stop.

```cpp
// src: examples/core_io/message_broker/server/main.cpp
qb::Main engine;

auto topic_manager_id = engine.addActor<TopicManagerActor>(2);   // core 2

auto server_ids = engine.core(1).builder()                        // core 1
    .addActor<ServerActor>(topic_manager_id)
    .addActor<ServerActor>(topic_manager_id)
    .idList();

engine.core(0).builder()                                          // core 0
    .addActor<AcceptActor>(qb::io::uri{"tcp://0.0.0.0:12345"}, server_ids);

engine.start(true);   // asynchronous: returns immediately
std::cin.get();
engine.stop();
engine.join();
```

`engine.core(n).builder()` is the fluent form for placing several actors on one core; `idList()` collects their `qb::ActorId`s so `AcceptActor` can round-robin across the `ServerActor` pool. Two `ServerActor`s share one `TopicManagerActor`.

## Client walkthrough

The client mirrors [`chat_tcp`](./chat_tcp_analysis.md): an `InputActor` reads the console, and a `ClientActor` owns the connection.

`ClientActor` inherits `qb::Actor` and `qb::io::use<ClientActor>::tcp::client<>` (no owner template argument — it is a standalone client). It connects asynchronously and reconnects on loss:

```cpp
// src: examples/core_io/message_broker/client/ClientActor.cpp
void ClientActor::connect() {
    qb::io::async::tcp::connect<qb::io::tcp::socket>(
        _server_uri,
        [this](qb::io::tcp::socket socket) {
            if (socket.is_open()) onConnected(std::move(socket));
            else                  onConnectionFailed();
        },
        std::chrono::seconds(5));     // connection deadline
}

void ClientActor::onConnected(qb::io::tcp::socket &&socket) {
    this->transport() = std::move(socket);
    this->template switch_protocol<Protocol>(*this);
    this->start();
}

void ClientActor::on(qb::io::async::event::disconnected const &) {
    _connected = false;
    if (_should_reconnect)
        qb::io::async::callback([this] { connect(); }, std::chrono::seconds(5));   // retry delay
}
```

The connection deadline and the reconnect delay are both five seconds. Reconnection is scheduled with `qb::io::async::callback`, which runs the lambda on the actor's own event loop after the delay — no extra thread.

> **Framework contract vs. the example.** `qb::io::async::tcp::connect()` takes its timeout as a `qb::duration` (`std::chrono::nanoseconds`, per [the canonical time model](../../7_reference/glossary.md)), and `qb::io::async::callback()` takes any `std::chrono::duration`. The checked-in example stores both deadlines as `static constexpr double CONNECT_TIMEOUT = 5.0;` / `RECONNECT_DELAY = 5.0;` and passes them directly — a form that predates the time migration and no longer compiles, because a bare `double` does not convert to `qb::duration`. New code should pass a chrono literal such as `std::chrono::seconds(5)`, as shown above.

Outbound commands are built as `broker::Message` and written with `*this << msg`:

```cpp
// src: examples/core_io/message_broker/client/ClientActor.cpp
void ClientActor::sendPublish(const std::string &topic, const std::string &message) {
    if (!_connected) { qb::io::cout() << "Not connected. Command discarded." << std::endl; return; }
    broker::Message msg{broker::MessageType::PUBLISH, topic + " " + message};
    *this << msg;
}
```

Inbound frames land in `on(const broker::Message&)`, which prints `RESPONSE`, `MESSAGE`, and `ERROR` payloads. `InputActor` uses `qb::ICallback` to poll `std::cin` without blocking the event loop and forwards each non-local line as a `BrokerInputEvent`; `quit` and `help` are handled in the input actor itself.

## Running it

```bash
# from the build directory
cmake --build . --target broker_server
cmake --build . --target broker_client

./broker_server                 # listens on 0.0.0.0:12345
./broker_client 127.0.0.1 12345 # one terminal per client
```

Client commands. The broker verbs (`SUB`, `UNSUB`, `PUB`, and their long forms `SUBSCRIBE`/`UNSUBSCRIBE`/`PUBLISH`) are matched case-insensitively in `ClientActor::processCommand`; `help` and `quit` are matched lowercase-exactly in `InputActor`.

```text
SUB   <topic>            subscribe
UNSUB <topic>            unsubscribe
PUB   <topic> <message>  publish
help                     usage
quit                     disconnect and exit
```

Subscribe two clients to `news`, publish from a third, and both subscribers receive `news: <message>`. On a successful publish to a topic that has subscribers, the publisher receives nothing back: `TopicManagerActor::on(PublishEvent&)` broadcasts the `MESSAGE` to subscribers and sends no acknowledgement to the sender. A `RESPONSE` is returned to the publisher only when the topic has no subscribers — `Message published to topic with no subscribers: <topic>` — and in that case nothing is broadcast. (If the publisher is itself subscribed to the topic, it receives the broadcast `MESSAGE` like any other subscriber.)

## Pitfalls

- **`registerSession()` is nullable now.** The current contract returns `BrokerSession *` and yields `nullptr` at the session cap. Do not copy the example's `auto& session = registerSession(...)`; check the pointer and let the framework close the rejected socket. See [Network-enabled actors](../network_actors.md).
- **Views must outlive their use.** Every `string_view` in an event references a payload owned by a `MessageContainer` *in the same event*. The construction order in `SubscribeEvent`/`PublishEvent` guarantees the container exists first. Reordering the members, or taking a view of a temporary `Message`, would dangle.
- **Topic conversion is the one copy.** `std::string_view` carries the topic to `TopicManagerActor`, but a `std::map<std::string, ...>` key forces one `std::string` construction per lookup. That copy is intrinsic to keying a map on a string; the views save the copies *before* that point, not the key itself.
- **The fan-out shares payloads, not socket writes.** Sharing the `MessageContainer` removes per-subscriber heap churn inside the broker. Each subscriber still incurs one serialization and one socket write in `ServerActor`. Do not expect the optimization to reduce per-client network work.
- **The timeout is 600 s, not 120 s.** The source comment is wrong; `setTimeout(std::chrono::seconds(600))` arms a 600-second inactivity window. Tune it for your traffic; idle subscribers that never publish will otherwise be disconnected.
- **Timeouts are `qb::duration` now, not `double`.** The checked-in client passes `static constexpr double` constants (`CONNECT_TIMEOUT`, `RECONNECT_DELAY`, both `5.0`) to `qb::io::async::tcp::connect()` and `qb::io::async::callback()`. That predates the canonical time model and no longer compiles — a bare `double` does not convert to `qb::duration`. Pass a chrono literal such as `std::chrono::seconds(5)`. See [Async I/O inside actors](../async_in_actors.md).
- **`getMessageSize()` stays side-effect-free.** When you adapt this protocol, keep `getMessageSize()` a pure query and confine consumption to `onMessage()`; the framework calls the former repeatedly while a frame is still arriving. See [Custom protocols](../../3_qb_io/protocols.md).

## See also

- [TCP chat analysis](./chat_tcp_analysis.md) — the simpler topology this example extends.
- [Custom protocols](../../3_qb_io/protocols.md) — the full `AProtocol` / `pipe::put` contract.
- [Network-enabled actors](../network_actors.md) — `tcp::acceptor`, `tcp::io_handler`, `tcp::client`, and the `registerSession` contract.
- [Async I/O inside actors](../async_in_actors.md) — `qb::io::async::connect`, `callback`, and timeouts.
- [The event system](../../2_core_concepts/event_system.md) — `push`, event ownership, and `getSource()`.
