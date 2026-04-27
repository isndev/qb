/**
 * @file doxygen_groups.h
 * @brief Definition of documentation groups for the QB Actor Framework.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - C++ Actor Framework (cpp.actor)
 */

// Top-Level Group
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup QB QB Actor Framework
 * @brief The QB Actor Framework is a modern **C++23** library for building high-performance,
 * concurrent, and distributed systems based on the Actor Model.
 *
 * It combines an efficient actor engine (`qb-core`) with a robust asynchronous I/O library
 * (`qb-io`) and native C++20/23 coroutine support to simplify complex application development.
 * Actors communicate exclusively via typed events, guaranteeing data isolation and
 * eliminating the need for manual locking within an actor's own state.
 */

// Core Actor System Modules
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup Core Core Actor System
 * @ingroup QB
 * @brief Fundamental components implementing the Actor Model.
 *
 * This module includes the actor base class (`qb::Actor`), the event system (`qb::Event`),
 * the engine controller (`qb::Main`), virtual cores for scheduling (`qb::VirtualCore`),
 * actor communication primitives (`qb::Pipe`), and lifecycle utilities.
 */

/**
 * @defgroup Actor Actor Components
 * @ingroup Core
 * @brief Defines actors, their identification, and lifecycle management.
 *
 * Includes `qb::Actor`, `qb::ActorId`, `qb::ServiceActor`, `qb::RefActorHandle`,
 * `qb::CoroContext`, and related concepts for creating and managing concurrent entities.
 *
 * ### Quick start
 * ```cpp
 * class MyActor : public qb::Actor {
 * public:
 *     bool onInit() override {
 *         registerEvent<MyEvent>(*this);
 *         return true;
 *     }
 *     void on(MyEvent const& e) { /* handle */ }
 * };
 * ```
 */

/**
 * @defgroup Concepts C++20 Concepts
 * @ingroup Core
 * @brief C++20 concepts used for compile-time constraint enforcement throughout qb-core.
 *
 * These concepts provide statically-checked requirements on template parameters,
 * enabling clearer compiler diagnostics and safer generic code.
 *
 * Key concepts:
 * - `qb::event_type<T>` — T must derive from `qb::Event`
 * - `qb::actor_type<T>` — T must derive from `qb::Actor`
 * - `qb::service_type<T>` — T must derive from `qb::Service`
 * - `qb::callback_type<T>` — T must derive from `qb::ICallback`
 * - `qb::trivial_event<T>` — T must be an `event_type` and trivially destructible
 * - `qb::event_qos0_type<T>` — T must derive from `qb::EventQOS0`
 * - `qb::service_event_type<T>` — T must derive from `qb::ServiceEvent`
 */

/**
 * @defgroup EventCore Core Event System
 * @ingroup Core
 * @brief Base event types and core system events for actors.
 *
 * Defines `qb::Event` and essential system events:
 * - `qb::KillEvent` — graceful actor termination
 * - `qb::SignalEvent` — OS signal delivery
 * - `qb::PingEvent` / `qb::RequireEvent` — actor discovery protocol
 * - `qb::UnregisterCallbackEvent` — dynamic callback removal
 *
 * This group covers actor-level events; for I/O events see @ref AsyncEvent.
 */

/**
 * @defgroup Event Event System (General)
 * @ingroup QB
 * @brief General event concepts and system-level event handling including I/O events.
 *
 * Covers the broader event mechanisms in QB, encompassing both actor events
 * and asynchronous I/O events from the IO module.
 */

/**
 * @defgroup Engine Engine & Scheduling
 * @ingroup Core
 * @brief Manages actor execution, virtual cores, and system lifecycle.
 *
 * Contains `qb::Main` for engine control, `qb::VirtualCore` for per-thread actor
 * execution, `qb::CoreInitializer` for pre-start configuration, `qb::CoreSet` for
 * CPU affinity, and `qb::SharedCoreCommunication` for lock-free inter-core messaging.
 *
 * ### Minimal engine bootstrap
 * ```cpp
 * qb::Main engine;
 * engine.addActor<MyActor>(0 /* core id */);
 * engine.start();      // async by default
 * engine.join();
 * ```
 */

/**
 * @defgroup Callback Callback System
 * @ingroup Core
 * @brief Support for periodic callbacks within actors.
 *
 * Includes the `qb::ICallback` interface. Actors that inherit from it can register
 * themselves with `registerCallback(*this)` to have `onCallback()` invoked on every
 * iteration of their `VirtualCore`'s event loop. The callback must be fast and
 * non-blocking.
 */

/**
 * @defgroup PipeCore Core Communication Channels
 * @ingroup Core
 * @brief Primitives for direct actor-to-actor communication channels.
 *
 * Focuses on the `qb::Pipe` class, which is the low-level communication channel
 * returned by `Actor::getPipe()`. Using `Pipe` directly enables:
 * - Fluent multi-event sending via method chaining
 * - `allocated_push()` to pre-size the buffer for large payloads
 */

// IO System Modules
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup IO IO System
 * @ingroup QB
 * @brief Asynchronous I/O operations, networking, and related utilities.
 *
 * Provides non-blocking I/O for TCP, UDP, SSL, files, along with protocols,
 * cryptographic functions, and compression.
 */

/**
 * @defgroup Async Asynchronous System
 * @ingroup IO
 * @brief Core mechanisms for event-driven asynchronous programming.
 *
 * Includes the event listener (`qb::io::async::listener`), base async I/O classes
 * (`qb::io::async::input`, `qb::io::async::output`, `qb::io::async::io`),
 * timed callbacks (`qb::io::async::callback`, `qb::io::async::scoped_callback`),
 * timeout helpers (`qb::io::async::with_timeout`, `qb::io::async::Timeout`,
 * `qb::io::async::ScopedTimeout`), and file/directory watchers.
 *
 * The async system supports two complementary programming models that share
 * the same single-threaded event loop:
 * - **Event-driven callbacks** — override `on(event::X&&)` methods.
 * - **C++23 coroutines** — use `co_await` with `task<T>`, sleep(), and awaiters.
 *
 * @see Coroutine
 */

/**
 * @defgroup Coroutine C++23 Coroutine Support
 * @ingroup Async
 * @brief First-class C++23 coroutine infrastructure for `qb-io`.
 *
 * Provides a complete coroutine ecosystem layered on top of the `listener`/libev
 * event loop. All coroutines on a thread share a single `CoroutineScheduler` and
 * are **never concurrent** — interleaving occurs only at `co_await` points.
 *
 * ### Key components
 *
 * | Header | Exports |
 * |--------|---------|
 * | `coroutine/task.h`       | `task<T>` — lazy, move-only coroutine return type |
 * | `coroutine/shared_task.h`| `shared_task<T>` — multi-consumer shared result |
 * | `coroutine/scheduler.h`  | `CoroutineScheduler`, `coro_scheduler()` |
 * | `coroutine/awaiter.h`    | `timer_awaiter`, `socket_awaiter`, `async_awaiter<T>` |
 * | `coroutine/utils.h`      | `sleep()`, `wait_readable()`, `wait_writable()`, `run_for()` |
 * | `coroutine/combinators.h`| `when_all()`, `when_any()`, `race()`, `coro_with_timeout()` |
 * | `coroutine/cancellation.h`| `cancellation_token`, `cancellable_sleep()`, `with_deadline()` |
 * | `coroutine/sync.h`       | `semaphore`, `async_mutex`, `async_rw_lock`, `barrier`, `async_event`, `async_latch` |
 * | `coroutine/channel.h`    | `channel<T>`, `select()`, pipeline utilities |
 * | `coroutine/scope.h`      | `coroutine_scope`, `with_scope()`, `parallel_map()` |
 * | `coroutine/generator.h`  | `generator<T>`, `async_generator<T>`, consumers |
 * | `coroutine/stream.h`     | `async_stream<T>`, `interval()`, `merge_streams()`, `zip()` |
 * | `coroutine/retry.h`      | `retry_policy`, `with_retry()`, `make_retryable()` |
 * | `coroutine/mixin.h`      | `coro_mixin<Derived>` — CRTP helper |
 * | `coroutine.h`            | Umbrella include |
 *
 * ### Quick start
 * @code
 * #include <qb/io/async/coroutine.h>
 *
 * qb::io::async::task<int> fetch_data() {
 *     co_await qb::io::async::sleep(std::chrono::milliseconds(100));
 *     co_return 42;
 * }
 *
 * int main() {
 *     qb::io::async::init();
 *     qb::io::async::coro_scheduler().spawn(fetch_data());
 *     qb::io::async::run();
 * }
 * @endcode
 *
 * @see task
 * @see CoroutineScheduler
 * @see timer_awaiter
 * @see socket_awaiter
 */

/**
 * @defgroup AsyncEvent Asynchronous I/O Events
 * @ingroup Async
 * @brief Specific event types for asynchronous I/O operations.
 *
 * Such as \`qb::io::async::event::disconnected\`, \`qb::io::async::event::timer\`, etc.
 * These are distinct from actor system events in EventCore.
 * @see EventCore
 */

/**
 * @defgroup Networking Networking Utilities
 * @ingroup IO
 * @brief Socket wrappers, endpoint representation, and URI parsing.
 *
 * Contains \`qb::io::socket\`, \`qb::io::endpoint\`, and \`qb::io::uri\`.
 */

/**
 * @defgroup TCP TCP Communication
 * @ingroup Networking
 * @brief Components for TCP-based network communication.
 *
 * Includes \`qb::io::tcp::socket\` and \`qb::io::tcp::listener\`.
 */

/**
 * @defgroup UDP UDP Communication
 * @ingroup Networking
 * @brief Components for UDP-based network communication.
 *
 * Includes \`qb::io::udp::socket\`.
 */

/**
 * @defgroup SSL Secure Sockets Layer (SSL/TLS)
 * @ingroup Networking
 * @brief Components for secure, encrypted TCP communication (requires OpenSSL).
 *
 * Includes \`qb::io::tcp::ssl::socket\` and \`qb::io::tcp::ssl::listener\`.
 */

/**
 * @defgroup Transport Transport Layer
 * @ingroup IO
 * @brief Abstractions over network sockets and file operations for stream-based I/O.
 *
 * Contains classes like \`qb::io::transport::tcp\`, \`qb::io::transport::udp\`,
 * \`qb::io::transport::stcp\`.
 */

/**
 * @defgroup Protocol Protocol Handling
 * @ingroup IO
 * @brief Message framing and parsing implementations.
 *
 * Defines \`qb::io::async::AProtocol\` and built-in protocols like
 * text-based, binary, and JSON.
 */

/**
 * @defgroup FileSystem File System Operations
 * @ingroup IO
 * @brief Components for interacting with the local file system.
 *
 * Includes synchronous file operations (\`qb::io::sys::file\`) and asynchronous
 * file watching (\`qb::io::async::file_watcher\`).
 */

/**
 * @defgroup Crypto Cryptographic Utilities
 * @ingroup IO
 * @brief Hashing, encryption, and JWT functionalities (requires OpenSSL).
 *
 * Contains \`qb::crypto\` and \`qb::jwt\`.
 */

/**
 * @defgroup Compression Compression Utilities
 * @ingroup IO
 * @brief Data compression and decompression (requires Zlib).
 *
 * Contains \`qb::compression\`.
 */

/**
 * @defgroup JSON JSON Utilities
 * @ingroup IO
 * @brief JSON parsing, manipulation, and serialization utilities.
 *
 * Provides \`qb::json\`, \`qb::jsonb\`, and integration with the nlohmann/json library.
 */

// System-Level Utilities
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup System System-Level Utilities
 * @ingroup QB
 * @brief Low-level system interactions and information.
 */

/**
 * @defgroup LockFree Lock-Free Primitives
 * @ingroup System
 * @brief Concurrent data structures without traditional locks.
 *
 * Includes spinlocks, MPSC/SPSC queues.
 */

/**
 * @defgroup SystemInfo System Information
 * @ingroup System
 * @brief Utilities for querying CPU and system properties.
 *
 * Contains \`qb::CPU\` and \`qb::endian\`.
 */

/**
 * @defgroup Time Time Utilities
 * @ingroup System
 * @brief High-precision timestamp and duration classes.
 *
 * Contains \`qb::Timestamp\` and \`qb::Duration\`.
 */

// General Utilities
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup Utility General Utilities
 * @ingroup QB
 * @brief General-purpose helper classes and functions.
 */

/**
 * @defgroup Container Containers & Allocators
 * @ingroup Utility
 * @brief Custom containers and memory allocators for performance.
 *
 * Includes \`qb::allocator::pipe\`, \`qb::string\`, and optimized hash maps/sets.
 */

/**
 * @defgroup TypeTraits Type Traits & Metaprogramming
 * @ingroup Utility
 * @brief Advanced type traits and metaprogramming helpers.
 *
 * Contains utilities from \`qb/utility/type_traits.h\`.
 */

/**
 * @defgroup MiscUtils Miscellaneous Utilities
 * @ingroup Utility
 * @brief Other small helper utilities.
 *
 * Includes \`qb::nocopy\`, \`qb::functional\`, branch prediction hints, etc.
 */ 