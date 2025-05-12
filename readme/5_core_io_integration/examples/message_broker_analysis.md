@page example_analysis_msg_broker_md Case Study: A Publish/Subscribe Message Broker with QB Actors
@brief A walkthrough of the `message_broker` example, highlighting efficient message fan-out and topic management using QB actors and TCP.

# Case Study: A Publish/Subscribe Message Broker with QB Actors

*   **Location:** `example/core_io/message_broker/`
*   **Objective:** This example implements a fully functional, topic-based Publish/Subscribe (Pub/Sub) message broker. It demonstrates how QB actors can be used to build a robust messaging system with TCP-based client-server communication, custom protocol handling, and efficient message distribution to multiple subscribers.

Key learning points from this example include:
*   Implementing a classic Pub/Sub architecture with actors.
*   Efficiently fanning out messages to numerous subscribers using shared message payloads (`MessageContainer`).
*   Designing a custom binary protocol for diverse message types (`BrokerProtocol`).
*   Managing distributed state for topics and subscriptions in a central actor.
*   Leveraging `qb::string_view` for zero-copy string processing during internal event forwarding.

## Architectural Overview: Client-Broker-Client

The `message_broker` system comprises a server (the broker) and multiple clients that can publish messages to topics or subscribe to receive messages from topics.

### Server-Side Broker Architecture

The server architecture is similar in structure to the `chat_tcp` example, with distinct roles for accepting connections, managing client sessions, and handling the core broker logic.

1.  **`AcceptActor` (`server/AcceptActor.h/.cpp`)**
    *   **Core (Typical):** Core 0.
    *   **Role:** Listens for incoming TCP connections on a configured port (e.g., 12345).
    *   **QB Integration:** `qb::Actor`, `qb::io::use<AcceptActor>::tcp::acceptor`.
    *   **Functionality:** When a new client connects, `on(accepted_socket_type&& new_io)` is called. It then round-robins the new `qb::io::tcp::socket` (wrapped in a `NewSessionEvent`) to one of the available `ServerActor` instances for session management.

2.  **`ServerActor` (`server/ServerActor.h/.cpp`)**
    *   **Core (Typical):** Core 1 (or a pool across multiple cores, e.g., 1 & 2).
    *   **Role:** Manages a group of active client connections, represented by `BrokerSession` instances. It acts as an intermediary, forwarding client commands to the `TopicManagerActor` and relaying messages from the `TopicManagerActor` back to the appropriate clients.
    *   **QB Integration:** `qb::Actor`, `qb::io::use<ServerActor>::tcp::server<BrokerSession>` (which provides `io_handler` capabilities).
    *   **Session Creation:** `on(NewSessionEvent& evt)` calls `registerSession(std::move(evt.socket))` to create, start, and manage a new `BrokerSession`.
    *   **Efficient Command Forwarding:** When a `BrokerSession` calls methods like `server().handleSubscribe(id(), std::move(msg))`, the `ServerActor` often uses `std::move` for the incoming `broker::Message`. It then creates specialized events for the `TopicManagerActor` (e.g., `SubscribeEvent`, `PublishEvent`) that can utilize `broker::MessageContainer` (for shared ownership of message payloads) and `std::string_view` (for topic/content if parsed locally before forwarding). This minimizes unnecessary string copying, especially for message content being published.
        ```cpp
        // Simplified from ServerActor::handlePublish
        // The BrokerSession has already parsed the topic and content into string_views
        // and put the original message into a MessageContainer.
        void ServerActor::handlePublish(qb::uuid session_id, broker::MessageContainer&& container, 
                                        std::string_view topic, std::string_view content) {
            if (_topic_manager_id.is_valid()) {
                push<PublishEvent>(_topic_manager_id, session_id, id(), 
                                   std::move(container), topic, content);
            }
        }
        ```
    *   **Delivering Messages to Clients:** `on(SendMessageEvent& evt)` receives messages from the `TopicManagerActor`. It looks up the target `BrokerSession` using `evt.target_session_id` and sends the actual message payload (accessed via `evt.message_container.message().payload`) to the client using the session's `operator<<`.

3.  **`BrokerSession` (`server/BrokerSession.h/.cpp`)**
    *   **Context:** Managed by a `ServerActor`, runs on the same `VirtualCore`.
    *   **Role:** Handles I/O and protocol parsing for a single connected client.
    *   **QB Integration:** `qb::io::use<BrokerSession>::tcp::client<ServerActor>`, `qb::io::use<BrokerSession>::timeout`.
    *   **Protocol:** `using Protocol = broker::BrokerProtocol<BrokerSession>;` (defined in `shared/Protocol.h`).
    *   **Command Processing (`on(broker::Message msg)`):
        *   Receives a fully parsed `broker::Message` from its `BrokerProtocol`.
        *   Crucially, it takes `broker::Message msg` *by value* to allow `std::move` for efficient forwarding.
        *   Based on `msg.type` (`SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH`):
            *   For `PUBLISH`, it first parses the `msg.payload` to extract `topic` and `content` as `std::string_view`s.
            *   It then calls the appropriate handler on its parent `ServerActor` (e.g., `server().handleSubscribe(id(), std::move(msg))`, or for publish: `server().handlePublish(id(), broker::MessageContainer(std::move(msg)), topic_view, content_view);`). This passes ownership of the message (or a shared container of it) efficiently.
    *   **Lifecycle:** Notifies its `ServerActor` via `handleDisconnect()` on disconnection or inactivity timeout.

4.  **`TopicManagerActor` (`server/TopicManagerActor.h/.cpp`)**
    *   **Core (Typical):** Core 2 (dedicated to core broker logic).
    *   **Role:** The heart of the broker. Manages topic subscriptions and efficiently routes published messages to all relevant subscribers.
    *   **State Management:**
        *   `_sessions`: Maps `qb::uuid` (client session ID) to `SessionInfo { qb::ActorId server_id }` (to know which `ServerActor` manages that session).
        *   `_subscriptions`: Maps `qb::string topic_name` to `std::set<qb::uuid> subscriber_session_ids`.
        *   `_session_topics`: Maps `qb::uuid session_id` to `std::set<qb::string topic_name>` (for cleanup on disconnect).
    *   **Event Handling:**
        *   `on(SubscribeEvent&)`: Adds `evt.session_id` to the subscriber set for `evt.topic_sv.data()`. Sends a `RESPONSE` message back to the client via its `ServerActor`.
        *   `on(UnsubscribeEvent&)`: Removes the session from the topic's subscriber set.
        *   `on(PublishEvent&)`: This is where efficient fan-out happens.
            1.  Formats the message to be broadcast (e.g., "topic:publisher_id:content").
            2.  **Creates a single `broker::MessageContainer shared_message(...)`**. This container likely holds a `std::shared_ptr` to the actual formatted message string/payload.
            3.  Looks up all `subscriber_session_id`s for the given `evt.topic_sv.data()`.
            4.  For each subscriber, it retrieves their managing `ServerActor`'s ID from `_sessions`.
            5.  `push`es a `SendMessageEvent(subscriber_session_id, managing_server_id, shared_message)` to the managing `ServerActor`. Because `shared_message` is passed (likely by const-ref or by copying the `MessageContainer` which shares the underlying payload), the actual message data is not copied for each subscriber.
            ```cpp
            // Simplified from TopicManagerActor::on(PublishEvent& evt)
            if (_subscriptions.count(topic_key)) {
                qb::string<512> formatted_msg_content; // Or std::string if payload is large
                // ... format message content using evt.publisher_id and evt.content_sv ...

                broker::MessageContainer shared_payload_container(
                    broker::MessageType::MESSAGE, 
                    std::string(formatted_msg_content.c_str()) // Convert qb::string to std::string for MessageContainer
                );

                for (qb::uuid subscriber_session_id : _subscriptions.at(topic_key)) {
                    if (_sessions.count(subscriber_session_id)) {
                        qb::ActorId target_server_id = _sessions.at(subscriber_session_id).server_id;
                        push<SendMessageEvent>(target_server_id, subscriber_session_id, shared_payload_container);
                    }
                }
            }
            ```
        *   `on(DisconnectEvent&)`: Removes the disconnected session from all its topic subscriptions and from the `_sessions` map.

### Client-Side (`broker_client`)

Similar to the `chat_tcp` client:

1.  **`InputActor` (`client/InputActor.h/.cpp`):** Handles console input, parses basic commands (SUB, UNSUB, PUB, QUIT, HELP), and `push`es a `BrokerInputEvent` (containing the raw command string) to the `ClientActor`.
2.  **`ClientActor` (`client/ClientActor.h/.cpp`):**
    *   Manages the TCP connection to the broker server using `qb::io::use<ClientActor>::tcp::client<>`.
    *   Uses `BrokerProtocol` for message framing: `using Protocol = broker::BrokerProtocol<ClientActor>;`.
    *   `onInit()`: Connects to the server using `qb::io::async::tcp::connect`.
    *   `on(BrokerInputEvent&)`: Further parses the raw command from `InputActor`. For example, for "PUB topicname message content", it extracts "topicname" and "message content".
    *   Sends `SUBSCRIBE`, `UNSUBSCRIBE`, or `PUBLISH` messages (as `broker::Message` objects) to the server using `*this << broker_message << Protocol::end;`.
    *   `on(broker::Message&)`: Handles `RESPONSE` and `MESSAGE` types from the server, printing information to the console.
    *   `on(qb::io::async::event::disconnected const&)`: Handles disconnection and schedules reconnection attempts using `qb::io::async::callback`.

## Key QB Concepts & Advanced Techniques Illustrated

*   **Publish/Subscribe Architecture:** A full implementation of the Pub/Sub pattern using a central `TopicManagerActor`.
*   **Efficient Message Fan-Out (Zero/Minimal Copy for Payloads):**
    *   The `TopicManagerActor` demonstrates a crucial optimization: when broadcasting a published message to multiple subscribers, it creates the actual payload data (or a container for it like `broker::MessageContainer` which likely uses `std::shared_ptr`) *once*. Then, it sends events (`SendMessageEvent`) to various `ServerActor`s, each containing a reference or a shared pointer to this single payload. This avoids repeatedly copying potentially large message contents for every subscriber, which is critical for performance in systems with many subscribers to a topic.
    *   The use of `std::move` in `BrokerSession` when calling `server().handlePublish(...)` and in `ServerActor` when creating `PublishEvent` helps transfer ownership of message data efficiently towards the `TopicManagerActor`.
*   **Custom Binary Protocol (`BrokerProtocol`):** The example defines `MessageHeader` and `MessageType` to structure communication, showing how to build more complex protocols than simple delimiter-based ones.
*   **Use of `std::string_view` for Intermediate Processing:** `BrokerSession` and `ServerActor` use `std::string_view` to refer to parts of the message payload (like topic and content) without copying them before the data is packaged into the `MessageContainer` or specific events for the `TopicManagerActor`.
*   **Multi-Core Scalability:** Distributing the `AcceptActor`, `ServerActor`s (potentially multiple instances on different cores), and the `TopicManagerActor` across different `VirtualCore`s allows the system to handle high connection rates and message throughput by parallelizing work.
*   **Layered Responsibilities:** Clear separation of concerns: connection acceptance, session I/O and protocol parsing, and topic/subscription management.

This `message_broker` example is significantly more advanced than the `chat_tcp` example in its message handling and provides excellent insights into building high-performance, scalable messaging systems with QB.

**(Next Example Analysis:** We have covered the main `core_io` examples. Consider revisiting `[Developer Guides](./../6_guides/README.md)` for broader patterns or the `[Reference Documentation](./../7_reference/README.md)` section.**) 