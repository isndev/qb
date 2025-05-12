@page example_analysis_chat_tcp_md Case Study: Building a TCP Chat System with QB Actors
@brief A step-by-step analysis of the `chat_tcp` example, showcasing actor-based TCP client/server development, protocol handling, and session management in QB.

# Case Study: Building a TCP Chat System with QB Actors

*   **Location:** `example/core_io/chat_tcp/`
*   **Objective:** This example demonstrates how to construct a classic client-server TCP chat application using the QB Actor Framework. It's an excellent case study for understanding how actors manage network connections, handle custom communication protocols, manage user sessions, and distribute workload across multiple cores.

By dissecting this example, you'll see practical applications of:
*   Asynchronous TCP client and server patterns using `qb::io::use<>`.
*   Custom protocol definition for message framing (`ChatProtocol`).
*   Actor-based session management.
*   Inter-actor communication for application logic.
*   Multi-core actor distribution.

## Server-Side Architecture: A Multi-Core Design

The `chat_server` employs a distributed architecture, separating responsibilities across actors typically running on different `VirtualCore`s for scalability.

### 1. `AcceptActor`: The Connection Gateway
*   **Header:** `server/AcceptActor.h`, `server/AcceptActor.cpp`
*   **Core Assignment (Typical):** Core 0 (dedicated to accepting new connections).
*   **Role:** Listens for incoming TCP connections on one or more network ports.
*   **QB Integration:** Inherits from `qb::Actor` and `qb::io::use<AcceptActor>::tcp::acceptor`.
    *   The `tcp::acceptor` base provides the `transport()` method (a `qb::io::tcp::listener`) and handles the low-level asynchronous accept operations.
*   **Initialization (`onInit()`):
    ```cpp
    // Inside AcceptActor::onInit()
    for (const auto& uri_str : _listen_uris) {
        qb::io::uri u(uri_str.c_str());
        if (this->transport().listen(u) != 0) { /* error handling */ }
    }
    this->start(); // Start the internal async::input mechanism of the acceptor base
    ```
    It configures its `qb::io::tcp::listener` (via `this->transport()`) to listen on specified URIs (e.g., "tcp://0.0.0.0:3001"). `this->start()` activates the underlying event loop monitoring for new connections.
*   **Handling New Connections (`on(accepted_socket_type&& new_io)`):
    ```cpp
    // Inside AcceptActor::on(accepted_socket_type&& new_io)
    if (!_server_pool.empty()) {
        qb::ActorId target_server = _server_pool[_session_counter % _server_pool.size()];
        _session_counter++;
        push<NewSessionEvent>(target_server, std::move(new_io));
    }
    ```
    When a TCP connection is accepted by the `listener`, this method is invoked by the `tcp::acceptor` base. `new_io` is a `qb::io::tcp::socket` representing the newly connected client.
    The `AcceptActor` then dispatches this new socket to one of the `ServerActor` instances (from `_server_pool`) using a round-robin strategy, wrapping the socket in a `NewSessionEvent`.
*   **Shutdown:** Its `on(qb::io::async::event::disconnected const&)` handler for the listener socket (if it gets closed or errors out) triggers a broadcast of `qb::KillEvent` to gracefully shut down other server components.

### 2. `ServerActor`: The Session Manager
*   **Header:** `server/ServerActor.h`, `server/ServerActor.cpp`
*   **Core Assignment (Typical):** Core 1 (or a pool of `ServerActor`s across multiple cores, e.g., cores 1 & 2).
*   **Role:** Manages a collection of active client connections (`ChatSession` instances). It acts as a bridge between individual client sessions and the central `ChatRoomActor`.
*   **QB Integration:** Inherits from `qb::Actor` and `qb::io::use<ServerActor>::tcp::server<ChatSession>`.
    *   The `tcp::server<ChatSession>` base provides the `io_handler` functionality, automatically managing a map of `ChatSession` objects (keyed by `qb::uuid`).
*   **Handling New Sessions (`on(NewSessionEvent&)`):
    ```cpp
    // Inside ServerActor::on(NewSessionEvent& evt)
    auto& session = registerSession(std::move(evt.socket));
    // session is a ChatSession&
    // The registerSession method (from io_handler base) creates a ChatSession,
    // associates the socket, starts its I/O, and adds it to the managed session map.
    ```
    Receives the `NewSessionEvent` from an `AcceptActor`. The `registerSession(std::move(evt.socket))` call (provided by the `io_handler` part of its base) instantiates a `ChatSession`, associates the client's socket with it, starts its asynchronous I/O operations, and adds it to an internal session map.
*   **Message Routing (from `ChatSession` to `ChatRoomActor`):
    *   `ChatSession` calls methods like `server().handleAuth(id(), username)` on its managing `ServerActor`.
    *   `ServerActor` then creates specific events (e.g., `AuthEvent`, `ChatEvent`) and `push`es them to the `ChatRoomActor`.
*   **Message Routing (from `ChatRoomActor` to `ChatSession`):
    ```cpp
    // Inside ServerActor::on(SendMessageEvent& evt)
    auto session_ptr = sessions().find(evt.target_session_id);
    if (session_ptr != sessions().end() && session_ptr->second) {
        // Send the raw message content using the session's output stream
        *(session_ptr->second) << evt.message_container.message().payload;
    }
    ```
    Receives `SendMessageEvent` from `ChatRoomActor`. It looks up the target `ChatSession` in its session map and sends the message payload directly to the client using the session's `operator<<` (which uses `publish()`).
*   **Client Disconnects (`handleDisconnect(qb::uuid session_id)`):
    *   Called by a `ChatSession` when its connection drops. Forwards a `DisconnectEvent` to the `ChatRoomActor`.

### 3. `ChatSession`: The Client Connection Handler
*   **Header:** `server/ChatSession.h`, `server/ChatSession.cpp`
*   **Context:** Instantiated and managed by a `ServerActor`, runs on the same `VirtualCore` as its managing `ServerActor`.
*   **Role:** Represents and handles all I/O and protocol parsing for a single connected client.
*   **QB Integration:** Inherits from `qb::io::use<ChatSession>::tcp::client<ServerActor>` and `qb::io::use<ChatSession>::timeout`.
    *   `tcp::client<ServerActor>`: Provides the TCP transport and stream capabilities. The `ServerActor` template argument allows the session to call back to its manager (e.g., `server().handleAuth(...)`).
    *   `timeout`: Adds inactivity timeout functionality.
*   **Protocol Handling:**
    *   `using Protocol = chat::ChatProtocol<ChatSession>;` (defined in `shared/Protocol.h`).
    *   The constructor calls `this->template switch_protocol<Protocol>(*this);` to activate the custom protocol.
    *   `on(chat::Message& msg)`: This method is invoked by the framework when the `ChatProtocol` successfully parses a complete message from the client. Based on `msg.type` (`AUTH_REQUEST`, `CHAT_MESSAGE`), it calls the appropriate `handleAuth(...)` or `handleChat(...)` method on its parent `ServerActor` instance (accessed via `server()`).
*   **Lifecycle Events:**
    *   `on(qb::io::async::event::disconnected const&)`: Handles socket disconnection. Calls `server().handleDisconnect(this->id())`.
    *   `on(qb::io::async::event::timer const&)`: Handles inactivity timeout. Also calls `server().handleDisconnect(this->id())`.

### 4. `ChatRoomActor`: The Central Application Logic
*   **Header:** `server/ChatRoomActor.h`, `server/ChatRoomActor.cpp`
*   **Core Assignment (Typical):** Core 3 (a separate core for application logic).
*   **Role:** Manages the chat room's state, including the list of authenticated users and their associated `ServerActor` (for routing replies). It handles authentication, message broadcasting, and user presence.
*   **State:**
    *   `_sessions`: A map from `qb::uuid` (client session ID) to `SessionInfo { qb::ActorId server_id, qb::string username }`.
    *   `_usernames`: A map from `qb::string username` to `qb::uuid` for quick lookup.
*   **Event Handlers:**
    *   `on(AuthEvent&)`: Validates username. If valid, stores session info, sends an `AUTH_RESPONSE` (`chat::MessageType::RESPONSE`) back to the specific client via the correct `ServerActor` (using `push<SendMessageEvent>(evt.server_id, ...)`), and broadcasts a join message to all other clients.
    *   `on(ChatEvent&)`: Retrieves the username for the sending session. Formats the chat message (e.g., "username: message_content"). Broadcasts this formatted message to all connected clients via their respective `ServerActor`s.
    *   `on(DisconnectEvent&)`: Removes the user and session information from its state maps. Broadcasts a leave message to remaining clients.
*   **Message Broadcasting (`broadcastMessage`, `sendToSession` helpers):** These methods iterate through the `_sessions` map and `push` a `SendMessageEvent` to the appropriate `ServerActor` for each recipient. The `SendMessageEvent` contains the `qb::uuid` of the target `ChatSession` and the message payload (as a `chat::MessageContainer`, which uses `std::shared_ptr` for efficient sharing of message data).

## Client-Side Architecture: `chat_client`

The client is simpler, typically running actors on fewer cores.

### 1. `InputActor`: Console Input Handler
*   **Header:** `client/InputActor.h`, `client/InputActor.cpp`
*   **Core Assignment (Typical):** Core 0.
*   **Role:** Reads user input from the console asynchronously.
*   **QB Integration:** Inherits from `qb::Actor` and `qb::ICallback`.
*   **Functionality (`onCallback()`):** Uses `std::getline(std::cin, line)` (note: `std::cin` itself can be blocking if not handled carefully, though `onCallback` is non-blocking with respect to other actors). If the input is "quit", it sends a `qb::KillEvent` to the `ClientActor`. Otherwise, it `push`es a `ChatInputEvent` (containing the raw input string) to the `ClientActor`.

### 2. `ClientActor`: Network Communication & UI Display
*   **Header:** `client/ClientActor.h`, `client/ClientActor.cpp`
*   **Core Assignment (Typical):** Core 1.
*   **Role:** Manages the TCP connection to the server, sends user messages, and displays incoming chat messages to the console.
*   **QB Integration:** Inherits from `qb::Actor` and `qb::io::use<ClientActor>::tcp::client<>`.
*   **Protocol:** `using Protocol = chat::ChatProtocol<ClientActor>;`.
*   **Connection (`onInit()` and connection callback):
    *   Uses `qb::io::async::tcp::connect` to establish a non-blocking connection to the server URI.
    *   The callback lambda, upon successful TCP connection, moves the new socket into `this->transport().transport()`, switches to the `ChatProtocol` (`this->template switch_protocol<Protocol>(*this);`), starts I/O event monitoring (`this->start();`), and then sends an initial `AUTH_REQUEST` message to the server.
    *   If connection fails, it uses `qb::io::async::callback` to schedule a reconnection attempt.
*   **Event Handling:**
    *   `on(ChatInputEvent&)`: Receives raw command strings from `InputActor`. If connected and authenticated, formats them into `chat::Message` objects (e.g., `CHAT_MESSAGE` type) and sends them to the server using `*this << protocol_message << Protocol::end;`.
    *   `on(chat::Message&)`: Receives messages from the server parsed by `ChatProtocol`. Handles `AUTH_RESPONSE` (updates authenticated state), `CHAT_MESSAGE` (prints to console), and `ERROR` messages.
    *   `on(qb::io::async::event::disconnected const&)`: Handles server disconnection, clears authenticated state, and attempts to reconnect using `qb::io::async::callback`.
*   **Shutdown (`on(qb::KillEvent&)`):** Calls `this->disconnect()` (which internally calls `this->close()` on the transport) and then `this->kill()`.

## Key QB Concepts Illustrated by `chat_tcp`

*   **Client-Server Architecture with Actors:** A classic networking pattern implemented using actor principles.
*   **Multi-Core Actor Distribution:** Demonstrates assigning different roles (accepting, session handling, core logic, UI input) to actors potentially running on different cores.
*   **Asynchronous TCP Client & Server:** Extensive use of `qb::io::use<>` templates for TCP operations (`tcp::acceptor`, `tcp::server`, `tcp::client`).
*   **Custom Protocol (`ChatProtocol`):** Shows how to define a header-based binary protocol for message framing and how `qb::allocator::pipe` can be specialized with `put` for efficient serialization into the output buffer (see `Protocol.cpp`).
*   **Actor-Based Session Management:** The `ServerActor` uses the `io_handler` capabilities provided by `qb::io::use<...>::tcp::server<ChatSession>` to manage multiple `ChatSession` objects.
*   **Centralized State Management:** The `ChatRoomActor` acts as a central authority for shared application state (user lists, subscriptions), ensuring consistent access through sequential event processing.
*   **Inter-Actor Communication:** Clear examples of `push` for reliable event delivery between actors, and how `ActorId`s are used for addressing.
*   **Connection Resilience (Client-Side):** Basic reconnection logic implemented using `qb::io::async::callback`.
*   **Inactivity Timeouts (Session-Side):** `ChatSession` uses `qb::io::use<...>::timeout` to detect and handle idle client connections.
*   **Separation of Concerns:** Network I/O, user input, and core application logic are well-separated into distinct actor responsibilities.

By studying the `chat_tcp` example, developers can gain a solid understanding of how to combine `qb-core` and `qb-io` to build complex, scalable, and robust networked applications.

**(Next Example Analysis:** `[distributed_computing Example Analysis](./distributed_computing_analysis.md)`**) 