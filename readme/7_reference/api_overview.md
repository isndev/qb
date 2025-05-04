# Reference: API Overview

This page provides a high-level overview of the most important classes and functions in the QB framework. For detailed information, always refer to the source code headers (`qb/include/`).

## Core (`qb-core`)

*   **`qb::Main` (`qb/core/Main.h`)**
    *   The main engine controller.
    *   `Main()`: Constructor.
    *   `core(CoreId)`: Access `CoreInitializer` for pre-start configuration.
    *   `addActor<T>(CoreId, ...)`: Add an actor to a specific core.
    *   `start(async)`: Start the engine threads.
    *   `join()`: Wait for the engine to stop.
    *   `stop()`: Static method to signal shutdown.
    *   `hasError()`: Check for abnormal core termination.
    *   `registerSignal(int)`, `unregisterSignal(int)`: Manage signal handling.
*   **`qb::Actor` (`qb/core/Actor.h`)**
    *   Base class for all actors.
    *   `virtual bool onInit()`: Override for initialization (register events here!).
    *   `kill()`: Request actor termination.
    *   `id()`: Get the actor's `ActorId`.
    *   `getIndex()`: Get the actor's `CoreId`.
    *   `registerEvent<T>(*this)`: Subscribe to an event type.
    *   `unregisterEvent<T>(*this)`: Unsubscribe from an event type.
    *   `on(const EventType&)` / `on(EventType&)`: Implement these to handle specific events.
    *   `push<T>(dest, ...)`: Send ordered event.
    *   `send<T>(dest, ...)`: Send unordered event.
    *   `broadcast<T>(...)`: Send event to all actors.
    *   `reply(Event&)`: Send event back to source.
    *   `forward(dest, Event&)`: Redirect event to new destination.
    *   `to(dest)`: Get `EventBuilder` for chained pushes.
    *   `getPipe(dest)`: Get `Pipe` for direct channel access.
    *   `addRefActor<T>(...)`: Create a referenced child actor on the same core.
    *   `getService<T>()`: Get same-core service actor pointer.
    *   `getServiceId<Tag>(CoreId)`: Get service actor ID on a specific core.
    *   `require<T>()`: Request dependency notifications.
*   **`qb::ServiceActor<Tag>` (`qb/core/Actor.h`)**
    *   Base class for singleton-per-core actors.
*   **`qb::Event` (`qb/core/Event.h`)**
    *   Base class for all messages.
    *   `getSource()`: Get sender's `ActorId`.
    *   `getDestination()`: Get target `ActorId`.
    *   `getID()`: Get event type ID.
*   **`qb::KillEvent`, `qb::SignalEvent`, `qb::RequireEvent`, `qb::PingEvent` (`qb/core/Event.h`)**
    *   Predefined system events.
*   **`qb::ICallback` (`qb/core/ICallback.h`)**
    *   Interface for actors needing periodic execution.
    *   `virtual void onCallback() = 0;`: Implement this method.
    *   `registerCallback(*this)`, `unregisterCallback(*this)`: Manage callback registration.
*   **`qb::ActorId`, `qb::CoreId`, `qb::ServiceId` (`qb/core/ActorId.h`)**
    *   Identifier types.
    *   `ActorId::sid()`, `ActorId::index()`, `ActorId::is_valid()`, `ActorId::is_broadcast()`.
*   **`qb::BroadcastId(CoreId)` (`qb/core/ActorId.h`)**
    *   Special `ActorId` for core-specific broadcasts.
*   **`qb::CoreSet` (`qb/core/CoreSet.h`)**
    *   Represents a set of physical core IDs for affinity.
*   **`qb::Pipe` (`qb/core/Pipe.h`)**
    *   Communication channel object.
    *   `push<T>(...)`, `allocated_push<T>(size, ...)`.

## IO (`qb-io`)

*   **Async System (`qb/io/async.h`, `qb/io/async/io.h`, `listener.h`)**
    *   `qb::io::async::init()`: Initialize async system for the current thread.
    *   `qb::io::async::run(flag)`: Run the event loop.
    *   `qb::io::async::callback(func, delay)`: Schedule a callback.
    *   `qb::io::async::with_timeout<T>`: Base class for timeout handling (implement `on(event::timer&)`).
    *   `qb::io::async::event::*`: Event types (`disconnected`, `timer`, `file`, `signal`, `eof`, `eos`, etc.).
*   **Socket Wrapper (`qb::io::socket` in `qb/io/system/sys__socket.h`)**
    *   Cross-platform socket operations (used by transports).
    *   `open()`, `bind()`, `listen()`, `accept()`, `connect()`, `connect_n()`, `send()`, `recv()`, `sendto()`, `recvfrom()`, `close()`, `set_nonblocking()`, `set_optval()`, `local_endpoint()`, `peer_endpoint()`, `resolve()`, etc.
*   **Endpoint (`qb::io::endpoint` in `qb/io/system/sys__socket.h`)**
    *   Represents network addresses (IPv4, IPv6, Unix).
    *   `af()`, `ip()`, `port()`, `to_string()`.
*   **URI (`qb::io::uri` in `qb/io/uri.h`)**
    *   URI parsing and manipulation.
    *   `scheme()`, `host()`, `port()`, `path()`, `queries()`, `fragment()`, `encode()`, `decode()`.
*   **Transports (`qb/io/transport/*.h`)**
    *   Base stream templates: `qb::io::istream`, `ostream`, `stream`.
    *   Specific transports: `tcp`, `udp`, `file`, `stcp` (SSL).
    *   Provide `transport()`, `in()`, `out()`, `read()`, `write()`, `publish()`, `close()`.
*   **Protocols (`qb/io/protocol/*.h`, `qb/io/async/protocol.h`)**
    *   `qb::io::async::AProtocol<IO>`: Base interface.
    *   Built-in: `text::command`, `text::string`, `text::string_view`, `binary8/16/32`, `protocol::json`, `protocol::json_packed`.
    *   Implement `getMessageSize()`, `onMessage()`, `reset()`.
*   **File System (`qb::io::system/file.h`)**
    *   `qb::io::sys::file`: File operations.
    *   `qb::io::sys::file_to_pipe`, `pipe_to_file`: Efficient transfers.
*   **Utilities (`qb/io/*.h`, `qb/system/*.h`)**
    *   `qb::crypto` (Optional): Hashing, encryption, etc.
    *   `qb::jwt` (Optional): JSON Web Tokens.
    *   `qb::compression` (Optional): Gzip, Deflate.
    *   `qb::json`, `qb::jsonb`: JSON manipulation (via nlohmann/json).
    *   `qb::Timestamp`, `qb::Duration`: Time handling.
    *   `qb::CPU`, `qb::endian`: System info.
    *   `qb::allocator::pipe`, `qb::string`, `qb::unordered_map/set`: Optimized containers.
    *   `qb::lockfree::*`: Lock-free primitives (`SpinLock`, `spsc::ringbuffer`, `mpsc::ringbuffer`) used internally.
    *   `qb::uuid`: UUID generation.
    *   Utility headers (`qb/utility/*.h`): Type traits, `nocopy`, `functional`, build macros, etc. 