@page ref_api_overview_md QB Framework: Detailed API Overview
@brief A detailed reference mapping of key classes, their essential public methods, and core functionalities within the QB Actor Framework.

# QB Framework: Detailed API Overview

This document serves as a detailed quick reference to the primary classes and their most important public interfaces within the QB Actor Framework. It is organized by module (`qb-core` and `qb-io`) to help you quickly locate and understand the capabilities of key components. For exhaustive details, always consult the source header files.

## I. `qb-core`: The Actor Engine

Provides the core components for building actor-based concurrent applications.

### `qb::Main`
*   **Header:** `qb/core/Main.h`
*   **Role:** The central controller and orchestrator of the actor system.
*   **Key Public Methods & Features:**
    *   `Main()`: Constructor to initialize the engine.
    *   `core(CoreId index) -> CoreInitializer&`: Accesses the `CoreInitializer` for a specific virtual core *before* the engine starts. Used for setting affinity, latency, and adding initial actors.
    *   `addActor<ActorType>(CoreId index, Args... constructor_args) -> ActorId`: Adds an actor of `ActorType` to the specified `VirtualCore` `index`, passing `constructor_args` to its constructor. Returns the `ActorId` of the newly created actor, or an invalid ID on failure.
    *   `setLatency(uint64_t nanoseconds)`: Sets a default event loop latency for all `VirtualCore`s that haven't had a specific latency set via their `CoreInitializer`.
    *   `usedCoreSet() const -> qb::CoreIdSet`: Returns the set of `CoreId`s currently configured for use by the engine.
    *   `start(bool async = true)`: Starts all configured `VirtualCore` threads and their event loops. If `async` is true (default), returns immediately. If `false`, the calling thread becomes a worker and this blocks until the engine stops.
    *   `join()`: Waits for all `VirtualCore` threads to complete their shutdown and terminate. Essential if `start(true)` was used.
    *   `static stop()`: Signals all `VirtualCore`s and actors to begin a graceful shutdown process. Can be called from any thread or signal handler.
    *   `hasError() const -> bool`: After `join()` returns, indicates if any `VirtualCore` terminated due to an unhandled error.
    *   `static registerSignal(int signum)`: Registers an OS signal (e.g., `SIGINT`) to trigger `Main::stop()`.
    *   `static unregisterSignal(int signum)`: Removes a previously registered signal handler.
    *   `static ignoreSignal(int signum)`: Instructs the system to ignore a specific OS signal.

### `qb::Actor`
*   **Header:** `qb/core/Actor.h`
*   **Role:** The abstract base class for all user-defined actors. Actors encapsulate state and behavior, communicating via events.
*   **Key Public Methods & Features (for overriding or calling by derived/other actors):**
    *   **Lifecycle & Setup (primarily for overriding):**
        *   `Actor()`: Default constructor (often implicitly called). Assigns an initial `ActorId` and registers for default system events.
        *   `explicit Actor(ActorId id)`: Constructor for specific ID assignment (used internally, e.g., for `ServiceActor`).
        *   `virtual bool onInit()`: **Crucial override.** Called after construction and ID assignment, before event processing. Used for `registerEvent<T>()`, resource acquisition. Return `false` to abort actor launch.
        *   `virtual ~Actor()`: Virtual destructor for proper cleanup of derived actor resources.
    *   **Identification & Context:**
        *   `id() const -> ActorId`: Returns the actor's unique `ActorId`.
        *   `getIndex() const -> CoreId`: Returns the `CoreId` of the `VirtualCore` this actor is running on.
        *   `getName() const -> std::string_view`: Returns the demangled C++ class name of the actor instance.
        *   `getCoreSet() const -> const CoreIdSet&`: Returns the set of `CoreId`s its `VirtualCore` can communicate with.
        *   `time() const -> uint64_t`: Returns the current cached time (nanoseconds since epoch) from its `VirtualCore`.
    *   **State & Termination:**
        *   `kill() const`: Initiates the actor's termination sequence. Marks actor as not alive and notifies its `VirtualCore`.
        *   `is_alive() const -> bool`: Checks if the actor is currently active and processing events (i.e., `kill()` has not yet fully taken effect or been called).
    *   **Event Handling (for overriding and registration):**
        *   `registerEvent<EventType>(DerivedActor& actor_ref) const`: Subscribes the actor to handle messages of `EventType`. Typically `registerEvent<MyEvent>(*this);` in `onInit()`.
        *   `unregisterEvent<EventType>(DerivedActor& actor_ref) const`: Unsubscribes from `EventType`.
        *   `unregisterEvent<EventType>() const`: Unsubscribes self from `EventType` (less common).
        *   `void on(const EventType& event)` / `void on(EventType& event)`: **User-defined handlers** for specific event types. Must be public.
    *   **Default Event Handlers (can be overridden):**
        *   `on(const KillEvent&)`: Default handler calls `kill()`.
        *   `on(const SignalEvent&)`: Default handler calls `kill()` for `SIGINT`.
        *   `on(const PingEvent&)`: Responds to discovery pings with a `RequireEvent`.
        *   `on(const UnregisterCallbackEvent&)`: Handles internal event to unregister a callback.
    *   **Sending Events:**
        *   `push<Event>(ActorId dest, Args... args) const -> Event&`: Sends an event with **ordered** delivery. Returns a reference to the constructed event. Handles non-trivially destructible events.
        *   `send<Event>(ActorId dest, Args... args) const`: Sends an event with **unordered** delivery. `Event` type must be trivially destructible.
        *   `broadcast<Event>(Args... args) const`: Sends an event to all actors on all cores.
        *   `reply(Event& original_event) const`: Efficiently sends `original_event` (potentially modified) back to its source. Requires `on(Event&)` (non-const).
        *   `forward(ActorId new_dest, Event& original_event) const`: Efficiently redirects `original_event` to `new_dest`, preserving original source. Requires `on(Event&)` (non-const).
    *   **Advanced Event Sending:**
        *   `to(ActorId dest) const -> EventBuilder`: Returns an `EventBuilder` for chaining multiple `push` calls to the same `dest`.
        *   `getPipe(ActorId dest) const -> qb::Pipe`: Returns a `Pipe` object for direct access to the communication channel, enabling `pipe.allocated_push<Event>(size_hint, ...)` for large events.
    *   **Referenced Actors & Services:**
        *   `addRefActor<ChildActorType>(Args... args) const -> ChildActorType*`: Creates a child actor on the same `VirtualCore`. Parent receives a raw pointer but doesn't own the child.
        *   `getService<ServiceActorType>() const -> ServiceActorType*`: Retrieves a pointer to a `ServiceActor` instance on the *same core*. Returns `nullptr` if not found.
        *   `static getServiceId<ServiceTag>(CoreId target_core_id) -> qb::ActorId`: Gets the `ActorId` of a `ServiceActor` (identified by `ServiceTag`) on the specified `target_core_id`.
    *   **Dependency Discovery:**
        *   `require<ActorType...>() const`: Broadcasts a request to find live instances of `ActorType`(s). Responses arrive as `RequireEvent`s to the requester.
        *   `is<ActorType>(const RequireEvent& event) const -> bool`: Helper to check if a `RequireEvent` pertains to a specific `ActorType`.
    *   **Periodic Callbacks (if also inheriting `qb::ICallback`):**
        *   `registerCallback(DerivedActor& actor_ref) const`: Registers the actor for `onCallback()` invocations.
        *   `unregisterCallback(DerivedActor& actor_ref) const`: Unregisters a specific actor.
        *   `unregisterCallback() const`: Unregisters self from callbacks.

### `qb::ServiceActor<Tag>`
*   **Header:** `qb/core/Actor.h`
*   **Role:** A specialized base class for actors intended to be singletons per `VirtualCore`. Identified by a unique, empty `Tag` struct.
*   **Key Concepts:** Ensures only one instance per `Tag` per `VirtualCore`. Creation and access are managed via `qb::Main::addActor` and `qb::Actor::getServiceId`/`getService`.

### `qb::Event`
*   **Header:** `qb/core/Event.h`
*   **Role:** The abstract base class for all messages passed between actors.
*   **Key Public Members (accessed on a received event object):**
    *   `getSource() const -> ActorId`: Get the `ActorId` of the sender.
    *   `getDestination() const -> ActorId`: Get the `ActorId` of the intended recipient.
    *   `getID() const -> EventId` (or `const char*` in debug): Get the type identifier of the event.
    *   `is_alive() const -> bool`: Internal flag, relevant if an event object might be reused (e.g., by `reply`/`forward`).
*   **User-Defined Events:** Derived structs/classes add custom data members to carry payloads. Remember recommendations: `qb::string<N>` for direct string members, smart pointers for large/dynamic data.

### `qb::ICallback`
*   **Header:** `qb/core/ICallback.h`
*   **Role:** An interface that actors can multiply inherit from (along with `qb::Actor`) to receive periodic callbacks.
*   **Key Public Methods (for overriding by actor):**
    *   `virtual void onCallback() = 0;`: Implement this method with the logic to be executed on each `VirtualCore` loop iteration (after event processing). Must be non-blocking.

### Identifiers (`qb::ActorId`, `qb::CoreId`, `qb::ServiceId`, `qb::BroadcastId`)
*   **Header:** `qb/core/ActorId.h`
*   **Role:** Define the types used for uniquely identifying actors, cores, and services.
*   **`qb::ActorId` Methods:** `sid()`, `index()`, `is_valid()`, `is_broadcast()`, `operator uint32_t()`.
*   **`qb::BroadcastId(CoreId)`:** Constructor for creating a core-specific broadcast ID.

### `qb::CoreInitializer`
*   **Header:** `qb/core/Main.h`
*   **Role:** Helper class obtained via `qb::Main::core(id)` *before* engine start, used to configure per-`VirtualCore` settings.
*   **Key Methods:** `setAffinity(const CoreIdSet&)` and `setLatency(uint64_t nanoseconds)`, `addActor<T>()`, `builder()`.

--- 

## II. `qb-io`: Asynchronous I/O & Utilities

Provides the non-blocking I/O foundation, networking, and various system utilities.

### Asynchronous System (`qb::io::async`)

*   **`qb::io::async::listener`** (`qb/io/async/listener.h`)
    *   **Role:** Manages the thread-local `libev` event loop. Accessed via `listener::current`.
    *   **Key Methods:** `registerEvent<_Event, _Actor>()`, `unregisterEvent(IRegisteredKernelEvent*)`, `loop() -> ev::loop_ref`, `run(int flag)`, `break_one()`.
*   **Global Async Functions** (`qb/io/async/listener.h` or `qb/io/async.h`)
    *   `qb::io::async::init()`: Initializes `listener::current` for standalone use.
    *   `qb::io::async::run(int flag = 0)`: Runs the current thread's event loop.
    *   `qb::io::async::break_parent()`: Breaks the current thread's event loop.
*   **`qb::io::async::callback(Func&& func, double delay_seconds = 0.0)`** (`qb/io/async/io.h`)
    *   **Role:** Schedules a callable (lambda, function) for asynchronous execution on the current thread's event loop after `delay_seconds`.
*   **`qb::io::async::with_timeout<Derived>`** (`qb/io/async/io.h`)
    *   **Role:** CRTP base class to add timeout functionality. Derived class implements `on(qb::io::async::event::timer const&)`.
    *   **Key Methods:** `setTimeout(ev_tstamp duration)`, `updateTimeout()`.
*   **Async Event Types (`qb::io::async::event::*`)** (`qb/io/async/event/all.h`)
    *   Structs representing specific asynchronous occurrences, e.g., `event::io`, `event::timer`, `event::signal`, `event::file`, `event::disconnected`, `event::eof`, `event::eos`.

### Networking (`qb::io` namespace, `qb::io::tcp`, `qb::io::udp`, `qb::io::tcp::ssl`)

*   **`qb::io::socket`** (`qb/io/system/sys__socket.h`)
    *   **Role:** Cross-platform wrapper for low-level system socket operations.
    *   **Key Methods:** `open()`, `bind(endpoint)`, `listen()`, `connect(endpoint)`, `connect_n(endpoint)` (non-blocking), `accept()`, `send()`, `recv()`, `sendto()`, `recvfrom()`, `close()`, `shutdown()`, `set_nonblocking()`, `set_optval()`, `get_optval()`, `local_endpoint()`, `peer_endpoint()`, `static resolve()`, `static getipsv()`.
*   **`qb::io::endpoint`** (`qb/io/system/sys__socket.h`)
    *   **Role:** Represents network addresses (IPv4, IPv6, AF_UNIX paths).
    *   **Key Methods:** `af()`, `ip()`, `port()`, `to_string()`, `as_in()`, `as_un()`.
*   **`qb::io::uri`** (`qb/io/uri.h`)
    *   **Role:** RFC 3986 compliant URI parsing and manipulation.
    *   **Key Methods:** `scheme()`, `host()`, `port()`, `u_port()`, `path()`, `query("key")`, `queries()` (map), `fragment()`, `static encode()`, `static decode()`.
*   **TCP Sockets:**
    *   `qb::io::tcp::socket` (`qb/io/tcp/socket.h`): Inherits from `qb::io::socket`, specialized for TCP.
        *   Key Methods: `init(AF_INET/AF_INET6)`, `connect(endpoint/uri)`, `n_connect(endpoint/uri)`, `read()`, `write()`, `disconnect()`.
    *   `qb::io::tcp::listener` (`qb/io/tcp/listener.h`): For accepting TCP connections.
        *   Key Methods: `listen(endpoint/uri)`, `accept() -> tcp::socket`.
*   **UDP Sockets:**
    *   `qb::io::udp::socket` (`qb/io/udp/socket.h`): Inherits from `qb::io::socket`, specialized for UDP.
        *   Key Methods: `init(AF_INET/AF_INET6)`, `bind(endpoint/uri)`, `read(buffer, len, &peer_endpoint)`, `write(buffer, len, dest_endpoint)`, `set_broadcast()`, `join_multicast_group()`.
*   **SSL/TLS Sockets (Optional, requires `QB_IO_WITH_SSL`)**
    *   `qb::io::tcp::ssl::socket` (`qb/io/tcp/ssl/socket.h`): Secure TCP socket layering SSL/TLS over `tcp::socket`.
        *   Key Methods: `init(SSL_CTX*)`, `connect(endpoint, sni_host)`, `n_connect()`, `connected()` (SSL handshake), `read()`, `write()`, `disconnect()`, `ssl_handle() -> SSL*`.
    *   `qb::io::tcp::ssl::listener` (`qb/io/tcp/ssl/listener.h`): Secure TCP listener.
        *   Key Methods: `init(SSL_CTX*)`, `listen()`, `accept() -> ssl::socket`, `ssl_handle() -> SSL_CTX*`.
    *   SSL Context Helpers (`qb::io::ssl::create_client_context`, `create_server_context` in `qb/io/tcp/ssl/socket.h`).

### Streams, Transports, and Protocols

*   **Stream Bases (`qb::io::istream<IO_T>`, `ostream<IO_T>`, `stream<IO_T>`)** (`qb/io/stream.h`)
    *   **Role:** Templates providing buffered I/O. `IO_T` is the underlying transport type.
    *   **Key Methods:** `transport() -> IO_T&`, `in() -> qb::allocator::pipe&`, `out() -> qb::allocator::pipe&`, `read()`, `write()`, `publish()` (for `ostream`/`stream`), `flush()` (input buffer), `close()`.
*   **Transport Implementations (`qb::io::transport::*`)** (e.g., `qb/io/transport/tcp.h`, `udp.h`, `stcp.h`, `file.h`)
    *   Concrete specializations of stream templates (e.g., `transport::tcp` is `stream<tcp::socket>`).
*   **Protocol Interface (`qb::io::async::AProtocol<IO_Type>`)** (`qb/io/async/protocol.h`)
    *   **Role:** CRTP base for defining custom message framing logic.
    *   **Methods to Override:** `getMessageSize()`, `onMessage(size_t size_of_message)`, `reset()`. Must define `using message = YourParsedMessageType;`.
*   **Built-in Protocols (`qb::io::protocol::*`, `qb::protocol::text::*`, `qb::protocol::base::*`)** (`qb/io/protocol/*.h`)
    *   Provide common framing: `text::command` (newline-terminated), `text::string` (null-terminated), `text::string_view` variants, `text::binary8/16/32` (size-prefixed), `json` / `json_packed`.

### File System (`qb::io::sys` & `qb::io::async`)

*   **`qb::io::sys::file`** (`qb/io/system/file.h`): Wrapper for synchronous, native file descriptor operations (`open`, `read`, `write`, `close`).
*   **`qb::io::sys::file_to_pipe` / `pipe_to_file`** (`qb/io/system/file.h`): Utilities for efficient bulk data transfer between files and `qb::allocator::pipe`.
*   **`qb::io::async::file_watcher` / `directory_watcher`** (`qb/io/async/io.h`): CRTP bases for asynchronously monitoring file or directory attribute changes via `on(event::file&)`.

### General Utilities

*   **Cryptography (`qb::crypto`, `qb::jwt`)** (`qb/io/crypto.h`, `qb/io/crypto_jwt.h`): Hashing, encoding, symmetric/asymmetric encryption, JWTs (requires OpenSSL).
*   **Compression (`qb::compression`)** (`qb/io/compression.h`): Gzip, Deflate (requires Zlib).
*   **JSON (`qb::json`, `qb::jsonb`)** (`qb/json.h`): Based on `nlohmann/json` for parsing and manipulation.
*   **Time (`qb::TimePoint`, `qb::Duration`)** (`qb/system/timestamp.h`): High-precision time points and durations.
*   **System Info:** `qb::CPU` (`qb/system/cpu.h`), `qb::endian` (`qb/system/endian.h`).
*   **Containers & Allocators:**
    *   `qb::allocator::pipe<T>` (`qb/system/allocator/pipe.h`): High-performance dynamic buffer.
    *   `qb::string<N>` (`qb/string.h`): Fixed-capacity, stack-friendly string.
    *   `qb::unordered_map/set` (`qb/system/container/*.h`): Fast hash tables (uses `ska::flat_hash_map/set`).
    *   `qb::icase_unordered_map`.
*   **Lock-Free Primitives (`qb::lockfree::*`)** (`qb/system/lockfree/*.h`): `SpinLock`, `spsc::ringbuffer`, `mpsc::ringbuffer` (primarily internal framework use).
*   **UUID (`qb::uuid`)** (`qb/uuid.h`): RFC 4122 UUID generation.
*   **Low-Level Utilities (`qb/utility/*.h`):** Advanced type traits, `nocopy` mixin, `functional::hash_combine`, build macros, branch prediction hints.

This overview provides a map to the QB Framework's extensive API. Use it to identify relevant components for your tasks and then consult specific headers or detailed documentation pages for more in-depth information.

**(Next:** Consult the [QB Actor Framework: Frequently Asked Questions (FAQ)](./faq.md) or [QB Actor Framework: Glossary of Terms](./glossary.md) for further reference.**) 