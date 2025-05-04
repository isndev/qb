# Integrating Core & IO: Network Actors

Actors can directly manage network connections (TCP, UDP, SSL) by inheriting from base classes provided by the `qb::io::use<DerivedActor>` helper template.

## The `qb::io::use<>` Helper

(`qb/include/qb/io/async.h`)

This template acts as a factory for base classes that combine `qb::Actor` with specific `qb-io` asynchronous I/O capabilities. Inheriting from these allows an actor to *be* a network endpoint.

**Key `use<>` Specializations for Networking:**

*   `use<MyActor>::tcp::client<Server=void>`: Actor *is* an async TCP client.
*   `use<MyActor>::tcp::ssl::client<Server=void>`: Actor *is* an async SSL/TLS client.
*   `use<MyActor>::tcp::acceptor`: Actor *is* an async TCP listener.
*   `use<MyActor>::tcp::ssl::acceptor`: Actor *is* an async SSL/TLS listener.
*   `use<MyActor>::tcp::server<SessionActor>`: Actor *is* an async TCP server (acceptor + session manager).
*   `use<MyActor>::tcp::ssl::server<SessionActor>`: Actor *is* an async SSL/TLS server.
*   `use<MyActor>::tcp::client<ServerActor>`: Actor *is* a server-managed TCP session.
*   `use<MyActor>::tcp::ssl::client<ServerActor>`: Actor *is* a server-managed SSL session.
*   `use<MyActor>::udp::client`: Actor *is* an async UDP endpoint (client or server).
*   `use<MyActor>::udp::server`: Actor *is* an async UDP server endpoint.

These base classes provide:

*   `transport()`: Access to the underlying socket/listener (`tcp::socket`, `tcp::listener`, `udp::socket`, `tcp::ssl::socket`, etc.).
*   `in()`/`out()`: Access to input/output buffers (`qb::allocator::pipe`).
*   Integration with the `VirtualCore`'s `listener` for async events.
*   Requires the deriving actor class to define `using Protocol = ...;` (except for acceptors).
*   Requires the deriving actor class to implement specific `on()` handlers (e.g., `on(Protocol::message&&)`, `on(event::disconnected&)`, `on(accepted_socket_type&&)`).

## How to Implement a Network Client Actor

1.  **Inherit:** `class MyClient : public qb::Actor, public qb::io::use<MyClient>::tcp::client<> { ... };` (Use `::tcp::ssl::client<>` for SSL).
2.  **Define Protocol:** `using Protocol = qb::protocol::text::command<MyClient>;` (or other appropriate protocol).
3.  **Constructor:** Optionally store target URI, reference to application logic actor.
4.  **`onInit()`:**
    *   Register necessary actor events (e.g., `SendCommandEvent`, `qb::KillEvent`).
    *   If using SSL, create `SSL_CTX` and call `transport().init(_ssl_ctx);`.
    *   Initiate connection using `qb::io::async::tcp::connect<socket_type>(uri, callback, timeout);`.
5.  **Connection Callback:** The lambda passed to `connect` handles the result.
    *   If `socket.is_open()`, move the socket into the actor's transport: `transport() = std::move(socket);`.
    *   Switch to the protocol: `this->template switch_protocol<Protocol>(*this);`.
    *   Start async processing: `start();`.
    *   If connection failed, schedule retry using `async::callback`.
6.  **Implement `on(Protocol::message&& msg)`:** Handle messages received from the server, parsed by the protocol.
7.  **Implement `on(qb::io::async::event::disconnected const&)`:** Handle connection loss. Reset buffers (`in().reset()`, `out().reset()`), potentially reset protocol (`protocol()->reset()`), and schedule reconnection attempts (`async::callback`).
8.  **Implement `on(SendCommandEvent&)` (Example):** If receiving commands from other actors, check `transport().is_open()` and send data using `*this << ...;` (which uses `publish()` and eventually `transport().write()`). Remember to append protocol delimiters/headers.
9.  **Implement `on(qb::KillEvent&)`:** Call `close()` to shut down the transport and then `kill()`.

**(Ref:** `chat_tcp/client/ClientActor.cpp`, `message_broker/client/ClientActor.cpp`**)

## How to Implement a Network Server (Acceptor + Session Manager)

This pattern typically involves multiple actor types or I/O handler classes.

**Pattern 1: Combined Server Actor (Simpler Cases)**

1.  **Define Session Class:** Create a class (doesn't need to be an actor) inheriting from `qb::io::use<MySessionClass>::tcp::client<MyServerActor>` (or `::tcp::ssl::client`). This handles *one* client connection.
    *   Define `using Protocol = ...;`.
    *   Implement `on(Protocol::message&&)` to handle client data.
    *   Implement `on(event::disconnected&)` to handle cleanup/notify server.
    *   Use `this->server()` to access the parent server actor.
2.  **Define Server Actor:** Inherit from `qb::Actor` and `qb::io::use<MyServerActor>::tcp::server<MySessionClass>` (or `::tcp::ssl::server`).
    *   **Constructor:** If SSL, create `SSL_CTX`. Call `transport().init(_ssl_ctx);`. Call `transport().listen_v4(...)` or similar. Call `start();` to begin accepting.
    *   **`on(IOSession& session)`:** Implement this method. It's called automatically by the `io_handler` base *after* a new `MySessionClass` instance is created and its transport is set up.
    *   **Handle Communication:** The server actor coordinates overall logic, potentially receiving events from sessions (forwarded by `session->server().handleSomething()`) or sending messages *to* specific sessions using `sessions().find(uuid)` and `*session_ptr << ...;`.
    *   **`on(KillEvent&)`:** Iterate through `sessions()`, call `disconnect()` on each session pointer, clear the session map, call `close()` on the listener transport, then call `kill()`.

**(Ref:** `chat_tcp/server/ServerActor.cpp` + `ChatSession.cpp`, `message_broker/server/ServerActor.cpp` + `BrokerSession.cpp`**)

**Pattern 2: Separate Acceptor and Server Actors (More Scalable)**

1.  **Define Session Class:** Same as Pattern 1.
2.  **Define Server Actor(s):** Inherit from `qb::Actor` and `qb::io::async::io_handler<MyServerActor, MySessionClass>`. These actors *manage* sessions but don't accept connections directly.
    *   Implement `on(NewSessionEvent&)`: Receives the event from the acceptor. Calls `registerSession(std::move(event.socket))` to create and manage the `MySessionClass`.
    *   Implement handlers to receive messages forwarded from sessions or send messages to sessions (`sessions().find(...)`).
    *   Implement `on(KillEvent&)` similar to Pattern 1 (disconnect sessions, clear map, kill).
3.  **Define Acceptor Actor:** Inherit from `qb::Actor` and `qb::io::use<MyAcceptorActor>::tcp::acceptor` (or `::tcp::ssl::acceptor`).
    *   **Constructor:** Store `ActorIdList` of the `ServerActor` instances.
    *   **`onInit()`:** If SSL, create `SSL_CTX` and call `transport().init()`. Call `transport().listen_v4(...)`. Call `start();`.
    *   **Implement `on(accepted_socket_type&& new_socket)`:** This is called when a connection is accepted. Choose a `ServerActor` (e.g., round-robin) and `push<NewSessionEvent>(chosen_server_id)` containing the `std::move(new_socket)`.
    *   **`on(KillEvent&)`:** Call `close()` on the listener transport, then `kill()`.

**(Ref:** `chat_tcp/server/AcceptActor.cpp`, `message_broker/server/AcceptActor.cpp`**)

## Key Considerations

*   **Protocol Choice:** Select or define a `Protocol` appropriate for your application data.
*   **State Management:** Network actors often manage connection state (`_connected`, `_connecting`).
*   **Error Handling:** Implement `on(event::disconnected&)` robustly. Handle connection failures and potential protocol errors.
*   **Resource Cleanup:** Ensure sockets and SSL contexts are properly closed/freed, usually handled by destructors and the `dispose()` mechanism in the async base classes. 