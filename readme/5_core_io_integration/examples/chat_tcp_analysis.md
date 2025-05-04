# Example Analysis: `chat_tcp`

*   **Location:** `example/core_io/chat_tcp/`
*   **Purpose:** Demonstrates a multi-core TCP chat server and client using actors for core logic and session handling.

## Architecture Overview

This example effectively separates concerns across multiple actors and cores:

**Server (`chat_server`):**

1.  **`AcceptActor` (`AcceptActor.h/.cpp`, Core 0):
    *   **Role:** Listens for incoming TCP connections on multiple ports (3001, 3002 in `main.cpp`).
    *   **Inheritance:** `qb::Actor`, `qb::io::use<AcceptActor>::tcp::acceptor`.
    *   **Initialization:** In `onInit`, calls `transport().listen()` on the configured `qb::io::uri` and `start()` to begin accepting.
    *   **Connection Handling:** Implements `on(accepted_socket_type&& new_io)`. When a connection arrives, it selects a `ServerActor` from its pool (`_server_pool`) using round-robin (`_session_counter`) and `push`es a `NewSessionEvent` containing the `std::move(new_io)` socket to the chosen `ServerActor`.
    *   **Shutdown:** Handles `on(event::disconnected&)` for the listener socket by broadcasting `KillEvent`.
2.  **`ServerActor` (`ServerActor.h/.cpp`, Core 1 - Multiple Instances):
    *   **Role:** Manages a group of active client connections (`ChatSession`). Bridges communication between individual sessions and the central `ChatRoomActor`.
    *   **Inheritance:** `qb::Actor`, `qb::io::use<ServerActor>::tcp::io_handler<ChatSession>`.
    *   **Session Management:** Inherits session map (`_sessions`) from `io_handler`.
        *   `on(NewSessionEvent&)`: Receives the socket from `AcceptActor`, creates a `ChatSession` instance using `registerSession(std::move(evt.socket))`, which adds it to the `_sessions` map and starts its I/O monitoring.
        *   `handleDisconnect(uuid)`: Called by `ChatSession` when a client disconnects. Forwards a `DisconnectEvent` to the `ChatRoomActor`.
    *   **Message Routing:**
        *   `handleAuth(uuid, username)`: Called by `ChatSession`. Creates and `push`es an `AuthEvent` to `ChatRoomActor`.
        *   `handleChat(uuid, message)`: Called by `ChatSession`. Creates and `push`es a `ChatEvent` to `ChatRoomActor`.
        *   `on(SendMessageEvent&)`: Receives messages from `ChatRoomActor` destined for a specific client. Finds the corresponding `ChatSession` in `sessions()` map and sends the message using `*session_ptr << evt.message;`.
    *   **Shutdown:** `on(KillEvent&)` iterates through `sessions()`, calls `disconnect()` on each, clears the map, closes the listener (via base io_handler), and `kill()`s itself.
3.  **`ChatSession` (`ChatSession.h/.cpp`, Managed by `ServerActor` on Core 1):
    *   **Role:** Represents *one* connected client. Handles protocol parsing and connection lifecycle.
    *   **Inheritance:** `qb::io::use<ChatSession>::tcp::client<ServerActor>`, `qb::io::use<ChatSession>::timeout`.
    *   **Initialization:** Constructor calls `switch_protocol<Protocol>(*this)` (where `Protocol` is `chat::ChatProtocol<ChatSession>`) and `setTimeout()`.
    *   **Protocol Handling:** Implements `on(chat::Message& msg)`. Based on `msg.type`, calls appropriate `server().handleAuth(...)` or `server().handleChat(...)` method on its managing `ServerActor`.
    *   **Lifecycle:** Implements `on(event::disconnected&)` and `on(event::timer&)` (timeout) which call `server().handleDisconnect(this->id())`.
4.  **`ChatRoomActor` (`ChatRoomActor.h/.cpp`, Core 3):
    *   **Role:** Central application logic and state (list of users and their sessions).
    *   **Inheritance:** `qb::Actor`.
    *   **State:** `_sessions` (map `uuid` to `SessionInfo{server_id, username}`), `_usernames` (map `username` to `uuid`).
    *   **Event Handlers:**
        *   `on(AuthEvent&)`: Checks username validity, updates state maps, sends `AUTH_RESPONSE` back to the client via the correct `ServerActor` (using `push<SendMessageEvent>(event.getSource(), ...)`), and broadcasts join message to all clients.
        *   `on(ChatEvent&)`: Looks up username from `session_id`, formats message (`username: message`), and broadcasts it to all sessions using `broadcastMessage` helper.
        *   `on(DisconnectEvent&)`: Removes user from state maps, broadcasts leave message.
    *   **Helper:** `sendToSession`, `sendError`, `broadcastMessage` use `push<SendMessageEvent>(...)` to route messages via the appropriate `ServerActor`.

**Client (`chat_client`):**

1.  **`InputActor` (`InputActor.h/.cpp`, Core 0):
    *   **Role:** Handles non-blocking console input.
    *   **Inheritance:** `qb::Actor`, `qb::ICallback`.
    *   **`onCallback()`:** Uses `std::getline` to read input. If it's "quit", sends `KillEvent` to `ClientActor` and self. Otherwise, `push`es `ChatInputEvent` to `ClientActor`.
2.  **`ClientActor` (`ClientActor.h/.cpp`, Core 1):
    *   **Role:** Manages connection to the server and handles network I/O.
    *   **Inheritance:** `qb::Actor`, `qb::io::use<ClientActor>::tcp::client<>`.
    *   **Protocol:** `using Protocol = chat::ChatProtocol<ClientActor>;`.
    *   **Connection:** `connect()` uses `async::tcp::connect` with a callback. `onConnected` sets up the transport, switches protocol, calls `start()`, and sends initial `AUTH_REQUEST`. `onConnectionFailed` schedules retries using `async::callback`.
    *   **Event Handling:**
        *   `on(ChatInputEvent&)`: Sends a `CHAT_MESSAGE` to the server if connected/authenticated.
        *   `on(chat::Message&)`: Handles messages from the server (`AUTH_RESPONSE`, `CHAT_MESSAGE`, `ERROR`), printing them to the console.
        *   `on(event::disconnected&)`: Handles connection loss, potentially scheduling retries.
    *   **Shutdown:** `on(KillEvent&)` calls `disconnect()` (which closes the transport) and `kill()`.

## Key Concepts Illustrated

*   **Client-Server Architecture:** Classic pattern implemented with actors.
*   **Multi-Core Distribution:** Separation of concerns (UI, Network, Logic) across cores.
*   **Async TCP Client/Server:** Extensive use of `qb::io::use<>` templates, `tcp::socket`, `tcp::listener`, `async::tcp::connect`.
*   **Custom Protocol Implementation:** `ChatProtocol` demonstrates header-based framing and `pipe::put` specialization for serialization.
*   **Session Management:** `ServerActor` uses `io_handler` to manage multiple client `ChatSession` objects.
*   **Centralized State Actor:** `ChatRoomActor` manages shared application state sequentially.
*   **Inter-Actor Event Communication:** Clear examples of `push`, `reply` (implicitly via `handle*` calls), and broadcasting (`broadcastMessage` helper).
*   **Connection Resilience:** Client-side reconnection logic using `async::callback`.
*   **Timeout Handling:** `ChatSession` uses `with_timeout` for inactivity.
*   **Load Balancing (Basic):** `AcceptActor` distributes load round-robin. 