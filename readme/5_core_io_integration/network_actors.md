@page core_io_network_actors_md QB & IO: Building Network-Enabled Actors
@brief Master how QB actors become powerful network clients and servers using the `qb::io::use<>` template for seamless asynchronous I/O.

# QB & IO: Building Network-Enabled Actors

One of the most powerful aspects of the QB Actor Framework is its seamless integration of asynchronous network I/O directly into actors. This allows you to build highly concurrent and responsive network applications—clients, servers, or peer-to-peer systems—where each network endpoint can be an independent, stateful actor.

The primary mechanism for this integration is the **`qb::io::use<DerivedActor>`** helper template.

## The `qb::io::use<>` Template: Your Actor's Networking Toolkit

Defined in `qb/io/async.h`, the `qb::io::use<DerivedActor>` template is a sophisticated CRTP (Curiously Recurring Template Pattern) utility. When your actor class inherits from one of its nested specializations (e.g., `qb::io::use<MyClient>::tcp::client`), it automatically gains the necessary base classes and methods to function as a specific type of network endpoint. This integration is deep: the actor's network operations become part of its `VirtualCore`'s event loop, ensuring non-blocking behavior.

**Key Specializations for Networked Actors:**

*   **TCP Client Actors:**
    *   `qb::io::use<MyClientActor>::tcp::client<OptionalServerActorType = void>`: Transforms `MyClientActor` into an asynchronous TCP client.
    *   `qb::io::use<MySSLClientActor>::tcp::ssl::client<OptionalServerActorType = void>`: Creates an SSL/TLS-secured TCP client actor.
*   **TCP Server Actors & Session Handlers:**
    *   `qb::io::use<MyAcceptorActor>::tcp::acceptor`: Equips `MyAcceptorActor` to listen for and accept incoming TCP connections, typically forwarding them to other actors.
    *   `qb::io::use<MySSLAcceptorActor>::tcp::ssl::acceptor`: Same as above, but for SSL/TLS connections.
    *   `qb::io::use<MyServerActor>::tcp::server<MySessionClass>`: A comprehensive base for an actor that both listens for TCP connections and manages `MySessionClass` instances for each client.
    *   `qb::io::use<MySSLServerActor>::tcp::ssl::server<MySecureSessionClass>`: Secure version of the combined server.
    *   `qb::io::use<MySessionClass>::tcp::client<MyServerActor>`: This is commonly used for `MySessionClass` itself, making it a server-managed component that handles communication for one connected client. `MyServerActor` is its logical parent or manager.
    *   `qb::io::use<MySecureSessionClass>::tcp::ssl::client<MyServerActor>`: Secure version for server-managed sessions.
*   **UDP Endpoint Actors:**
    *   `qb::io::use<MyUDPActor>::udp::client`: Turns `MyUDPActor` into an asynchronous UDP endpoint capable of sending and receiving datagrams. Can function as a client or a simple server.
    *   `qb::io::use<MyUDPServerActor>::udp::server`: Semantically similar to `udp::client`, often used for actors primarily designed to receive UDP messages on a bound port.

**Core Functionality Provided by `qb::io::use<>`:**

When your actor inherits from one of these `use<>` specializations, it gains:

1.  **`transport()` Method:** Access to the underlying `qb-io` transport object (e.g., `qb::io::tcp::socket`, `qb::io::tcp::ssl::listener`, `qb::io::udp::socket`). This is your primary interface for initiating connections, listening on ports, binding, and other socket-level operations.
2.  **Input/Output Buffers (`in()` and `out()`):** Access to `qb::allocator::pipe<char>` instances for efficient, buffered management of incoming and outgoing byte streams.
3.  **Protocol Handling Framework:** Your actor class is expected to define a nested type alias `using Protocol = YourChosenProtocol<DerivedActor>;` (this is not required for pure acceptor actors). The `use<>` base classes leverage this `Protocol` to automatically frame incoming byte streams into meaningful messages and parse them.
4.  **Event Loop Integration:** The actor's network I/O operations are automatically registered with its `VirtualCore`'s `qb::io::async::listener`. This means I/O readiness (data arrival, send buffer available) triggers events processed by the actor's event loop.
5.  **Asynchronous Event Handlers:** You will implement specific `on(...)` methods in your actor to react to:
    *   Parsed messages from your `Protocol` (e.g., `void on(Protocol::message&& msg)`).
    *   Network status changes (e.g., `void on(qb::io::async::event::disconnected const& event)`).
    *   For acceptors, newly established connections (e.g., `void on(accepted_socket_type&& new_socket)`).

## Implementing a Network Client Actor (TCP/SSL Focus)

Let's outline the structure for an actor that connects to a server.

Conceptual Client-Side Network Interaction:
```text
+---------------------+     +-----------------------+     +-------------------+
| MyNetworkClient     |     | qb-io (Transport/     |     | External Server   |
| (Actor on VC0)      |---->| Protocol via use<>)   |---->| (Remote Machine)  |
|                     |     | (Manages async socket)|     |                   |
| - Calls connect()   |<----| & SSL Handshake)      |<----|                   |
| - Sends app events  |     |                       |     |                   |
| - Handles responses |     +-----------------------+     +-------------------+
+---------------------+
```

1.  **Inherit and Define Protocol:**
    ```cpp
    #include <qb/actor.h>
    #include <qb/io/async.h>         // For qb::io::use<>
    #include <qb/io/uri.h>           // For qb::io::uri parsing
    #include <qb/io/protocol/text.h> // Example: for text::command protocol
    #include <qb/io.h>               // For qb::io::cout
    // For SSL:
    // #include <qb/io/tcp/ssl/socket.h> // For SSL_CTX, qb::io::ssl::create_client_context

    // Forward declaration for any events this actor sends/receives from other actors
    struct SendToServerCommand : qb::Event { qb::string<128> command_data; };

    class MyNetworkClient : public qb::Actor,
                              public qb::io::use<MyNetworkClient>::tcp::client<> {
                              // For SSL: public qb::io::use<MyNetworkClient>::tcp::ssl::client<> {
    public:
        // Define the protocol for framing messages over the connection
        using Protocol = qb::protocol::text::command<MyNetworkClient>; // Example: newline-terminated

    private:
        qb::io::uri _server_uri;
        bool _connected = false;
        // For SSL clients:
        // SSL_CTX* _ssl_ctx = nullptr; // Remember to manage its lifecycle (create/free)

    public:
        explicit MyNetworkClient(const std::string& server_uri_string) 
            : _server_uri(server_uri_string) {
            // For SSL: 
            // _ssl_ctx = qb::io::ssl::create_client_context(TLS_client_method());
            // if (!_ssl_ctx) { /* Handle error: throw or log & fail onInit */ }
        }

        // For SSL: 
        // ~MyNetworkClient() override {
        //     if (_ssl_ctx) { SSL_CTX_free(_ssl_ctx); }
        // }

        // ... (onInit, event handlers, etc., follow)
    };
    ```

2.  **`onInit()` - Establish Connection:**
    *   Register any actor-specific events.
    *   **For SSL:** Initialize the transport with the `SSL_CTX`: `this->transport().init(_ssl_ctx);`
    *   Use `qb::io::async::tcp::connect` for non-blocking connection establishment. Provide a callback lambda to handle the connection result.
    ```cpp
    // Inside MyNetworkClient
    bool onInit() override {
        registerEvent<SendToServerCommand>(*this);
        registerEvent<qb::KillEvent>(*this);

        // For SSL: 
        // if (!_ssl_ctx) return false; // Ensure SSL_CTX was created
        // this->transport().init(_ssl_ctx);

        qb::io::cout() << "Client [" << id() << "]: Attempting connection to " 
                       << _server_uri.source().data() << ".\n";

        // Deduce the socket type (tcp::socket or tcp::ssl::socket)
        using UnderlyingSocketType = decltype(this->transport());

        qb::io::async::tcp::connect<UnderlyingSocketType>(
            _server_uri,                                 // Target URI
            _server_uri.host().data(),                   // SNI hostname (esp. for SSL)
            [this](UnderlyingSocketType resulting_socket) { // Connection callback
                if (!this->is_alive()) return; // Actor might have been killed

                if (resulting_socket.is_open()) {
                    qb::io::cout() << "Client [" << id() << "]: TCP connection established.\n";
                    // Move the connected socket into our transport
                    this->transport() = std::move(resulting_socket);
                    // Initialize our chosen protocol on the now-active transport
                    this->template switch_protocol<Protocol>(*this);
                    // Start monitoring I/O events (read/write readiness)
                    this->start(); 
                    _connected = true;

                    // For SSL, after start(), complete the handshake
                    if constexpr (std::is_same_v<UnderlyingSocketType, qb::io::tcp::ssl::socket>) {
                        if (this->transport().connected() != 0) {
                            qb::io::cout() << "Client [" << id() << "]: SSL handshake failed.\n";
                            _connected = false;
                            this->close(); // Close the underlying socket
                            // Consider retry logic here via async::callback
                            return;
                        }
                        qb::io::cout() << "Client [" << id() << "]: SSL handshake successful.\n";
                    }
                    
                    // Optional: Send an initial message (e.g., authentication)
                    // *this << "HELLO_SERVER" << Protocol::end;
                } else {
                    qb::io::cout() << "Client [" << id() << "]: Connection failed.\n";
                    // Schedule a retry or terminate
                    // qb::io::async::callback([this](){ if(this->is_alive()) this->onInit(); }, 5.0);
                }
            }
            //, 5.0 // Optional timeout for the connect attempt in seconds
        );
        return true;
    }
    ```

3.  **Implement Event Handlers:**
    *   `void on(Protocol::message&& msg)`: Process messages received from the server, which have been parsed by your `Protocol`.
    *   `void on(qb::io::async::event::disconnected const& event)`: This is critical. Handle connection loss: reset state (`_connected = false;`), clear I/O buffers (`this->in().reset(); this->out().reset();`), optionally reset protocol state (`if(this->protocol()) this->protocol()->reset();`), and implement reconnection logic if desired (often using `qb::io::async::callback` to schedule the next connection attempt).
    *   `on(SendToServerCommand& event)`: If your client actor receives commands from other parts of your actor system to send data, check if `_connected` and `this->transport().is_open()`, then send the data using `*this << event.command_data.c_str() << Protocol::end;` (ensuring you append any protocol-specific delimiters like `Protocol::end`).
    *   `void on(const qb::KillEvent&)`: Call `this->close();` (which handles the transport shutdown, including SSL_shutdown if applicable) and then `this->kill();`.

**(Reference:** `chat_tcp/client/ClientActor.h/.cpp`, `message_broker/client/ClientActor.h/.cpp` for complete client implementations. `test-async-io.cpp` in `qb/source/io/tests/system/` also shows SSL client setup within tests.**)

## Implementing Network Server Actors

Servers generally consist of two main roles: an **acceptor** that listens for new connections, and **session handlers** that manage communication with individual connected clients. QB supports different ways to structure this:

Basic Server Architecture (Separate Acceptor & Session Managers):
```text
                               +---------------------+
                               | External TCP Client |
                               +----------^----------+
                                          | 1. Connects
                               +----------v----------+
                               | AcceptorActor       |
                               | (on VC0, uses       |
                               |  tcp::acceptor)     |
                               +----------|----------+
                                          | 2. Accepts socket, forwards via Event
                               +----------v----------+
                               | SessionManagerActor |
                               | (on VC1, uses       |
                               |  io_handler<Session>|
                               +----------|----------+
                                          | 3. Creates & Manages SessionActor
                               +----------v----------+
                               | SessionActor        |
                               | (on VC1, uses       |
                               |  tcp::client<Mgr>)  |
                               | (Handles I/O for    |
                               |  one client)        |
                               +----------^----------+
                                          | 4. Bidirectional App Data
                               +----------v----------+
                               | External TCP Client |
                               +---------------------+
```

### Pattern 1: Combined Server Actor (Acceptor + Session Manager)

Suitable for simpler servers where a single actor class can manage both listening for new connections and handling all active client sessions.

1.  **Define Session Class:** This class will handle I/O for *one* connected client. It usually inherits from `qb::io::use<MySessionClass>::tcp::client<MyServerActorType>` (or its SSL variant), making it a client from `qb-io`'s perspective but managed by your `MyServerActorType`.
    ```cpp
    // MySession.h
    class MyServerActor; // Forward declaration

    class MyClientSession : public qb::io::use<MyClientSession>::tcp::client<MyServerActor> {
    public:
        using Protocol = qb::protocol::text::command<MyClientSession>; // Or your custom protocol

        explicit MyClientSession(MyServerActor& server_logic) : client(server_logic) {}

        void on(Protocol::message&& msg) {
            // Process data received from this client
            // Example: server().handleClientCommand(this->id(), msg.text);
        }
        void on(qb::io::async::event::disconnected const& event) {
            // Notify the main server actor of this client's disconnection
            // server().handleClientDisconnect(this->id());
        }
        // ... other session logic ...
    };
    ```
2.  **Define Server Actor:** This actor inherits from `qb::Actor` and `qb::io::use<MyServerActorType>::tcp::server<MySessionClass>` (or `::tcp::ssl::server`). The `tcp::server` base provides `io_handler` capabilities.
    ```cpp
    // MyServerActor.h
    class MyServerActor : public qb::Actor,
                          public qb::io::use<MyServerActor>::tcp::server<MyClientSession> {
    private:
        // For SSL:
        // SSL_CTX* _server_ssl_ctx = nullptr; // Manage its lifecycle
    public:
        explicit MyServerActor(const qb::io::uri& listen_uri) {
            // For SSL:
            // _server_ssl_ctx = qb::io::ssl::create_server_context(TLS_server_method(), cert_path, key_path);
            // if (!_server_ssl_ctx) { /* error */ }
            // this->transport().init(_server_ssl_ctx);

            if (this->transport().listen(listen_uri) != 0) { /* Handle listen error */ }
            this->start(); // Start accepting connections
            qb::io::cout() << "Server [" << id() << "] listening on " << listen_uri.source().data() << ".\n";
        }
        // For SSL: ~MyServerActor() { if (_server_ssl_ctx) SSL_CTX_free(_server_ssl_ctx); }

        // This method is called by the `tcp::server` base *after* a new MyClientSession 
        // instance is created and its transport (the accepted socket) is set up.
        void on(IOSession& new_session) { // IOSession is MyClientSession here
            qb::io::cout() << "Server [" << id() << "]: New client session [" << new_session.id() 
                           << "] connected from " << new_session.transport().peer_endpoint().to_string() << ".\n";
            // new_session.start() is typically called by the base when registering the session.
            // You can send a welcome message, etc.
            // new_session << "Welcome!" << MyClientSession::Protocol::end;
        }

        // Example methods to be called by MyClientSession instances:
        // void handleClientCommand(qb::uuid session_uuid, const std::string& command) { /* ... */ }
        // void handleClientDisconnect(qb::uuid session_uuid) {
        //     if (this->sessions().count(session_uuid)) {
        //         this->sessions().erase(session_uuid); // Remove from managed sessions
        //         qb::io::cout() << "Server: Session " << session_uuid << " removed.\n";
        //     }
        // }

        void on(const qb::KillEvent& /*event*/) {
            qb::io::cout() << "Server [" << id() << "] shutting down.\n";
            for (auto& [uuid, session_ptr] : this->sessions()) {
                if (session_ptr) session_ptr->disconnect(); // Request graceful session shutdown
            }
            this->sessions().clear();
            this->close(); // Close the listener socket
            this->kill();
        }
    };
    ```

### Pattern 2: Separate Acceptor Actor and Session-Managing Actor(s)

For greater scalability, especially to distribute session handling across multiple cores, you can separate the connection accepting logic from the session management logic.

1.  **Session Class:** Defined as in Pattern 1 (e.g., `MyClientSession` inheriting from `use<MyClientSession>::tcp::client<MySessionManagerActor>`).
2.  **Session-Managing Actor(s):** One or more actors, potentially on different cores, that inherit from `qb::Actor` and `qb::io::async::io_handler<MySessionManagerActor, MyClientSession>`. These actors *do not listen* for connections themselves.
    *   They receive an event (e.g., `NewClientConnectionEvent`) from the Acceptor Actor, which contains the newly accepted socket.
    *   In the handler for this event (e.g., `on(NewClientConnectionEvent& event)`), they call `this->registerSession(std::move(event.client_socket_data))` to take ownership of the socket, create a `MyClientSession` instance, and start managing it.
3.  **Acceptor Actor:** An actor inheriting from `qb::Actor` and `qb::io::use<MyAcceptorActor>::tcp::acceptor` (or its SSL variant).
    *   **`onInit()`:** Initializes its SSL context (if SSL), then calls `this->transport().init(...)` if applicable, `this->transport().listen(...)`, and finally `this->start()` to begin accepting connections.
    *   **`on(accepted_socket_type&& new_socket)`:** This method is automatically called by the `acceptor` base when a new raw TCP connection is established. Inside this handler, your Acceptor Actor would:
        *   Choose a Session-Managing Actor (e.g., via round-robin, load balancing logic, or based on some criteria).
        *   `push` a `NewClientConnectionEvent` (containing the `std::move(new_socket)`) to the chosen Session-Managing Actor.

**(Reference:** The `chat_tcp` and `message_broker` examples robustly implement Pattern 2. They have an `AcceptActor`, one or more `ServerActor`s (which act as session managers), and specific `Session` classes (`ChatSession`, `BrokerSession`).**)

## Key Considerations for Networked Actors

*   **Protocol Definition:** A well-defined `Protocol` is essential for reliable communication. Choose a built-in one or implement a custom one carefully.
*   **Connection State Management:** Actors often need to track their connection state (e.g., `_is_connecting`, `_is_connected`, `_is_authenticated`).
*   **Error Handling & Disconnections:** Robustly implement `on(qb::io::async::event::disconnected const&)` in client/session actors. Handle potential connection failures during `async::tcp::connect`. Servers should gracefully manage client disconnections.
*   **Resource Cleanup:** Ensure underlying sockets, `SSL_CTX*` (if manually managed), and other resources are properly closed/freed. `qb-io`'s RAII patterns and the actor lifecycle (destructors, `close()` in `KillEvent` handlers) generally handle this, but explicit management of `SSL_CTX` is often necessary.
*   **Flow Control:** For high-throughput applications, consider application-level flow control if actors can produce data faster than the network or receiving actors can consume it (e.g., pausing senders if `out()` buffer sizes grow too large, or using `pending_write` events).

By leveraging `qb::io::use<>` and understanding these patterns, your QB actors can become powerful, self-contained network participants, capable of handling complex asynchronous communication with clarity and efficiency.

**(Next:** Review specific example analyses like `[chat_tcp Example Analysis](./examples/chat_tcp_analysis.md)` to see these patterns in larger contexts.**)
**(See also:** `[QB-IO: Transports](./../3_qb_io/transports.md)`, `[QB-IO: Protocols](./../3_qb_io/protocols.md)`**) 