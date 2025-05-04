# Glossary

*   **Actor (`qb::Actor`):** The fundamental unit of computation and state in the QB framework. Actors encapsulate state and behavior, communicating solely through asynchronous messages (Events).
*   **Actor ID (`qb::ActorId`):** A unique identifier for an actor, composed of a `CoreId` and a `ServiceId`.
*   **Actor Model:** A concurrency model where actors are primitive units that communicate via message passing.
*   **Asynchronous I/O:** Input/Output operations that do not block the calling thread while waiting for completion.
*   **Async System (`qb::io::async`):** The subsystem within `qb-io` responsible for managing the event loop and asynchronous operations like timers and callbacks.
*   **Broadcast (`broadcast<T>()`, `qb::BroadcastId`):** Sending an event to all actors (either system-wide or on a specific core).
*   **Callback (`qb::ICallback`, `qb::io::async::callback`):** A function or method executed asynchronously. `ICallback` is for periodic actor execution; `async::callback` schedules a one-off function call.
*   **Core ID (`qb::CoreId`):** An identifier for a specific `VirtualCore` (worker thread).
*   **CoreSet (`qb::CoreSet`):** A set of physical CPU core IDs used for setting thread affinity.
*   **CRTP (Curiously Recurring Template Pattern):** A C++ idiom using templates where a base class template takes the derived class as a template parameter (e.g., `qb::io::async::io<Derived>`). Used in `qb-io` for static polymorphism.
*   **Endpoint (`qb::io::endpoint`):** A representation of a network address (IP address and port) or Unix domain socket path.
*   **Event (`qb::Event`):** A message passed between actors. Derived structs define specific message types and payloads.
*   **Event Loop (`qb::io::async::listener`):** The central mechanism (based on libev) that monitors I/O events, timers, and signals, dispatching them to appropriate handlers.
*   **Forward (`forward(dest, event)`):** Efficiently redirects a received event to a new destination actor, reusing the original event object.
*   **Framing:** The process of delimiting messages within a continuous byte stream (handled by Protocols).
*   **IO Handler (`qb::io::async::io_handler`):** A base class used in servers to manage multiple client sessions.
*   **libev:** A high-performance C event loop library used internally by `qb::io::async::listener`.
*   **Listener (`qb::io::async::listener`, `qb::io::tcp::listener`):** The async event loop manager. Also, specifically, the class responsible for accepting incoming TCP/SSL connections.
*   **Lock-Free:** Data structures or algorithms designed to allow concurrent access from multiple threads without using traditional locks (mutexes), often relying on atomic operations.
*   **Mailbox:** An implicit queue associated with each actor (or `VirtualCore`) where incoming events are stored before processing.
*   **MPSC Queue (Multiple-Producer, Single-Consumer):** A queue optimized for scenarios where many threads write to the queue, but only one thread reads from it. Used for inter-core communication.
*   **Pipe (`qb::allocator::pipe`, `qb::Pipe`):** An efficient, dynamically resizing buffer (`allocator::pipe`) used for I/O and event serialization. `qb::Pipe` is a handle to the communication channel between actors.
*   **Protocol (`qb::io::async::AProtocol`):** Defines the rules for interpreting a raw byte stream as a sequence of messages (framing and potentially parsing).
*   **Push (`push<T>(...)`):** The primary method for sending ordered events between actors.
*   **RAII (Resource Acquisition Is Initialization):** A C++ technique where resource lifetime is bound to object lifetime using constructors and destructors (e.g., `std::unique_ptr`, `std::ofstream`). Recommended for resource management within actors.
*   **Referenced Actor:** An actor created via `actor.addRefActor<T>()`, returning a raw pointer to the parent. Resides on the same core as the parent.
*   **Reply (`reply(event)`):** Efficiently sends a received event back to its original sender, reusing the event object.
*   **Send (`send<T>(...)`):** Method for sending unordered events, potentially with lower latency for same-core communication but requiring careful use.
*   **Service Actor (`qb::ServiceActor<Tag>`):** A pattern for creating singleton actors identified by a unique `Tag` struct, typically one instance per core.
*   **Service ID (`qb::ServiceId`):** Part of an `ActorId` that uniquely identifies an actor *within* a specific `VirtualCore`.
*   **Session:** Represents a single client connection managed by a server-side component (e.g., `ChatSession`, `BrokerSession`).
*   **Spinlock (`qb::lockfree::SpinLock`):** A lock that uses busy-waiting instead of blocking.
*   **SPSC Queue (Single-Producer, Single-Consumer):** A queue optimized for communication between exactly two threads.
*   **Stream (`qb::io::stream`, `istream`, `ostream`):** Base templates providing buffered I/O abstractions.
*   **Transport:** A component responsible for the specifics of a particular I/O type (TCP, UDP, File, SSL), usually inheriting from a stream template.
*   **URI (`qb::io::uri`):** Represents and parses Uniform Resource Identifiers.
*   **`use<>` (`qb::io::use<Derived>`):** A template helper struct providing convenient type aliases for integrating `qb-io` components (like clients, servers) into other classes (often actors).
*   **Virtual Core (`qb::VirtualCore`):** A worker thread managed by `qb::Main` that executes actors and runs an event loop.
*   **JSON (`qb::json`, `qb::jsonb`):** JSON data representation and manipulation, typically using the nlohmann/json library internally.
*   **JWT (`qb::jwt`):** JSON Web Token creation, signing, and verification.
*   **UUID (`qb::uuid`):** Universally Unique Identifier (RFC 4122) for unique object identification.