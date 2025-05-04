# Example Analysis: `message_broker`

*   **Location:** `example/core_io/message_broker/`
*   **Purpose:** Implements a publish/subscribe message broker using actors for core logic and TCP for network transport.

## Architecture Overview

This example shares architectural similarities with `chat_tcp` but focuses on topic-based routing:

**Server (`broker_server`):**

1.  **`AcceptActor` (`AcceptActor.h/.cpp`, Core 0):
    *   **Role & Inheritance:** Listens for TCP connections (`qb::io::use<...>::tcp::acceptor`).
    *   **Functionality:** Accepts connections on port 12345, distributes the new socket (`std::move(new_io)`) via `NewSessionEvent` to one of the `ServerActor` instances using round-robin.
2.  **`ServerActor` (`ServerActor.h/.cpp`, Core 1 - Multiple Instances):
    *   **Role & Inheritance:** Manages client sessions (`qb::io::use<...>::tcp::server<BrokerSession>`). Acts as a bridge to the `TopicManagerActor`.
    *   **Session Handling:** Creates `BrokerSession` via `registerSession` upon receiving `NewSessionEvent`.
    *   **Message Forwarding:** Receives parsed commands from `BrokerSession` via `handleSubscribe`, `handleUnsubscribe`, `handlePublish` methods.
        *   **Optimization:** It uses `std::move` for the incoming `broker::Message` and creates specialized events (`SubscribeEvent`, `UnsubscribeEvent`, `PublishEvent`) that utilize `broker::MessageContainer` and `std::string_view`. This avoids unnecessary copying of message payloads, especially the topic and content strings, when forwarding to the `TopicManagerActor`.
    *   **Outgoing Messages:** Receives `SendMessageEvent` from `TopicManagerActor` and sends the contained message (`evt.message()`) to the target `BrokerSession` using `*session_ptr << ...`.
3.  **`BrokerSession` (`BrokerSession.h/.cpp`, Managed by `ServerActor` on Core 1):
    *   **Role & Inheritance:** Handles a single client connection (`qb::io::use<...>::tcp::client<ServerActor>`, `with_timeout`).
    *   **Protocol:** `using Protocol = broker::BrokerProtocol<BrokerSession>;`.
    *   **Message Handling:** `on(broker::Message msg)` (takes by value to allow move):
        *   Processes `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH` messages.
        *   For `PUBLISH`, parses the payload into `topic` and `content` string_views.
        *   Calls `server().handleSubscribe(id(), std::move(msg))`, `server().handleUnsubscribe(id(), std::move(msg))`, or `server().handlePublish(id(), std::move(container), topic_view, content_view)` on the parent `ServerActor`, efficiently transferring message ownership or using views.
    *   **Lifecycle:** Notifies `ServerActor` via `handleDisconnect` on disconnection or timeout.
4.  **`TopicManagerActor` (`TopicManagerActor.h/.cpp`, Core 2):
    *   **Role:** Central logic for topic management and message routing.
    *   **State:** Manages `_sessions` (map `uuid`->`SessionInfo{server_id}`), `_subscriptions` (map `topic`->`set<uuid>`), `_session_topics` (map `uuid`->`set<topic>`).
    *   **Event Handlers:**
        *   `on(SubscribeEvent&)`: Adds session to topic/session maps. Sends `RESPONSE` back.
        *   `on(UnsubscribeEvent&)`: Removes session from topic/session maps. Sends `RESPONSE` back.
        *   `on(PublishEvent&)`: Finds all subscribers for `evt.topic`. **Crucially, creates a single `broker::MessageContainer shared_message(...)` containing the formatted message**. Iterates subscribers and sends a `SendMessageEvent(subscriber_id, server_id, shared_message)` to each. This shares the message data efficiently.
        *   `on(DisconnectEvent&)`: Cleans up the session from all internal state maps.

**Client (`broker_client`):**

1.  **`InputActor` (`InputActor.h/.cpp`, Core 0):
    *   **Role & Inheritance:** Handles console input (`qb::Actor`, `qb::ICallback`).
    *   **Functionality:** Reads commands (`SUB`, `UNSUB`, `PUB`, `quit`, `help`), sends raw command string in `BrokerInputEvent` to `ClientActor`.
2.  **`ClientActor` (`ClientActor.h/.cpp`, Core 1):
    *   **Role & Inheritance:** Manages network connection (`qb::Actor`, `qb::io::use<...>::tcp::client<>`).
    *   **Protocol:** `using Protocol = broker::BrokerProtocol<ClientActor>;`
    *   **Connection:** Async connect with retries (`connect`, `onConnected`, `onConnectionFailed`).
    *   **Event Handling:**
        *   `on(BrokerInputEvent&)`: Calls `processCommand` to parse the input string.
        *   `processCommand()`: Parses commands like `SUB topic`, `PUB topic message`, etc., and sends corresponding `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH` messages to the server using `*this << broker::Message(...) << Protocol::end;`.
        *   `on(broker::Message&)`: Handles `RESPONSE` and `MESSAGE` types from the server, printing output.
        *   `on(event::disconnected&)`: Handles disconnection, schedules reconnection.

## Key Concepts Illustrated

*   **Publish/Subscribe Implementation:** Demonstrates a central broker (`TopicManagerActor`) managing subscriptions.
*   **Zero-Copy/Minimal-Copy Messaging:** Shows advanced techniques:
    *   Moving messages (`std::move`) from `BrokerSession` to `ServerActor`.
    *   Using `string_view` and `MessageContainer` in events between `ServerActor` and `TopicManagerActor` to avoid copying payload data.
    *   Broadcasting published messages from `TopicManagerActor` by creating *one* shared `MessageContainer` referenced by multiple `SendMessageEvent`s, minimizing memory overhead and copies for fan-out.
*   **Custom Protocol (`BrokerProtocol`):** Defines message types and framing.
*   **Multi-Core Architecture:** Distributes acceptor, session handlers, and broker logic across cores.
*   **Session Management (`ServerActor`):** Manages multiple client connections (`BrokerSession`).
*   **Centralized Logic (`TopicManagerActor`):** Manages shared state related to topics and subscriptions.
*   **Asynchronous TCP:** Uses `qb-io` for all network communication. 