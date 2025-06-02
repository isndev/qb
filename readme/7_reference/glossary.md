@page ref_glossary_md QB Actor Framework: Glossary of Terms
@brief A comprehensive glossary defining key terms, classes, and concepts used throughout the QB Actor Framework documentation.

# QB Actor Framework: Glossary of Terms

This glossary provides definitions for key terms, classes, and concepts frequently encountered when working with the QB Actor Framework. Understanding this terminology will help you navigate the documentation and the framework itself more effectively.

--- 

*   **Actor (`qb::Actor`):** The fundamental, concurrent unit of computation and state in the QB framework. Actors encapsulate their state and behavior, interacting with other actors exclusively through asynchronous message passing (Events).

*   **Actor ID (`qb::ActorId`):** A unique system-wide identifier for an actor instance. It's typically composed of a `CoreId` (identifying the `VirtualCore` the actor runs on) and a `ServiceId` (unique within that core).

*   **Actor Model:** A mathematical model of concurrent computation where "actors" are universal primitives. Key characteristics include state encapsulation, message-based communication, and independent execution.

*   **Affinity (CPU Affinity):** The tendency or configuration for a thread (like a `VirtualCore`) to run on a specific set or subset of CPU cores. QB allows configuring this via `CoreInitializer::setAffinity`.

*   **Asynchronous I/O (`qb-io`):** Input/Output operations (network, file) that do not block the calling thread while waiting for completion. QB relies heavily on this model for responsiveness and scalability.

*   **Async System (`qb::io::async`):** The subsystem within `qb-io` responsible for managing the event loop (`listener`), timers, and asynchronous callbacks.

*   **`AProtocol<IO_Type>` (`qb::io::async::AProtocol`):** An abstract base class (using CRTP) for defining custom message framing and parsing logic over byte streams in `qb-io`.

*   **Broadcast (`actor.broadcast<T>()`, `qb::BroadcastId`):** A mechanism for sending an event to all actors, either system-wide or to all actors on a specific `VirtualCore` (using `qb::BroadcastId`).

*   **Callback (Actor - `qb::ICallback`):** An interface an actor can implement (`onCallback()`) to have a method executed periodically by its `VirtualCore` after event processing.

*   **Callback (I/O - `qb::io::async::callback`):** A `qb-io` utility to schedule a callable (lambda, function) for asynchronous execution on the current thread's event loop, often after a specified delay.

*   **`CoreId` (`qb::CoreId`):** An identifier for a specific `VirtualCore` instance (worker thread).

*   **`CoreInitializer` (`qb::CoreInitializer`):** A helper class obtained via `qb::Main::core(id)` to configure `VirtualCore` properties (like affinity and latency) *before* the engine starts.

*   **`CoreSet` (`qb::CoreSet`):** Represents a set of physical CPU core identifiers, primarily used for setting thread affinity.

*   **CRTP (Curiously Recurring Template Pattern):** A C++ idiom where a class derives from a class template that takes the derived class itself as a template argument (e.g., `qb::io::async::io<MyDerivedClass>`). Used extensively in `qb-io` for static polymorphism and code reuse without vtable overhead.

*   **Endpoint (`qb::io::endpoint`):** A `qb-io` structure representing a network address, encompassing IP address (v4 or v6) and port number, or a Unix domain socket path.

*   **Event (`qb::Event`):** The fundamental unit of communication between actors. Events are messages derived from the `qb::Event` base class, carrying data and representing requests, notifications, or state changes.

*   **Event Loop (`qb::io::async::listener`):** The central mechanism in `qb-io` (and thus in each `VirtualCore`) that monitors I/O readiness, timers, and other event sources, dispatching them to appropriate handlers. It is based on `libev`.

*   **Event Sourcing:** An advanced persistence pattern where changes to an actor's state are captured as a sequence of events, which are then stored. The actor's state can be rebuilt by replaying these events.

*   **Forward (`actor.forward(dest, event)`):** An efficient method for an actor to redirect a received event to a new destination actor, reusing the original event object while updating its source to be the forwarding actor.

*   **Framing (Message Framing):** The process of defining and recognizing message boundaries within a continuous byte stream (e.g., from a TCP connection). Handled by `qb-io` Protocols.

*   **IO Handler (`qb::io::async::io_handler`):** A base class template in `qb-io` (often used via `qb::io::use<...>::tcp::server`) for managing multiple client I/O sessions within a server component.

*   **`libev`:** A high-performance C event loop library used internally by `qb::io::async::listener` to interact with the operating system's event notification mechanisms (like epoll, kqueue).

*   **Listener (Networking - `qb::io::tcp::listener`, `qb::io::tcp::ssl::listener`):** A `qb-io` class responsible for binding to a local network address and port, and accepting incoming TCP or SSL/TLS connections.

*   **Lock-Free:** Refers to data structures or algorithms designed to allow concurrent access from multiple threads without using traditional locks (like mutexes), often relying on atomic CPU operations. QB uses these for high-performance inter-core communication.

*   **Mailbox:** Conceptually, a queue associated with each actor (managed by its `VirtualCore`) where incoming events are stored before being processed by the actor sequentially.

*   **MPSC Queue (Multiple-Producer, Single-Consumer):** A queue optimized for scenarios where many threads can write (produce) items, but only one thread reads (consumes) items. Used in QB for inter-`VirtualCore` event communication (`qb::lockfree::mpsc::ringbuffer`).

*   **Pipe (`qb::allocator::pipe<T>`):** A highly efficient, dynamically resizable memory buffer provided by QB. It's used extensively for I/O buffering and event serialization.

*   **Pipe (Communication Channel - `qb::Pipe`):** An object obtained via `actor.getPipe(dest_id)` representing a direct, optimized communication channel for sending events to another actor.

*   **Protocol (`qb::io::async::AProtocol<IO_Type>`):** In `qb-io`, a class that defines the rules for interpreting a raw byte stream as a sequence of discrete application messages. It handles message framing and often initial parsing.

*   **`push<Event>(...)` (`actor.push<T>()`):** The primary and recommended method for sending **ordered** asynchronous events between actors. Handles non-trivially destructible event types.

*   **RAII (Resource Acquisition Is Initialization):** A core C++ programming technique where resource lifetime is bound to object lifetime. Resources are acquired in an object's constructor and released in its destructor. This is the recommended way to manage resources (memory, file handles, sockets) within QB actors.

*   **Referenced Actor:** An actor created via `parent_actor.addRefActor<ChildType>()`. The parent receives a raw pointer to the child, which resides on the same `VirtualCore`. The parent does not own the child in terms of C++ lifetime; the child manages its own termination.

*   **`reply(Event&)` (`actor.reply()`):** An efficient method for an actor to send a received event (potentially modified) back to its original sender, reusing the event object.

*   **`require<T>()` (`actor.require<T>()`):** A mechanism for an actor to request discovery of other live actors of type `T`. Responses are received as `qb::RequireEvent`s.

*   **`send<Event>(...)` (`actor.send<T>()`):** A method for sending **unordered** asynchronous events. It may offer slightly lower latency for same-core communication but requires the event type to be trivially destructible and offers no ordering guarantees.

*   **Service Actor (`qb::ServiceActor<Tag>`):** A specialized actor type designed to be a singleton instance per `VirtualCore`, identified by a unique `Tag` struct.

*   **Service ID (`qb::ServiceId`):** The component of an `ActorId` that uniquely identifies an actor *within* its assigned `VirtualCore`.

*   **Session (Networking):** Typically represents a single, stateful client connection managed by a server-side component (e.g., `ChatSession` in the chat example).

*   **Spinlock (`qb::lockfree::SpinLock`):** A low-level mutual exclusion lock that uses busy-waiting (spinning) rather than yielding the CPU. Used for very short critical sections where contention is low.

*   **SPSC Queue (Single-Producer, Single-Consumer):** A queue optimized for communication where only one thread produces items and only one thread consumes them (`qb::lockfree::spsc::ringbuffer`).

*   **Stream (`qb::io::istream`, `qb::io::ostream`, `qb::io::stream`):** Base class templates in `qb-io` that provide buffered I/O abstractions over various underlying I/O types (transports).

*   **Transport (`qb::io::transport::*`):** A `qb-io` component that implements the specifics of communication over a particular medium (e.g., `transport::tcp`, `transport::udp`, `transport::file`, `transport::stcp` for SSL/TLS), typically by specializing a `qb::io::stream` template.

*   **URI (`qb::io::uri`):** A `qb-io` class for parsing, representing, and manipulating Uniform Resource Identifiers (RFC 3986).

*   **`use<>` (`qb::io::use<DerivedActor>`):** A CRTP helper template in `qb-io` that simplifies integrating asynchronous I/O capabilities (like TCP client/server behavior, UDP endpoint functionality) directly into an actor class by providing appropriate base class inheritance.

*   **UUID (`qb::uuid`):** Universally Unique Identifier (RFC 4122). `qb::generate_random_uuid()` provides version 4 UUIDs.

*   **`VirtualCore` (`qb::VirtualCore`):** A worker thread managed by `qb::Main`. Each `VirtualCore` executes a group of assigned actors and runs its own independent `qb-io` event loop.

*   **`qb::string<N>`:** A fixed-capacity string class optimized for performance by avoiding heap allocations for strings up to `N` characters. Recommended over `std::string` for direct members in events due to ABI stability.

**(Next:** You might want to review the [QB Actor Framework: Frequently Asked Questions (FAQ)](./faq.md) or revisit the [QB Framework: Detailed API Overview](./api_overview.md).)**